#include "Renderer.h"
#include <dxgi1_2.h>
#include <dxgi1_3.h>   // IDXGISwapChain2, FRAME_LATENCY_WAITABLE_OBJECT
#include <dxgi1_5.h>   // IDXGIFactory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING
#include <dwmapi.h>    // DwmFlush (pace layered/bit-blt presents to the compositor)
#include <thread>      // std::this_thread::sleep_for / yield for the render-rate cap
#include <dcomp.h>     // DirectComposition presenter
#include <d3dcompiler.h>
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace srw;

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Initialize(HWND hwnd, bool useDComp)
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

    // Run on the GPU that drives the window's (the SR) display. The default
    // adapter is the one driving the MAIN display; on a machine with two GPUs
    // that can be the other one, and then every capture and present crosses
    // between GPUs (extra copies, extra latency, and it can hold the frame
    // rate down). Every adapter + its displays is logged for diagnosis.
    IDXGIAdapter1* srAdapter = nullptr;
    {
        const HMONITOR target = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        IDXGIFactory1* f1 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f1)))
        {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                DXGI_ADAPTER_DESC1 ad{};
                a->GetDesc1(&ad);
                bool drivesTarget = false;
                IDXGIOutput* o = nullptr;
                for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j)
                {
                    DXGI_OUTPUT_DESC od{};
                    o->GetDesc(&od);
                    const bool isTarget = (od.Monitor == target);
                    drivesTarget |= isTarget;
                    Log("Renderer: adapter %u '%ls' output %u '%ls'%s", i, ad.Description, j, od.DeviceName,
                        isTarget ? "  <- SR display" : "");
                    o->Release();
                }
                if (drivesTarget && !srAdapter) { srAdapter = a; srAdapter->AddRef(); }
                a->Release();
            }
            f1->Release();
        }
    }
    HRESULT hr = D3D11CreateDevice(
        srAdapter, srAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &m_device, nullptr, &m_context);
    if (FAILED(hr) && srAdapter)
    {
        Log("Renderer: device on the SR display's adapter failed (0x%08X) -- using the default", (unsigned)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
    }
    if (srAdapter) srAdapter->Release();
    if (SUCCEEDED(hr))
    {
        IDXGIDevice* dd = nullptr; IDXGIAdapter* used = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&used)))
        {
            DXGI_ADAPTER_DESC ud{};
            used->GetDesc(&ud);
            Log("Renderer: running on '%ls'", ud.Description);
        }
        if (used) used->Release();
        if (dd) dd->Release();
    }
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

    // DirectComposition presenter: a composition target on the window, one
    // visual whose content is a composition swap chain. Works for every mode
    // (layered or not), so the swap chain is never swapped for another model.
    if (useDComp)
    {
        IDXGIDevice* dx = nullptr;
        hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dx, __uuidof(IDCompositionDevice), (void**)&m_dcomp);
        SAFE_RELEASE(dx);
        if (SUCCEEDED(hr)) hr = m_dcomp->CreateTargetForHwnd(hwnd, TRUE, &m_dcTarget);
        if (SUCCEEDED(hr)) hr = m_dcomp->CreateVisual(&m_dcVisual);
        if (SUCCEEDED(hr)) hr = m_dcTarget->SetRoot(m_dcVisual);
        if (SUCCEEDED(hr) && CreateDCompSwapChain() && InitMask())
        {
            Log("Renderer: DirectComposition presenter (%ux%u)", m_width, m_height);
            return true;
        }
        Log("Renderer: DirectComposition setup failed (hr=0x%08X)", (unsigned)hr);
        SAFE_RELEASE(m_rtv); SAFE_RELEASE(m_swapChain);
        SAFE_RELEASE(m_dcVisual); SAFE_RELEASE(m_dcTarget); SAFE_RELEASE(m_dcomp);
        return false;   // the caller recreates the window for the classic presenter
    }

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
bool Renderer::CreateDCompSwapChain()
{
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_swapChain);
    m_waitable = nullptr;
    m_flip       = true;
    m_swapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;   // + an _SRGB view, as for flip

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = m_width;
    sd.Height           = m_height;
    sd.Format           = m_swapFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling          = DXGI_SCALING_STRETCH;
    sd.AlphaMode        = DXGI_ALPHA_MODE_PREMULTIPLIED;   // see-through where alpha = 0
    sd.Flags            = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = m_factory->CreateSwapChainForComposition(m_device, &sd, nullptr, &m_swapChain);
    if (FAILED(hr))
    {
        sd.Flags = 0;
        hr = m_factory->CreateSwapChainForComposition(m_device, &sd, nullptr, &m_swapChain);
    }
    if (FAILED(hr))
    {
        Log("Renderer: CreateSwapChainForComposition FAILED hr=0x%08X", (unsigned)hr);
        return false;
    }
    m_swapFlags = sd.Flags;
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
    if (FAILED(m_dcVisual->SetContent(m_swapChain)) || FAILED(m_dcomp->Commit()))
        return false;
    return CreateBackBufferView();
}

// The see-through mask: a full-window pass after the weave that keeps the
// picture (alpha 1) inside the visible rects and makes everything else fully
// transparent (premultiplied: colour and alpha 0) -- in the same frame.
bool Renderer::InitMask()
{
    static const char* kMaskHLSL = R"(
cbuffer M : register(b0)
{
    uint count; uint all; uint trCount; uint exCount;
    float4 rects[64];     // visible (static)
    float4 trRect[16];    // visible, moved by a GPU-tracked offset
    float4 trClip[16];    //   ... within this (its viewport)
    float4 trInfo[16];    //   x = GpuTracker slot (-1 none), y = px per tracker row
    float4 excl[64];      // never visible (windows in front, taskbar, 2D windows)
    float4 exRad[16];     //   ... their corner radii (4 per float4)
};
StructuredBuffer<int4> gpuRes : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
bool Inside(float2 p, float4 r) { return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w; }
// Inside a rectangle with rounded corners of radius rad.
bool InsideRounded(float2 p, float4 r, float rad)
{
    if (!Inside(p, r)) return false;
    if (rad <= 0) return true;
    const float2 c = clamp(p, r.xy + rad, r.zw - rad);   // nearest point of the inner rect
    return length(p - c) <= rad;
}
float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float m = all ? 1.0 : 0.0;
    uint i;
    [loop] for (i = 0; i < count && m == 0; ++i)
        if (Inside(pos.xy, rects[i])) m = 1.0;
    [loop] for (i = 0; i < trCount && m == 0; ++i)
    {
        float dx = 0, dy = 0;
        const int slot = (int)trInfo[i].x;
        if (slot >= 0) { const int4 g = gpuRes[slot]; if (g.y) dy = g.x * trInfo[i].y; if (g.w) dx = g.z * trInfo[i].y; }
        if (Inside(pos.xy, trRect[i] + float4(dx, dy, dx, dy)) && Inside(pos.xy, trClip[i])) m = 1.0;
    }
    [loop] for (i = 0; i < exCount && m != 0; ++i)
        if (InsideRounded(pos.xy, excl[i], exRad[i / 4][i % 4])) m = 0.0;
    return float4(m, m, m, m);
}
)";
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    const size_t len = strlen(kMaskHLSL);
    if (FAILED(D3DCompile(kMaskHLSL, len, "Mask", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kMaskHLSL, len, "Mask", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
    {
        Log("Renderer: mask shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SAFE_RELEASE(err); SAFE_RELEASE(vsb); SAFE_RELEASE(psb);
        return false;
    }
    m_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_maskVS);
    m_device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_maskPS);
    SAFE_RELEASE(vsb); SAFE_RELEASE(psb);

    // result = picture * m (colour), alpha = m.
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_SRC_COLOR;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, &m_maskBlend);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth      = 16 + 16 * (64 + 16 * 3 + 64 + 16);
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cb, nullptr, &m_maskCB);
    return m_maskVS && m_maskPS && m_maskBlend && m_maskCB;
}

void Renderer::SetVisibleAll()
{
    m_maskAll = true;
    m_maskRects.clear(); m_maskTracked.clear(); m_maskExcl.clear();
}

void Renderer::SetVisibleRects(const std::vector<RECT>& rects)
{
    m_maskAll = false;
    m_maskRects.assign(rects.begin(), rects.begin() + (std::min)(rects.size(), (size_t)64));
    m_maskTracked.clear(); m_maskExcl.clear();
}

void Renderer::SetVisibleAllExcept(const std::vector<MaskCut>& holes)
{
    m_maskAll = true;
    m_maskRects.clear(); m_maskTracked.clear();
    m_maskExcl.assign(holes.begin(), holes.begin() + (std::min)(holes.size(), (size_t)64));
}

void Renderer::SetVisibleTracked(const std::vector<MaskTracked>& tracked, const std::vector<MaskCut>& excl,
                                 ID3D11ShaderResourceView* gpuResults)
{
    m_maskAll = false;
    m_maskRects.clear();
    m_maskTracked.assign(tracked.begin(), tracked.begin() + (std::min)(tracked.size(), (size_t)16));
    m_maskExcl.assign(excl.begin(), excl.begin() + (std::min)(excl.size(), (size_t)64));
    m_maskGpu = gpuResults;
}

void Renderer::ApplyMask()
{
    if (!m_rtv || !m_maskPS || !m_dcomp) return;
    m_maskDone = true;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(m_context->Map(m_maskCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    uint32_t* head = static_cast<uint32_t*>(m.pData);
    head[0] = (uint32_t)m_maskRects.size();
    head[1] = m_maskAll ? 1u : 0u;
    head[2] = (uint32_t)m_maskTracked.size();
    head[3] = (uint32_t)m_maskExcl.size();
    float* f = reinterpret_cast<float*>(head + 4);
    auto put = [&](float* at, const RECT& r) { at[0] = (float)r.left; at[1] = (float)r.top; at[2] = (float)r.right; at[3] = (float)r.bottom; };
    float* rects  = f;               // 64
    float* trRect = rects  + 64 * 4; // 16
    float* trClip = trRect + 16 * 4; // 16
    float* trInfo = trClip + 16 * 4; // 16
    float* excl   = trInfo + 16 * 4; // 64
    float* exRad  = excl + 64 * 4;   // 64 (16 float4s)
    for (int i = 0; i < 64; ++i) exRad[i] = 0.0f;
    for (size_t i = 0; i < m_maskRects.size(); ++i) put(rects + i * 4, m_maskRects[i]);
    for (size_t i = 0; i < m_maskTracked.size(); ++i)
    {
        put(trRect + i * 4, m_maskTracked[i].rect);
        put(trClip + i * 4, m_maskTracked[i].clip);
        trInfo[i * 4 + 0] = (float)m_maskTracked[i].slot;
        trInfo[i * 4 + 1] = m_maskTracked[i].scale;
        trInfo[i * 4 + 2] = trInfo[i * 4 + 3] = 0.0f;
    }
    for (size_t i = 0; i < m_maskExcl.size(); ++i)
    {
        put(excl + i * 4, m_maskExcl[i].rect);
        exRad[i] = (float)m_maskExcl[i].radius;
    }
    m_context->Unmap(m_maskCB, 0);

    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    D3D11_VIEWPORT vp{ 0, 0, (FLOAT)m_width, (FLOAT)m_height, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(nullptr);
    m_context->OMSetBlendState(m_maskBlend, nullptr, 0xFFFFFFFF);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_maskVS, nullptr, 0);
    m_context->PSSetShader(m_maskPS, nullptr, 0);
    m_context->PSSetConstantBuffers(0, 1, &m_maskCB);
    ID3D11ShaderResourceView* gpu = m_maskTracked.empty() ? nullptr : m_maskGpu;
    m_context->PSSetShaderResources(0, 1, &gpu);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

bool Renderer::CreateSwapChain(bool flip)
{
    if (m_dcomp) return CreateDCompSwapChain();   // one model for every mode
    SAFE_RELEASE(m_rtv);
    const bool replacing = (m_swapChain != nullptr);
    SAFE_RELEASE(m_swapChain);
    m_waitable = nullptr;   // owned by the swap chain; invalidated on release
    // D3D11 destroys a released swap chain lazily, when the immediate context
    // next flushes. Until then the HWND still "has" it, and creating another
    // swap chain on the same window can fail (E_ACCESSDENIED) -- see MS docs,
    // "Deferred destruction issues with flip presentation swap chains". The
    // caller already unbound the RTV, so Flush() is enough to finish it off.
    if (replacing && m_context) m_context->Flush();

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
    {
        Log("Renderer::CreateSwapChain(%s) FAILED hr=0x%08X",
            flip ? "flip" : "bitblt", (unsigned)hr);
        return false;
    }

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
    if (m_dcomp) { m_layered = layered; return; }   // composition: any window style
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
    // (2% under one refresh, so frames never land just after the display's
    // own tick and slip a whole refresh.)
    m_targetIntervalNs = (hz > 0.0) ? (int64_t)(0.98e9 / hz) : 0;
}

void Renderer::WaitForFrame()
{
    if (m_waitable)
        WaitForSingleObjectEx(m_waitable, 1000, TRUE);

    // Render-rate cap: hold here until one display refresh has passed since
    // the START of the previous frame (not the end of its present: that made
    // each frame refresh + work long, e.g. ~7.5 ms instead of 6.25 at 160 Hz).
    // Avoids rendering past the SR panel's refresh, which would just be
    // wasted GPU + heat. Hybrid timer+yield+spin pacing gives sub-ms precision
    // without burning a full core continuously.
    if (m_targetIntervalNs > 0)
    {
        using namespace std::chrono;
        const auto target = m_lastFrameStart + nanoseconds(m_targetIntervalNs);
        for (;;)
        {
            const auto now = steady_clock::now();
            if (now >= target) break;
            const auto remain = duration_cast<nanoseconds>(target - now).count();
            if (remain > 2'000'000)        // > 2ms: sleep the bulk, leave ~1ms slack
            {
                // A HIGH-RESOLUTION waitable timer: a plain Sleep / sleep_for
                // rounds up to Windows' timer tick (15.6 ms by default), which
                // silently capped this whole loop at ~64 frames/s.
                static HANDLE s_timer = CreateWaitableTimerExW(nullptr, nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                if (s_timer)
                {
                    LARGE_INTEGER due{};
                    due.QuadPart = -(LONGLONG)((remain - 1'000'000) / 100);   // relative, 100 ns units
                    if (SetWaitableTimerEx(s_timer, &due, 0, nullptr, nullptr, nullptr, 0))
                        WaitForSingleObject(s_timer, INFINITE);
                }
                else
                    std::this_thread::sleep_for(nanoseconds(remain - 1'000'000));
            }
            else if (remain > 200'000)     // 200µs – 2ms: yield (cheap)
                std::this_thread::yield();
            // else < 200µs: tight spin for sub-ms accuracy
        }
    }
    m_lastFrameStart = std::chrono::steady_clock::now();
}

void Renderer::Present(bool vsync, bool flushDwm)
{
    if (!m_swapChain) return;
    if (m_dcomp)
    {
        // DirectComposition: apply the see-through mask to this very frame,
        // then queue it for the next composition. No DwmFlush -- the
        // waitable object (WaitForFrame) paces the loop.
        if (!m_maskDone) ApplyMask();
        m_maskDone = false;
        m_swapChain->Present(0, 0);
        m_lastPresentEnd = std::chrono::steady_clock::now();
        return;
    }
    if (!m_flip)
    {
        // Bit-blt (layered overlay / looking glass): present immediately, then block
        // on the DWM compositor that actually displays a layered window. This paces
        // the loop AND aligns it with composition — lower latency and much less
        // jitter than waiting on the swap chain's own vsync (which beats against the
        // compositor's). DwmFlush returns at the next composition (~refresh).
        m_swapChain->Present(0, 0);
        if (flushDwm) DwmFlush();
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
    SAFE_RELEASE(m_maskCB); SAFE_RELEASE(m_maskBlend); SAFE_RELEASE(m_maskPS); SAFE_RELEASE(m_maskVS);
    SAFE_RELEASE(m_dcVisual); SAFE_RELEASE(m_dcTarget); SAFE_RELEASE(m_dcomp);
    SAFE_RELEASE(m_factory);
    SAFE_RELEASE(m_context);
    SAFE_RELEASE(m_device);
}

double Renderer::CompositionRateHz() const
{
    if (!m_dcomp) return 0.0;
    DCOMPOSITION_FRAME_STATISTICS st{};
    if (FAILED(m_dcomp->GetFrameStatistics(&st)) || st.currentCompositionRate.Denominator == 0) return 0.0;
    return (double)st.currentCompositionRate.Numerator / st.currentCompositionRate.Denominator;
}
