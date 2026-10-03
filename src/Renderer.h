// Renderer.h — owns the D3D11 device, swap chain and back-buffer for the
// output window. The SR weaver renders into this swap chain's back buffer.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>   // IDXGISwapChain1, IDXGIFactory2
#include <chrono>
#include <vector>
#include <mutex>

struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;
namespace srw { class Present12; }

namespace srw
{
    class Renderer
    {
    public:
        bool IsFlipModel() const { return m_flip; }
        Renderer() = default;
        ~Renderer();

        // Create the device + swap chain for the given window. Returns false on failure.
        // useDComp: present through DirectComposition (the window must have been
        // created with WS_EX_NOREDIRECTIONBITMAP); one composition swap chain
        // serves every mode, and see-through areas are drawn as transparent
        // pixels (SetVisibleRects) instead of reshaping the window.
        bool Initialize(HWND hwnd, bool useDComp = false);
        bool IsDComp() const { return m_dcomp != nullptr; }
        // DirectComposition: the rate our window is being composed at (Hz, 0 = unknown).
        double CompositionRateHz() const;

        // DirectComposition only: which parts of the window show the weave
        // (client pixels); everything else is see-through, in the same frame
        // as the picture. All = the whole window.
        void SetVisibleAll();
        // DirectComposition: apply the see-through mask now (normally done
        // inside Present; separate so it can be timed on its own).
        void ApplyMask();
        void SetVisibleRects(const std::vector<RECT>& rects);
        // Auto Stereo: pictures that move with a GpuTracker offset (read on the
        // GPU this frame, so the see-through cut-out moves with the picture),
        // minus areas never shown (windows in front, taskbar, 2D windows).
        struct MaskTracked { RECT rect{}; RECT clip{}; int slot = -1; float scale = 2.0f; };
        // A hole (rounded corners). owner: the tracked picture it belongs to (a
        // window in front of THAT picture -- other pictures there stay
        // visible), or -1: a hole in everything (taskbar, 2D windows).
        struct MaskCut     { RECT rect{}; int radius = 0; int owner = -1; };
        // Whole window except these holes (Fullscreen / Looking Glass cut-outs).
        void SetVisibleAllExcept(const std::vector<MaskCut>& holes);
        void SetVisibleTracked(const std::vector<MaskTracked>& tracked, const std::vector<MaskCut>& excl,
                               ID3D11ShaderResourceView* gpuResults);
        void Shutdown();

        // Resize the swap chain to the new client size (called on WM_SIZE).
        bool Resize(UINT width, UINT height);

        // Bind the back buffer as render target, clear it, and set a full-window viewport.
        // Call this right before SRWeaver::Weave().
        void BindAndClearBackBuffer();

        // Present the woven frame. vsync=true waits for one v-blank; vsync=false
        // presents immediately (with tearing allowed on a flip swapchain — ideal for
        // VRR displays and lowest latency).
        // flushDwm (bit-blt only): block until the next composition after
        // presenting (paces the loop). Pass false when the caller paces
        // itself -- e.g. on capture frame arrival.
        void Present(bool vsync, bool flushDwm = true);

        // Block until the flip swap chain can accept a new frame (frame-latency
        // waitable object). Paces the render loop to the display's refresh with ~1
        // frame of latency WITHOUT a vsync stall, so no-vsync presents don't spin
        // the GPU at thousands of fps. No-op on the bit-blt path (vsync paces it).
        // Also enforces the render-rate cap set by SetTargetRefreshHz, if any.
        void WaitForFrame();
        // The last WaitForFrame's parts (ms): waiting for the compositor to take
        // the previous frame, and for the SR display's vertical blank.
        double LastCompositorWaitMs() const { return m_lastCompositorWaitMs; }
        // Experiment (Settings WeavePlane): the DirectComposition swap chain opaque and
        // allowed to tear, so it can be put on a display plane of its own. Before
        // Initialize.
        void SetPlaneMode(bool on) { m_planeMode = on; }
        // ... chosen by itself: opaque (straight to the display) while nothing has to
        // show through, see-through otherwise. See UpdateAutoPlane.
        void SetAutoPlane(bool on) { m_autoPlane = on; }
        bool IsPlane() const { return m_planeMode; }
        // The Direct3D 12 presenter (the DX12 weaver choice) in place of the
        // Direct3D 11 one, or back: DirectComposition only. False when it could
        // not be made (the Direct3D 11 presenter then stays, or is restored).
        // hdr: its swap chain 16-bit float scRGB (Settings HdrOutput).
        bool SetDX12(bool on, bool hdr = false);
        bool WeaveBounds(RECT& out) const;
        bool DisplayIsHdr(float* maxNits = nullptr);
        bool BlitPicture(ID3D11Texture2D* src);   // (see Renderer.cpp)
        bool ReadBackRows(UINT y0, UINT rows, std::vector<uint8_t>& out);   // (see Renderer.cpp)
        // Weave call to picture on the display, as measured since the last call
        // (ms; false: no measurements).
        bool TakeDisplayLatency(double& avgMs, double& minMs, double& maxMs, int& n);
        // (Measured only when asked for: the Perf Log.)
        void SetLatencyStats(bool on) { m_latencyStats = on; }
        // The Direct3D 11 presenter drawing 16-bit float scRGB (Settings HdrOutput).
        void SetHdrOutput(bool on);
        bool IsDX12() const { return m_p12 != nullptr; }
        Present12* DX12() const { return m_p12; }
        // Loops since the last call, and those that began with the frame before
        // still in the swap chain's queue (see WaitForFrame).
        void TakeQueueStats(int& checks, int& busy) { checks = m_queueChecks; busy = m_queueBusy; m_queueChecks = m_queueBusy = 0; }
        double LastVBlankWaitMs() const { return m_lastVBlankWaitMs; }
        // Time left until the SR display's next refresh (ms), from the last one
        // WaitForFrame saw; -1 if not known (no refresh sync).
        double MsToNextVBlank() const;

        // Cap the render loop's Present rate to the given refresh in Hz. Pass 0 to
        // disable. Avoids burning GPU rendering faster than the SR panel can show;
        // the waitable object alone doesn't always stop us going past refresh on a
        // no-vsync flip swap chain. Hybrid sleep+yield+spin pacing for sub-ms
        // precision without burning a full CPU core.
        void SetTargetRefreshHz(double hz);
        // The SR display moved or changed (Windows display settings): wait for
        // its refreshes from now on. Keeps the output when it's still the one.
        void SetVBlankMonitor(HMONITOR target);

        // Choose the swap-chain model for the window's current state: a low-latency
        // FLIP swap chain when the window is NOT layered (fullscreen/windowed — e.g.
        // games), or a bit-blt swap chain when it is (click-through overlay/loupe/
        // passthrough — flip doesn't render on layered windows). Recreates only when
        // the model actually needs to change.
        void SetLayered(bool layered);

        ID3D11Device*        Device() const        { return m_device; }
        ID3D11DeviceContext* Context() const       { return m_context; }
        // The sRGB format the weaver should treat its input/output as.
        DXGI_FORMAT          BackBufferFormat() const { return m_rtvFormat; }
        UINT                 Width()  const        { return m_width;  }
        UINT                 Height() const        { return m_height; }
        bool                 IsValid() const        { return m_device != nullptr; }

    private:
        bool CreateSwapChain(bool flip);   // (re)create the swap chain in the chosen model
        bool CreateBackBufferView();
        bool CreateDCompSwapChain();
        bool InitMask();
        static constexpr UINT   kMaskCBBytes = 16 + 16 * (64 + 16 * 3 + 64 + 16 + 16);   // (the mask shader's cbuffer M)
        void FillMaskConstants(uint32_t* head, bool gpuSlots) const;
        void FillMaskTiles(uint32_t* t) const;
        bool UpdateGpuShare();
        ID3D11Texture2D*        m_gpuShareTex = nullptr;   // the GPU tracker's results, shareable (UpdateGpuShare)
        ID3D11RenderTargetView* m_gpuShareRTV = nullptr;
        ID3D11PixelShader*      m_gpuSharePS = nullptr;
        bool                    m_gpuShareFailed = false;
        Present12*              m_p12       = nullptr; // (SetDX12)
        bool                    m_hdrOut    = false;   // (SetHdrOutput)
        void NotePresent();
        struct PresNote { UINT count = 0; LONGLONG qpc = 0; };
        static constexpr UINT   kPresRing = 32;
        PresNote                m_presRing[kPresRing];
        LONGLONG                m_weaveQpc = 0;
        UINT                    m_statCount = 0;
        double                  m_latSum = 0.0, m_latMax = 0.0, m_latMin = 1e9;
        int                     m_latN = 0;
        bool                    m_latencyStats = false;
        const char*             m_maskHLSL  = nullptr; // the mask shader source (InitMask)
        bool                    m_maskDone  = false;   // applied this frame already (ApplyMaskNow)
        int                     m_alphaProbe = 0;      // does the weave leave alpha 1? 0 unknown, 1 read back pending, 2 yes, 3 no (ApplyMask)
        ID3D11Texture2D*        m_alphaProbeTex = nullptr;
        ID3D11RasterizerState*  m_scissorRS = nullptr; // (the mask drawn hole by hole)

        IDCompositionDevice*    m_dcomp     = nullptr;
        IDCompositionTarget*    m_dcTarget  = nullptr;
        IDCompositionVisual*    m_dcVisual  = nullptr;
        ID3D11VertexShader*     m_maskVS    = nullptr;
        ID3D11PixelShader*      m_maskPS    = nullptr;
        ID3D11BlendState*       m_maskBlend = nullptr;
        ID3D11Buffer*           m_maskCB    = nullptr;
        ID3D11Buffer*           m_maskTiles = nullptr;   // which rects touch each screen tile (the mask shader's t1)
        ID3D11ShaderResourceView* m_maskTilesSRV = nullptr;
        static constexpr int    kMaskTilesX = 32, kMaskTilesY = 18;
        static constexpr UINT   kMaskTilesBytes = 16 * (1 + 2 * kMaskTilesX * kMaskTilesY);
        bool                    m_maskAll   = true;
        std::vector<RECT>       m_maskRects;
        std::vector<MaskTracked> m_maskTracked;
        std::vector<MaskCut>    m_maskExcl;
        ID3D11ShaderResourceView* m_maskGpu = nullptr;   // not owned

        HWND                    m_hwnd      = nullptr;
        ID3D11Device*           m_device    = nullptr;
        ID3D11DeviceContext*    m_context   = nullptr;
        IDXGIFactory2*          m_factory   = nullptr;   // kept for swap-chain recreation
        IDXGISwapChain1*        m_swapChain = nullptr;
        ID3D11RenderTargetView* m_rtv       = nullptr;
        UINT                    m_width     = 0;
        UINT                    m_height    = 0;
        // The weaver writes sRGB. Flip swap chains can't be created with an _SRGB
        // buffer format, so the buffer is plain UNORM and we make an _SRGB render
        // target view over it; the bit-blt path uses an _SRGB buffer directly.
        DXGI_FORMAT             m_swapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT             m_rtvFormat  = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        UINT                    m_swapFlags  = 0;       // flags the swap chain was created with
        HANDLE                  m_waitable   = nullptr; // frame-latency waitable (flip only)
        bool                    m_compositorOtherClock = false;   // (WaitForFrame: Windows composes at another rate than the SR display)
        ULONGLONG               m_clockCheckMs = 0;
        bool                    m_allowTearing = false; // GPU/OS supports tearing (VRR)
        bool                    m_planeMode = false;    // (SetPlaneMode)
        bool                    m_autoPlane = false;    // (SetAutoPlane)
        int                     m_clearRun = 0;         // frames in a row with nothing to show through
        void UpdateAutoPlane();
        bool                    m_flip       = true;    // current model: true=flip, false=bit-blt
        bool                    m_layered    = false;   // current window layered state

        // Render-rate cap state. m_targetIntervalNs is the minimum number of
        // nanoseconds between consecutive Present()s; 0 disables the cap.
        // m_lastPresentEnd is stamped at the end of Present() so WaitForFrame()
        // can compute the remaining wait time.
        int64_t                              m_targetIntervalNs = 0;
        std::chrono::steady_clock::time_point m_lastPresentEnd{};
        std::chrono::steady_clock::time_point m_lastFrameStart{};   // render cap counts from here
        double m_lastCompositorWaitMs = 0.0, m_lastVBlankWaitMs = 0.0;
        int    m_queueChecks = 0, m_queueBusy = 0;
        std::chrono::steady_clock::time_point m_lastVBlank{};   // when the last WaitForVBlank returned (the display's phase)
        // The SR display's output: frames start on its vertical blank (see
        // WaitForFrame), locking the loop to its refresh rate.
        IDXGIOutput*                         m_vblankOutput = nullptr;
        std::mutex                           m_vblankMutex;   // (the pointer above: WaitForFrame vs SetVBlankMonitor on another thread)
    };
}
