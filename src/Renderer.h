// Renderer.h — owns the D3D11 device, swap chain and back-buffer for the
// output window. The SR weaver renders into this swap chain's back buffer.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>   // IDXGISwapChain1, IDXGIFactory2
#include <chrono>
#include <vector>

struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

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
        struct MaskCut     { RECT rect{}; int radius = 0; };   // a hole (rounded corners)
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

        // Cap the render loop's Present rate to the given refresh in Hz. Pass 0 to
        // disable. Avoids burning GPU rendering faster than the SR panel can show;
        // the waitable object alone doesn't always stop us going past refresh on a
        // no-vsync flip swap chain. Hybrid sleep+yield+spin pacing for sub-ms
        // precision without burning a full CPU core.
        void SetTargetRefreshHz(double hz);

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
        bool                    m_maskDone  = false;   // applied this frame already (ApplyMaskNow)

        IDCompositionDevice*    m_dcomp     = nullptr;
        IDCompositionTarget*    m_dcTarget  = nullptr;
        IDCompositionVisual*    m_dcVisual  = nullptr;
        ID3D11VertexShader*     m_maskVS    = nullptr;
        ID3D11PixelShader*      m_maskPS    = nullptr;
        ID3D11BlendState*       m_maskBlend = nullptr;
        ID3D11Buffer*           m_maskCB    = nullptr;
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
        bool                    m_allowTearing = false; // GPU/OS supports tearing (VRR)
        bool                    m_flip       = true;    // current model: true=flip, false=bit-blt
        bool                    m_layered    = false;   // current window layered state

        // Render-rate cap state. m_targetIntervalNs is the minimum number of
        // nanoseconds between consecutive Present()s; 0 disables the cap.
        // m_lastPresentEnd is stamped at the end of Present() so WaitForFrame()
        // can compute the remaining wait time.
        int64_t                              m_targetIntervalNs = 0;
        std::chrono::steady_clock::time_point m_lastPresentEnd{};
        std::chrono::steady_clock::time_point m_lastFrameStart{};   // render cap counts from here
    };
}
