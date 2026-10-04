// SRWeaver.h — wraps the Simulated Reality context and the DirectX 11 weaver.
// Owns the SBS "view" texture that is handed to the weaver each frame.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <memory>

// Forward declarations of SR SDK types (kept out of the header to avoid
// leaking <windows.h>/SDK includes into the rest of the app).
namespace SR
{
    class SRContext;
    class IDX11Weaver1;
    class IDX12Weaver1;
    class PredictingDX12Weaver;
    class IWeaverSettings1;
    class PredictingDX11Weaver;
    class HeadPoseTracker;
    class Window2;
    class SwitchableLensHint;
}

struct ID3D12Resource;
namespace srw { class Present12; }
namespace srw
{
    class SRWeaver
    {
    public:
        SRWeaver() = default;
        ~SRWeaver();

        // Connect to the SR service. Waits up to maxSeconds for it to come up.
        // lensPreference false: the lens stays off (unless another app wants it)
        // -- for asking the runtime things without switching the lenses on.
        bool CreateContext(double maxSeconds, bool lensPreference = true);

        // Query the SR display's screen rectangle (virtual-desktop coords).
        // Returns false until the display is connected/ready.
        bool GetSRDisplayRect(RECT& out);
        // The SR runtime's recommended size for the side-by-side views texture
        // (both eyes): more detail per eye than this can't show on the panel.
        bool GetRecommendedViewsSize(int& w, int& h);

        // Returns the SR Platform runtime version, in the form
        // "MAJOR.MINOR.PATCH.GITHASH" (empty string on failure). Useful in
        // logs + the "About" surface for support diagnostics.
        static const char* GetSRPlatformVersion();

        // Returns true if the (0,0,w,h) region of `hwnd` is at least partially
        // visible (not fully occluded by other windows). Backed by the SR
        // SDK's Window2 (lazily created per HWND). Defaults to true on any
        // failure -- the caller should treat false strictly (i.e. "definitely
        // occluded, safe to skip rendering").
        bool IsWindowPartVisible(HWND hwnd, int width, int height);
        // Late latching on/off (runtime). Returns the resulting state.
        bool SetLateLatching(bool on);

        // Cooperative lens-state hints. Asks the SR runtime to enable or
        // disable the SR display's lenticular lens. Cooperative across all
        // running SR apps -- if multiple apps run, the lens stays ON while
        // any one of them has requested it. No-ops if the SR context isn't
        // up. Use to keep the SR session alive while temporarily backing
        // off to plain 2D (e.g. Katanga arm without a publishing game).
        void LensEnable();
        void LensDisable();
        bool IsLensEnabled() const;

        // Latency-predicted per-eye positions (mm, absolute) -- returns the
        // L+R eye centres the weaver itself will use for the next weave().
        // More accurate than head-centre +/- assumed-IOD because it uses
        // ACTUAL per-eye positions, and it's already prediction-compensated
        // for the configured render-pipeline latency. Returns false if the
        // weaver isn't created yet.
        bool GetPredictedEyePositions(float leftXYZ[3], float rightXYZ[3]);
        // The weaver's pipeline latency (see SRWeaver.cpp).
        uint64_t GetLatencyUs() const;
        void     SetLatencyUs(uint64_t us);
        void     ReadLatency();
        bool     TakeEventCounts(int out[5]);   // (SR system events, for the perf log)
        void     ApplyAct(int mode, int strengthPct, int contrastPct);   // (anti-crosstalk: see SRWeaver.cpp)
        // The lens' slant (a coefficient) and pitch (pixels across), as the runtime
        // reports them; false: not known.
        bool     GetLens(float& slant, float& pitchPx) const;
        bool     GetLensGeometry(float& doNmm, float& dotPitchMm) const;
        // Light field: the lens as seen from a viewing position (cm, from the
        // display's centre), from the SDK's weaving library. See SRWeaver.cpp.
        bool     GetLightFieldGeometry(float w, float h, float xCm, float yCm, float zCm, float& pitchPx, float& slant, float& centrePhase);

        // Create the weaver bound to the output window + device context, then
        // finalize the SR context. Call after the D3D device exists.
        bool CreateWeaver(ID3D11DeviceContext* immediateContext, HWND window);

        // Start/stop the SR session (context + weaver). Stopping releases the SR
        // display's lens and eye-tracking; the loaded image texture is kept.
        bool StartSR(ID3D11DeviceContext* immediateContext, HWND window);
        void StopSR();

        // Register the texture the weaver should sample as its SBS input. The
        // weaver re-samples this view live each weave(), so callers that update
        // the underlying texture in place only need to call this on size change.
        // perEyeWidth is half the full SBS width.
        void SetInputView(ID3D11ShaderResourceView* srv, int perEyeWidth,
                          int height, DXGI_FORMAT format);

        // Load a stereo image from disk into an owned SBS texture and register
        // it as the input. fmt currently distinguishes SBS variants only.
        bool SetStereoImageFromFile(ID3D11Device* device,
                                    const char* path,
                                    StereoFormat fmt,
                                    DXGI_FORMAT texFormat);

        // Upload an already-decoded RGBA8 image as the stereo texture (used
        // for formats with their own loader -- e.g. LFP plenoptic data
        // which gets decoded to an SBS image on the CPU side before
        // landing here). `pixels` must be tightly-packed RGBA8 of size
        // w * h * 4 bytes.
        bool SetStereoImageFromPixels(ID3D11Device* device,
                                       const uint8_t* pixels,
                                       int w, int h,
                                       DXGI_FORMAT texFormat);

        // Perform weaving into the currently-bound render target.
        void Weave();

        void Shutdown();

        // The loaded test image as a source for the converter.
        ID3D11ShaderResourceView* SourceSRV() const { return m_viewSRV; }
        int SourceWidth()  const { return m_imgW; }
        int SourceHeight() const { return m_imgH; }

        bool HasContext() const { return m_context != nullptr; }

        // Borrowed pointer to the SRContext. Used by the OpenTrack bridge
        // (and anything else that wants to attach its own SR::HeadPoseTracker
        // / sense stream without spinning up a second context). Lifetime is
        // tied to this SRWeaver -- callers must Disable any borrowed
        // subscriptions before Shutdown.
        SR::SRContext* Context() const { return m_context; }
        bool HasWeaver()  const { return m_weaver != nullptr || m_legacy != nullptr || m_weaver12 != nullptr || m_legacy12 != nullptr || m_lensOnlyUp; }
        // Lens only: the next SR session holds the lens on with no weaver and no
        // tracker (see StartSR). Set before the session starts; IsLensOnly: the
        // session that is up is one.
        void SetLensOnly(bool on) { m_lensOnly = on; }
        bool IsLensOnly() const { return m_lensOnlyUp; }
        // The Direct3D 12 presenter to weave on when the weaver choice is DX12
        // (4); set before the SR session starts. Null: Direct3D 11 weavers only.
        void SetPresenter12(Present12* p) { m_p12 = p; }
        bool IsDX12() const { return m_weaver12 != nullptr || m_legacy12 != nullptr; }
        void SetInputOverride12(ID3D12Resource* res, int perEyeWidth, int height, DXGI_FORMAT format);
        bool HasInputOverride12() const { return m_override12; }
        bool TakeOverrideDropped() { const bool d = m_overrideDropped; m_overrideDropped = false; return d; }
        // Which weaver is running (Settings::ReadWeaverChoice when it was made):
        // 0 modern, 1-3 legacy with anti-crosstalk Off / Static / Dynamic, 4 the
        // modern Direct3D 12 one, 5 / 6 the older Direct3D 12 one with static /
        // dynamic anti-crosstalk.
        int  WeaverChoice() const { return m_choice; }

        // Latest tracked head pose (position in mm relative to display centre,
        // orientation in radians as (pitch, yaw, roll)). Returns false if the
        // head-pose stream has never delivered a sample. Thread-safe.
        bool GetHeadPose(double pos[3], double orient[3]) const;

    private:
        class HeadListenerImpl;          // opaque to keep SDK headers out of this file
        class SystemListenerImpl;        // (the runtime's system events, logged)
        SystemListenerImpl*       m_sysListener = nullptr;
        void ReleaseViewTexture();
        void StartHeadTracker();
        bool CreateLegacyWeaver(ID3D11DeviceContext* immediateContext, HWND window);
        bool CreateWeaver12(HWND window);
        bool CreateLegacy12(HWND window);
        void SetInput12(ID3D12Resource* res, int perEyeWidth, int height, DXGI_FORMAT format);
        bool FinishWeaver();   // (head tracker, context initialise, lens hint: either weaver)
        void StopHeadTracker();

        SR::SRContext*            m_context = nullptr;
        SR::IDX11Weaver1*         m_weaver  = nullptr;
        // The Direct3D 12 weaver, instead of m_weaver, drawing on m_p12's command list.
        SR::IDX12Weaver1*         m_weaver12 = nullptr;
        SR::PredictingDX12Weaver* m_legacy12 = nullptr;  // (the older one: anti-crosstalk on Direct3D 12)
        Present12*                m_p12     = nullptr;   // not owned
        ID3D12Resource*           m_input12 = nullptr;   // its input as last set (not owned: the presenter's)
        bool                      m_override12 = false;  // (SetInputOverride12)
        bool                      m_overrideDropped = false;
        uint64_t                  m_latencyUs = 0;       // (ReadLatency / SetLatencyUs)
        bool                      m_lensOnly = false, m_lensOnlyUp = false;   // (SetLensOnly / the session up is one)
        float                     m_lensSlant = 0.0f, m_lensPitchPx = 0.0f, m_lensDoNmm = 0.0f, m_dotPitchMm = 0.0f;
        SR::IWeaverSettings1*     m_ws = nullptr;        // the runtime's settings for the standard weaver in use (not owned)
        bool                      m_actDefKnown = false; // (ApplyAct: the display's own values, read once a weaver)
        int                       m_actDefMode = 0;
        float                     m_actDefStatic = 0.0f, m_actDefDynamic = 0.0f;
        int                       m_input12W = 0, m_input12H = 0;
        DXGI_FORMAT               m_input12Fmt = DXGI_FORMAT_UNKNOWN;
        // The legacy (deprecated) weaver, instead of m_weaver when chosen: it
        // has the anti-crosstalk modes the modern one lacks.
        SR::PredictingDX11Weaver* m_legacy  = nullptr;
        ID3D11DeviceContext*      m_immediate = nullptr;   // (the legacy weave needs the output size: its viewport)
        int                       m_choice  = 0;
        SR::HeadPoseTracker*      m_headTracker  = nullptr;
        HeadListenerImpl*         m_headListener = nullptr;
        // Window2 instance for occlusion-check (isWindowPartVisible). Owned
        // here so the lifetime matches our SRContext. Lazily created per
        // HWND -- rebuilt if the HWND changes.
        std::shared_ptr<SR::Window2> m_window2;
        HWND                      m_window2Hwnd = nullptr;
        // SwitchableLensHint instance is owned by the SRContext (per SDK
        // docs -- "should not be explicitly deleted"). We keep a raw pointer
        // and null it when the context is torn down.
        SR::SwitchableLensHint*   m_lensHint = nullptr;
        int                       m_lensReq  = -1;        // last enable(1) / disable(0) we asked for (-1: none yet)
        ID3D11Texture2D*          m_viewTex = nullptr;
        ID3D11ShaderResourceView* m_viewSRV = nullptr;
        int                       m_imgW    = 0;   // loaded image full size
        int                       m_imgH    = 0;
    };
}
