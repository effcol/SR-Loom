#include "Renderer.h"
#include "Present12.h"
#include <dxgi1_2.h>
#include <dxgi1_3.h>   // IDXGISwapChain2, FRAME_LATENCY_WAITABLE_OBJECT
#include <dxgi1_5.h>   // IDXGIFactory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING
#include <dxgi1_6.h>   // IDXGIOutput6 (HDR state)
#include <climits>
#include <dwmapi.h>    // DwmFlush (pace layered/bit-blt presents to the compositor)
#include <thread>      // std::this_thread::sleep_for / yield for the render-rate cap
#include <dcomp.h>     // DirectComposition presenter
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
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
                if (drivesTarget && !m_vblankOutput)
                    for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j)
                    {
                        DXGI_OUTPUT_DESC od{};
                        o->GetDesc(&od);
                        if (od.Monitor == target && !m_vblankOutput) { m_vblankOutput = o; continue; }
                        o->Release();
                    }
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
            // Our GPU work ahead of other apps' in the scheduler: while a page
            // scrolls the browser renders 4K at full rate on the same GPU, and a
            // weave queued behind it misses the SR display's refresh (judder).
            // (Ours is a few ms a frame; theirs isn't slowed noticeably.)
            const HRESULT hp = dxgiDevice1->SetGPUThreadPriority(7);
            Log("Renderer: GPU thread priority +7 %s (0x%08lX)", SUCCEEDED(hp) ? "set" : "refused", (unsigned long)hp);
            // ... and the whole process a class up in the GPU scheduler (what
            // capture / VR compositors ask for): that thread priority only
            // orders work within the same class, and under a busy browser our
            // passes measured about three times their time on an idle GPU.
            // High needs no special rights on most systems; else above normal.
            if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll"))
            {
                typedef LONG (WINAPI* SetClassFn)(HANDLE, int);   // (D3DKMTSetProcessSchedulingPriorityClass)
                if (auto fn = (SetClassFn)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
                {
                    LONG st = fn(GetCurrentProcess(), 4);          // (D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH)
                    int cls = 4;
                    if (st != 0) { st = fn(GetCurrentProcess(), 3); cls = 3; }   // (..._ABOVE_NORMAL)
                    Log("Renderer: GPU scheduling class %s %s (0x%08lX)", cls == 4 ? "high" : "above normal",
                        st == 0 ? "set" : "refused", (unsigned long)st);
                }
            }
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
    m_swapFormat = m_hdrOut ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;   // + an _SRGB view, as for flip
    m_rtvFormat  = m_hdrOut ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

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
    // (Plane mode, Settings WeavePlane -- an experiment: opaque, and allowed to
    // tear, so Windows may give the weave a display plane of its own: shown
    // without being composited, and not re-dirtying the screen capture. No
    // cut-outs then: nothing is see-through.)
    if (m_planeMode)
    {
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (m_allowTearing) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    HRESULT hr = m_factory->CreateSwapChainForComposition(m_device, &sd, nullptr, &m_swapChain);
    if (FAILED(hr) && (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        sd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = m_factory->CreateSwapChainForComposition(m_device, &sd, nullptr, &m_swapChain);
    }
    if (m_planeMode) Log("Renderer: plane mode -- opaque%s (0x%08X)", (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? ", tearing allowed" : "", (unsigned)hr);
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
    if (m_hdrOut)
    {
        IDXGISwapChain3* sc3 = nullptr;
        HRESULT hc = m_swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3);
        if (SUCCEEDED(hc)) hc = sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        SAFE_RELEASE(sc3);
        Log("Renderer: 16-bit float output (scRGB) %s (0x%08X)", SUCCEEDED(hc) ? "set" : "REFUSED", (unsigned)hc);
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
    float4 exOwner[16];   //   ... whose they are: the trRect index, or -1 = everyone's (4 per float4)
};
// (The Direct3D 12 presenter is handed them as a small texture: GPU_TEX.)
#ifdef GPU_TEX
Texture2D<int4> gpuResT : register(t0);
#define GPURES(s) gpuResT.Load(int3(s, 0, 0))
#else
StructuredBuffer<int4> gpuRes : register(t0);
#define GPURES(s) gpuRes[s]
#endif
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
// Which rects can touch each tile of the screen (Renderer::ApplyMask): [0] =
// (tiles across, down, tile width, height); per tile two uint4s -- the static
// visible rects (64 bits), the holes (64 bits), then the tracked pictures
// (16 bits, grown by the GPU tracker's search). Only those are tested: most
// of a page is nowhere near a picture.
StructuredBuffer<uint4> tiles : register(t1);
uint TakeBit(inout uint lo, inout uint hi)
{
    if (lo) { const uint b = firstbitlow(lo); lo &= lo - 1; return b; }
    const uint b = firstbitlow(hi); hi &= hi - 1; return 32 + b;
}
float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float m = all ? 1.0 : 0.0;
    const uint4 th = tiles[0];
    const uint t = min((uint)pos.y / th.w, th.y - 1) * th.x + min((uint)pos.x / th.z, th.x - 1);
    const uint4 ta = tiles[1 + 2 * t];
    uint rLo = ta.x, rHi = ta.y, trBits = tiles[2 + 2 * t].x;
    uint i;
    [loop] while (m == 0 && (rLo | rHi))
        if (Inside(pos.xy, rects[TakeBit(rLo, rHi)])) m = 1.0;
    [loop] while (m == 0 && trBits)
    {
        i = firstbitlow(trBits); trBits &= trBits - 1;
        float dx = 0, dy = 0;
        const int slot = (int)trInfo[i].x;
        if (slot >= 0) { const int4 g = GPURES(slot); if (g.y) dy = g.x * trInfo[i].y; if (g.w) dx = g.z * trInfo[i].y; }
        if (!Inside(pos.xy, trRect[i] + float4(dx, dy, dx, dy)) || !Inside(pos.xy, trClip[i])) continue;
        // ... unless one of THIS picture's own holes (a window in front of
        // it) covers the spot. Another picture's holes don't: a window in
        // front of a browser's pictures is often the one holding a picture
        // of its own (Discord over Zen), which was cut out along the edge of
        // the browser's area.
        bool hidden = false;
        uint eLo = ta.z, eHi = ta.w;
        [loop] while (!hidden && (eLo | eHi))
        {
            const uint j = TakeBit(eLo, eHi);
            if ((int)exOwner[j / 4][j % 4] == (int)i && InsideRounded(pos.xy, excl[j], exRad[j / 4][j % 4])) hidden = true;
        }
        if (!hidden) m = 1.0;
    }
    // Holes in everything (the taskbar, 2D windows).
    uint gLo = ta.z, gHi = ta.w;
    [loop] while (m != 0 && (gLo | gHi))
    {
        i = TakeBit(gLo, gHi);
        if ((int)exOwner[i / 4][i % 4] < 0 && InsideRounded(pos.xy, excl[i], exRad[i / 4][i % 4])) m = 0.0;
    }
    return float4(m, m, m, m);
}
)";
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    m_maskHLSL = kMaskHLSL;   // (the Direct3D 12 presenter compiles the same source: SetDX12)
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
    cb.ByteWidth      = kMaskCBBytes;
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cb, nullptr, &m_maskCB);
    // The tile lists (PSMain's tiles): [0] header + two uint4 per tile.
    D3D11_BUFFER_DESC tb{};
    tb.ByteWidth           = kMaskTilesBytes;
    tb.Usage               = D3D11_USAGE_DYNAMIC;
    tb.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
    tb.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
    tb.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    tb.StructureByteStride = 16;
    if (SUCCEEDED(m_device->CreateBuffer(&tb, nullptr, &m_maskTiles)))
        m_device->CreateShaderResourceView(m_maskTiles, nullptr, &m_maskTilesSRV);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_scissorRS);
    return m_maskVS && m_maskPS && m_maskBlend && m_maskCB && m_maskTilesSRV;
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

// The mask shader's constants (cbuffer M) and tile lists, written to head / t
// (kMaskCBBytes / kMaskTilesBytes). gpuSlots false: tracked pictures without
// the GPU tracker's offsets (the Direct3D 12 presenter doesn't have them).
void Renderer::FillMaskConstants(uint32_t* head, bool gpuSlots) const
{
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
    float* exOwn  = exRad + 64;      // 64 (16 float4s)
    for (int i = 0; i < 64; ++i) { exRad[i] = 0.0f; exOwn[i] = -1.0f; }
    for (size_t i = 0; i < m_maskRects.size(); ++i) put(rects + i * 4, m_maskRects[i]);
    for (size_t i = 0; i < m_maskTracked.size(); ++i)
    {
        put(trRect + i * 4, m_maskTracked[i].rect);
        put(trClip + i * 4, m_maskTracked[i].clip);
        trInfo[i * 4 + 0] = gpuSlots ? (float)m_maskTracked[i].slot : -1.0f;
        trInfo[i * 4 + 1] = m_maskTracked[i].scale;
        trInfo[i * 4 + 2] = trInfo[i * 4 + 3] = 0.0f;
    }
    for (size_t i = 0; i < m_maskExcl.size(); ++i)
    {
        put(excl + i * 4, m_maskExcl[i].rect);
        exRad[i] = (float)m_maskExcl[i].radius;
        exOwn[i] = (float)m_maskExcl[i].owner;
    }
}

// Which rects can touch each tile (PSMain's tiles). A tracked picture may
// move by up to the GPU tracker's search this frame: its tiles cover that
// (within its viewport).
void Renderer::FillMaskTiles(uint32_t* t) const
{
    memset(t, 0, kMaskTilesBytes);
    const int tw = (m_width + kMaskTilesX - 1) / kMaskTilesX, th = (m_height + kMaskTilesY - 1) / kMaskTilesY;
    t[0] = kMaskTilesX; t[1] = kMaskTilesY; t[2] = (uint32_t)(std::max)(tw, 1); t[3] = (uint32_t)(std::max)(th, 1);
    // word: 0/1 static rects (lo/hi), 2/3 holes (lo/hi), 4 tracked.
    auto mark = [&](RECT r, int word, int bit) {
        if (r.right <= r.left || r.bottom <= r.top || tw <= 0 || th <= 0) return;
        const int x0 = (std::max)(0, (int)r.left / tw), x1 = (std::min)(kMaskTilesX - 1, (int)(r.right - 1) / tw);
        const int y0 = (std::max)(0, (int)r.top / th),  y1 = (std::min)(kMaskTilesY - 1, (int)(r.bottom - 1) / th);
        const int w = word + (bit >= 32 ? 1 : 0);
        const uint32_t b = 1u << (bit & 31);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                t[4 + (y * kMaskTilesX + x) * 8 + w] |= b;
    };
    for (size_t i = 0; i < m_maskRects.size() && i < 64; ++i) mark(m_maskRects[i], 0, (int)i);
    for (size_t i = 0; i < m_maskExcl.size() && i < 64; ++i) mark(m_maskExcl[i].rect, 2, (int)i);
    for (size_t i = 0; i < m_maskTracked.size() && i < 16; ++i)
    {
        const MaskTracked& k = m_maskTracked[i];
        RECT r = k.rect;
        if (k.slot >= 0)
        {
            const int gx = (int)std::ceil(48 * k.scale) + 1, gy = (int)std::ceil(96 * k.scale) + 1;   // (GpuTracker::kSearchX / kSearch)
            InflateRect(&r, gx, gy);
        }
        RECT c{};
        if (IntersectRect(&c, &r, &k.clip)) mark(c, 4, (int)i);
    }
}

// The GPU tracker's results (a Direct3D 11 buffer: not shareable) drawn into a
// 16x1 texture that is, and handed to the Direct3D 12 presenter's mask.
bool Renderer::UpdateGpuShare()
{
    if (!m_p12) return false;
    if (!m_gpuShareTex)
    {
        if (m_gpuShareFailed) return false;
        static const char* kPS = "StructuredBuffer<int4> r : register(t0); int4 PSMain(float4 p : SV_Position) : SV_Target { return r[(int)p.x]; }";
        ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 16; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_SINT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(D3DCompile(kPS, strlen(kPS), "GpuShare", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)) ||
            FAILED(m_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_gpuSharePS)) ||
            FAILED(m_device->CreateTexture2D(&td, nullptr, &m_gpuShareTex)) ||
            FAILED(m_device->CreateRenderTargetView(m_gpuShareTex, nullptr, &m_gpuShareRTV)))
        {
            Log("Renderer: the tracker offsets could not be shared with the Direct3D 12 presenter");
            SAFE_RELEASE(m_gpuShareRTV); SAFE_RELEASE(m_gpuShareTex); SAFE_RELEASE(m_gpuSharePS);
            m_gpuShareFailed = true;
        }
        SAFE_RELEASE(psb); SAFE_RELEASE(err);
        if (!m_gpuShareTex) return false;
    }
    const D3D11_VIEWPORT vp{ 0, 0, 16, 1, 0, 1 };
    m_context->OMSetRenderTargets(1, &m_gpuShareRTV, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_maskVS, nullptr, 0);
    m_context->PSSetShader(m_gpuSharePS, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, &m_maskGpu);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    return m_p12->SetGpuResults(m_gpuShareTex);
}

// A band of the woven back buffer (rows y0 .. y0 + rows, the whole width, 4
// bytes a pixel in memory order R G B A), read back now: after the weave of the
// frame being drawn, before its present. Blocks; for a measurement, not a frame
// in, frame out thing. False: not available (16-bit float output).
bool Renderer::ReadBackRows(UINT y0, UINT rows, std::vector<uint8_t>& out)
{
    if (rows == 0 || y0 + rows > m_height) return false;
    if (m_p12)
    {
        if (!m_p12->CopyRows(y0, rows)) return false;
        m_p12->EndFrame();
        m_p12->WaitIdle();
        return m_p12->FetchRows(out);
    }
    if (!m_rtv || m_swapFormat != DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = m_width; sd.Height = rows; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; ID3D11Resource* bb = nullptr;
    bool ok = false;
    m_rtv->GetResource(&bb);
    if (bb && SUCCEEDED(m_device->CreateTexture2D(&sd, nullptr, &st)))
    {
        const D3D11_BOX box{ 0, y0, 0, m_width, y0 + rows, 1 };
        m_context->CopySubresourceRegion(st, 0, 0, 0, 0, bb, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(m_context->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            out.resize((size_t)m_width * rows * 4);
            for (UINT y = 0; y < rows; ++y)
                memcpy(&out[(size_t)y * m_width * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)m_width * 4);
            m_context->Unmap(st, 0);
            ok = true;
        }
    }
    SAFE_RELEASE(st); SAFE_RELEASE(bb);
    return ok;
}

// The picture drawn as it is (the light field: already interlaced for the lens
// by the converter), in place of an SR weave: the left half of src -- one whole
// picture, the size of the panel -- copied into the back buffer. Direct3D 11
// presenter only.
bool Renderer::BlitPicture(ID3D11Texture2D* src)
{
    if (!src || m_p12 || !m_rtv) return false;
    ID3D11Resource* bb = nullptr;
    m_rtv->GetResource(&bb);
    if (!bb) return false;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    const D3D11_BOX box{ 0, 0, 0, (std::min)(m_width, sd.Width / 2), (std::min)(m_height, sd.Height), 1 };
    m_context->CopySubresourceRegion(bb, 0, 0, 0, 0, src, 0, &box);
    bb->Release();
    return true;
}

// One wholly see-through frame, shown now: for when the weave stops. The window
// is hidden straight after, but its last picture could stay on the screen --
// seen with the swap chain put straight on the display (UpdateAutoPlane), where
// hiding the window left the picture standing. So: back to the composed,
// see-through swap chain if need be, and nothing visible in it. The mask as it
// was is put back for when weaving starts again.
void Renderer::PresentBlank()
{
    if (!m_dcomp || !m_swapChain) return;
    const bool all = m_maskAll;
    const std::vector<RECT> rects = m_maskRects;
    const std::vector<MaskTracked> tracked = m_maskTracked;
    const std::vector<MaskCut> excl = m_maskExcl;
    m_maskAll = false; m_maskRects.clear(); m_maskTracked.clear(); m_maskExcl.clear();
    for (int i = 0; i < 2; ++i)   // (both buffers of the swap chain)
    {
        BindAndClearBackBuffer();   // (nothing to show through any more: leaves the direct mode)
        m_maskDone = false;
        Present(false, false);
    }
    if (m_p12) m_p12->WaitIdle();
    m_clearRun = 0;
    m_maskAll = all; m_maskRects = rects; m_maskTracked = tracked; m_maskExcl = excl;
}

// The bounds of what the mask leaves visible (client pixels), when that is
// less than everything: false = all of the window. An empty rect: nothing.
bool Renderer::WeaveBounds(RECT& out) const
{
    if (m_maskAll || m_planeMode) return false;
    RECT b{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    auto add = [&](const RECT& r) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        b.left = (std::min)(b.left, r.left); b.top = (std::min)(b.top, r.top);
        b.right = (std::max)(b.right, r.right); b.bottom = (std::max)(b.bottom, r.bottom);
    };
    for (const RECT& r : m_maskRects) add(r);
    for (const MaskTracked& k : m_maskTracked)
    {
        RECT r = k.rect;
        // (It may move by up to the GPU tracker's search this frame: as FillMaskTiles.)
        if (k.slot >= 0) InflateRect(&r, (int)std::ceil(48 * k.scale) + 1, (int)std::ceil(96 * k.scale) + 1);
        RECT c{};
        if (IntersectRect(&c, &r, &k.clip)) add(c);
    }
    const RECT all{ 0, 0, (LONG)m_width, (LONG)m_height };
    if (b.right <= b.left || !IntersectRect(&out, &b, &all)) out = RECT{};
    return true;
}

// Is the SR display in HDR mode (Windows' HDR switched on for it)? maxNits:
// its peak brightness as it reports it.
bool Renderer::DisplayIsHdr(float* maxNits)
{
    // (A factory of its own: an output's description is as it was when its
    // factory was made, and HDR may have been switched since ours was.)
    bool hdr = false;
    const HMONITOR target = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) return false;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j)
        {
            IDXGIOutput6* o6 = nullptr; DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == target)
            {
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                if (maxNits) *maxNits = d.MaxLuminance;
            }
            SAFE_RELEASE(o6);
            o->Release();
        }
        a->Release();
    }
    f->Release();
    return hdr;
}

// The Direct3D 11 presenter's swap chain 16-bit float scRGB (HDR output)
// instead of 8-bit sRGB: the weaver draws into whatever is bound.
void Renderer::SetHdrOutput(bool on)
{
    if (on == m_hdrOut) return;
    m_hdrOut = on;
    if (!m_dcomp || m_p12) return;   // (the Direct3D 12 presenter has its own: SetDX12)
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    CreateDCompSwapChain();
}

bool Renderer::SetDX12(bool on, bool hdr)
{
    if (!m_dcomp || !m_dcVisual) return false;
    if (on == (m_p12 != nullptr) && (!on || m_p12->IsHdr() == hdr)) return true;
    if (on && m_p12) SetDX12(false, false);   // (the other output format: made anew)
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (on)
    {
        m_p12 = new Present12();
        m_p12->SetHdr(hdr);
        if (!m_p12->Initialize(m_device, m_context, m_factory, m_width, m_height, m_maskHLSL))
        {
            delete m_p12; m_p12 = nullptr;
            return false;
        }
        SAFE_RELEASE(m_rtv);
        SAFE_RELEASE(m_swapChain);
        m_context->Flush();
        m_swapChain = m_p12->SwapChain(); m_swapChain->AddRef();
        m_swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        m_waitable = nullptr;
        IDXGISwapChain2* sc2 = nullptr;
        if (SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)))
        {
            m_waitable = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
        if (FAILED(m_dcVisual->SetContent(m_swapChain)) || FAILED(m_dcomp->Commit()))
        {
            Log("Renderer: the Direct3D 12 swap chain could not be shown -- back to Direct3D 11");
            SAFE_RELEASE(m_swapChain); m_waitable = nullptr;
            delete m_p12; m_p12 = nullptr;
            CreateDCompSwapChain();
            return false;
        }
        return true;
    }
    SAFE_RELEASE(m_swapChain); m_waitable = nullptr;
    delete m_p12; m_p12 = nullptr;
    return CreateDCompSwapChain();
}

void Renderer::ApplyMask()
{
    if (m_planeMode) { m_maskDone = true; return; }   // (opaque: nothing to cut out)
    if (m_p12)
    {
        // The same mask, drawn by the Direct3D 12 presenter after its weave.
        m_p12->Mark(1);
        m_maskDone = true;
        const bool opaque = m_p12->AlphaState() == 2;
        const bool holes = opaque && m_maskAll && m_maskRects.empty() && m_maskTracked.empty();
        if (holes && m_maskExcl.empty()) return;
        static std::vector<uint32_t> cb(kMaskCBBytes / 4), tiles(kMaskTilesBytes / 4);
        // (The GPU tracker's offsets, for pictures that follow the page: copied
        // into a small texture the presenter shares.)
        const bool gpu = !m_maskTracked.empty() && m_maskGpu && UpdateGpuShare();
        FillMaskConstants(cb.data(), gpu);
        FillMaskTiles(tiles.data());
        std::vector<D3D12_RECT> sc;
        if (holes)
        {
            for (const MaskCut& c : m_maskExcl)
            {
                const D3D12_RECT r{ (std::max)(c.rect.left, 0L), (std::max)(c.rect.top, 0L),
                                    (std::min)(c.rect.right, (LONG)m_width), (std::min)(c.rect.bottom, (LONG)m_height) };
                if (r.right > r.left && r.bottom > r.top) sc.push_back(r);
            }
        }
        else
            sc.push_back(m_p12->Scissor());
        if (!sc.empty()) m_p12->DrawMask(cb.data(), kMaskCBBytes, tiles.data(), kMaskTilesBytes, sc.data(), (int)sc.size());
        return;
    }
    if (!m_rtv || !m_maskPS || !m_dcomp) return;
    m_context->RSSetState(nullptr);   // (the weave's scissor, if BindAndClearBackBuffer set one)
    m_maskDone = true;
    // Does the SR weave leave the picture opaque (alpha 1, as cleared)? Then
    // when everything is shown -- Fullscreen, a game -- the full-screen mask
    // pass (~0.3 ms of GPU a frame at 4K) is skipped, and holes (the
    // taskbar, a window in front) are drawn on their own. Found out once:
    // one pixel of the weave read back a frame later (never waited on).
    D3D11_MAPPED_SUBRESOURCE m{};
    if (m_alphaProbe == 1 && m_alphaProbeTex &&
        SUCCEEDED(m_context->Map(m_alphaProbeTex, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)))
    {
        const uint8_t a = static_cast<const uint8_t*>(m.pData)[3];
        m_context->Unmap(m_alphaProbeTex, 0);
        m_alphaProbe = a == 255 ? 2 : 3;
        Log("Renderer: the SR weave %s -- full-screen mask pass %s", a == 255 ? "keeps the picture opaque" : "writes its own alpha",
            a == 255 ? "skipped when nothing is hidden" : "kept");
    }
    if (m_alphaProbe == 0 && m_swapFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        D3D11_TEXTURE2D_DESC pd{};
        pd.Width = 1; pd.Height = 1; pd.MipLevels = 1; pd.ArraySize = 1; pd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1; pd.Usage = D3D11_USAGE_STAGING; pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Resource* bb = nullptr; m_rtv->GetResource(&bb);
        if (bb && (m_alphaProbeTex || SUCCEEDED(m_device->CreateTexture2D(&pd, nullptr, &m_alphaProbeTex))))
        {
            D3D11_BOX box{ (UINT)m_width / 2, (UINT)m_height / 2, 0, (UINT)m_width / 2 + 1, (UINT)m_height / 2 + 1, 1 };
            m_context->CopySubresourceRegion(m_alphaProbeTex, 0, 0, 0, 0, bb, 0, &box);   // (before the mask below)
            m_alphaProbe = 1;
        }
        SAFE_RELEASE(bb);
    }
    const bool holesOnly = m_alphaProbe == 2 && m_maskAll && m_maskRects.empty() && m_maskTracked.empty();
    if (holesOnly && m_maskExcl.empty()) return;   // (nothing hidden: nothing to do)
    if (FAILED(m_context->Map(m_maskCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    FillMaskConstants(static_cast<uint32_t*>(m.pData), true);
    m_context->Unmap(m_maskCB, 0);
    if (m_maskTiles && SUCCEEDED(m_context->Map(m_maskTiles, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        FillMaskTiles(static_cast<uint32_t*>(m.pData));
        m_context->Unmap(m_maskTiles, 0);
    }

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
    ID3D11ShaderResourceView* srvs[2] = { gpu, m_maskTilesSRV };
    m_context->PSSetShaderResources(0, 2, srvs);
    if (holesOnly && m_scissorRS)
    {
        // (Everything else is opaque already: only each hole's own pixels.)
        m_context->RSSetState(m_scissorRS);
        for (const MaskCut& c : m_maskExcl)
        {
            D3D11_RECT sc{ (std::max)(c.rect.left, 0L), (std::max)(c.rect.top, 0L),
                           (std::min)(c.rect.right, (LONG)m_width), (std::min)(c.rect.bottom, (LONG)m_height) };
            if (sc.right <= sc.left || sc.bottom <= sc.top) continue;
            m_context->RSSetScissorRects(1, &sc);
            m_context->Draw(3, 0);
        }
        m_context->RSSetState(nullptr);
    }
    else
        m_context->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_context->PSSetShaderResources(0, 2, nulls);
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

    if (m_p12)
    {
        if (!m_p12->Resize(width, height)) return false;
        m_width = width; m_height = height;
        return true;
    }
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

// Straight to the display when nothing has to show through. A see-through
// swap chain is composed with the desktop by Windows: a step that costs about
// 5 ms from the weave to the display (measured 6.5 ms against 1.1), and makes
// the screen capture see the whole screen as changed every refresh. An opaque
// one, allowed to tear, Windows puts on the display directly. So: after a
// second with the whole window shown and no cut-outs (a video or a game
// filling the screen), the swap chain is made opaque; the moment anything has
// to show through again (the taskbar, a window in front), see-through.
void Renderer::UpdateAutoPlane()
{
    if (!m_autoPlane || !m_dcomp) return;
    const bool clear = m_maskAll && m_maskRects.empty() && m_maskTracked.empty() && m_maskExcl.empty();
    m_clearRun = clear ? m_clearRun + 1 : 0;
    const bool want = clear && m_clearRun > 160;
    if (want == m_planeMode) return;
    m_planeMode = want;
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    bool ok = false;
    if (m_p12)
    {
        SAFE_RELEASE(m_swapChain); m_waitable = nullptr;
        ok = m_p12->RecreateSwapChain(m_factory, want, m_allowTearing);
        if (ok)
        {
            m_swapChain = m_p12->SwapChain(); m_swapChain->AddRef();
            m_swapFlags = m_p12->SwapFlags();
            IDXGISwapChain2* sc2 = nullptr;
            if (SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) { m_waitable = sc2->GetFrameLatencyWaitableObject(); sc2->Release(); }
            ok = SUCCEEDED(m_dcVisual->SetContent(m_swapChain)) && SUCCEEDED(m_dcomp->Commit());
        }
    }
    else
        ok = CreateDCompSwapChain();
    Log("Renderer: the weave now goes %s%s", want ? "straight to the display (opaque: nothing to show through)" : "through Windows' compositor (see-through for cut-outs)",
        ok ? "" : " -- FAILED");
}

void Renderer::BindAndClearBackBuffer()
{
    UpdateAutoPlane();
    { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); m_weaveQpc = q.QuadPart; }   // (the weave is called next: NotePresent)
    if (m_p12)
    {
        // (Cleared and bound on its own command list. The weave is wanted only
        // within the visible areas' bounds.)
        RECT wb{};
        if (WeaveBounds(wb)) m_p12->SetWeaveScissor(D3D12_RECT{ wb.left, wb.top, wb.right, wb.bottom });
        else                 m_p12->ClearWeaveScissor();
        m_p12->BeginFrame();
        return;
    }
    if (!m_rtv) return;

    const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv, black);
    // (The weave only within the visible areas' bounds, when that is less than
    // the window: a scissor rectangle, if the weaver keeps the state it finds.)
    RECT wb{};
    if (m_scissorRS && WeaveBounds(wb))
    {
        const D3D11_RECT sc{ wb.left, wb.top, wb.right, wb.bottom };
        m_context->RSSetScissorRects(1, &sc);
        m_context->RSSetState(m_scissorRS);
    }
    else
        m_context->RSSetState(nullptr);
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
    using wclk = std::chrono::steady_clock;
    const auto w0 = wclk::now();
    // The compositor's frame signal ticks on Windows' composition clock. When
    // that runs at another display's rate (e.g. 240 Hz for a second monitor,
    // with the SR display at 160 Hz), its ticks slide against the SR display's
    // refreshes as the two clocks drift, and for minutes at a time frames
    // were late (9-15 a second). With the SR display's own vertical blank to
    // pace on (below), that signal is then skipped. (Checked every second.)
    if (m_vblankOutput && m_targetIntervalNs > 0 && GetTickCount64() - m_clockCheckMs > 1000)
    {
        m_clockCheckMs = GetTickCount64();
        DWM_TIMING_INFO ti{}; ti.cbSize = sizeof(ti);
        bool other = false;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateCompose.uiDenominator)
        {
            const double dwmHz = (double)ti.rateCompose.uiNumerator / ti.rateCompose.uiDenominator;
            const double srHz  = 1e9 / (m_targetIntervalNs / 0.98);
            other = std::abs(dwmHz - srHz) > srHz * 0.03;
            if (other != m_compositorOtherClock)
                Log("Renderer: Windows composes at %.1f Hz, the SR display refreshes at %.1f Hz -- %s", dwmHz, srHz,
                    other ? "pacing on the SR display alone" : "pacing on the compositor and the SR display");
        }
        m_compositorOtherClock = other;
    }
    // (Not waited on with the compositor on another clock -- above -- but asked,
    // without waiting: is the last frame still queued? Then this loop's present
    // has to wait for it. Counted for the perf log: TakeQueueStats.)
    if (m_waitable && m_compositorOtherClock)
    {
        ++m_queueChecks;
        if (WaitForSingleObjectEx(m_waitable, 0, TRUE) != WAIT_OBJECT_0) ++m_queueBusy;
    }
    if (m_waitable && !m_compositorOtherClock)
        WaitForSingleObjectEx(m_waitable, 100, TRUE);   // (short: the swap chain -- and this handle -- may be remade from another thread meanwhile)
    m_lastCompositorWaitMs = std::chrono::duration<double, std::milli>(wclk::now() - w0).count();
    m_lastVBlankWaitMs = 0.0;

    // Render-rate cap: hold here until one display refresh has passed since
    // the START of the previous frame (not the end of its present: that made
    // each frame refresh + work long, e.g. ~7.5 ms instead of 6.25 at 160 Hz).
    // Avoids rendering past the SR panel's refresh, which would just be
    // wasted GPU + heat. Hybrid timer+yield+spin pacing gives sub-ms precision
    // without burning a full core continuously.
    if (m_targetIntervalNs > 0)
    {
        using namespace std::chrono;
        // With the display's vertical blank to sync on (below), the cap only
        // has to get close: 80% of a refresh, the blank wait does the exact
        // rest. Aiming at 98% left ~0.1 ms of slack, and a wake-up a millisecond
        // or two late -- Windows under load (a video decoding, a browser
        // scrolling) -- missed the refresh: the frame shown twice.
        const int64_t capNs = m_vblankOutput ? (int64_t)(m_targetIntervalNs / 0.98 * 0.80) : m_targetIntervalNs;
        const auto target = m_lastFrameStart + nanoseconds(capNs);
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
    // Then start the frame on the SR display's vertical blank: one frame per
    // refresh, locked to the display (the cap alone drifted, 155-163 frames/s
    // at 160 Hz). Not when this frame is already late -- a whole refresh
    // since the last one started -- where waiting would skip a refresh.
    // Nor when a blank has only just passed: waiting on the compositor (above)
    // runs on Windows' own composition clock, which drifts against the
    // display's, and it often let go just after a blank -- then "the next
    // blank" was almost a whole refresh away and the frame showed twice (a
    // judder, 10-35 times a second). Just past one, the frame is on time as
    // it is. (When the last blank was is known from the last wait.)
    if (m_vblankOutput && m_targetIntervalNs > 0)
    {
        using namespace std::chrono;
        const auto now = steady_clock::now();
        const int64_t period = (int64_t)(m_targetIntervalNs / 0.98);
        const auto since = duration_cast<nanoseconds>(now - m_lastFrameStart).count();
        bool justPast = false;
        if (m_lastVBlank.time_since_epoch().count() != 0)
        {
            const int64_t phase = duration_cast<nanoseconds>(now - m_lastVBlank).count() % period;
            justPast = phase < 1'500'000;
        }
        if (since < period && !justPast)
        {
            const auto v0 = wclk::now();
            // (A reference of its own for the wait: another thread may switch the
            // output meanwhile -- SetVBlankMonitor -- while this one waits on it.)
            IDXGIOutput* out = nullptr;
            { std::lock_guard<std::mutex> g(m_vblankMutex); out = m_vblankOutput; if (out) out->AddRef(); }
            if (out) { out->WaitForVBlank(); out->Release(); }
            m_lastVBlank = wclk::now();
            m_lastVBlankWaitMs = std::chrono::duration<double, std::milli>(m_lastVBlank - v0).count();
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
        if (m_p12) m_p12->EndFrame();   // (sent to its queue, behind Direct3D 11's drawing)
        m_swapChain->Present(0, (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (m_latencyStats) NotePresent();
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
    if (m_latencyStats) NotePresent();
    m_lastPresentEnd = std::chrono::steady_clock::now();
}

// From the weave call to the picture being on the display, measured: each
// present is remembered with when its weave was called; the swap chain's frame
// statistics later say which present reached the display at which refresh.
void Renderer::NotePresent()
{
    UINT count = 0;
    if (FAILED(m_swapChain->GetLastPresentCount(&count))) return;
    m_presRing[count % kPresRing] = { count, m_weaveQpc };
    DXGI_FRAME_STATISTICS st{};
    if (FAILED(m_swapChain->GetFrameStatistics(&st)) || st.PresentCount == m_statCount) return;
    m_statCount = st.PresentCount;
    const PresNote& r = m_presRing[st.PresentCount % kPresRing];
    if (r.count != st.PresentCount || r.qpc == 0) return;
    static LARGE_INTEGER f = [] { LARGE_INTEGER q{}; QueryPerformanceFrequency(&q); return q; }();
    const double ms = (double)(st.SyncQPCTime.QuadPart - r.qpc) * 1000.0 / (double)f.QuadPart;
    if (ms > 0.0 && ms < 200.0) { m_latSum += ms; if (ms > m_latMax) m_latMax = ms; if (ms < m_latMin) m_latMin = ms; ++m_latN; }
}

bool Renderer::TakeDisplayLatency(double& avgMs, double& minMs, double& maxMs, int& n)
{
    n = m_latN; avgMs = n ? m_latSum / n : 0.0; minMs = n ? m_latMin : 0.0; maxMs = m_latMax;
    m_latSum = 0.0; m_latMax = 0.0; m_latMin = 1e9; m_latN = 0;
    return n > 0;
}

void Renderer::Shutdown()
{
    SAFE_RELEASE(m_rtv);
    if (m_p12) { SAFE_RELEASE(m_swapChain); delete m_p12; m_p12 = nullptr; }
    SAFE_RELEASE(m_swapChain);
    SAFE_RELEASE(m_maskTilesSRV); SAFE_RELEASE(m_maskTiles);
    SAFE_RELEASE(m_scissorRS); SAFE_RELEASE(m_alphaProbeTex); m_alphaProbe = 0;
    SAFE_RELEASE(m_gpuShareRTV); SAFE_RELEASE(m_gpuShareTex); SAFE_RELEASE(m_gpuSharePS);
    SAFE_RELEASE(m_maskCB); SAFE_RELEASE(m_maskBlend); SAFE_RELEASE(m_maskPS); SAFE_RELEASE(m_maskVS);
    SAFE_RELEASE(m_dcVisual); SAFE_RELEASE(m_dcTarget); SAFE_RELEASE(m_dcomp);
    { std::lock_guard<std::mutex> g(m_vblankMutex); SAFE_RELEASE(m_vblankOutput); }
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

double Renderer::MsToNextVBlank() const
{
    if (m_targetIntervalNs <= 0 || m_lastVBlank.time_since_epoch().count() == 0) return -1.0;
    using namespace std::chrono;
    const int64_t period = (int64_t)(m_targetIntervalNs / 0.98);
    const int64_t phase = duration_cast<nanoseconds>(steady_clock::now() - m_lastVBlank).count() % period;
    return (period - phase) / 1.0e6;
}

void Renderer::SetVBlankMonitor(HMONITOR target)
{
    if (!target || !m_device) return;
    if (m_vblankOutput)
    {
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(m_vblankOutput->GetDesc(&od)) && od.Monitor == target) return;
    }
    // (On the device's adapter: the SR display's, from Initialize.)
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* a = nullptr;
    IDXGIOutput* found = nullptr;
    if (SUCCEEDED(m_device->QueryInterface(&dd)) && SUCCEEDED(dd->GetAdapter(&a)))
    {
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; !found && a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j)
        {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == target) found = o;
            else o->Release();
        }
    }
    SAFE_RELEASE(a);
    SAFE_RELEASE(dd);
    {
        std::lock_guard<std::mutex> g(m_vblankMutex);   // (WaitForFrame may be about to wait on it)
        SAFE_RELEASE(m_vblankOutput);
        m_vblankOutput = found;   // (none: another adapter -- paced by the cap and the compositor)
    }
    m_lastVBlank = {};        // (the old display's phase means nothing now)
    m_clockCheckMs = 0;       // (compare the clocks again straight away)
    Log("Renderer: waiting for refreshes on %s", found ? "the SR display's new output" : "nothing (the SR display isn't on this adapter)");
}
