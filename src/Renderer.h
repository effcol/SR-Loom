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

        // Present the woven frame. vsync=true waits for one v-blank.
        void Present(bool vsync);

        ID3D11Device*        Device() const        { return m_device; }
        ID3D11DeviceContext* Context() const       { return m_context; }
        // The sRGB format the weaver should treat its input/output as.
        DXGI_FORMAT          BackBufferFormat() const { return m_format; }
        bool                 IsValid() const        { return m_device != nullptr; }

    private:
        bool CreateBackBufferView();

        HWND                    m_hwnd     = nullptr;
        ID3D11Device*           m_device   = nullptr;
        ID3D11DeviceContext*    m_context  = nullptr;
        IDXGISwapChain1*        m_swapChain = nullptr;
        ID3D11RenderTargetView* m_rtv      = nullptr;
        UINT                    m_width    = 0;
        UINT                    m_height   = 0;
        // Bit-blt swap chain (DXGI_SWAP_EFFECT_DISCARD) so the window can be made
        // layered for click-through; bit-blt allows an sRGB buffer directly.
        DXGI_FORMAT             m_format   = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
