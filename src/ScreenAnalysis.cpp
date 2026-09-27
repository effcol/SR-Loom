// ScreenAnalysis.cpp -- see ScreenAnalysis.h.
#include "ScreenAnalysis.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <array>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    // Run fn(i) for i in [0,n) on up to all CPU cores (the scan runs on its
    // own background thread; this fans it out further).
    template <class F> void ParallelFor(int n, F fn)
    {
        if (n <= 0) return;
        // At most half the cores, at the caller's priority (the scanner runs
        // below normal): the render loop must never have to wait for a core.
        const int hw = (int)(std::max)(1u, std::thread::hardware_concurrency());
        const int workers = (std::min)(n, (std::max)(1, hw / 2));
        const int prio = GetThreadPriority(GetCurrentThread());
        std::atomic<int> next{ 0 };
        auto run = [&] { for (int i; (i = next.fetch_add(1)) < n;) fn(i); };
        std::vector<std::thread> pool;
        for (int t = 1; t < workers; ++t)
            pool.emplace_back([&, prio] { SetThreadPriority(GetCurrentThread(), prio); run(); });
        run();
        for (auto& t : pool) t.join();
    }
    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // Half-resolution luma (target 0) and red / green / blue (target 1). Each output
    // pixel samples the centre of a 2x2 source block, so bilinear filtering
    // averages the four. The sRGB view decodes to linear; re-encode with
    // ~gamma 2.2 so edge thresholds behave perceptually.
    const char* kLumaHLSL = R"(
Texture2D    src  : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
struct PSOut { float l : SV_Target0; float r : SV_Target1; float g : SV_Target2; float b : SV_Target3; float c : SV_Target4; };
PSOut PSMain(VSOut i)
{
    float3 c = src.Sample(samp, i.uv).rgb;
    PSOut o;
    o.l  = pow(saturate(dot(c, float3(0.2126, 0.7152, 0.0722))), 1.0 / 2.2);
    float3 e = pow(saturate(c), 1.0 / 2.2);
    o.r = e.r; o.g = e.g; o.b = e.b;
    o.c = pow(saturate((c.g + c.b) * 0.5), 1.0 / 2.2);
    return o;
}
// Luma only (every frame while tracking; the colour target is only written
// when a scan or pick needs red/cyan).
float PSLuma(VSOut i) : SV_Target0
{
    float3 c = src.Sample(samp, i.uv).rgb;
    return pow(saturate(dot(c, float3(0.2126, 0.7152, 0.0722))), 1.0 / 2.2);
}
// Interleave statistics at FULL resolution (the analysis image is half-res,
// which blurs 1-px interleaving away). One output pixel per 16x16 source
// tile: mean luma differences to the neighbour 1 and 2 rows down (v1, v2),
// 1 and 2 columns right (h1, h2) and diagonally (d1). A picture's nearer
// neighbours are the more alike; in row-interleaved 3D the rows two apart
// (same eye) are more alike than adjacent ones (the other eye); columns
// likewise; in a checkerboard the diagonal neighbours are the same eye.
float IlLum(int2 p)
{
    float3 c = src.Load(int3(p, 0)).rgb;
    return pow(saturate(dot(c, float3(0.2126, 0.7152, 0.0722))), 1.0 / 2.2);
}
struct IlOut { float4 vh : SV_Target0; float d : SV_Target1; };
IlOut PSInterleave(VSOut i)
{
    const int2 t0 = int2(i.pos.xy) * 16;
    float v1 = 0, v2 = 0, h1 = 0, h2 = 0, d1 = 0;
    [loop] for (int y = 0; y < 16; ++y)
    {
        [loop] for (int x = 0; x < 16; ++x)
        {
            const int2 p = t0 + int2(x, y);
            const float c = IlLum(p);
            v1 += abs(c - IlLum(p + int2(0, 1)));
            v2 += abs(c - IlLum(p + int2(0, 2)));
            h1 += abs(c - IlLum(p + int2(1, 0)));
            h2 += abs(c - IlLum(p + int2(2, 0)));
            d1 += abs(c - IlLum(p + int2(1, 1)));
        }
    }
    IlOut o;
    o.vh = float4(v1, v2, h1, h2) / 256.0;
    o.d  = d1 / 256.0;
    return o;
}
)";

    // ---- FindImageRect tuning (analysis pixels, 0-255 luma) ----------------
    constexpr int   kEdgeThreshold = 14;    // luma step that counts as an edge
    constexpr int   kBand          = 48;    // half-height of the band around the point used to find candidates
    constexpr float kBandCoverage  = 0.6f;  // edge coverage within the band for an edge candidate
    constexpr int   kStrip         = 3;     // strip width for "flat outside / busy inside"
    constexpr int   kGap           = 2;     // strips start this far from the line (edge pixels are never flat)
    constexpr float kFlatOutside   = 0.85f; // outside strip at least this flat ...
    constexpr float kBusyInside    = 0.35f; // ... and inside strip at most this flat
    constexpr int   kMaxCandidates = 12;    // edge-line candidates per side
    constexpr int   kMaxBorderCands = 8;    // flat-bordered candidates per side
    constexpr float kGutterFlat    = 0.97f; // a line this flat across the rect is a gap between images ...
    constexpr float kGutterLumaTol = 4.0f;  // ... if it's the page background's colour (within this)
    constexpr float kMinCoverage   = 0.5f;  // fraction of a side that must be edge
    constexpr int   kMinRectW      = 32, kMinRectH = 24;
    constexpr float kMaxFlat       = 0.40f; // above this flat fraction it isn't an image
}

// ============================================================================
// ScreenAnalyzer
// ============================================================================

ScreenAnalyzer::~ScreenAnalyzer() { Shutdown(); }

bool ScreenAnalyzer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    const size_t len = std::strlen(kLumaHLSL);
    if (FAILED(D3DCompile(kLumaHLSL, len, "ScreenAnalysis", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kLumaHLSL, len, "ScreenAnalysis", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
    {
        Log("ScreenAnalyzer: shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err); SafeRelease(vsb); SafeRelease(psb);
        return false;
    }
    m_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
    m_device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);
    {
        ID3DBlob* pl = nullptr; ID3DBlob* e2 = nullptr;
        if (SUCCEEDED(D3DCompile(kLumaHLSL, len, "ScreenAnalysis", nullptr, nullptr, "PSLuma", "ps_5_0", 0, 0, &pl, &e2)))
            m_device->CreatePixelShader(pl->GetBufferPointer(), pl->GetBufferSize(), nullptr, &m_psLuma);
        SafeRelease(pl); SafeRelease(e2);
        if (SUCCEEDED(D3DCompile(kLumaHLSL, len, "ScreenAnalysis", nullptr, nullptr, "PSInterleave", "ps_5_0", 0, 0, &pl, &e2)))
            m_device->CreatePixelShader(pl->GetBufferPointer(), pl->GetBufferSize(), nullptr, &m_psIl);
        else
            Log("ScreenAnalyzer: interleave shader failed: %s", e2 ? (const char*)e2->GetBufferPointer() : "?");
        SafeRelease(pl); SafeRelease(e2);
    }
    SafeRelease(vsb); SafeRelease(psb);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sd, &m_sampler);
    return m_vs && m_ps && m_sampler;
}

void ScreenAnalyzer::Release()
{
    for (auto& s : m_staging) SafeRelease(s);
    for (auto& slot : m_stagingC) for (auto& s : slot) SafeRelease(s);
    for (auto& f : m_slotFrame) f = 0;
    for (auto& c : m_slotColour) c = false;
    SafeRelease(m_srv); SafeRelease(m_rtv); SafeRelease(m_rt);
    for (int p = 0; p < kPlanes; ++p) { SafeRelease(m_rtvC[p]); SafeRelease(m_rtC[p]); }
    for (auto& slot : m_stagingIl) for (auto& s : slot) SafeRelease(s);
    for (int k = 0; k < 2; ++k) { SafeRelease(m_rtvIl[k]); SafeRelease(m_rtIl[k]); }
    m_ilW = m_ilH = 0;
    m_w = m_h = 0;
}

void ScreenAnalyzer::Shutdown()
{
    Release();
    SafeRelease(m_sampler); SafeRelease(m_psIl); SafeRelease(m_psLuma); SafeRelease(m_ps); SafeRelease(m_vs);
    m_device = nullptr; m_context = nullptr;
}

bool ScreenAnalyzer::Ensure(int w, int h)
{
    if (m_rt && m_w == w && m_h == h) return true;
    Release();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_rt))) return false;
    m_device->CreateRenderTargetView(m_rt, nullptr, &m_rtv);
    m_device->CreateShaderResourceView(m_rt, nullptr, &m_srv);   // for GPU tracking
    // Colour: one R8 plane each for red, green, blue and cyan -- read back
    // with plain row copies (no splitting interleaved pixels on the CPU).
    D3D11_TEXTURE2D_DESC tc = td;
    for (int p = 0; p < kPlanes; ++p)
    {
        if (FAILED(m_device->CreateTexture2D(&tc, nullptr, &m_rtC[p]))) { Release(); return false; }
        m_device->CreateRenderTargetView(m_rtC[p], nullptr, &m_rtvC[p]);
        if (!m_rtvC[p]) { Release(); return false; }
    }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < kRing; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_staging[i]))) { Release(); return false; }
        for (int p = 0; p < kPlanes; ++p)
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_stagingC[i][p]))) { Release(); return false; }
    }
    // Interleave statistics: one texel per 16x16 source tile.
    m_ilW = (w * Scale()) / LumaImage::kIlTile;
    m_ilH = (h * Scale()) / LumaImage::kIlTile;
    if (m_psIl && m_ilW > 0 && m_ilH > 0)
    {
        const DXGI_FORMAT fmts[2] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_FLOAT };
        for (int k = 0; k < 2; ++k)
        {
            D3D11_TEXTURE2D_DESC ti{};
            ti.Width = (UINT)m_ilW; ti.Height = (UINT)m_ilH; ti.MipLevels = 1; ti.ArraySize = 1;
            ti.Format = fmts[k]; ti.SampleDesc.Count = 1;
            ti.Usage = D3D11_USAGE_DEFAULT; ti.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(m_device->CreateTexture2D(&ti, nullptr, &m_rtIl[k]))) { Release(); return false; }
            m_device->CreateRenderTargetView(m_rtIl[k], nullptr, &m_rtvIl[k]);
            ti.Usage = D3D11_USAGE_STAGING; ti.BindFlags = 0; ti.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (int i = 0; i < kRing; ++i)
                if (FAILED(m_device->CreateTexture2D(&ti, nullptr, &m_stagingIl[i][k]))) { Release(); return false; }
        }
    }
    m_w = w; m_h = h;
    return m_rtv != nullptr;
}

void ScreenAnalyzer::Submit(ID3D11ShaderResourceView* src, int srcW, int srcH, bool withColour)
{
    if (!m_device || !src || srcW < 2 || srcH < 2) return;
    const int w = srcW / Scale(), h = srcH / Scale();
    if (!Ensure(w, h)) return;

    D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
    m_context->RSSetViewports(1, &vp);
    // Colour (red/cyan) only when asked: otherwise a luma-only pass, half the
    // output bandwidth.
    const bool colour = withColour || !m_psLuma;
    ID3D11RenderTargetView* rtvs[1 + kPlanes] = { m_rtv, m_rtvC[0], m_rtvC[1], m_rtvC[2], m_rtvC[3] };
    m_context->OMSetRenderTargets(colour ? 1 + kPlanes : 1, rtvs, nullptr);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->IASetInputLayout(nullptr);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(colour ? m_ps : m_psLuma, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetShaderResources(0, 1, &src);
    m_context->Draw(3, 0);
    // With colour (a scan's frame): the full-resolution interleave statistics too.
    const bool il = withColour && m_psIl && m_rtvIl[0] && m_rtvIl[1];
    if (il)
    {
        D3D11_VIEWPORT vi{ 0, 0, (float)m_ilW, (float)m_ilH, 0, 1 };
        m_context->RSSetViewports(1, &vi);
        m_context->OMSetRenderTargets(2, m_rtvIl, nullptr);
        m_context->PSSetShader(m_psIl, nullptr, 0);
        m_context->Draw(3, 0);
    }
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);

    m_context->CopyResource(m_staging[m_next], m_rt);
    if (withColour)
        for (int p = 0; p < kPlanes; ++p) m_context->CopyResource(m_stagingC[m_next][p], m_rtC[p]);
    if (il)
        for (int k = 0; k < 2; ++k) m_context->CopyResource(m_stagingIl[m_next][k], m_rtIl[k]);
    m_slotIl[m_next] = il;
    m_slotColour[m_next] = withColour;
    m_slotFrame[m_next] = ++m_submitted;
    m_next = (m_next + 1) % kRing;
}

bool ScreenAnalyzer::Latest(LumaImage& out, uint64_t& frameId, bool waitForNewest)
{
    if (!m_device || m_w <= 0) return false;
    // Try newest first; skip slots the GPU hasn't finished (never stall) --
    // unless asked to wait for the newest one.
    int order[kRing];
    for (int i = 0; i < kRing; ++i) order[i] = i;
    std::sort(order, order + kRing, [&](int a, int b) { return m_slotFrame[a] > m_slotFrame[b]; });
    for (int k = 0; k < kRing; ++k)
    {
        const int s = order[k];
        if (m_slotFrame[s] == 0 || m_slotFrame[s] <= m_lastRead) break;
        D3D11_MAPPED_SUBRESOURCE m{};
        const bool wait = waitForNewest && k == 0;
        const HRESULT hr = m_context->Map(m_staging[s], 0, D3D11_MAP_READ,
                                          wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) continue;
        out.width = m_w; out.height = m_h;
        out.pixels.resize((size_t)m_w * m_h);
        const uint8_t* srcRow = static_cast<const uint8_t*>(m.pData);
        for (int y = 0; y < m_h; ++y)
            std::memcpy(&out.pixels[(size_t)y * m_w], srcRow + (size_t)y * m.RowPitch, (size_t)m_w);
        m_context->Unmap(m_staging[s], 0);
        out.red.clear(); out.green.clear(); out.blue.clear(); out.cyan.clear();
        if (m_slotColour[s])
        {
            std::vector<uint8_t>* planes[kPlanes] = { &out.red, &out.green, &out.blue, &out.cyan };
            bool ok = true;
            for (int p = 0; p < kPlanes && ok; ++p)
            {
                D3D11_MAPPED_SUBRESOURCE mc{};
                if (FAILED(m_context->Map(m_stagingC[s][p], 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mc)))
                { ok = false; break; }
                planes[p]->resize((size_t)m_w * m_h);
                const uint8_t* src = static_cast<const uint8_t*>(mc.pData);
                for (int y = 0; y < m_h; ++y)
                    std::memcpy(&(*planes[p])[(size_t)y * m_w], src + (size_t)y * mc.RowPitch, (size_t)m_w);
                m_context->Unmap(m_stagingC[s][p], 0);
            }
            if (!ok) { out.red.clear(); out.green.clear(); out.blue.clear(); out.cyan.clear(); }
        }
        out.il.clear(); out.ilW = out.ilH = 0;
        if (m_slotIl[s])
        {
            D3D11_MAPPED_SUBRESOURCE ma{}, mb{};
            const UINT fl = wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
            if (SUCCEEDED(m_context->Map(m_stagingIl[s][0], 0, D3D11_MAP_READ, fl, &ma)))
            {
                if (SUCCEEDED(m_context->Map(m_stagingIl[s][1], 0, D3D11_MAP_READ, fl, &mb)))
                {
                    out.ilW = m_ilW; out.ilH = m_ilH;
                    out.il.resize((size_t)m_ilW * m_ilH * 5);
                    for (int y = 0; y < m_ilH; ++y)
                    {
                        const float* a = reinterpret_cast<const float*>(static_cast<const uint8_t*>(ma.pData) + (size_t)y * ma.RowPitch);
                        const float* b = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mb.pData) + (size_t)y * mb.RowPitch);
                        float* o = &out.il[(size_t)y * m_ilW * 5];
                        for (int x = 0; x < m_ilW; ++x)
                        {
                            o[5 * x + 0] = a[4 * x + 0]; o[5 * x + 1] = a[4 * x + 1];
                            o[5 * x + 2] = a[4 * x + 2]; o[5 * x + 3] = a[4 * x + 3];
                            o[5 * x + 4] = b[x];
                        }
                    }
                    m_context->Unmap(m_stagingIl[s][1], 0);
                }
                m_context->Unmap(m_stagingIl[s][0], 0);
            }
        }
        m_lastRead = m_slotFrame[s];
        frameId = m_lastRead;
        return true;
    }
    return false;
}

// ============================================================================
// FindImageRect
// ============================================================================
//
// Photos / videos / pictures on a page are bounded by straight lines where
// the page's FLAT background meets BUSY image content; lines inside a photo
// (horizons, buildings, frames) have busy content on both sides. So:
//  1. Edge maps (widened +/-1 px), a "flat pixel" map, and integral images /
//     running totals, so edge coverage, flat fraction and mean luma of any
//     side, strip or rectangle cost O(1).
//  2. Border candidates on each side of the point, in a band around it:
//     "flat outside / busy inside" lines (kept in their own list so a photo's
//     internal clutter can't crowd them out) and straight edge lines.
//  3. Every combination is scored on its four sides' border evidence
//     (max of edge coverage and flat-outside/busy-inside), minus a penalty for
//     flat content. Rejected: mostly-flat content (cards, text, page) and
//     rectangles crossed by a GUTTER -- a flat line in the page background's
//     colour, i.e. the gap between separate images.
//  4. The best-supported wins; among near-ties the larger wins (an SBS
//     picture's centre seam can look like a border -- we want the whole
//     picture). Finally each side snaps to the exact edge line.
// Verified offline against synthetic pages: photos with internal lines,
// SBS pictures, an image inside a card, a gallery, a bright photo on a white
// page, a busy "building" on a dark page, and text-only areas.
bool srw::FindImageRect(const LumaImage& img, int px, int py, const RECT& bounds, RECT& out)
{
    ImageRectFinder f;
    return f.Prepare(img, bounds) && f.Find(px, py, out);
}

bool ImageRectFinder::Prepare(const LumaImage& img, const RECT& boundsIn)
{
    m_img = nullptr;
    const int W = img.width, H = img.height;
    if (img.empty()) return false;
    RECT b = boundsIn;
    b.left = (std::max)(b.left, 0L);   b.top = (std::max)(b.top, 0L);
    b.right = (std::min)(b.right, (LONG)W); b.bottom = (std::min)(b.bottom, (LONG)H);
    const int bw = b.right - b.left, bh = b.bottom - b.top;
    if (bw < kMinRectW || bh < kMinRectH) return false;
    m_b = b; m_bw = bw; m_bh = bh;

    // Everything below works row by row with row pointers, split across CPU
    // cores in bands of rows; the only cross-row steps (column running
    // totals) are simple vector adds.
    const uint8_t* base = &img.pixels[(size_t)b.top * W + b.left];
    auto rowPtr = [&](int y) { return base + (size_t)y * W; };
    const int bands = (std::max)(1, (std::min)(8, bh / 64));
    auto forRows = [&](auto fn) {   // fn(y0, y1) over row bands, in parallel
        ParallelFor(bands, [&](int k) { fn(bh * k / bands, bh * (k + 1) / bands); });
    };

    // Edge maps over the bounds (local coords), widened by 1 px.
    std::vector<uint8_t>& ev = m_ev; std::vector<uint8_t>& eh = m_eh;
    ev.assign((size_t)bw * bh, 0); eh.assign((size_t)bw * bh, 0);
    forRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        {
            const uint8_t* p = rowPtr(y);
            const uint8_t* up = y > 0 ? rowPtr(y - 1) : nullptr;
            uint8_t* v = &ev[(size_t)y * bw];
            uint8_t* hz = &eh[(size_t)y * bw];
            for (int x = 1; x < bw; ++x) v[x] = std::abs((int)p[x] - (int)p[x - 1]) >= kEdgeThreshold;
            if (up) for (int x = 0; x < bw; ++x) hz[x] = std::abs((int)p[x] - (int)up[x]) >= kEdgeThreshold;
        }
    });
    std::vector<uint8_t> evw((size_t)bw * bh), ehw((size_t)bw * bh);
    forRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        {
            const size_t o = (size_t)y * bw;
            for (int x = 0; x < bw; ++x)
            {
                const size_t i = o + x;
                evw[i] = ev[i] | (x > 0 ? ev[i - 1] : 0) | (x < bw - 1 ? ev[i + 1] : 0);
                ehw[i] = eh[i] | (y > 0 ? eh[i - bw] : 0) | (y < bh - 1 ? eh[i + bw] : 0);
            }
        }
    });

    // Running totals: colSum[y][x] = vertical-edge count in column x rows [0,y);
    // rowSum[y][x] = horizontal-edge count in row y columns [0,x).
    std::vector<uint32_t>& colSum = m_colSum; std::vector<uint32_t>& rowSum = m_rowSum;
    colSum.assign((size_t)bw * (bh + 1), 0); rowSum.assign((size_t)bh * (bw + 1), 0);
    for (int y = 0; y < bh; ++y)
    {
        const uint32_t* prev = &colSum[(size_t)y * bw];
        uint32_t* cur = &colSum[(size_t)(y + 1) * bw];
        const uint8_t* e = &evw[(size_t)y * bw];
        for (int x = 0; x < bw; ++x) cur[x] = prev[x] + e[x];
    }
    forRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        {
            uint32_t* rs = &rowSum[(size_t)y * (bw + 1)];
            const uint8_t* e = &ehw[(size_t)y * bw];
            for (int x = 0; x < bw; ++x) rs[x + 1] = rs[x] + e[x];
        }
    });
    // "Flat" = page background: pixels with no luma change right or down, in
    // one of the page's own background colours. Photos and video almost
    // never contain perfectly flat areas; page backgrounds, cards and text
    // blocks are mostly flat. Computer-generated pictures CAN have flat areas
    // (a rendered sky) -- but not in the page's colour, which is why the
    // colour matters. The page colours are the most common flat lumas in the
    // bounds (up to 3, each at least 5% of the flat pixels). An integral
    // image gives any rectangle's flat fraction in O(1).
    std::vector<uint8_t> flatMap((size_t)bw * bh, 0);
    std::vector<std::array<uint32_t, 256>> bandHist((size_t)bands);
    ParallelFor(bands, [&](int k) {
        auto& hist = bandHist[k];
        hist.fill(0);
        for (int y = bh * k / bands; y < bh * (k + 1) / bands; ++y)
        {
            const uint8_t* p = rowPtr(y);
            const uint8_t* dn = y + 1 < bh ? rowPtr(y + 1) : nullptr;
            uint8_t* f = &flatMap[(size_t)y * bw];
            for (int x = 0; x < bw; ++x)
            {
                const int v = p[x];
                if ((x + 1 >= bw || std::abs((int)p[x + 1] - v) <= 1) && (!dn || std::abs((int)dn[x] - v) <= 1))
                {
                    f[x] = 1;
                    ++hist[v];
                }
            }
        }
    });
    uint32_t hist[256] = {};
    uint32_t nFlat = 0;
    for (const auto& h : bandHist) for (int v = 0; v < 256; ++v) { hist[v] += h[v]; nFlat += h[v]; }
    bool (&pageLuma)[256] = m_pageLuma;
    std::fill(std::begin(pageLuma), std::end(pageLuma), false);
    for (int k = 0; k < 3; ++k)
    {
        int best = -1; uint32_t bestN = 0;
        for (int v = 0; v < 256; ++v)
        {
            if (pageLuma[v]) continue;
            uint32_t n = 0;
            for (int d = -2; d <= 2; ++d) if (v + d >= 0 && v + d < 256 && !pageLuma[v + d]) n += hist[v + d];
            if (n > bestN) { bestN = n; best = v; }
        }
        if (best < 0 || bestN * 20 < nFlat) break;
        for (int d = -2; d <= 2; ++d) if (best + d >= 0 && best + d < 256) pageLuma[best + d] = true;
    }
    // Flat-fraction and luma integral images: row running totals in
    // parallel, then the column accumulation.
    std::vector<uint32_t>& flatSum = m_flatSum;
    std::vector<uint32_t>& lumaSum = m_lumaSum;
    flatSum.assign((size_t)(bw + 1) * (bh + 1), 0);
    lumaSum.assign((size_t)(bw + 1) * (bh + 1), 0);
    const size_t W1 = (size_t)bw + 1;
    forRows([&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        {
            const uint8_t* p = rowPtr(y);
            const uint8_t* f = &flatMap[(size_t)y * bw];
            uint32_t* fs = &flatSum[(size_t)(y + 1) * W1];
            uint32_t* ls = &lumaSum[(size_t)(y + 1) * W1];
            uint32_t fa = 0, la = 0;
            for (int x = 0; x < bw; ++x)
            {
                fa += (f[x] && pageLuma[p[x]]) ? 1u : 0u;
                la += p[x];
                fs[x + 1] = fa;
                ls[x + 1] = la;
            }
        }
    });
    for (int y = 1; y <= bh; ++y)
    {
        uint32_t* fs = &flatSum[(size_t)y * W1];  const uint32_t* fp = fs - W1;
        uint32_t* ls = &lumaSum[(size_t)y * W1];  const uint32_t* lp = ls - W1;
        for (size_t x = 0; x < W1; ++x) { fs[x] += fp[x]; ls[x] += lp[x]; }
    }
    m_img = &img;
    return true;
}

float ImageRectFinder::FlatFraction(const RECT& rIn) const
{
    if (!m_img) return 1.0f;
    const int l = (std::max)(0, (int)(rIn.left - m_b.left)),  t = (std::max)(0, (int)(rIn.top - m_b.top));
    const int r = (std::min)(m_bw, (int)(rIn.right - m_b.left)), bt = (std::min)(m_bh, (int)(rIn.bottom - m_b.top));
    if (r <= l || bt <= t) return 1.0f;
    const size_t W1 = (size_t)m_bw + 1;
    const uint32_t s = m_flatSum[(size_t)bt * W1 + r] - m_flatSum[(size_t)t * W1 + r]
                     - m_flatSum[(size_t)bt * W1 + l] + m_flatSum[(size_t)t * W1 + l];
    return (float)s / (float)((long)(r - l) * (bt - t));
}

bool ImageRectFinder::Find(int px, int py, RECT& out, int* flatSidesOut) const
{
    if (!m_img) return false;
    const RECT b = m_b;
    const int bw = m_bw, bh = m_bh;
    if (px <= b.left || px >= b.right - 1 || py <= b.top || py >= b.bottom - 1) return false;
    const std::vector<uint8_t>& ev = m_ev; const std::vector<uint8_t>& eh = m_eh;
    const std::vector<uint32_t>& colSum = m_colSum; const std::vector<uint32_t>& rowSum = m_rowSum;
    const std::vector<uint32_t>& flatSum = m_flatSum; const std::vector<uint32_t>& lumaSum = m_lumaSum;

    auto flatFrac = [&](int l, int t, int r, int bt) -> float {
        const size_t W1 = (size_t)bw + 1;
        const uint32_t s = flatSum[(size_t)bt * W1 + r] - flatSum[(size_t)t * W1 + r]
                         - flatSum[(size_t)bt * W1 + l] + flatSum[(size_t)t * W1 + l];
        return (float)s / (float)((long)(r - l) * (bt - t));
    };

    auto vCov = [&](int x, int y0, int y1) -> float {   // vertical side at column x, rows [y0,y1)
        if (x <= 0 || x >= bw) return 1.0f;              // the bounds count as a full border
        if (y1 <= y0) return 0.0f;
        return (float)(colSum[(size_t)y1 * bw + x] - colSum[(size_t)y0 * bw + x]) / (float)(y1 - y0);
    };
    auto hCov = [&](int y, int x0, int x1) -> float {   // horizontal side at row y, cols [x0,x1)
        if (y <= 0 || y >= bh) return 1.0f;
        if (x1 <= x0) return 0.0f;
        const size_t base = (size_t)y * (bw + 1);
        return (float)(rowSum[base + x1] - rowSum[base + x0]) / (float)(x1 - x0);
    };

    // ---- Border candidates on each side of the point ----------------------
    // A column/row is a candidate if, in a band around the point, it either
    //  (a) carries a mostly-continuous straight edge, or
    //  (b) separates FLAT content (page background) from BUSY content (an
    //      image) -- this catches borders whose colour is close to the page's,
    //      which the edge map misses.
    // Photo-internal lines (buildings, horizons, frames) qualify via (a) too,
    // so we keep many candidates per side and let the scoring below decide.
    const int sx = px - b.left, sy = py - b.top;
    const int by0 = (std::max)(0, sy - kBand), by1 = (std::min)(bh, sy + kBand);
    const int bx0 = (std::max)(0, sx - kBand), bx1 = (std::min)(bw, sx + kBand);
    // Returns 2 = "flat outside / busy inside" border, 1 = edge line, 0 = no.
    auto vKind = [&](int x, bool leftSide) {
        // Strips sit kGap px away from the line: the pixels right at an
        // image's edge are never flat, whichever side they're on.
        const int o0 = leftSide ? x - kGap - kStrip : x + kGap, o1 = o0 + kStrip;   // outside strip
        const int i0 = leftSide ? x + kGap : x - kGap - kStrip, i1 = i0 + kStrip;   // inside strip
        if (o0 >= 0 && o1 <= bw && i0 >= 0 && i1 <= bw &&
            flatFrac(o0, by0, o1, by1) >= kFlatOutside && flatFrac(i0, by0, i1, by1) <= kBusyInside)
            return 2;
        return vCov(x, by0, by1) >= kBandCoverage ? 1 : 0;
    };
    auto hKind = [&](int y, bool topSide) {
        const int o0 = topSide ? y - kGap - kStrip : y + kGap, o1 = o0 + kStrip;
        const int i0 = topSide ? y + kGap : y - kGap - kStrip, i1 = i0 + kStrip;
        if (o0 >= 0 && o1 <= bh && i0 >= 0 && i1 <= bh &&
            flatFrac(bx0, o0, bx1, o1) >= kFlatOutside && flatFrac(bx0, i0, bx1, i1) <= kBusyInside)
            return 2;
        return hCov(y, bx0, bx1) >= kBandCoverage ? 1 : 0;
    };
    // Two lists per side, nearest first: flat-bordered candidates (rare, the
    // strongest sign of a real image edge) and edge-line candidates (common --
    // photos are full of internal lines). Separate caps so internal clutter
    // can't crowd the real border out of the search.
    auto collect = [&](int from, int to, int step, auto kindOf, int boundary) {
        std::vector<int> borders, edges, out;
        for (int v = from; v != to; v += step)
        {
            const int k = kindOf(v);
            if (k == 2 && (int)borders.size() < kMaxBorderCands &&
                (borders.empty() || std::abs(borders.back() - v) > 2)) borders.push_back(v);
            else if (k == 1 && (int)edges.size() < kMaxCandidates &&
                     (edges.empty() || std::abs(edges.back() - v) > 2)) edges.push_back(v);
            if ((int)borders.size() >= kMaxBorderCands && (int)edges.size() >= kMaxCandidates) break;
        }
        out = borders;
        out.insert(out.end(), edges.begin(), edges.end());
        out.push_back(boundary);
        return out;
    };
    const std::vector<int> lefts   = collect(sx - 1, 0,  -1, [&](int x) { return vKind(x, true);  }, 0);
    const std::vector<int> rights  = collect(sx + 1, bw, +1, [&](int x) { return vKind(x, false); }, bw);
    const std::vector<int> tops    = collect(sy - 1, 0,  -1, [&](int y) { return hKind(y, true);  }, 0);
    const std::vector<int> bottoms = collect(sy + 1, bh, +1, [&](int y) { return hKind(y, false); }, bh);

    // Gutters: columns/rows that are flat right across the band around the
    // point -- the gaps between separate images in a gallery. A candidate
    // rectangle that has one running through it is several images, not one.
    std::vector<int> gutterCols, gutterRows;
    for (int x = 1; x < bw - 1; ++x) if (flatFrac(x, by0, x + 1, by1) >= kGutterFlat) gutterCols.push_back(x);
    for (int y = 1; y < bh - 1; ++y) if (flatFrac(bx0, y, bx1, y + 1) >= kGutterFlat) gutterRows.push_back(y);
    // A real gutter IS the page showing between images, so it's the page
    // background's colour. A flat line that's a different colour (a pole in a
    // photo, an SBS picture's divider line) is part of the image.
    auto meanLuma = [&](int l, int t, int r, int bt) -> float {
        if (r <= l || bt <= t) return -1.0f;
        const size_t W1 = (size_t)bw + 1;
        const uint32_t s = lumaSum[(size_t)bt * W1 + r] - lumaSum[(size_t)t * W1 + r]
                         - lumaSum[(size_t)bt * W1 + l] + lumaSum[(size_t)t * W1 + l];
        return (float)s / (float)((long)(r - l) * (bt - t));
    };
    // Background colour just outside the rectangle (the first flat outside strip found).
    auto outsideLuma = [&](int l, int t, int r, int bt) -> float {
        const int g = kGap, s = kStrip;
        if (l - g - s >= 0  && flatFrac(l - g - s, t, l - g, bt) >= kFlatOutside) return meanLuma(l - g - s, t, l - g, bt);
        if (r + g + s <= bw && flatFrac(r + g, t, r + g + s, bt) >= kFlatOutside) return meanLuma(r + g, t, r + g + s, bt);
        if (t - g - s >= 0  && flatFrac(l, t - g - s, r, t - g) >= kFlatOutside) return meanLuma(l, t - g - s, r, t - g);
        if (bt + g + s <= bh && flatFrac(l, bt + g, r, bt + g + s) >= kFlatOutside) return meanLuma(l, bt + g, r, bt + g + s);
        return -1.0f;   // no flat background around it -- can't tell a gutter
    };
    auto hasGutter = [&](int l, int t, int r, int bt) {
        // The page colour just outside, if there's flat page there; otherwise
        // "flat" already means one of the page's own colours (see Prepare).
        const float bg = outsideLuma(l, t, r, bt);
        auto pageColoured = [&](float m) {
            return bg < 0.0f || std::fabs(m - bg) <= kGutterLumaTol || m_pageLuma[(int)(m + 0.5f) & 255];
        };
        for (int x : gutterCols)
            if (x > l + kStrip && x < r - kStrip && flatFrac(x, t, x + 1, bt) >= kGutterFlat &&
                pageColoured(meanLuma(x, t, x + 1, bt))) return true;
        for (int y : gutterRows)
            if (y > t + kStrip && y < bt - kStrip && flatFrac(l, y, r, y + 1) >= kGutterFlat &&
                pageColoured(meanLuma(l, y, r, y + 1))) return true;
        return false;
    };

    // ---- Border evidence for one side of a candidate rectangle -------------
    // max(edge coverage along the side, "flat outside & busy inside"). The
    // second term is what separates a real image border from a line inside a
    // photo: inside a photo both sides of a line are busy.
    auto sideEvidence = [&](float cov, float outFlat, float inFlat) {
        return (std::max)(cov, outFlat * (1.0f - inFlat));
    };
    // Strips kGap px away from the side (see vKind); clamped to the bounds.
    auto clampX = [&](int v) { return (std::max)(0, (std::min)(bw, v)); };
    auto clampY = [&](int v) { return (std::max)(0, (std::min)(bh, v)); };
    auto leftE = [&](int l, int t, int bt) -> float {
        if (l <= 0) return 1.0f;
        const float out = flatFrac(clampX(l - kGap - kStrip), t, clampX(l - kGap), bt);
        const float in  = flatFrac(clampX(l + kGap), t, clampX(l + kGap + kStrip), bt);
        return sideEvidence(vCov(l, t, bt), l - kGap > 0 ? out : 0.0f, in);
    };
    auto rightE = [&](int r, int t, int bt) -> float {
        if (r >= bw) return 1.0f;
        const float out = flatFrac(clampX(r + kGap), t, clampX(r + kGap + kStrip), bt);
        const float in  = flatFrac(clampX(r - kGap - kStrip), t, clampX(r - kGap), bt);
        return sideEvidence(vCov(r, t, bt), r + kGap < bw ? out : 0.0f, in);
    };
    auto topE = [&](int t, int l, int r) -> float {
        if (t <= 0) return 1.0f;
        const float out = flatFrac(l, clampY(t - kGap - kStrip), r, clampY(t - kGap));
        const float in  = flatFrac(l, clampY(t + kGap), r, clampY(t + kGap + kStrip));
        return sideEvidence(hCov(t, l, r), t - kGap > 0 ? out : 0.0f, in);
    };
    auto bottomE = [&](int bt, int l, int r) -> float {
        if (bt >= bh) return 1.0f;
        const float out = flatFrac(l, clampY(bt + kGap), r, clampY(bt + kGap + kStrip));
        const float in  = flatFrac(l, clampY(bt - kGap - kStrip), r, clampY(bt - kGap));
        return sideEvidence(hCov(bt, l, r), bt + kGap < bh ? out : 0.0f, in);
    };

    // Does this side have flat page background outside and busy content
    // inside? (Bounds sides count: an image can fill its window.) An image
    // embedded in a page has this on most sides; a box or seam INSIDE an
    // image (a UI box in a video frame, an SBS/TAB seam) has busy content on
    // both sides.
    auto flatBordered = [&](int side, int l, int t, int r, int bt) -> bool {
        RECT o{}, i{};
        switch (side)
        {
        case 0: if (l <= 0)   return true; o = { l - kGap - kStrip, t, l - kGap, bt };   i = { l + kGap, t, l + kGap + kStrip, bt };   break;
        case 1: if (t <= 0)   return true; o = { l, t - kGap - kStrip, r, t - kGap };    i = { l, t + kGap, r, t + kGap + kStrip };    break;
        case 2: if (r >= bw)  return true; o = { r + kGap, t, r + kGap + kStrip, bt };   i = { r - kGap - kStrip, t, r - kGap, bt };   break;
        default: if (bt >= bh) return true; o = { l, bt + kGap, r, bt + kGap + kStrip }; i = { l, bt - kGap - kStrip, r, bt - kGap }; break;
        }
        if (o.left < 0 || o.top < 0 || o.right > bw || o.bottom > bh) return false;
        return flatFrac(o.left, o.top, o.right, o.bottom) >= kFlatOutside &&
               flatFrac(i.left, i.top, i.right, i.bottom) <= kBusyInside;
    };

    struct Cand { float score; long area; RECT r; int flatSides; };
    std::vector<Cand> cands;
    for (int l : lefts) for (int r : rights)
    {
        if (r - l < kMinRectW) continue;
        for (int t : tops) for (int bt : bottoms)
        {
            if (bt - t < kMinRectH) continue;
            const bool whole = (l == 0 && r == bw && t == 0 && bt == bh);
            if (whole) continue;   // "the whole bounds" isn't an image rect
            const float el = leftE(l, t, bt), er = rightE(r, t, bt);
            const float et = topE(t, l, r),   eb = bottomE(bt, l, r);
            const float mn = (std::min)((std::min)(el, er), (std::min)(et, eb));
            if (mn < kMinCoverage) continue;
            // Mostly-flat content is a card / text block / page, not an image.
            const float flat = flatFrac(l, t, r, bt);
            if (flat > kMaxFlat) continue;
            if (hasGutter(l, t, r, bt)) continue;   // several images, not one
            const float score = 0.6f * mn + 0.4f * (el + er + et + eb) * 0.25f - 1.5f * flat;
            int fs = 0;
            for (int side = 0; side < 4; ++side) fs += flatBordered(side, l, t, r, bt) ? 1 : 0;
            cands.push_back({ score, (long)(r - l) * (bt - t), RECT{ l, t, r, bt }, fs });
        }
    }
    if (cands.empty()) return false;
    // 1) Most flat-bordered sides first (a real embedded image beats a box or
    //    seam inside it); 2) then best-supported; 3) among near-ties
    //    (within 0.04) the largest.
    int bestSides = 0;
    for (const Cand& c : cands) bestSides = (std::max)(bestSides, c.flatSides);
    float top = -1e9f;
    for (const Cand& c : cands) if (c.flatSides == bestSides) top = (std::max)(top, c.score);
    const Cand* pick = nullptr;
    for (const Cand& c : cands)
        if (c.flatSides == bestSides && c.score >= top - 0.04f && (!pick || c.area > pick->area)) pick = &c;
    // The widened edge maps and the kGap strips put each side up to ~3 px off.
    // Snap each side to the offset (within +/-3) where the UNwidened edge is strongest.
    RECT r = pick->r;
    auto rawV = [&](int x, int y0, int y1) {
        if (x <= 0 || x >= bw) return -1;
        int n = 0; for (int y = y0; y < y1; ++y) n += ev[(size_t)y * bw + x];
        return n;
    };
    auto rawH = [&](int y, int x0, int x1) {
        if (y <= 0 || y >= bh) return -1;
        int n = 0; for (int x = x0; x < x1; ++x) n += eh[(size_t)y * bw + x];
        return n;
    };
    auto snapV = [&](LONG& x) {
        int bestX = x, bestN = rawV(x, r.top, r.bottom);
        for (int d : { -1, 1, -2, 2, -3, 3 }) { const int n = rawV(x + d, r.top, r.bottom); if (n > bestN) { bestN = n; bestX = x + d; } }
        x = bestX;
    };
    auto snapH = [&](LONG& y) {
        int bestY = y, bestN = rawH(y, r.left, r.right);
        for (int d : { -1, 1, -2, 2, -3, 3 }) { const int n = rawH(y + d, r.left, r.right); if (n > bestN) { bestN = n; bestY = y + d; } }
        y = bestY;
    };
    snapV(r.left); snapV(r.right); snapH(r.top); snapH(r.bottom);
    out = { r.left + b.left, r.top + b.top, r.right + b.left, r.bottom + b.top };
    if (flatSidesOut) *flatSidesOut = pick->flatSides;
    return true;
}

// ============================================================================
// RegionTracker
// ============================================================================

namespace
{
    constexpr int   kSearchY    = 160;   // analysis px per frame (~320 capture px)
    constexpr int   kSearchX    = 24;
    constexpr int   kSpotGrid   = 16;
    constexpr float kLostError  = 22.0f; // mean |luma diff| on the spot grid
    constexpr float kMinVisible = 0.4f;  // fraction of the region that must stay on screen
}

void RegionTracker::Fingerprint(const LumaImage& img, const RECT& r)
{
    const int w = r.right - r.left, h = r.bottom - r.top;
    m_rowProfile.assign((size_t)h, 0.0f);
    m_colProfile.assign((size_t)w, 0.0f);
    for (int y = 0; y < h; ++y)
    {
        float s = 0; int n = 0;
        for (int x = 0; x < w; x += 2) { s += img.at(r.left + x, r.top + y); ++n; }
        m_rowProfile[y] = s / (float)(std::max)(n, 1);
    }
    for (int x = 0; x < w; ++x)
    {
        float s = 0; int n = 0;
        for (int y = 0; y < h; y += 2) { s += img.at(r.left + x, r.top + y); ++n; }
        m_colProfile[x] = s / (float)(std::max)(n, 1);
    }
    m_spotsX = (std::min)(kSpotGrid, w); m_spotsY = (std::min)(kSpotGrid, h);
    m_spots.resize((size_t)m_spotsX * m_spotsY);
    for (int j = 0; j < m_spotsY; ++j)
        for (int i = 0; i < m_spotsX; ++i)
            m_spots[(size_t)j * m_spotsX + i] =
                img.at(r.left + (i * 2 + 1) * w / (2 * m_spotsX), r.top + (j * 2 + 1) * h / (2 * m_spotsY));
}

namespace
{
    // Flat fraction + mean luma of a small rectangle (strip), clipped to the image.
    bool StripStats(const LumaImage& img, RECT r, float& flat, float& mean)
    {
        r.left = (std::max)(r.left, 0L); r.top = (std::max)(r.top, 0L);
        r.right = (std::min)(r.right, (LONG)img.width - 1); r.bottom = (std::min)(r.bottom, (LONG)img.height - 1);
        if (r.right <= r.left || r.bottom <= r.top) return false;
        long n = 0, nFlat = 0; double sum = 0;
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x)
            {
                const int v = img.at(x, y);
                sum += v; ++n;
                if (std::abs(img.at(x + 1, y) - v) <= 1 && std::abs(img.at(x, y + 1) - v) <= 1) ++nFlat;
            }
        flat = (float)nFlat / (float)n;
        mean = (float)(sum / (double)n);
        return true;
    }
    constexpr int   kSurGap = 2, kSurStrip = 3;
    constexpr float kSurFlat = 0.8f, kSurLumaTol = 6.0f;

    RECT SurroundStrip(const RECT& r, int side)
    {
        switch (side)
        {
        case 0:  return { r.left - kSurGap - kSurStrip, r.top, r.left - kSurGap, r.bottom };      // left
        case 1:  return { r.left, r.top - kSurGap - kSurStrip, r.right, r.top - kSurGap };        // top
        case 2:  return { r.right + kSurGap, r.top, r.right + kSurGap + kSurStrip, r.bottom };    // right
        default: return { r.left, r.bottom + kSurGap, r.right, r.bottom + kSurGap + kSurStrip };  // bottom
        }
    }
}

void RegionTracker::CaptureSurround(const LumaImage& img, const RECT& r)
{
    for (int s = 0; s < 4; ++s)
    {
        float flat = 0, mean = 0;
        m_surValid[s] = StripStats(img, SurroundStrip(r, s), flat, mean) && flat >= kSurFlat;
        m_surLuma[s]  = mean;
    }
}

// Inside strip along one side (kSurGap px in from the edge).
static RECT InsideStrip(const RECT& r, int side)
{
    switch (side)
    {
    case 0:  return { r.left + kSurGap, r.top, r.left + kSurGap + kSurStrip, r.bottom };
    case 1:  return { r.left, r.top + kSurGap, r.right, r.top + kSurGap + kSurStrip };
    case 2:  return { r.right - kSurGap - kSurStrip, r.top, r.right - kSurGap, r.bottom };
    default: return { r.left, r.bottom - kSurGap - kSurStrip, r.right, r.bottom - kSurGap };
    }
}

bool RegionTracker::SurroundStillMatches(const LumaImage& img, const RECT& r) const
{
    int valid = 0, match = 0, busyInside = 0;
    for (int s = 0; s < 4; ++s)
    {
        if (!m_surValid[s]) continue;
        ++valid;
        float flat = 0, mean = 0;
        if (StripStats(img, SurroundStrip(r, s), flat, mean) &&
            flat >= kSurFlat && std::fabs(mean - m_surLuma[s]) <= kSurLumaTol)
            ++match;
        float inFlat = 1, inMean = 0;
        if (StripStats(img, InsideStrip(r, s), inFlat, inMean) && inFlat <= 0.5f)
            ++busyInside;
    }
    // At least two sides of real page background, ALL still exactly as they
    // were (a partly-scrolled image fails this), and content -- not page --
    // still just inside them (a box that became background fails this).
    return valid >= 2 && match == valid && busyInside == valid;
}

void RegionTracker::Reset(const LumaImage& img, const RECT& rect)
{
    m_valid = false;
    if (img.empty()) return;
    RECT r = rect;
    r.left = (std::max)(r.left, 0L); r.top = (std::max)(r.top, 0L);
    r.right = (std::min)(r.right, (LONG)img.width); r.bottom = (std::min)(r.bottom, (LONG)img.height);
    if (r.right - r.left < 8 || r.bottom - r.top < 8) return;
    m_rect = r;
    m_imgW = img.width; m_imgH = img.height;
    m_vx = m_vy = 0.0f;
    Fingerprint(img, r);
    CaptureSurround(img, r);
    RememberRows(img);
    m_valid = true;
}

void RegionTracker::SetViewport(const RECT& outer)
{
    // A differently-sized window has a different layout: forget the insets.
    if ((outer.right - outer.left) != (m_outer.right - m_outer.left) ||
        (outer.bottom - outer.top) != (m_outer.bottom - m_outer.top))
        m_insetTop = m_insetBottom = 0;
    m_outer = outer;
}

RECT RegionTracker::View(const LumaImage& img) const
{
    const RECT all{ 0, 0, img.width, img.height };
    RECT v = all;
    if (!IsRectEmpty(&m_outer) && !IntersectRect(&v, &m_outer, &all)) return RECT{};
    v.top += m_insetTop; v.bottom -= m_insetBottom;
    if (v.bottom < v.top) v.bottom = v.top;
    return v;
}

RECT RegionTracker::Visible() const
{
    LumaImage dims; dims.width = m_imgW; dims.height = m_imgH;
    const RECT v = View(dims);
    RECT out{};
    IntersectRect(&out, &m_rect, &v);
    return out;
}

// Row means of the whole frame over the region's columns (for LearnViewport).
void RegionTracker::RememberRows(const LumaImage& img)
{
    m_prevX0 = (std::max)(0L, m_rect.left);
    m_prevX1 = (std::min)((LONG)img.width, m_rect.right);
    m_prevRows.assign((size_t)img.height, 0.0f);
    if (m_prevX1 - m_prevX0 < 4) { m_prevRows.clear(); return; }
    for (int y = 0; y < img.height; ++y)
    {
        float s = 0; int n = 0;
        for (int x = m_prevX0; x < m_prevX1; x += 3) { s += img.at(x, y); ++n; }
        m_prevRows[y] = s / (float)n;
    }
}

// The image just moved by dy. Rows above / below it that did NOT move with
// the page (identical to last frame, different from where the page content
// went) are a toolbar, sticky header or input box that the page scrolls
// under: the viewport ends there.
void RegionTracker::LearnViewport(const LumaImage& img, int dy)
{
    if (dy == 0 || (int)m_prevRows.size() != img.height) return;
    const int x0 = m_prevX0, x1 = m_prevX1;
    if (x1 - x0 < 4) return;
    std::vector<float> cur((size_t)img.height, 0.0f);
    for (int y = 0; y < img.height; ++y)
    {
        float s = 0; int n = 0;
        for (int x = x0; x < x1; x += 3) { s += img.at(x, y); ++n; }
        cur[y] = s / (float)n;
    }
    // 2 = stayed put while the page moved; 1 = changed (moved with the page);
    // 0 = can't tell (flat rows look the same either way).
    auto cls = [&](int y) -> int {
        const int ys = y - dy;
        const float c = cur[y], s = m_prevRows[y];
        const float m = (ys >= 0 && ys < img.height) ? m_prevRows[ys] : -1000.0f;
        if (std::fabs(c - s) <= 0.5f && std::fabs(c - m) >= 3.0f) return 2;
        if (std::fabs(c - s) >= 3.0f) return 1;
        return 0;
    };
    const RECT all{ 0, 0, img.width, img.height };
    RECT outer = all;
    if (!IsRectEmpty(&m_outer) && !IntersectRect(&outer, &m_outer, &all)) return;
    const int oh = outer.bottom - outer.top;
    const int maxInset = oh * 2 / 5;
    const int mid = (std::max)((int)outer.top, (std::min)((int)outer.bottom - 1,
                               (int)(m_rect.top + m_rect.bottom) / 2));
    // A header / toolbar is a solid block of rows that stay put, reaching the
    // viewport's edge. Two still rows alone happen by chance inside scrolling
    // content (flat backgrounds, repeated lines) -- taking those clipped a
    // picture at a random row -- so a candidate only counts when no row
    // between it and the edge moved with the page.
    auto stillToEdge = [&](int from, int to) {   // [from, to) has no moving row
        for (int y = from; y < to; ++y) if (cls(y) == 1) return false;
        return true;
    };
    // Upwards from the image's middle to the first run of 2 static rows.
    {
        int first = -1, run = 0, lastMoving = mid;
        for (int y = mid; y >= outer.top; --y)
        {
            const int k = cls(y);
            if (k == 2) { if (run++ == 0) first = y; if (run >= 2) break; }
            else if (k == 1) { run = 0; first = -1; lastMoving = y; }
        }
        if (run >= 2 && first + 1 - outer.top <= maxInset && stillToEdge(outer.top, first))
            m_insetTop = first + 1 - outer.top;
        else if (run < 2 && lastMoving - outer.top <= 4) m_insetTop = 0;   // page moves right up to the top
    }
    // Downwards likewise.
    {
        int first = -1, run = 0, lastMoving = mid;
        for (int y = mid; y < outer.bottom; ++y)
        {
            const int k = cls(y);
            if (k == 2) { if (run++ == 0) first = y; if (run >= 2) break; }
            else if (k == 1) { run = 0; first = -1; lastMoving = y; }
        }
        if (run >= 2 && outer.bottom - first <= maxInset && stillToEdge(first + 1, outer.bottom))
            m_insetBottom = outer.bottom - first;
        else if (run < 2 && outer.bottom - 1 - lastMoving <= 4) m_insetBottom = 0;
    }
}

// Mean |diff| of the spot grid at offset (dx,dy); spots outside the viewport
// are skipped (the region may be partly scrolled off / under a toolbar).
float RegionTracker::SpotCheck(const LumaImage& img, int dx, int dy) const
{
    const int w = m_rect.right - m_rect.left, h = m_rect.bottom - m_rect.top;
    const RECT v = View(img);
    float s = 0; int n = 0;
    for (int j = 0; j < m_spotsY; ++j)
        for (int i = 0; i < m_spotsX; ++i)
        {
            const int x = m_rect.left + dx + (i * 2 + 1) * w / (2 * m_spotsX);
            const int y = m_rect.top  + dy + (j * 2 + 1) * h / (2 * m_spotsY);
            if (x < v.left || y < v.top || x >= v.right || y >= v.bottom) continue;
            s += std::fabs((float)img.at(x, y) - (float)m_spots[(size_t)j * m_spotsX + i]);
            ++n;
        }
    return n >= (m_spotsX * m_spotsY) / 3 ? s / (float)n : 1e9f;
}

bool RegionTracker::Track(const LumaImage& img)
{
    if (!m_valid || img.empty()) return false;
    const int w = m_rect.right - m_rect.left, h = m_rect.bottom - m_rect.top;
    if (img.width != m_imgW || img.height != m_imgH)
    {
        m_imgW = img.width; m_imgH = img.height;
        m_prevRows.clear();
    }
    const RECT view = View(img);
    auto inside = [&](const RECT& r) {
        return r.left >= view.left && r.top >= view.top && r.right <= view.right && r.bottom <= view.bottom;
    };

    // Unmoved? (the common case) -- cheap exit.
    int bestDx = 0, bestDy = 0;
    float err = SpotCheck(img, 0, 0);
    // Content changed but the box and the page around it haven't moved: a
    // playing video / changing picture. Stay put, re-learn the content.
    // Checked BEFORE searching: changed content can otherwise produce a false
    // match at a wrong offset.
    if (err > 3.0f && SurroundStillMatches(img, m_rect))
    {
        if (inside(m_rect)) Fingerprint(img, m_rect);
        m_vx *= 0.3f; m_vy *= 0.3f;   // not moving
        RememberRows(img);
        return true;
    }
    if (err > 3.0f)
    {
        // 1) Vertical: row profile of the new frame over the region's columns.
        //    Each frame row's mean is computed once, then every offset is
        //    just a comparison of two lists.
        const int x0 = (std::max)(view.left, m_rect.left), x1 = (std::min)(view.right, m_rect.right);
        std::vector<float> rowMean((size_t)img.height, 0.0f);
        if (x1 > x0)
            for (int yy = (std::max)(0L, view.top); yy < (std::min)((LONG)img.height, view.bottom); ++yy)
            {
                float m = 0; int c = 0;
                for (int x = x0; x < x1; x += 8) { m += img.at(x, yy); ++c; }
                rowMean[yy] = m / (float)(std::max)(c, 1);
            }
        float bestV = 1e9f;
        for (int dy = -kSearchY; dy <= kSearchY; ++dy)
        {
            float s = 0; int n = 0;
            for (int y = 0; y < h; y += 2)
            {
                const int yy = m_rect.top + dy + y;
                if (yy < view.top || yy >= view.bottom) continue;
                s += std::fabs(rowMean[yy] - m_rowProfile[y]);
                ++n;
            }
            if (n < (h / 2) * kMinVisible) continue;
            const float v = s / (float)n;
            if (v < bestV) { bestV = v; bestDy = dy; }
        }
        // 2) Horizontal (small): column profile at the new vertical position,
        //    column means likewise computed once.
        const int cx0 = (std::max)((int)view.left, (int)m_rect.left - kSearchX);
        const int cx1 = (std::min)((int)view.right, (int)m_rect.right + kSearchX);
        std::vector<float> colMean((size_t)(std::max)(0, cx1 - cx0), -1.0f);
        for (int xx = cx0; xx < cx1; ++xx)
        {
            float m = 0; int c = 0;
            for (int y = 0; y < h; y += 8)
            {
                const int yy = m_rect.top + bestDy + y;
                if (yy < view.top || yy >= view.bottom) continue;
                m += img.at(xx, yy); ++c;
            }
            if (c) colMean[xx - cx0] = m / (float)c;
        }
        float bestH = 1e9f;
        for (int dx = -kSearchX; dx <= kSearchX; ++dx)
        {
            float s = 0; int n = 0;
            for (int x = 0; x < w; x += 2)
            {
                const int xx = m_rect.left + dx + x;
                if (xx < cx0 || xx >= cx1 || colMean[xx - cx0] < 0.0f) continue;
                s += std::fabs(colMean[xx - cx0] - m_colProfile[x]);
                ++n;
            }
            if (n < (w / 2) * kMinVisible) continue;
            const float v = s / (float)n;
            if (v < bestH) { bestH = v; bestDx = dx; }
        }
        // 3) Verify.
        err = SpotCheck(img, bestDx, bestDy);
    }
    if (err > kLostError) { m_valid = false; return false; }

    RECT moved = { m_rect.left + bestDx, m_rect.top + bestDy, m_rect.right + bestDx, m_rect.bottom + bestDy };
    RECT onScreen{};
    if (!IntersectRect(&onScreen, &moved, &view) ||
        (float)((onScreen.right - onScreen.left) * (onScreen.bottom - onScreen.top)) <
            kMinVisible * (float)(w * h))
    {
        m_valid = false;
        return false;
    }
    m_rect = moved;
    // Smoothed scroll speed (for drawing the picture where it will be by
    // the time it's on screen). Stops quickly: no shift = speed fades.
    m_vx = 0.6f * (float)bestDx + 0.4f * m_vx;
    m_vy = 0.6f * (float)bestDy + 0.4f * m_vy;
    if (bestDx == 0 && bestDy != 0) LearnViewport(img, bestDy);
    // Re-learn the content at its new position when it's fully on show, so
    // slowly changing content (fades, animation) doesn't drift out of match.
    // (Never with part of it under a toolbar: that would learn the toolbar.)
    if (inside(moved) && [&] { const RECT v2 = View(img); return moved.top >= v2.top && moved.bottom <= v2.bottom; }())
        Fingerprint(img, moved);
    RememberRows(img);
    return true;
}

// ============================================================================
// Debug BMP
// ============================================================================

bool srw::SaveLumaDebugBmp(const LumaImage& img, const RECT& bounds, const RECT* found,
                           POINT pick, const wchar_t* path, bool annotate)
{
    if (img.empty() || !path) return false;
    const int W = img.width, H = img.height;
    std::vector<uint8_t> px(img.pixels);
    auto put = [&](int x, int y, uint8_t v) { if (x >= 0 && y >= 0 && x < W && y < H) px[(size_t)y * W + x] = v; };
    auto box = [&](const RECT& r, uint8_t v, int thick) {
        for (int t = 0; t < thick; ++t)
        {
            for (int x = r.left; x < r.right; ++x) { put(x, r.top + t, v); put(x, r.bottom - 1 - t, v); }
            for (int y = r.top; y < r.bottom; ++y) { put(r.left + t, y, v); put(r.right - 1 - t, y, v); }
        }
    };
    if (annotate)
    {
        box(bounds, 128, 1);                       // search bounds: grey
        if (found) { box(*found, 255, 2); RECT in{ found->left + 2, found->top + 2, found->right - 2, found->bottom - 2 }; box(in, 0, 1); }
        for (int d = -8; d <= 8; ++d) { put(pick.x + d, pick.y, 255); put(pick.x, pick.y + d, 255); }   // pick: white cross
    }

    // 8-bit indexed BMP, grey palette, bottom-up rows padded to 4 bytes.
    const int rowBytes = (W + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD paletteBytes = 256 * 4;
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih) + paletteBytes;
    fh.bfSize    = fh.bfOffBits + (DWORD)rowBytes * H;
    ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = H; ih.biPlanes = 1;
    ih.biBitCount = 8; ih.biCompression = BI_RGB; ih.biSizeImage = (DWORD)rowBytes * H;
    ih.biClrUsed = 256;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int i = 0; i < 256; ++i) { const uint8_t e[4] = { (uint8_t)i, (uint8_t)i, (uint8_t)i, 0 }; fwrite(e, 4, 1, f); }
    std::vector<uint8_t> row((size_t)rowBytes, 0);
    for (int y = H - 1; y >= 0; --y)
    {
        std::memcpy(row.data(), &px[(size_t)y * W], (size_t)W);
        fwrite(row.data(), 1, (size_t)rowBytes, f);
    }
    fclose(f);
    return true;
}

bool srw::SaveColourDebugBmp(const LumaImage& img, const wchar_t* path)
{
    if (!img.hasColour() || !path) return false;
    const int W = img.width, H = img.height;
    const int rowBytes = (W * 3 + 3) & ~3;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + (DWORD)rowBytes * H;
    ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = H; ih.biPlanes = 1;
    ih.biBitCount = 24; ih.biCompression = BI_RGB; ih.biSizeImage = (DWORD)rowBytes * H;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row((size_t)rowBytes, 0);
    for (int y = H - 1; y >= 0; --y)
    {
        for (int x = 0; x < W; ++x)
        {
            const size_t i = (size_t)y * W + x;
            row[x * 3 + 0] = img.blue.empty() ? img.cyan[i] : img.blue[i];     // B
            row[x * 3 + 1] = img.green.empty() ? img.cyan[i] : img.green[i];   // G
            row[x * 3 + 2] = img.red[i];    // R
        }
        fwrite(row.data(), 1, (size_t)rowBytes, f);
    }
    fclose(f);
    return true;
}

// ============================================================================
// ClassifyStereo
// ============================================================================
//
// Side-by-side / top-and-bottom: the two halves are the SAME scene seen from
// two slightly different places -- after ONE global horizontal shift (the
// average disparity) they match closely, detail for detail. Compared on
// high-passed block means (block minus its 3x3-block surround), so smooth
// gradients (sky, vignettes) can't fake a match and small local disparity
// differences don't break one. Rejected: identical halves (two copies of the
// same thumbnail are not a stereo pair) and repetitive textures (the halves
// match at a clearly wrong offset too).
//
// Anaglyph: red carries one eye, cyan the other. The two eyes differ only by
// HORIZONTAL shifts, so the red-minus-cyan difference changes across
// vertical edges (colour fringes left/right of objects) far more than across
// horizontal ones; in an ordinary colour photo, colour changes with the
// scene in every direction about as much as brightness does. Measured on the
// analysis image (already half resolution, which averages away ClearType's
// sub-pixel colour fringes).
namespace
{
    struct Integral
    {
        int w = 0, h = 0;
        std::vector<uint32_t> s;
        template <class F> void Build(int w_, int h_, F value)
        {
            w = w_; h = h_;
            s.assign((size_t)(w + 1) * (h + 1), 0);
            for (int y = 0; y < h; ++y)
            {
                uint32_t acc = 0;
                for (int x = 0; x < w; ++x)
                {
                    acc += (uint32_t)value(x, y);
                    s[(size_t)(y + 1) * (w + 1) + x + 1] = s[(size_t)y * (w + 1) + x + 1] + acc;
                }
            }
        }
        // Mean over [x, x+bw) x [y, y+bh), clamped.
        float Mean(int x, int y, int bw, int bh) const
        {
            const int x0 = (std::max)(0, x), y0 = (std::max)(0, y);
            const int x1 = (std::min)(w, x + bw), y1 = (std::min)(h, y + bh);
            if (x1 <= x0 || y1 <= y0) return 0.0f;
            const size_t W1 = (size_t)w + 1;
            const uint32_t v = s[(size_t)y1 * W1 + x1] - s[(size_t)y0 * W1 + x1]
                             - s[(size_t)y1 * W1 + x0] + s[(size_t)y0 * W1 + x0];
            return (float)v / (float)((x1 - x0) * (y1 - y0));
        }
        // High-passed block value at (x,y): B-block mean minus 3B surround mean.
        float HP(int x, int y, int B) const { return Mean(x, y, B, B) - Mean(x - B, y - B, 3 * B, 3 * B); }
    };

    // I.HP(x, y, B) for every pixel of the picture, computed once per block
    // size -- the shift search then only reads it.
    struct HPMap
    {
        int w = 0, h = 0;
        std::vector<float> v;
        void Build(const Integral& I, int B)
        {
            w = I.w; h = I.h;
            v.resize((size_t)w * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) v[(size_t)y * w + x] = I.HP(x, y, B);
        }
        float at(int x, int y) const
        {
            x = (std::max)(0, (std::min)(w - 1, x)); y = (std::max)(0, (std::min)(h - 1, y));
            return v[(size_t)y * w + x];
        }
    };

    // Correlation of high-passed blocks of half A (origin ax,ay) with half B
    // (origin bx,by) shifted by (dx,dy), over an sw x sh half. Optionally
    // returns half A's high-pass standard deviation.
    float PairNcc(const HPMap& M, int ax, int ay, int bx, int by, int sw, int sh,
                  int B, int margin, int dx, int dy, float* stdA = nullptr)
    {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int n = 0;
        for (int y = margin; y + B + margin <= sh; y += B)
            for (int x = margin; x + B + margin <= sw; x += B)
            {
                const float a = M.at(ax + x, ay + y);
                const float b = M.at(bx + x + dx, by + y + dy);
                sa += a; sb += b; saa += (double)a * a; sbb += (double)b * b; sab += (double)a * b; ++n;
            }
        if (n < 64) return 0.0f;
        const double ma = sa / n, mb = sb / n;
        const double va = saa / n - ma * ma, vb = sbb / n - mb * mb;
        if (stdA) *stdA = (float)std::sqrt((std::max)(va, 0.0));
        if (va <= 1e-6 || vb <= 1e-6) return 0.0f;
        return (float)((sab / n - ma * mb) / std::sqrt(va * vb));
    }

    struct PairResult { float ncc = 0, baseline = 0, stdA = 0, madBest = 0, madWrong = 0; int dx = 0, dy = 0; };
    constexpr int kPairMinEyeW = 80, kPairMinEyeH = 60;   // analysis px per eye (160x120 on screen)

    // Best global-shift match of two halves; horizontal shift up to 6% of a half.
    PairResult MatchHalves(const LumaImage& img, const RECT& r, const Integral& I, HPMap& M, int& mapB,
                           int ax, int ay, int bx, int by, int sw, int sh)
    {
        PairResult pr;
        const int B = (std::max)(2, (std::min)(sw, sh) / 32);
        const int maxD = (std::max)(2, sw * 6 / 100);
        const int margin = maxD + 2 * B;
        // Each eye must be big enough to judge (and to be worth weaving):
        // thumbnails are too small for a trustworthy half-vs-half match.
        if (sw < kPairMinEyeW || sh < kPairMinEyeH) return pr;
        if (sw <= 2 * margin + 8 * B || sh <= 2 * margin + 8 * B) return pr;
        if (mapB != B) { M.Build(I, B); mapB = B; }   // (TAB often reuses SBS's)
        // Coarse (half-block steps), then refine to 1 px.
        const int cstep = (std::max)(1, B / 2);
        float best = -2.0f; int bdx = 0, bdy = 0;
        for (int dy = -B; dy <= B; dy += B)
            for (int dx = -maxD; dx <= maxD; dx += cstep)
            {
                const float v = PairNcc(M, ax, ay, bx, by, sw, sh, B, margin, dx, dy);
                if (v > best) { best = v; bdx = dx; bdy = dy; }
            }
        const int cx = bdx, cy = bdy;
        for (int dy = cy - 1; dy <= cy + 1; ++dy)
            for (int dx = cx - cstep; dx <= cx + cstep; ++dx)
            {
                if (std::abs(dx) > maxD) continue;
                const float v = PairNcc(M, ax, ay, bx, by, sw, sh, B, margin, dx, dy);
                if (v > best) { best = v; bdx = dx; bdy = dy; }
            }
        pr.ncc = best; pr.dx = bdx; pr.dy = bdy;
        PairNcc(M, ax, ay, bx, by, sw, sh, B, margin, bdx, bdy, &pr.stdA);
        // Baseline: the same comparison at a clearly wrong (vertical) offset.
        // A stereo pair doesn't match there; a repetitive texture does.
        const int off = (std::max)(3 * B, sh / 6);
        pr.baseline = (std::max)(PairNcc(M, ax, ay, bx, by + off, sw, sh - off, B, margin, bdx, 0),
                                 PairNcc(M, ax, ay + off, bx, by, sw, sh - off, B, margin, bdx, 0));
        // Pixel-level residual at the best shift vs at the wrong offset. Two
        // copies of one picture, or a repeating pattern, match (almost)
        // exactly; a real stereo pair can't -- depth makes near and far
        // things shift by different amounts.
        auto mad = [&](int dx, int dy, int offY) {
            double s = 0; int n = 0;
            for (int y = margin; y + margin + offY < sh; y += 2)
                for (int x = margin; x + margin < sw; x += 2)
                {
                    s += std::abs((int)img.at(r.left + ax + x, r.top + ay + y) -
                                  (int)img.at(r.left + bx + x + dx, r.top + by + y + dy + offY));
                    ++n;
                }
            return n ? (float)(s / n) : 0.0f;
        };
        pr.madBest  = mad(bdx, bdy, 0);
        pr.madWrong = mad(bdx, 0, off);
        return pr;
    }

    // Eye order of an SBS / TAB pair whose halves matched (pr), A = the half
    // taken as the left eye. Two cues, both reversed when the halves are the
    // wrong way round:
    //  - stereo pictures lie mostly behind the screen, so the right eye sees
    //    things further right than the left eye (B shifted right of A), and
    //  - lower in the picture is nearer than higher up (the ground), so the
    //    shift is smaller at the bottom than at the top.
    // Called swapped only when the evidence says so and none says otherwise.
    EyeOrder JudgeEyeOrder(const Integral& I, HPMap& M, int& mapB, const PairResult& pr,
                           int ax, int ay, int bx, int by, int sw, int sh)
    {
        EyeOrder eo;
        const int B = (std::max)(2, (std::min)(sw, sh) / 32);
        const int maxD = (std::max)(2, sw * 6 / 100);
        const int mx = maxD + 2 * B;
        if (sw <= 2 * mx + 8 * B || sh < 12 * B) return eo;
        if (mapB != B) { M.Build(I, B); mapB = B; }
        // Match strength of rows [y0, y1) of A against B shifted by dx (and
        // the pair's vertical offset).
        auto ncc = [&](int y0, int y1, int dx) -> float {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int n = 0;
            for (int y = y0; y + B <= y1; y += B)
            {
                const int yb = y + pr.dy;
                if (yb < 0 || yb + B > sh) continue;
                for (int x = mx; x + B + mx <= sw; x += B)
                {
                    const float a = M.at(ax + x, ay + y), b = M.at(bx + x + dx, by + yb);
                    sa += a; sb += b; saa += (double)a * a; sbb += (double)b * b; sab += (double)a * b; ++n;
                }
            }
            if (n < 32) return -2.0f;
            const double ma = sa / n, mb = sb / n, va = saa / n - ma * ma, vb = sbb / n - mb * mb;
            if (va <= 1e-6 || vb <= 1e-6) return -2.0f;
            return (float)((sab / n - ma * mb) / std::sqrt(va * vb));
        };
        auto bestDx = [&](int y0, int y1, int& dxOut) {
            float best = -2.0f;
            for (int dx = -maxD; dx <= maxD; ++dx)
            {
                const float v = ncc(y0, y1, dx);
                if (v > best) { best = v; dxOut = dx; }
            }
            return best;
        };
        const int band = sh / 3;
        int dt = 0, db = 0;
        const float ct = bestDx(B, band, dt);
        const float cb = bestDx(sh - band, sh - B, db);
        eo.dxAll = pr.dx; eo.dxTop = dt; eo.dxBottom = db;
        int pos = 0, neg = 0;
        if (pr.dx >= 2) ++pos; else if (pr.dx <= -2) ++neg;
        if (ct > 0.3f && cb > 0.3f)
        {
            const int g = dt - db;
            if (g >= 2) ++pos; else if (g <= -2) ++neg;
        }
        eo.known = (pos + neg) > 0 && !(pos && neg);
        eo.swap  = eo.known && neg > 0;
        return eo;
    }

    constexpr float kPairNcc      = 0.35f;  // halves must match at least this well ...
    constexpr float kPairMargin   = 0.25f;  // ... and this much better than at a wrong offset
    constexpr float kPairMinStd   = 1.2f;   // enough detail to judge
    constexpr float kRepeatRatio  = 0.12f;  // residual below this x wrong-offset residual = a copy / repeat, not a pair
    constexpr int   kAnaMinBlocks     = 8;     // detailed blocks needed to judge an anaglyph
    constexpr float kAnaMinChroma     = 4.0f;  // mean |red - cyan| below this = grey content
    constexpr float kAnaMaxAligned    = 0.70f; // red/cyan edges line up worse than this unshifted ...
    constexpr float kAnaShiftGain     = 0.20f; // ... a horizontal shift improves that by this much ...
    constexpr float kAnaHorizOverVert = 0.15f; // ... and by this much more than a vertical shift (same range) does
    constexpr float kAnaColourMaxAligned = 0.35f; // (AnaSplitByColour) red/cyan edges line up this badly or worse ...
    constexpr float kAnaColourShiftGain  = 0.08f; // ... a horizontal shift helps at least this much ...
    constexpr float kAnaColourAxis       = 2.0f;  // ... and the colour lies this much more along the filters' axis than across it

    // Anaglyph signature of two colour planes A and B over picture r (w x h):
    // mean block |correlation| of their horizontal gradients unshifted (a0),
    // at the best horizontal shift (aH) and at the best vertical shift (aV,
    // the control). Two planes showing the SAME eye line up unshifted; two
    // eyes line up only after a horizontal shift. Gradients over 2 px of a
    // 2-row sum: steadier than 1-px steps on JPEG'd, detailed pictures.
    // (+ which way the second plane is shifted against the first, where a block
    // clearly matched better shifted: median over all / the top third / the
    // bottom third of the blocks. Positive: the second plane sees it further right.)
    struct AnaSig { float a0 = 0.0f, aH = 0.0f, aV = 0.0f; int blocks = 0; float medDx = 0, topDx = 0, botDx = 0; int nSigned = 0, nTop = 0, nBot = 0; };
    AnaSig AnaSignature(const uint8_t* A, const uint8_t* B, size_t W, const RECT& r, int w, int h)
    {
        constexpr int kBlk = 32;
        const int maxD = (std::max)(8, (std::min)(32, w * 5 / 100));
        const int gw = w, gh = h;
        std::vector<float> GR((size_t)gw * gh, 0.0f), GC((size_t)gw * gh, 0.0f);
        for (int y = 0; y < gh - 1; ++y)
        {
            const size_t row = (size_t)(r.top + y) * W + r.left;
            const uint8_t* r0 = &A[row]; const uint8_t* r1 = r0 + W;
            const uint8_t* c0 = &B[row]; const uint8_t* c1 = c0 + W;
            float* gr = &GR[(size_t)y * gw];
            float* gc = &GC[(size_t)y * gw];
            for (int x = 1; x < gw - 2; ++x)
            {
                gr[x] = (float)(r0[x + 2] + r1[x + 2] + r0[x + 1] + r1[x + 1]) - (float)(r0[x] + r1[x] + r0[x - 1] + r1[x - 1]);
                gc[x] = (float)(c0[x + 2] + c1[x + 2] + c0[x + 1] + c1[x + 1]) - (float)(c0[x] + c1[x] + c0[x - 1] + c1[x - 1]);
            }
        }
        // Block origin (bx,by) in picture coords; A's block stats fixed per block.
        struct BlockA { double ma, va; };
        auto corr = [&](int bx, int by, const BlockA& Ab, int dx, int dy) {
            double sb = 0, sbb = 0, sab = 0; int n = 0;
            for (int y = by; y < by + kBlk; y += 2)
            {
                const float* a = &GR[(size_t)y * gw];
                const float* b = &GC[(size_t)(y + dy) * gw + dx];
                for (int x = bx; x < bx + kBlk; x += 2)
                {
                    const float bv = b[x];
                    sb += bv; sbb += bv * bv; sab += a[x] * bv; ++n;
                }
            }
            const double mb = sb / n, vb = sbb / n - mb * mb;
            if (vb < 16.0) return -1.0f;
            return (float)std::fabs((sab / n - Ab.ma * mb) / std::sqrt(Ab.va * vb));
        };
        auto bestAlong = [&](int bx, int by, const BlockA& Ab, bool horiz, float c0v, int* bdOut = nullptr) {
            float best = c0v; int bd = 0;
            for (int d = -maxD; d <= maxD; d += 2)
            {
                if (d == 0) continue;
                const float v = horiz ? corr(bx, by, Ab, d, 0) : corr(bx, by, Ab, 0, d);
                if (v > best) { best = v; bd = d; }
            }
            const int coarse = bd;
            for (int d = coarse - 1; d <= coarse + 1; d += 2)
            {
                if (d == 0 || std::abs(d) > maxD) continue;
                const float v = horiz ? corr(bx, by, Ab, d, 0) : corr(bx, by, Ab, 0, d);
                if (v > best) { best = v; bd = d; }
            }
            if (bdOut) *bdOut = bd;
            return best;
        };
        AnaSig s;
        double s0 = 0, sH = 0, sV = 0;
        std::vector<int> allD, topD, botD;   // signed horizontal shifts of clearly-shifted blocks
        const int m = maxD + 3;
        for (int by = m; by + kBlk + m + 1 < gh; by += kBlk)
            for (int bx = m; bx + kBlk + m + 2 < gw; bx += kBlk)
            {
                double sa = 0, saa = 0; int n = 0;
                for (int y = by; y < by + kBlk; y += 2)
                    for (int x = bx; x < bx + kBlk; x += 2) { const float a = GR[(size_t)y * gw + x]; sa += a; saa += a * a; ++n; }
                BlockA Ab{ sa / n, saa / n - (sa / n) * (sa / n) };
                if (Ab.va < 16.0) continue;   // too little detail to judge
                const float c0v = corr(bx, by, Ab, 0, 0);
                if (c0v < 0.0f) continue;
                s0 += c0v;
                int bd = 0;
                const float bh = bestAlong(bx, by, Ab, true, c0v, &bd);
                sH += bh;
                sV += bestAlong(bx, by, Ab, false, c0v);
                ++s.blocks;
                if (bd != 0 && bh - c0v >= 0.05f)
                {
                    allD.push_back(bd);
                    if (by < gh / 3)          topD.push_back(bd);
                    else if (by >= gh * 2 / 3) botD.push_back(bd);
                }
            }
        if (s.blocks > 0) { s.a0 = (float)(s0 / s.blocks); s.aH = (float)(sH / s.blocks); s.aV = (float)(sV / s.blocks); }
        auto median = [](std::vector<int>& v) -> float {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return (float)v[v.size() / 2];
        };
        s.nSigned = (int)allD.size(); s.nTop = (int)topD.size(); s.nBot = (int)botD.size();
        s.medDx = median(allD); s.topDx = median(topD); s.botDx = median(botD);
        return s;
    }
    // Row / column interleaved or checkerboard, from the full-resolution
    // tile statistics (LumaImage::il) inside picture r (analysis px). Tiles
    // with next to no detail are skipped (flat areas look the same either
    // way). Only a picture shown pixel for pixel keeps its interleaving.
    constexpr int   kIlMinTiles  = 12;     // detailed tiles needed to judge
    constexpr float kIlMinDetail = 0.015f; // mean 2-px difference below this: flat tile
    constexpr float kIlRatio     = 1.25f;  // 1-px difference beats the 2-px one by this much
    bool DetectInterleave(const LumaImage& img, const RECT& r, StereoFormat& fmt, char* note, size_t noteLen)
    {
        if (img.il.empty() || img.ilW <= 0) return false;
        const int s = ScreenAnalyzer::Scale(), T = LumaImage::kIlTile;
        // Whole tiles inside the picture.
        const int tx0 = (std::max)(0, (int)((r.left * s + T - 1) / T)), ty0 = (std::max)(0, (int)((r.top * s + T - 1) / T));
        const int tx1 = (std::min)(img.ilW, (int)(r.right * s / T)),     ty1 = (std::min)(img.ilH, (int)(r.bottom * s / T));
        double v1 = 0, v2 = 0, h1 = 0, h2 = 0, d1 = 0; int n = 0;
        for (int ty = ty0; ty < ty1; ++ty)
            for (int tx = tx0; tx < tx1; ++tx)
            {
                const float* t = &img.il[((size_t)ty * img.ilW + tx) * 5];
                if (t[1] + t[3] < 2.0f * kIlMinDetail) continue;
                v1 += t[0]; v2 += t[1]; h1 += t[2]; h2 += t[3]; d1 += t[4]; ++n;
            }
        if (n < kIlMinTiles) return false;
        if (note && noteLen)
            snprintf(note, noteLen, "IL tiles=%d v1/v2=%.2f h1/h2=%.2f d1/min=%.2f", n,
                     v1 / (std::max)(v2, 1e-6), h1 / (std::max)(h2, 1e-6), d1 / (std::max)((std::min)(h1, v1), 1e-6));
        const bool rowsSplit = v1 > kIlRatio * v2;   // adjacent rows: the other eye
        const bool colsSplit = h1 > kIlRatio * h2;   // adjacent columns: the other eye
        if (rowsSplit && colsSplit && d1 < 0.85 * (std::min)(h1, v1)) { fmt = StereoFormat::Checkerboard; return true; }
        if (rowsSplit && !colsSplit) { fmt = StereoFormat::RowInterleaved; return true; }
        if (colsSplit && !rowsSplit) { fmt = StereoFormat::ColumnInterleaved; return true; }
        return false;
    }

    // Eye order of an anaglyph from its signature, the first plane taken as
    // the left eye's filter (red for red/cyan): the same two cues as SBS
    // (JudgeEyeOrder) -- the second eye's view shifted right (behind the
    // screen), and less so lower down (nearer). `invert`: the first plane is
    // really the right eye's.
    EyeOrder AnaEyeOrder(const AnaSig& s, bool invert)
    {
        EyeOrder eo;
        eo.dxAll = (int)s.medDx; eo.dxTop = (int)s.topDx; eo.dxBottom = (int)s.botDx;
        int pos = 0, neg = 0;
        if (s.nSigned >= 6) { if (s.medDx >= 2) ++pos; else if (s.medDx <= -2) ++neg; }
        if (s.nTop >= 3 && s.nBot >= 3)
        {
            const float g = s.topDx - s.botDx;
            if (g >= 2) ++pos; else if (g <= -2) ++neg;
        }
        eo.known = (pos + neg) > 0 && !(pos && neg);
        eo.swap  = eo.known && (neg > 0) != invert;
        return eo;
    }

    bool AnaSplit(const AnaSig& s)   // the two planes are two different eyes
    {
        return s.blocks >= kAnaMinBlocks && s.a0 <= kAnaMaxAligned &&
               s.aH - s.a0 >= kAnaShiftGain && s.aH - s.aV >= kAnaHorizOverVert;
    }

    // A second way to recognise an anaglyph (after the cues in Gallagher's
    // anaglyph detector, Kodak US8384774 -- expired, and its claims only cover
    // viewing glasses). A picture with big plain colour areas and little fine
    // detail (floating cubes, flat CG) has red and cyan edges that clearly do NOT
    // line up, yet a horizontal shift only helps a little -- too little for
    // AnaSplit. Its colour then settles it: an anaglyph's colour lies along the
    // two filters' axis (red vs cyan) far more than across it (green vs blue),
    // while an ordinary photo whose red and cyan edges line up this badly
    // doesn't exist (every 2D photo tested lines up at 0.77+).
    bool AnaSplitByColour(const AnaSig& s, float axis)
    {
        return s.blocks >= kAnaMinBlocks && s.a0 <= kAnaColourMaxAligned &&
               s.aH - s.a0 >= kAnaColourShiftGain && s.aH >= s.aV && axis >= kAnaColourAxis;
    }

    // Which anaglyph colour pair, and whether the picture under it was colour
    // or black-and-white. Each channel pair is tested: channels that carry
    // the same eye line up, different eyes need a horizontal shift. A channel
    // with (almost) no detail isn't used by the pair (red/green, red/blue).
    // Black-and-white source: the eye seen through two channels shows the
    // same picture in both (red/cyan: green == blue), so there's no colour
    // to recover -- Mono decodes it best.
    AnaglyphKind DetectAnaglyphKind(const LumaImage& img, const RECT& r, int w, int h)
    {
        AnaglyphKind k;
        if (img.green.size() != img.pixels.size() || img.blue.size() != img.pixels.size()) return k;
        const size_t W = (size_t)img.width;
        const uint8_t* R = img.red.data();
        const uint8_t* G = img.green.data();
        const uint8_t* B = img.blue.data();
        // Detail (mean |horizontal step|) per channel, and the mean channel differences.
        double eR = 0, eG = 0, eB = 0, dGB = 0, dRB = 0, dRG = 0; long n = 0;
        for (int y = r.top; y < r.bottom; y += 2)
            for (int x = r.left; x + 1 < r.right; x += 2)
            {
                const size_t i = (size_t)y * W + x;
                eR += std::abs((int)R[i + 1] - (int)R[i]);
                eG += std::abs((int)G[i + 1] - (int)G[i]);
                eB += std::abs((int)B[i + 1] - (int)B[i]);
                dGB += std::abs((int)G[i] - (int)B[i]);
                dRB += std::abs((int)R[i] - (int)B[i]);
                dRG += std::abs((int)R[i] - (int)G[i]);
                ++n;
            }
        if (n == 0) return k;
        eR /= n; eG /= n; eB /= n; dGB /= n; dRB /= n; dRG /= n;
        const double eMax = (std::max)(eR, (std::max)(eG, eB));
        const bool emptyG = eG < 0.15 * eMax, emptyB = eB < 0.15 * eMax;
        constexpr double kGreySame = 6.0;   // mean |difference| below this: the same (grey) picture
        if (emptyG) { k.combo = 2; k.mode = 3; k.known = true; return k; }   // red/blue: one channel per eye
        if (emptyB) { k.combo = 1; k.mode = 3; k.known = true; return k; }   // red/green
        const bool sRG = AnaSplit(AnaSignature(R, G, W, r, w, h));
        const bool sRB = AnaSplit(AnaSignature(R, B, W, r, w, h));
        const bool sGB = AnaSplit(AnaSignature(G, B, W, r, w, h));
        k.known = true;
        if (sRG && sRB && !sGB)      { k.combo = 0; k.mode = dGB < kGreySame ? 3 : 4; }   // red | green+blue
        else if (sRG && sGB && !sRB) { k.combo = 3; k.mode = dRB < kGreySame ? 3 : 4; }   // green | red+blue
        else if (sRB && sGB && !sRG) { k.combo = 4; k.mode = dRG < kGreySame ? 3 : 4; }   // red+green | blue
        else if (sRG && !sRB && !sGB) { k.combo = 5; k.mode = 4; }                        // cyan | magenta (blue shared)
        else                         { k.combo = 0; k.mode = dGB < kGreySame ? 3 : 4; k.known = false; }
        return k;
    }
}

bool srw::ClassifyStereo(const LumaImage& img, const RECT& rIn, StereoFormat& format, float& score,
                         char* diag, size_t diagLen, StereoScores* scores, AnaglyphKind* anaKind,
                         EyeOrder* eyeOrder)
{
    score = 0.0f;
    if (img.empty()) return false;
    RECT r{};
    const RECT all{ 0, 0, img.width, img.height };
    if (!IntersectRect(&r, &rIn, &all)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 64 || h < 48) return false;

    Integral I;
    I.Build(w, h, [&](int x, int y) { return img.at(r.left + x, r.top + y); });

    HPMap M; int mapB = 0;
    const PairResult sbs = MatchHalves(img, r, I, M, mapB, 0, 0, w / 2, 0, w / 2, h);
    const PairResult tab = MatchHalves(img, r, I, M, mapB, 0, 0, 0, h / 2, w, h / 2);
    if (scores)
    {
        // Match strength above the wrong-offset baseline (copies / repeats count as none).
        auto raw = [](const PairResult& p) {
            return (p.madBest >= kRepeatRatio * p.madWrong) ? (std::max)(0.0f, p.ncc - (std::max)(0.0f, p.baseline)) : 0.0f;
        };
        scores->sbs = raw(sbs);
        scores->tab = raw(tab);
    }
    auto passes = [](const PairResult& p) {
        return p.ncc >= kPairNcc && p.ncc - p.baseline >= kPairMargin &&
               p.stdA >= kPairMinStd && p.madBest >= kRepeatRatio * p.madWrong;
    };

    // Interleaved / checkerboard: judged on its own full-resolution
    // statistics (the halves of such a picture don't match as SBS / TAB).
    if (!passes(sbs) && !passes(tab))
    {
        StereoFormat ilf{};
        char note[128] = "";
        if (DetectInterleave(img, r, ilf, note, sizeof(note)))
        {
            if (diag && diagLen) snprintf(diag, diagLen, "rect=(%ld,%ld %dx%d) %s", r.left, r.top, w, h, note);
            format = ilf;
            score  = 1.0f;
            return true;
        }
    }

    // Anaglyph (colour only). In an ordinary photo the colour channels have
    // their edges in the same places (block by block, strongly correlated).
    // In an anaglyph two of them are different eyes: where there's depth
    // they don't line up -- but a small HORIZONTAL shift lines them up again
    // (a vertical one doesn't). See AnaSignature. Red against cyan first
    // (the common pair); with nothing there, green against magenta and
    // amber against blue.
    float a0 = 0.0f, aH = 0.0f, aV = 0.0f; int aBlocks = 0;
    EyeOrder anaEye;
    bool ana = false;
    if (img.hasColour())   // (also when a layout matched: an anaglyph can pass as SBS -- see below)
    {
        const size_t W = (size_t)img.width;
        const bool rgb = img.green.size() == img.pixels.size() && img.blue.size() == img.pixels.size();
        // Cheap first: grey content (the two planes equal) can't be an anaglyph.
        auto chromaOf = [&](const uint8_t* A, const uint8_t* B) {
            double c = 0; long nc = 0;
            for (int y = r.top; y < r.bottom; y += 4)
                for (int x = r.left; x < r.right; x += 4)
                {
                    const size_t i = (size_t)y * W + x;
                    c += std::abs((int)A[i] - (int)B[i]);
                    ++nc;
                }
            return nc ? c / nc : 0.0;
        };
        AnaSig passSig; int passPair = 0;   // (the pair that showed it: 0 red|cyan, 1 green|magenta, 2 amber|blue)
        // X, Y: the two channels making up the pair's mixed plane (cyan = green
        // + blue ...), for AnaSplitByColour's "colour across the axis"; null
        // without colour planes.
        auto tryPair = [&](const uint8_t* A, const uint8_t* B, const uint8_t* X, const uint8_t* Y) {
            const double along = chromaOf(A, B);
            if (along < kAnaMinChroma) return false;
            const AnaSig s = AnaSignature(A, B, W, r, w, h);
            const float axis = (X && Y) ? (float)(along / (std::max)(chromaOf(X, Y), 1.0)) : 0.0f;
            const bool split = AnaSplit(s) || AnaSplitByColour(s, axis);
            if (aBlocks == 0 || split) { a0 = s.a0; aH = s.aH; aV = s.aV; aBlocks = s.blocks; passSig = s; }
            return split;
        };
        ana = tryPair(img.red.data(), img.cyan.data(), rgb ? img.green.data() : nullptr, rgb ? img.blue.data() : nullptr);
        if (!ana && rgb)
        {
            // (Planes made on the spot: green vs magenta, amber vs blue.)
            const size_t n = img.pixels.size();
            std::vector<uint8_t> P(n), Q(n);
            for (size_t i = 0; i < n; ++i) P[i] = (uint8_t)((img.red[i] + img.blue[i] + 1) >> 1);
            ana = tryPair(img.green.data(), P.data(), img.red.data(), img.blue.data());
            if (ana) passPair = 1;
            if (!ana)
            {
                for (size_t i = 0; i < n; ++i) Q[i] = (uint8_t)((img.red[i] + img.green[i] + 1) >> 1);
                ana = tryPair(Q.data(), img.blue.data(), img.red.data(), img.green.data());
                if (ana) passPair = 2;
            }
        }
        // A large picture: the test above works in 32 px blocks and shifts of up
        // to 32 px, tuned for pictures a few hundred px across -- on a big one
        // the eyes' offsets outgrow the search and the blocks hold too little.
        // So (red|cyan) again on the picture shrunk 2x, and 4x if still large.
        for (int sc = 2; !ana && rgb && w / sc >= 240 && sc <= 4; sc *= 2)
        {
            const int w2 = w / sc, h2 = h / sc;
            if (h2 < 64) break;
            std::vector<uint8_t> R2((size_t)w2 * h2), C2(R2.size()), G2(R2.size()), B2(R2.size());
            double along = 0, across = 0;
            for (int y = 0; y < h2; ++y)
                for (int x = 0; x < w2; ++x)
                {
                    int sr = 0, sg = 0, sb = 0;
                    for (int yy = 0; yy < sc; ++yy)
                        for (int xx = 0; xx < sc; ++xx)
                        {
                            const size_t i = (size_t)(r.top + y * sc + yy) * W + r.left + x * sc + xx;
                            sr += img.red[i]; sg += img.green[i]; sb += img.blue[i];
                        }
                    const int n2 = sc * sc; const size_t o = (size_t)y * w2 + x;
                    R2[o] = (uint8_t)(sr / n2); G2[o] = (uint8_t)(sg / n2); B2[o] = (uint8_t)(sb / n2); C2[o] = (uint8_t)((G2[o] + B2[o] + 1) >> 1);
                    along += std::abs((int)R2[o] - (int)C2[o]); across += std::abs((int)G2[o] - (int)B2[o]);
                }
            if (along / R2.size() < kAnaMinChroma) break;
            const RECT r2{ 0, 0, w2, h2 };
            const AnaSig s = AnaSignature(R2.data(), C2.data(), (size_t)w2, r2, w2, h2);
            if (AnaSplit(s) || AnaSplitByColour(s, (float)(along / (std::max)(across, 1.0))))
            {
                ana = true; passPair = 0; passSig = s;
                // (Its offsets are at the shrunk scale: back to the picture's.)
                passSig.medDx *= sc; passSig.topDx *= sc; passSig.botDx *= sc;
                a0 = s.a0; aH = s.aH; aV = s.aV; aBlocks = s.blocks;
            }
        }
        if (ana)
        {
            AnaglyphKind k = DetectAnaglyphKind(img, r, w, h);
            if (anaKind) *anaKind = k;
            // The pair's first plane is the left filter's -- except cyan/magenta,
            // found through red|cyan, where red belongs to the right (magenta).
            anaEye = AnaEyeOrder(passSig, k.known && k.combo == 5 && passPair == 0);
        }
    }

    if (diag && diagLen)
        snprintf(diag, diagLen,
                 "rect=(%ld,%ld %dx%d) SBS ncc=%.2f base=%.2f std=%.1f mad=%.1f/%.1f d=(%d,%d) | "
                 "TAB ncc=%.2f base=%.2f std=%.1f mad=%.1f/%.1f d=(%d,%d) | ANA aligned=%.2f hshift=%.2f vshift=%.2f blocks=%d%s",
                 r.left, r.top, w, h, sbs.ncc, sbs.baseline, sbs.stdA, sbs.madBest, sbs.madWrong, sbs.dx, sbs.dy,
                 tab.ncc, tab.baseline, tab.stdA, tab.madBest, tab.madWrong, tab.dx, tab.dy, a0, aH, aV, aBlocks,
                 img.hasColour() ? "" : " (no colour)");

    // Each layout is tested on its own; if BOTH match, the content is
    // ambiguous (e.g. a repeating grid) and nothing is detected -- the two
    // results are never weighed against each other to pick one.
    const bool isSbs = passes(sbs), isTab = passes(tab);
    // Anaglyph first: colour fringes that only line up with a horizontal
    // shift are specific evidence, while an anaglyph video's two halves can
    // happen to look alike enough to pass as SBS.
    if (ana)
    {
        format = StereoFormat::Anaglyph;
        score  = aH - a0;
        if (eyeOrder) *eyeOrder = anaEye;
        return true;
    }
    if (isSbs && isTab) return false;
    if (isSbs)
    {
        // Each eye's shape: landscape halves = two full pictures; narrower =
        // one squeezed (anamorphic) picture per half.
        const float eyeAspect = (float)(w / 2) / (float)h;
        format = eyeAspect >= 1.1f ? StereoFormat::FullSBS : StereoFormat::HalfSBS;
        score  = sbs.ncc;
        if (eyeOrder) *eyeOrder = JudgeEyeOrder(I, M, mapB, sbs, 0, 0, w / 2, 0, w / 2, h);
        return true;
    }
    if (isTab)
    {
        const float eyeAspect = (float)w / (float)(h / 2);
        format = eyeAspect >= 2.6f ? StereoFormat::HalfTAB : StereoFormat::FullTAB;
        score  = tab.ncc;
        if (eyeOrder) *eyeOrder = JudgeEyeOrder(I, M, mapB, tab, 0, 0, 0, h / 2, w, h / 2);
        return true;
    }
    return false;
}

void srw::JudgeEyeOrderOf(const LumaImage& img, const RECT& rIn, StereoFormat format, int anaCombo, EyeOrder& out)
{
    out = EyeOrder{};
    if (img.empty()) return;
    RECT r{};
    const RECT all{ 0, 0, img.width, img.height };
    if (!IntersectRect(&r, &rIn, &all)) return;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 64 || h < 48) return;
    const bool sbs = format == StereoFormat::FullSBS || format == StereoFormat::HalfSBS;
    const bool tab = format == StereoFormat::FullTAB || format == StereoFormat::HalfTAB;
    if (sbs || tab)
    {
        Integral I;
        I.Build(w, h, [&](int x, int y) { return img.at(r.left + x, r.top + y); });
        HPMap M; int mapB = 0;
        const PairResult pr = sbs ? MatchHalves(img, r, I, M, mapB, 0, 0, w / 2, 0, w / 2, h)
                                  : MatchHalves(img, r, I, M, mapB, 0, 0, 0, h / 2, w, h / 2);
        if (pr.ncc < 0.3f) return;   // the halves don't match well enough to measure
        out = sbs ? JudgeEyeOrder(I, M, mapB, pr, 0, 0, w / 2, 0, w / 2, h)
                  : JudgeEyeOrder(I, M, mapB, pr, 0, 0, 0, h / 2, w, h / 2);
        return;
    }
    if (format != StereoFormat::Anaglyph || !img.hasColour() ||
        img.green.size() != img.pixels.size() || img.blue.size() != img.pixels.size())
        return;
    // The combo's left-filter plane first, the right's second (as the
    // converter's anaChanL / anaChanR, but with both channels where a filter
    // passes two).
    const size_t n = img.pixels.size();
    std::vector<uint8_t> A(n), B(n);
    for (size_t i = 0; i < n; ++i)
    {
        const int R = img.red[i], G = img.green[i], Bl = img.blue[i];
        switch (anaCombo)
        {
        case 1:  A[i] = (uint8_t)R;                  B[i] = (uint8_t)G;                  break;   // red/green
        case 2:  A[i] = (uint8_t)R;                  B[i] = (uint8_t)Bl;                 break;   // red/blue
        case 3:  A[i] = (uint8_t)G;                  B[i] = (uint8_t)((R + Bl + 1) >> 1); break;  // green/magenta
        case 4:  A[i] = (uint8_t)((R + G + 1) >> 1); B[i] = (uint8_t)Bl;                 break;   // amber/blue
        case 5:  A[i] = (uint8_t)G;                  B[i] = (uint8_t)R;                  break;   // cyan/magenta (blue shared)
        default: A[i] = (uint8_t)R;                  B[i] = (uint8_t)((G + Bl + 1) >> 1); break;  // red/cyan
        }
    }
    const AnaSig s = AnaSignature(A.data(), B.data(), (size_t)img.width, r, w, h);
    if (s.blocks < kAnaMinBlocks) return;
    out = AnaEyeOrder(s, false);
}

// ============================================================================
// StereoScanner
// ============================================================================

namespace
{
    constexpr int   kSeedStep     = 40;     // analysis px between seed points
    constexpr int   kMinHitW      = 80, kMinHitH = 60;   // smallest stereo rect worth weaving
    constexpr float kWholeMaxFlat = 0.25f;  // a viewport this busy may be one picture (a player)

    float OverlapFrac(const RECT& a, const RECT& b)   // fraction of a covered by b
    {
        RECT i{};
        if (!IntersectRect(&i, &a, &b)) return 0.0f;
        const float aa = (float)(a.right - a.left) * (a.bottom - a.top);
        return aa > 0 ? (float)(i.right - i.left) * (i.bottom - i.top) / aa : 0.0f;
    }
}

StereoScanner::~StereoScanner()
{
    if (m_thread.joinable()) m_thread.join();
}

bool StereoScanner::Start(std::shared_ptr<const LumaImage> img, uint64_t frameId,
                          std::vector<ScanWindow> windows, std::vector<RECT> exclude,
                          std::vector<ScanVerify> verify)
{
    if (m_busy.load() || !img) return false;
    if (m_thread.joinable()) m_thread.join();
    m_busy = true;
    m_thread = std::thread([this, img, frameId, windows = std::move(windows), exclude = std::move(exclude),
                            verify = std::move(verify)]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // never compete with rendering
        ScanResult res;
        res.frameId = frameId;
        res.image   = img;
        const ULONGLONG t0 = GetTickCount64();
        Scan(*img, windows, exclude, res, verify, &m_cache);
        res.ms = (double)(GetTickCount64() - t0);
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_result = std::move(res);
            m_ready  = true;
        }
        m_busy = false;
    });
    return true;
}

bool StereoScanner::Take(ScanResult& out)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_ready) return false;
    out = std::move(m_result);
    m_result = ScanResult{};
    m_ready = false;
    return true;
}

namespace
{
    uint64_t ContentHash(const LumaImage& img, const RECT& r)
    {
        uint64_t hsh = 1469598103934665603ull;
        const int w = r.right - r.left, h = r.bottom - r.top;
        for (int j = 0; j < 24; ++j)
            for (int i = 0; i < 24; ++i)
            {
                const int x = r.left + (i * 2 + 1) * w / 48, y = r.top + (j * 2 + 1) * h / 48;
                const int v = (x >= 0 && y >= 0 && x < img.width && y < img.height) ? img.at(x, y) : 0;
                hsh = (hsh ^ (uint64_t)v) * 1099511628211ull;
            }
        return hsh;
    }
}

void StereoScanner::Scan(const LumaImage& img, const std::vector<ScanWindow>& windows,
                         const std::vector<RECT>& exclude, ScanResult& out,
                         const std::vector<ScanVerify>& verify, std::vector<CachedVerdict>* cache)
{
    // 1) Candidate pictures: per window (in parallel), a grid of seed
    //    points -> image rectangles. Every rectangle found covers its seeds,
    //    so each picture is found once.
    struct Cand { int win; RECT rect; bool whole; };
    std::vector<std::vector<Cand>> perWin(windows.size());
    std::vector<char> wholeOk(windows.size(), 0);
    ParallelFor((int)windows.size(), [&](int wi) {
        const ScanWindow& sw = windows[wi];
        ImageRectFinder f;
        if (!f.Prepare(img, sw.bounds)) return;
        const RECT b = f.Bounds();
        auto blocked = [&](const RECT& r, float frac) {
            for (const RECT& e : exclude)    if (OverlapFrac(r, e) > frac) return true;
            for (const RECT& c : sw.covered) if (OverlapFrac(r, c) > frac) return true;
            return false;
        };
        std::vector<RECT> seen;
        const int half = kSeedStep / 2;
        // Seeds are tried in parallel, then merged in grid order exactly as
        // a one-by-one pass would (a seed inside an earlier find is dropped).
        // A coarse grid first finds most pictures with few seeds; the fine
        // grid then only fills in what's left.
        auto runSeeds = [&](int step) {
            std::vector<POINT> pts;
            for (int y = b.top + half; y < b.bottom - 1; y += step)
                for (int x = b.left + half; x < b.right - 1; x += step)
                {
                    const POINT p{ x, y };
                    bool skip = false;
                    for (const RECT& s : seen)                  if (PtInRect(&s, p)) { skip = true; break; }
                    if (!skip) for (const RECT& e : exclude)    if (PtInRect(&e, p)) { skip = true; break; }
                    if (!skip) for (const RECT& c : sw.covered) if (PtInRect(&c, p)) { skip = true; break; }
                    if (skip) continue;
                    if (f.FlatFraction({ x - 8, y - 8, x + 8, y + 8 }) > 0.6f) continue;   // page, not picture
                    pts.push_back(p);
                }
            struct Found { RECT r{}; int sides = 0; bool ok = false; };
            std::vector<Found> res(pts.size());
            ParallelFor((int)pts.size(), [&](int i) { res[i].ok = f.Find(pts[i].x, pts[i].y, res[i].r, &res[i].sides); });
            for (size_t i = 0; i < pts.size(); ++i)
            {
                bool skip = false;
                for (const RECT& s : seen) if (PtInRect(&s, pts[i])) { skip = true; break; }
                if (skip || !res[i].ok) continue;
                const RECT r = res[i].r;
                // Only a confident rect (page background on 3+ sides) is
                // judged and covers its seeds; a seed sitting right on an
                // image's edge line can produce a rect straddling two things.
                // A side on the viewport's own edge counts too: a picture
                // part-scrolled out of view has no page background there.
                int sides = res[i].sides;
                if (r.top <= b.top + 2)       ++sides;
                if (r.bottom >= b.bottom - 2) ++sides;
                if (r.left <= b.left + 2)     ++sides;
                if (r.right >= b.right - 2)   ++sides;
                if (sides < 3) { seen.push_back({ pts[i].x - half, pts[i].y - half, pts[i].x + half, pts[i].y + half }); continue; }
                seen.push_back(r);
                if (r.right - r.left < kMinHitW || r.bottom - r.top < kMinHitH) continue;
                if (blocked(r, 0.1f)) continue;
                perWin[wi].push_back({ wi, r, false });
            }
        };
        runSeeds(kSeedStep * 3);
        runSeeds(kSeedStep);
        // The whole viewport as one picture (a video player, a fullscreen
        // video, a stereo photo viewer -- whose SBS seam can make each half
        // look like an image of its own). Judged only if nothing inside it
        // turns out to be 3D: a player's controls bar must not be woven as
        // part of its video.
        wholeOk[wi] = !blocked(b, 0.1f) && f.FlatFraction(b) <= kWholeMaxFlat;
    });

    // 2) Judge them all (in parallel) -- reusing the verdict from an earlier
    //    scan when the same place still holds the same content.
    struct Job { int win; RECT rect; bool whole; int verifyIdx; uint64_t hash; bool st; StereoFormat fmt; float score; std::string diag; AnaglyphKind ana; EyeOrder eye; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < verify.size(); ++i) jobs.push_back({ -1, verify[i].rect, false, (int)i });
    for (auto& v : perWin) for (const Cand& c : v) jobs.push_back({ c.win, c.rect, false, -1 });
    auto judgeAll = [&](size_t from) {
        ParallelFor((int)(jobs.size() - from), [&](int k) {
            Job& j = jobs[from + k];
            j.hash = ContentHash(img, j.rect);
            if (cache)
                for (const CachedVerdict& c : *cache)
                    if (c.hash == j.hash && EqualRect(&c.rect, &j.rect))
                    {
                        j.st = c.stereo; j.fmt = c.format; j.score = c.score; j.diag = c.diag; j.ana = c.ana; j.eye = c.eye;
                        return;
                    }
            char diag[512] = "";
            j.fmt = StereoFormat::HalfSBS; j.score = 0.0f;
            j.st = ClassifyStereo(img, j.rect, j.fmt, j.score, diag, sizeof(diag), nullptr, &j.ana, &j.eye);
            j.diag = diag;
        });
    };
    judgeAll(0);
    // Twins: an SBS (TAB) picture whose scene has a strip of the page's own
    // colour at one edge -- a white sky on a white page -- has that strip in
    // BOTH eyes: at the picture's outer edge, where it passes for page, and
    // between the eyes, where it passes for a gap between two pictures. So it
    // was found as two separate pictures, neither of them 3D (or, with the
    // strip at the outer edge only, as one box cut short on that side, its
    // eyes split in the wrong place). Two same-size pictures side by side
    // (stacked) a small gap apart are tried as one: the second eye starts one
    // eye-width after the first, so the box is widened by the gap on the
    // outside -- whichever end the strip is really at, the eyes line up
    // exactly. Kept only if it IS side-by-side (top-and-bottom) stereo.
    const size_t twinFrom = jobs.size();
    std::vector<char> twinVert;
    auto fillsWindow = [](const RECT& t, const RECT& b) {
        return t.left <= b.left + 4 && t.top <= b.top + 4 && t.right >= b.right - 4 && t.bottom >= b.bottom - 4;
    };
    for (size_t a = 0; a < twinFrom; ++a)
        for (size_t c = 0; c < twinFrom; ++c)
        {
            const Job& A = jobs[a]; const Job& B = jobs[c];
            if (a == c || A.win < 0 || A.win != B.win || A.st || B.st || A.whole || B.whole) continue;
            const RECT& ra = A.rect; const RECT& rb = B.rect;
            const RECT wb = windows[A.win].bounds;
            const int wa = ra.right - ra.left, ha = ra.bottom - ra.top, wB = rb.right - rb.left, hB = rb.bottom - rb.top;
            if (std::abs(ra.top - rb.top) <= 3 && std::abs(ra.bottom - rb.bottom) <= 3 && std::abs(wa - wB) <= (std::max)(4, wa / 50) &&
                rb.left >= ra.right && rb.left - ra.right <= wa * 35 / 100)
            {
                const int eye = rb.left - ra.left;
                RECT t{ ra.left, (std::min)(ra.top, rb.top), ra.left + 2 * eye, (std::max)(ra.bottom, rb.bottom) };
                if (t.right > wb.right) t = { rb.right - 2 * eye, t.top, rb.right, t.bottom };   // (the strip at the other end)
                if (t.left < wb.left) continue;
                if (fillsWindow(t, wb)) continue;   // (a player filled by its video: the whole-viewport check below)
                jobs.push_back({ A.win, t, false, -1 }); twinVert.push_back(0);
            }
            if (std::abs(ra.left - rb.left) <= 3 && std::abs(ra.right - rb.right) <= 3 && std::abs(ha - hB) <= (std::max)(4, ha / 50) &&
                rb.top >= ra.bottom && rb.top - ra.bottom <= ha * 35 / 100)
            {
                const int eye = rb.top - ra.top;
                RECT t{ (std::min)(ra.left, rb.left), ra.top, (std::max)(ra.right, rb.right), ra.top + 2 * eye };
                if (t.bottom > wb.bottom) t = { t.left, rb.bottom - 2 * eye, t.right, rb.bottom };
                if (t.top < wb.top) continue;
                if (fillsWindow(t, wb)) continue;
                jobs.push_back({ A.win, t, false, -1 }); twinVert.push_back(1);
            }
        }
    judgeAll(twinFrom);
    for (size_t t = twinFrom; t < jobs.size(); ++t)
    {
        Job& j = jobs[t];
        const bool sbs = j.fmt == StereoFormat::FullSBS || j.fmt == StereoFormat::HalfSBS;
        const bool tab = j.fmt == StereoFormat::FullTAB || j.fmt == StereoFormat::HalfTAB;
        if (j.st && !(twinVert[t - twinFrom] ? tab : sbs)) j.st = false;
    }
    // Whole viewports of windows where nothing inside was 3D.
    std::vector<char> winHit(windows.size(), 0);
    for (const Job& j : jobs) if (j.win >= 0 && j.st) winHit[j.win] = 1;
    const size_t wholeFrom = jobs.size();
    for (size_t wi = 0; wi < windows.size(); ++wi)
        if (wholeOk[wi] && !winHit[wi])
        {
            RECT b{};
            const RECT all{ 0, 0, img.width, img.height };
            if (IntersectRect(&b, &windows[wi].bounds, &all)) jobs.push_back({ (int)wi, b, true, -1 });
        }
    judgeAll(wholeFrom);

    // 3) Results.
    std::vector<CachedVerdict> used;
    for (const Job& j : jobs)
    {
        used.push_back({ j.rect, j.hash, j.st, j.fmt, j.score, j.diag, j.ana, j.eye });
        if (j.verifyIdx >= 0)
        {
            const ScanVerify& v = verify[j.verifyIdx];
            const bool ok = j.st && j.fmt == v.format;
            out.verified.push_back({ v.id, ok });
            if (!ok) out.log.push_back("verify fail " + j.diag);
            continue;
        }
        out.log.push_back(std::string(j.whole ? (j.st ? "whole HIT " : "whole no  ") : (j.st ? "image HIT " : "image no  ")) + j.diag);
        if (j.st)
        {
            const ScanWindow& sw = windows[j.win];
            out.hits.push_back({ sw.host, sw.view, j.rect, j.fmt, j.score, j.whole, j.ana, j.eye });
        }
    }
    if (cache) *cache = std::move(used);   // keep only what's still on screen
}
