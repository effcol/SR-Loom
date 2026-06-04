#include "Renderer.h"
#include <dxgi1_2.h>
#include <dxgi1_3.h>   // IDXGISwapChain2, FRAME_LATENCY_WAITABLE_OBJECT
#include <dxgi1_5.h>   // IDXGIFactory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING
#include <dwmapi.h>    // DwmFlush (pace layered/bit-blt presents to the compositor)
#include <thread>      // std::this_thread::sleep_for / yield for the render-rate cap
#pragma comment(lib, "dwmapi.lib")

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

    // Obtain the DXGI factory associated with our device (kept for swap-chain
    // recreation when the window's layered state changes).
    IDXGIDevice*  dxgiDevice  = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (SUCCEEDED(hr)) hr = dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&m_factory);
    SAFE_RELEASE(dxgiAdapter);
    SAFE_RELEASE(dxgiDevice);
    if (FAILED(hr))
    {
        ShowError("Failed to obtain DXGI factory.");
        return false;
    }

    // Does the GPU/OS support tearing (needed for true no-vsync / VRR presents)?
    {
        IDXGIFactory5* f5 = nullptr;
        if (SUCCEEDED(m_factory->QueryInterface(__uuidof(IDXGIFactory5), (void**)&f5)))
        {
            BOOL allow = FALSE;
            if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                m_allowTearing = (allow == TRUE);
            f5->Release();
        }
    }

    // Block Alt+Enter; we manage fullscreen ourselves as a borderless window.
    m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // The window starts non-layered, so begin with the low-latency flip model.
    if (!CreateSwapChain(true))
    {
        ShowError("Failed to create swap chain.");
        return false;
    }
    return true;
}

// (Re)create the swap chain in the requested model. flip = low-latency flip model
// (non-layered windows), else bit-blt (works on layered click-through windows).
bool Renderer::CreateSwapChain(bool flip)
{
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_swapChain);
    m_waitable = nullptr;   // owned by the swap chain; invalidated on release

    m_flip       = flip;
    m_swapFormat = flip ? DXGI_FORMAT_R8G8B8A8_UNORM        // flip can't use an _SRGB buffer
                        : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // bit-blt uses sRGB directly

    // Flip model: low latency + a waitable object for pacing, plus tearing for VRR
    // no-vsync where supported. Bit-blt (layered windows): no special flags.
    UINT flipFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (m_allowTearing) flipFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = m_width;
    sd.Height           = m_height;
    sd.Format           = m_swapFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;

    // Try the preferred flags, then progressively simpler ones, so an unusual
    // driver can't leave us with no swap chain at all.
    const UINT attempts[] = { flip ? flipFlags : 0u,
                              flip ? (UINT)DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u,
                              0u };
    HRESULT hr = E_FAIL;
    for (UINT f : attempts)
    {
        sd.Flags = f;
        hr = m_factory->CreateSwapChainForHwnd(m_device, m_hwnd, &sd, nullptr, nullptr, &m_swapChain);
        if (SUCCEEDED(hr)) { m_swapFlags = f; break; }
    }
    if (FAILED(hr))
        return false;

    // Grab the waitable object (if we got one) and keep at most one frame queued.
    if (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    {
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
        {
            sc2->SetMaximumFrameLatency(1);
            m_waitable = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
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

    // Explicit _SRGB view: required for the flip path (UNORM buffer + sRGB view);
    // identical to the default for the bit-blt sRGB buffer.
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format        = m_rtvFormat;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = m_device->CreateRenderTargetView(backBuffer, &rd, &m_rtv);
    SAFE_RELEASE(backBuffer);
    if (FAILED(hr))
    {
        ShowError("Failed to create render target view.");
        return false;
    }
    return true;
}

void Renderer::SetLayered(bool layered)
{
    if (!m_device) return;
    const bool wantFlip = !layered;
    m_layered = layered;
    if (wantFlip == m_flip && m_swapChain)
        return;   // model already correct
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    CreateSwapChain(wantFlip);
}

bool Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0)
        return false;
    if (width == m_width && height == m_height)
        return true;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    SAFE_RELEASE(m_rtv);

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_swapFlags);
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

void Renderer::SetTargetRefreshHz(double hz)
{
    m_targetIntervalNs = (hz > 0.0) ? (int64_t)(1.0e9 / hz) : 0;
}

void Renderer::WaitForFrame()
{
    if (m_waitable)
        WaitForSingleObjectEx(m_waitable, 1000, TRUE);

    // Render-rate cap: hold here until the configured minimum interval has
    // elapsed since the last Present(). Avoids rendering past the SR panel's
    // refresh, which would just be wasted GPU + heat (panel can't display
    // frames it doesn't have time to scan). Hybrid sleep+yield+spin pacing
    // gives sub-ms precision without burning a full core continuously.
    if (m_targetIntervalNs > 0)
    {
        using namespace std::chrono;
        const auto target = m_lastPresentEnd + nanoseconds(m_targetIntervalNs);
        for (;;)
        {
            const auto now = steady_clock::now();
            if (now >= target) break;
            const auto remain = duration_cast<nanoseconds>(target - now).count();
            if (remain > 2'000'000)        // > 2ms: sleep the bulk, leave ~1ms slack
                std::this_thread::sleep_for(nanoseconds(remain - 1'000'000));
            else if (remain > 200'000)     // 200µs – 2ms: yield (cheap)
                std::this_thread::yield();
            // else < 200µs: tight spin for sub-ms accuracy
        }
    }
}

void Renderer::Present(bool vsync)
{
    if (!m_swapChain) return;
    if (!m_flip)
    {
        // Bit-blt (layered overlay / looking glass): present immediately, then block
        // on the DWM compositor that actually displays a layered window. This paces
        // the loop AND aligns it with composition — lower latency and much less
        // jitter than waiting on the swap chain's own vsync (which beats against the
        // compositor's). DwmFlush returns at the next composition (~refresh).
        m_swapChain->Present(0, 0);
        DwmFlush();
        m_lastPresentEnd = std::chrono::steady_clock::now();
        return;
    }
    // Flip: no-vsync presents immediately with tearing allowed (VRR drives the
    // refresh → minimum latency); the waitable object paces the loop.
    UINT flags = (!vsync && (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
               ? DXGI_PRESENT_ALLOW_TEARING : 0;
    m_swapChain->Present(vsync ? 1 : 0, flags);
    m_lastPresentEnd = std::chrono::steady_clock::now();
}

void Renderer::Shutdown()
{
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_swapChain);
    SAFE_RELEASE(m_factory);
    SAFE_RELEASE(m_context);
    SAFE_RELEASE(m_device);
}
