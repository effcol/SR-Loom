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
    class HeadPoseTracker;
    class Window2;
    class SwitchableLensHint;
}

namespace srw
{
    class SRWeaver
    {
    public:
        SRWeaver() = default;
        ~SRWeaver();

        // Connect to the SR service. Waits up to maxSeconds for it to come up.
        bool CreateContext(double maxSeconds);

        // Query the SR display's screen rectangle (virtual-desktop coords).
        // Returns false until the display is connected/ready.
        bool GetSRDisplayRect(RECT& out);

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
        bool HasWeaver()  const { return m_weaver != nullptr; }

        // Latest tracked head pose (position in mm relative to display centre,
        // orientation in radians as (pitch, yaw, roll)). Returns false if the
        // head-pose stream has never delivered a sample. Thread-safe.
        bool GetHeadPose(double pos[3], double orient[3]) const;

    private:
        class HeadListenerImpl;          // opaque to keep SDK headers out of this file
        void ReleaseViewTexture();
        void StartHeadTracker();
        void StopHeadTracker();

        SR::SRContext*            m_context = nullptr;
        SR::IDX11Weaver1*         m_weaver  = nullptr;
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
        ID3D11Texture2D*          m_viewTex = nullptr;
        ID3D11ShaderResourceView* m_viewSRV = nullptr;
        int                       m_imgW    = 0;   // loaded image full size
        int                       m_imgH    = 0;
    };
}
