// RegionWeave.cpp -- see RegionWeave.h.
#include "RegionWeave.h"
#include "ScreenAnalysis.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    // Draw one quad covering the current viewport, sampling a sub-rectangle
    // (uvRect.xy = origin, uvRect.zw = size) of the source.
    const char* kCompositeHLSL = R"(
cbuffer CB : register(b0) { float4 uvRect; };
Texture2D    src  : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
// Per-region data for GPU-tracked pictures: the picture's vertical offset
// this frame comes from the GPU tracker's results (never the CPU).
cbuffer Region : register(b1) { int4 rSrc; int4 rClip; int rSlot; float rScale; float rVpH; float rVpW; };
StructuredBuffer<int4> gpuRes : register(t1);
int RegionDy()
{
    if (rSlot < 0) return 0;
    const int4 g = gpuRes[rSlot];
    return g.y ? (int)(g.x * rScale) : 0;
}
int RegionDx()
{
    if (rSlot < 0) return 0;
    const int4 g = gpuRes[rSlot];
    return g.w ? (int)(g.z * rScale) : 0;
}
// Placement shifted by the tracked offset (in this viewport's pixels).
VSOut VSShift(uint id : SV_VertexID)
{
    VSOut o = VSMain(id);
    o.pos.y -= 2.0 * RegionDy() / rVpH;
    o.pos.x += 2.0 * RegionDx() / rVpW;
    return o;
}
// The crop: the picture's pixels at its tracked position; black outside the
// part that's actually on show (its viewport).
float4 PSCrop(VSOut i) : SV_Target
{
    const int2 p = int2(i.pos.xy);
    const int sx = rSrc.x + p.x + RegionDx(), sy = rSrc.y + p.y + RegionDy();
    if (sx < rClip.x || sy < rClip.y || sx >= rClip.z || sy >= rClip.w) return float4(0, 0, 0, 1);
    return float4(src.Load(int3(sx, sy, 0)).rgb, 1);
}
// Fill for the space beside a true-shape Full SBS picture: one flat colour
// for both sides -- the average of the eye's own left and right edges.
float4 PSBlur(VSOut i) : SV_Target
{
    float3 acc = 0;
    [unroll] for (int k = 0; k < 32; ++k)
    {
        const float v = (k + 0.5) / 32.0;
        acc += src.SampleLevel(samp, uvRect.xy + float2(0.01, v) * uvRect.zw, 0).rgb;
        acc += src.SampleLevel(samp, uvRect.xy + float2(0.99, v) * uvRect.zw, 0).rgb;
    }
    return float4(acc / 64.0, 1);
}
float4 PSMain(VSOut i) : SV_Target
{
    return src.Sample(samp, uvRect.xy + i.uv * uvRect.zw);
}
)";

    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

RegionWeaver::~RegionWeaver() { Shutdown(); }

bool RegionWeaver::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device  = device;
    m_context = context;

    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    const size_t len = std::strlen(kCompositeHLSL);
    if (FAILED(D3DCompile(kCompositeHLSL, len, "RegionWeave", nullptr, nullptr,
                          "VSMain", "vs_5_0", 0, 0, &vsb, &err)))
    {
        Log("RegionWeaver: VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    if (FAILED(D3DCompile(kCompositeHLSL, len, "RegionWeave", nullptr, nullptr,
                          "PSMain", "ps_5_0", 0, 0, &psb, &err)))
    {
        Log("RegionWeaver: PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err); SafeRelease(vsb);
        return false;
    }
    m_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
    m_device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);
    SafeRelease(psb);
    if (SUCCEEDED(D3DCompile(kCompositeHLSL, len, "RegionWeave", nullptr, nullptr,
                             "PSBlur", "ps_5_0", 0, 0, &psb, &err)))
        m_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_psBlur);
    SafeRelease(psb); SafeRelease(err);
    if (SUCCEEDED(D3DCompile(kCompositeHLSL, len, "RegionWeave", nullptr, nullptr,
                             "PSCrop", "ps_5_0", 0, 0, &psb, &err)))
        m_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_psCrop);
    SafeRelease(psb); SafeRelease(err);
    {
        ID3DBlob* vs2 = nullptr;
        if (SUCCEEDED(D3DCompile(kCompositeHLSL, len, "RegionWeave", nullptr, nullptr,
                                 "VSShift", "vs_5_0", 0, 0, &vs2, &err)))
            m_device->CreateVertexShader(vs2->GetBufferPointer(), vs2->GetBufferSize(), nullptr, &m_vsShift);
        SafeRelease(vs2);
    }
    SafeRelease(err);
    SafeRelease(vsb); SafeRelease(psb);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sd, &m_sampler);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&bd, nullptr, &m_cb);
    bd.ByteWidth = 48;   // Region: int4 src, int4 clip, slot, scale, vpH, pad
    m_device->CreateBuffer(&bd, nullptr, &m_regionCB);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable   = TRUE;
    m_device->CreateRasterizerState(&rd, &m_rs);

    return m_vs && m_ps && m_sampler && m_cb && m_rs;
}

void RegionWeaver::Shutdown()
{
    Clear();
    for (auto& s : m_slotPool) { SafeRelease(s.cropRTV); SafeRelease(s.cropSRV); SafeRelease(s.crop); }
    m_slotPool.clear();
    SafeRelease(m_compSRV); SafeRelease(m_compRTV); SafeRelease(m_comp);
    SafeRelease(m_lgSRV); SafeRelease(m_lgTex); m_lgW = m_lgH = 0;
    m_compW = m_compH = 0;
    SafeRelease(m_rs); SafeRelease(m_cb); SafeRelease(m_sampler); SafeRelease(m_psBlur); SafeRelease(m_psCrop); SafeRelease(m_vsShift); SafeRelease(m_regionCB); SafeRelease(m_ps); SafeRelease(m_vs);
    m_device = nullptr; m_context = nullptr;
}

int RegionWeaver::Add(const WeaveRegion& r)
{
    WeaveRegion copy = r;
    copy.id = m_nextId++;
    m_regions.push_back(copy);
    return copy.id;
}

void RegionWeaver::Remove(int id)
{
    m_regions.erase(std::remove_if(m_regions.begin(), m_regions.end(),
                                   [&](const WeaveRegion& r) { return r.id == id; }),
                    m_regions.end());
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
    {
        if (it->id != id) continue;
        ReleaseSlot(*it);
        m_slots.erase(it);
        break;
    }
}

void RegionWeaver::Clear()
{
    m_regions.clear();
    for (auto& s : m_slots) ReleaseSlot(s);
    m_slots.clear();
}

void RegionWeaver::ReleaseSlot(Slot& s)
{
    // Into the pool as it is -- converter (compiled shaders) and crop, with
    // their textures -- for the next picture (SlotFor). A few at most.
    if (!s.conv) { SafeRelease(s.cropRTV); SafeRelease(s.cropSRV); SafeRelease(s.crop); s.cropW = s.cropH = 0; return; }
    m_slotPool.push_back(std::move(s));
    s.crop = nullptr; s.cropSRV = nullptr; s.cropRTV = nullptr; s.cropW = s.cropH = 0;
    constexpr size_t kPoolMax = 12;
    if (m_slotPool.size() > kPoolMax)
    {
        Slot& old = m_slotPool.front();
        SafeRelease(old.cropRTV); SafeRelease(old.cropSRV); SafeRelease(old.crop);
        m_slotPool.erase(m_slotPool.begin());
    }
}

RegionWeaver::Slot* RegionWeaver::SlotFor(int id, int w, int h)
{
    for (auto& s : m_slots) if (s.id == id) return &s;
    Slot s;
    // From the pool: one last used for a picture this size first (its
    // textures fit as they are), else the newest.
    int pick = -1;
    for (int i = (int)m_slotPool.size() - 1; i >= 0; --i)
        if (m_slotPool[i].cropW == w && m_slotPool[i].cropH == h) { pick = i; break; }
    if (pick < 0 && !m_slotPool.empty()) pick = (int)m_slotPool.size() - 1;
    if (pick >= 0)
    {
        s = std::move(m_slotPool[pick]);
        m_slotPool[pick].crop = nullptr; m_slotPool[pick].cropSRV = nullptr; m_slotPool[pick].cropRTV = nullptr;
        m_slotPool.erase(m_slotPool.begin() + pick);
    }
    else
    {
        s.conv = std::make_unique<Converter>();
        if (!s.conv->Initialize(m_device, m_context))
        {
            Log("RegionWeaver: Converter init failed for region %d", id);
            return nullptr;
        }
    }
    s.id = id;
    m_slots.push_back(std::move(s));
    return &m_slots.back();
}


bool RegionWeaver::EnsureCrop(Slot& s, int w, int h, DXGI_FORMAT texFmt, DXGI_FORMAT srvFmt)
{
    if (s.crop && s.cropW == w && s.cropH == h && s.cropFmt == texFmt) return true;
    SafeRelease(s.cropRTV); SafeRelease(s.cropSRV); SafeRelease(s.crop);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = texFmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &s.crop))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = srvFmt; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = srvFmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(m_device->CreateShaderResourceView(s.crop, &sd, &s.cropSRV)) ||
        FAILED(m_device->CreateRenderTargetView(s.crop, &rd, &s.cropRTV)))
    {
        SafeRelease(s.cropSRV); SafeRelease(s.crop);
        return false;
    }
    s.cropW = w; s.cropH = h; s.cropFmt = texFmt;
    return true;
}

bool RegionWeaver::CropComposite(const RECT& rIn, bool& resized)
{
    resized = false;
    if (!m_comp || m_compW <= 0) return false;
    const int fw = m_compW / 2, fh = m_compH;
    RECT r{};
    const RECT all{ 0, 0, fw, fh };
    if (!IntersectRect(&r, &rIn, &all)) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (!m_lgTex || w != m_lgW || h != m_lgH)
    {
        SafeRelease(m_lgSRV); SafeRelease(m_lgTex);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)(2 * w); td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = kCompFormat; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_lgTex))) { m_lgW = m_lgH = 0; return false; }
        m_device->CreateShaderResourceView(m_lgTex, nullptr, &m_lgSRV);
        m_lgW = w; m_lgH = h;
        resized = true;
    }
    // Left eye's part, then the right eye's (the composite's right half).
    D3D11_BOX bl{ (UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1 };
    D3D11_BOX br{ (UINT)(fw + r.left), (UINT)r.top, 0, (UINT)(fw + r.right), (UINT)r.bottom, 1 };
    m_context->CopySubresourceRegion(m_lgTex, 0, 0, 0, 0, m_comp, 0, &bl);
    m_context->CopySubresourceRegion(m_lgTex, 0, (UINT)w, 0, 0, m_comp, 0, &br);
    return m_lgSRV != nullptr;
}

bool RegionWeaver::EnsureComposite(int w, int h)
{
    if (m_comp && m_compW == w && m_compH == h) return false;
    SafeRelease(m_compSRV); SafeRelease(m_compRTV); SafeRelease(m_comp);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = kCompFormat; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_comp))) { m_compW = m_compH = 0; return false; }
    m_device->CreateRenderTargetView(m_comp, nullptr, &m_compRTV);
    m_device->CreateShaderResourceView(m_comp, nullptr, &m_compSRV);
    m_compW = w; m_compH = h;
    Log("RegionWeaver: composite %dx%d", w, h);
    return true;
}

void RegionWeaver::DrawInto(ID3D11ShaderResourceView* src, float u0, float v0, float du, float dv,
                            int dx, int dy, int dw, int dh)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(m_context->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        const float uv[4] = { u0, v0, du, dv };
        std::memcpy(m.pData, uv, sizeof(uv));
        m_context->Unmap(m_cb, 0);
    }
    D3D11_VIEWPORT vp{ (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    m_context->PSSetShaderResources(0, 1, &src);
    m_context->Draw(3, 0);
}

bool RegionWeaver::Build(ID3D11Texture2D* captureTex, ID3D11ShaderResourceView* captureSRV,
                         DXGI_FORMAT captureSrvFormat, int frameW, int frameH, bool& compositeResized)
{
    compositeResized = false;
    if (!m_device || !captureTex || frameW <= 0 || frameH <= 0) return false;

    compositeResized = EnsureComposite(frameW * 2, frameH);
    if (!m_compRTV) return false;

    const float black[4] = { 0, 0, 0, 1 };
    m_context->ClearRenderTargetView(m_compRTV, black);

    D3D11_TEXTURE2D_DESC capDesc{};
    captureTex->GetDesc(&capDesc);
    const RECT frameRect{ 0, 0, frameW, frameH };

    for (const WeaveRegion& r : m_regions)
    {
        // The crop is the WHOLE image (so SBS/TAB halves split in the right
        // place even when part of it is scrolled off / under a toolbar); only
        // its visible part is filled, the rest stays black.
        const RECT f = r.frame;
        const int w = f.right - f.left, h = f.bottom - f.top;
        if (w < 16 || h < 16 || w > 16384 || h > 16384) continue;
        // GPU-tracked: the picture's position this frame is only known on the
        // GPU, so the crop and placement read it there; what's on show is
        // bounded by its viewport. Otherwise: the CPU's visible part.
        const bool gpu = r.gpuSlot >= 0 && m_gpuRes && m_psCrop && m_vsShift && captureSRV;
        RECT v = gpu ? r.clip : r.vis;
        if (IsRectEmpty(&v)) v = gpu ? frameRect : f;
        RECT src{};
        if (!IntersectRect(&src, &v, &frameRect)) continue;
        if (!gpu && !IntersectRect(&src, &src, &f)) continue;

        Slot* s = SlotFor(r.id, w, h);
        if (!s || !EnsureCrop(*s, w, h, capDesc.Format, captureSrvFormat)) continue;

        // Region constants (b1): used by the GPU crop and shifted placement.
        // vpW = the width of the viewport being drawn (the sideways shift is
        // converted with it), so it's re-sent before draws of another width.
        auto uploadRegion = [&](float vpW) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_context->Map(m_regionCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
            int* c = static_cast<int*>(m.pData);
            c[0] = f.left; c[1] = f.top; c[2] = 0; c[3] = 0;
            // (the crop reads the capture, where the window -- and so this
            // viewport -- still is where it was when captured)
            c[4] = src.left - r.winLead.x;  c[5] = src.top - r.winLead.y;
            c[6] = src.right - r.winLead.x; c[7] = src.bottom - r.winLead.y;
            c[8] = gpu ? r.gpuSlot : -1;
            float* fl = reinterpret_cast<float*>(c);
            fl[9]  = (float)m_gpuScale;
            fl[10] = (float)h;
            fl[11] = vpW;
            m_context->Unmap(m_regionCB, 0);
        };
        uploadRegion((float)w);

        if (gpu)
        {
            ID3D11ShaderResourceView* srvs[2] = { captureSRV, m_gpuRes };
            D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
            m_context->OMSetRenderTargets(1, &s->cropRTV, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->RSSetState(nullptr);
            m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->IASetInputLayout(nullptr);
            m_context->VSSetShader(m_vs, nullptr, 0);
            m_context->PSSetShader(m_psCrop, nullptr, 0);
            m_context->PSSetShaderResources(0, 2, srvs);
            m_context->PSSetConstantBuffers(1, 1, &m_regionCB);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nulls[2] = {};
            m_context->PSSetShaderResources(0, 2, nulls);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        else
        {
            if (!EqualRect(&src, &f)) m_context->ClearRenderTargetView(s->cropRTV, black);
            D3D11_BOX box{ (UINT)src.left, (UINT)src.top, 0, (UINT)src.right, (UINT)src.bottom, 1 };
            m_context->CopySubresourceRegion(s->crop, 0, (UINT)(src.left - f.left), (UINT)(src.top - f.top), 0,
                                             captureTex, 0, &box);
        }

        s->conv->SetFormat(r.format, r.swapEyes, r.anaglyphCombo, r.anaglyphMode);
        s->conv->SetAnaTint(r.anaTint ? r.anaTint->single : nullptr, r.anaTint ? r.anaTint->missing : nullptr);
        s->conv->SetConvergence(0.0f);
        s->conv->SetTargetPaneSize(w, h);
        bool resized = false;
        if (!s->conv->Convert(s->cropSRV, w, h, resized)) continue;
        ID3D11ShaderResourceView* out = s->conv->OutputSRV();
        if (!out) continue;

        // Converter output is side-by-side: left half = left eye, right half
        // = right eye. Place each eye at the region's position in the matching
        // half of the composite (stretching back to the region's size, which
        // also un-squeezes half-width formats).
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(gpu ? m_vsShift : m_vs, nullptr, 0);
        if (gpu)
        {
            m_context->VSSetConstantBuffers(1, 1, &m_regionCB);
            m_context->VSSetShaderResources(1, 1, &m_gpuRes);
        }
        m_context->PSSetShader(m_ps, nullptr, 0);
        m_context->PSSetSamplers(0, 1, &m_sampler);
        m_context->PSSetConstantBuffers(0, 1, &m_cb);
        m_context->OMSetRenderTargets(1, &m_compRTV, nullptr);
        m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_rs);
        // A Full SBS picture's eyes are each half its width: shown at their
        // real shape they're half as wide, centred; stretched over the whole
        // region they'd look twice too wide.
        int ex = f.left, ew = w;
        if (r.trueAspect && r.format == StereoFormat::FullSBS) { ex = f.left + w / 4; ew = w / 2; }
        // Drawn `lead` ahead of where it was captured (a scrolling picture
        // is shown where it will be by the time this frame is on screen).
        // Scissored to what's on show -- never outside its viewport, and an
        // image running off the frame edge can't spill into the other eye.
        // (GPU-tracked: the exact position is only known on the GPU, so the
        // scissor is the whole viewport and the crop's black does the rest.)
        ex += r.lead.x;
        const int ey = f.top + r.lead.y;
        RECT dst = src;
        if (!gpu) OffsetRect(&dst, r.lead.x, r.lead.y);
        if (!IsRectEmpty(&r.clip) && !IntersectRect(&dst, &dst, &r.clip)) { m_context->RSSetState(nullptr); continue; }
        if (!IntersectRect(&dst, &dst, &frameRect)) { m_context->RSSetState(nullptr); continue; }
        // True-shape Full SBS: first fill the whole region with each side's
        // own edge colour (a plain surround instead of black bars), then the
        // sharp picture in the middle.
        const bool fill = r.trueAspect && r.format == StereoFormat::FullSBS && m_psBlur;
        const int fx = f.left + r.lead.x;
        SetScissor(dst);
        if (fill)
        {
            m_context->PSSetShader(m_psBlur, nullptr, 0);
            if (gpu) uploadRegion((float)w);
            DrawInto(out, 0.0f, 0.0f, 0.5f, 1.0f, fx, ey, w, h);
            m_context->PSSetShader(m_ps, nullptr, 0);
        }
        if (gpu) uploadRegion((float)ew);
        DrawInto(out, 0.0f, 0.0f, 0.5f, 1.0f, ex, ey, ew, h);
        SetScissor({ frameW + dst.left, dst.top, frameW + dst.right, dst.bottom });
        if (fill)
        {
            m_context->PSSetShader(m_psBlur, nullptr, 0);
            if (gpu) uploadRegion((float)w);
            DrawInto(out, 0.5f, 0.0f, 0.5f, 1.0f, frameW + fx, ey, w, h);
            m_context->PSSetShader(m_ps, nullptr, 0);
        }
        if (gpu) uploadRegion((float)ew);
        DrawInto(out, 0.5f, 0.0f, 0.5f, 1.0f, frameW + ex, ey, ew, h);
        m_context->RSSetState(nullptr);
        if (gpu)
        {
            ID3D11ShaderResourceView* nullSRV = nullptr;
            m_context->VSSetShaderResources(1, 1, &nullSRV);
        }
    }

    // Unbind: the composite is about to be sampled by the weaver.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void RegionWeaver::SetScissor(const RECT& r)
{
    const D3D11_RECT sr{ r.left, r.top, r.right, r.bottom };
    m_context->RSSetScissorRects(1, &sr);
}
