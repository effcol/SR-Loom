#include "Renderer.h"
#include <dxgi1_2.h>

using namespace srw;

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Initialize(HWND hwnd)
{
    m_hwnd = hwnd;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    m_width  = (UINT)(rc.right - rc.left);
    m_height = (UINT)(rc.bottom - rc.top);
    if (m_width == 0)  m_width = 1280;
    if (m_height == 0) m_height = 720;

    // BGRA support is required for interop with Windows.Graphics.Capture / D2D.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);
    if (FAILED(hr))
    {
        ShowError("Failed to create Direct3D 11 device.");
        return false;
    }

    // Keep at most one frame queued on the GPU to reduce display latency.
    {
        IDXGIDevice1* dxgiDevice1 = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice1)))
        {
            dxgiDevice1->SetMaximumFrameLatency(1);
            dxgiDevice1->Release();
        }
    }

    // Obtain the DXGI factory associated with our device.
    IDXGIDevice*  dxgiDevice  = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    IDXGIFactory2* factory    = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (SUCCEEDED(hr)) hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    if (FAILED(hr))
    {
        SAFE_RELEASE(dxgiAdapter);
        SAFE_RELEASE(dxgiDevice);
        ShowError("Failed to obtain DXGI factory.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = m_width;
    sd.Height           = m_height;
    sd.Format           = m_bufferFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = factory->CreateSwapChainForHwnd(m_device, hwnd, &sd, nullptr, nullptr, &m_swapChain);

    // Block Alt+Enter; we manage fullscreen ourselves as a borderless window.
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    SAFE_RELEASE(factory);
    SAFE_RELEASE(dxgiAdapter);
    SAFE_RELEASE(dxgiDevice);

    if (FAILED(hr))
    {
        ShowError("Failed to create swap chain.");
        return false;
    }

    return CreateBackBufferView();
}

bool Renderer::CreateBackBufferView()
{
    SAFE_RELEASE(m_rtv);

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr))
    {
        ShowError("Failed to get swap chain back buffer.");
        return false;
    }

    // sRGB view over the UNORM buffer (flip-model requirement).
    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format        = m_rtvFormat;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = m_device->CreateRenderTargetView(backBuffer, &rtvd, &m_rtv);
    SAFE_RELEASE(backBuffer);
    if (FAILED(hr))
    {
        ShowError("Failed to create render target view.");
        return false;
    }
    return true;
}

bool Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
        return false;
    if (width == m_width && height == m_height)
        return true;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    SAFE_RELEASE(m_rtv);

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
    {
        ShowError("Failed to resize swap chain buffers.");
        return false;
    }

    m_width  = width;
    m_height = height;
    return CreateBackBufferView();
}

void Renderer::BindAndClearBackBuffer()
{
    if (!m_rtv) return;

    const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv, black);
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_width;
    vp.Height   = (FLOAT)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void Renderer::Present(bool vsync)
{
    if (m_swapChain)
        m_swapChain->Present(vsync ? 1 : 0, 0);
}

void Renderer::Shutdown()
{
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_swapChain);
    SAFE_RELEASE(m_context);
    SAFE_RELEASE(m_device);
}
