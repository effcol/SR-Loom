// Renderer.h — owns the D3D11 device, swap chain and back-buffer for the
// output window. The SR weaver renders into this swap chain's back buffer.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>   // IDXGISwapChain1, IDXGIFactory2

namespace srw
{
    class Renderer
    {
    public:
        Renderer() = default;
        ~Renderer();

        // Create the device + swap chain for the given window. Returns false on failure.
        bool Initialize(HWND hwnd);
        void Shutdown();

        // Resize the swap chain to the new client size (called on WM_SIZE).
        bool Resize(UINT width, UINT height);

        // Bind the back buffer as render target, clear it, and set a full-window viewport.
        // Call this right before SRWeaver::Weave().
        void BindAndClearBackBuffer();

        // Present the woven frame. vsync=true waits for one v-blank; vsync=false
        // presents immediately (with tearing allowed on a flip swapchain — ideal for
        // VRR displays and lowest latency).
        void Present(bool vsync);

        // Block until the flip swap chain can accept a new frame (frame-latency
        // waitable object). Paces the render loop to the display's refresh with ~1
        // frame of latency WITHOUT a vsync stall, so no-vsync presents don't spin
        // the GPU at thousands of fps. No-op on the bit-blt path (vsync paces it).
        void WaitForFrame();

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
        bool                 IsValid() const        { return m_device != nullptr; }

    private:
        bool CreateSwapChain(bool flip);   // (re)create the swap chain in the chosen model
        bool CreateBackBufferView();

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
    };
}
