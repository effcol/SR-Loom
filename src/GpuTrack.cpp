// GpuTrack.cpp -- see GpuTrack.h.
#include "GpuTrack.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // Keep in sync with GpuTracker::kSearch / kMaxRows.
    const char* kTrackHLSL = R"(
#define SEARCH   96
#define MAXROWS  1200
#define ROWSPAN  (MAXROWS + 2 * SEARCH)
#define MINSCORE 10.0      // mean |row difference| (0-255 luma) above this = no confident match

Texture2D<float>          luma    : register(t0);   // analysis image, 0..1
StructuredBuffer<float>   profile : register(t1);   // per slot: MAXROWS fingerprint rows
RWStructuredBuffer<float> rows    : register(u0);   // per slot: ROWSPAN row means (-1 = outside view)
RWStructuredBuffer<int4>  results : register(u1);   // per slot

cbuffer Job : register(b0)
{
    int4 baseRect;     // l t r b (analysis px)
    int4 viewRect;     // l t r b
    int  slot;
    int  h;            // rows in the fingerprint (= base height, <= MAXROWS)
    int  x0, x1;       // columns summed (base cols within the view)
    int  w;            // columns in the column fingerprint (<= MAXCOLS)
    int3 padJ;
};

groupshared float gsSum[64];

// One group per candidate row: its mean brightness over the picture's columns.
[numthreads(64, 1, 1)]
void CSRows(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int row = baseRect.y - SEARCH + (int)gid.x;
    const bool inView = row >= viewRect.y && row < viewRect.w && x1 > x0;
    float s = 0;
    if (inView)
        for (int x = x0 + (int)gi * 2; x < x1; x += 128) s += luma.Load(int3(x, row, 0));
    gsSum[gi] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 32; k > 0; k >>= 1)
    {
        if (gi < k) gsSum[gi] += gsSum[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        const float n = (float)((x1 - x0 + 1) / 2);
        rows[slot * ROWSPAN + gid.x] = inView ? gsSum[0] * 255.0 / max(n, 1.0) : -1.0;
    }
}

groupshared float gsScore[64];
groupshared int   gsOff[64];
groupshared int   gsN[64];

// One group: every offset in -SEARCH..+SEARCH, the best (lowest mean |diff|).
[numthreads(64, 1, 1)]
void CSSad(uint gi : SV_GroupIndex)
{
    float best = 1e9; int bestOff = 0; int bestN = 0;
    const int minN = max(4, (int)(h / 2 * 0.4));
    for (int d = (int)gi; d <= 2 * SEARCH; d += 64)
    {
        float s = 0; int n = 0;
        for (int y = 0; y < h; y += 2)
        {
            const float r = rows[slot * ROWSPAN + d + y];
            if (r < 0) continue;
            s += abs(r - profile[slot * MAXROWS + y]);
            ++n;
        }
        if (n >= minN)
        {
            const float sc = s / n;
            // ties: prefer the smaller movement
            if (sc < best || (sc == best && abs(d - SEARCH) < abs(bestOff))) { best = sc; bestOff = d - SEARCH; bestN = n; }
        }
    }
    gsScore[gi] = best; gsOff[gi] = bestOff; gsN[gi] = bestN;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 32; k > 0; k >>= 1)
    {
        if (gi < k)
        {
            const float o = gsScore[gi + k];
            if (o < gsScore[gi] || (o == gsScore[gi] && abs(gsOff[gi + k]) < abs(gsOff[gi])))
            {
                gsScore[gi] = o; gsOff[gi] = gsOff[gi + k]; gsN[gi] = gsN[gi + k];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
        results[slot] = int4(gsOff[0], gsScore[0] <= MINSCORE ? 1 : 0, 0, 0);   // .zw: sideways (CSSadX)
}

// ---- Sideways: column means at the found vertical position, then search ----
#define SEARCHX  48
#define MAXCOLS  2000
#define COLSPAN  (MAXCOLS + 2 * SEARCHX)
StructuredBuffer<float>   colProfile : register(t2);   // per slot: MAXCOLS fingerprint columns
RWStructuredBuffer<float> cols       : register(u2);   // per slot: COLSPAN column means (-1 = outside)


// One group per candidate column.
[numthreads(64, 1, 1)]
void CSCols(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int4 r = results[slot];
    const int dy = r.y ? r.x : 0;
    const int col = baseRect.x - SEARCHX + (int)gid.x;
    const int y0 = max(baseRect.y + dy, viewRect.y), y1 = min(baseRect.w + dy, viewRect.w);
    const bool inView = col >= viewRect.x && col < viewRect.z && y1 > y0;
    float s = 0;
    if (inView)
        for (int y = y0 + (int)gi * 2; y < y1; y += 128) s += luma.Load(int3(col, y, 0));
    gsSum[gi] = s;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 32; k > 0; k >>= 1)
    {
        if (gi < k) gsSum[gi] += gsSum[gi + k];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        const float n = (float)((y1 - y0 + 1) / 2);
        cols[slot * COLSPAN + gid.x] = inView ? gsSum[0] * 255.0 / max(n, 1.0) : -1.0;
    }
}

[numthreads(64, 1, 1)]
void CSSadX(uint gi : SV_GroupIndex)
{
    float best = 1e9; int bestOff = 0;
    const int minN = max(4, (int)(w / 2 * 0.4));
    for (int d = (int)gi; d <= 2 * SEARCHX; d += 64)
    {
        float s = 0; int n = 0;
        for (int x = 0; x < w; x += 2)
        {
            const float c = cols[slot * COLSPAN + d + x];
            if (c < 0) continue;
            s += abs(c - colProfile[slot * MAXCOLS + x]);
            ++n;
        }
        if (n >= minN)
        {
            const float sc = s / n;
            if (sc < best || (sc == best && abs(d - SEARCHX) < abs(bestOff))) { best = sc; bestOff = d - SEARCHX; }
        }
    }
    gsScore[gi] = best; gsOff[gi] = bestOff;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint k = 32; k > 0; k >>= 1)
    {
        if (gi < k)
        {
            const float o = gsScore[gi + k];
            if (o < gsScore[gi] || (o == gsScore[gi] && abs(gsOff[gi + k]) < abs(gsOff[gi])))
            { gsScore[gi] = o; gsOff[gi] = gsOff[gi + k]; }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        int4 r = results[slot];
        r.z = gsOff[0];
        r.w = (r.y && gsScore[0] <= MINSCORE) ? 1 : 0;
        results[slot] = r;
    }
}
)";

    struct JobCB
    {
        int baseRect[4];
        int viewRect[4];
        int slot, h, x0, x1;
        int w, pad[3];
    };
}

GpuTracker::~GpuTracker() { Shutdown(); }

bool GpuTracker::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device;
    m_context = context;
    ID3DBlob* b1 = nullptr; ID3DBlob* b2 = nullptr; ID3DBlob* err = nullptr;
    const size_t len = std::strlen(kTrackHLSL);
    if (FAILED(D3DCompile(kTrackHLSL, len, "GpuTrack", nullptr, nullptr, "CSRows", "cs_5_0", 0, 0, &b1, &err)) ||
        FAILED(D3DCompile(kTrackHLSL, len, "GpuTrack", nullptr, nullptr, "CSSad",  "cs_5_0", 0, 0, &b2, &err)))
    {
        Log("GpuTracker: compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err); SafeRelease(b1); SafeRelease(b2);
        return false;
    }
    m_device->CreateComputeShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, &m_csRows);
    m_device->CreateComputeShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &m_csSad);
    {
        ID3DBlob* b3 = nullptr; ID3DBlob* b4 = nullptr;
        if (SUCCEEDED(D3DCompile(kTrackHLSL, len, "GpuTrack", nullptr, nullptr, "CSCols", "cs_5_0", 0, 0, &b3, &err)) &&
            SUCCEEDED(D3DCompile(kTrackHLSL, len, "GpuTrack", nullptr, nullptr, "CSSadX", "cs_5_0", 0, 0, &b4, &err)))
        {
            m_device->CreateComputeShader(b3->GetBufferPointer(), b3->GetBufferSize(), nullptr, &m_csCols);
            m_device->CreateComputeShader(b4->GetBufferPointer(), b4->GetBufferSize(), nullptr, &m_csSadX);
        }
        else Log("GpuTracker: sideways compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(b3); SafeRelease(b4); SafeRelease(err);
    }
    SafeRelease(b1); SafeRelease(b2);

    auto structured = [&](UINT count, UINT stride, UINT bind, ID3D11Buffer** out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth           = count * stride;
        bd.Usage               = D3D11_USAGE_DEFAULT;
        bd.BindFlags           = bind;
        bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = stride;
        return SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, out));
    };
    const UINT rowSpan = kMaxRows + 2 * kSearch;
    bool ok = structured(kMaxSlots * kMaxRows, 4, D3D11_BIND_SHADER_RESOURCE, &m_profile) &&
              structured(kMaxSlots * rowSpan, 4, D3D11_BIND_UNORDERED_ACCESS, &m_rows) &&
              structured(kMaxSlots, 16, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &m_res) &&
              structured(kMaxSlots * kMaxCols, 4, D3D11_BIND_SHADER_RESOURCE, &m_colProfile) &&
              structured(kMaxSlots * (kMaxCols + 2 * kSearchX), 4, D3D11_BIND_UNORDERED_ACCESS, &m_cols);
    if (ok)
    {
        ok = SUCCEEDED(m_device->CreateShaderResourceView(m_profile, nullptr, &m_profileSRV)) &&
             SUCCEEDED(m_device->CreateUnorderedAccessView(m_rows, nullptr, &m_rowsUAV)) &&
             SUCCEEDED(m_device->CreateUnorderedAccessView(m_res, nullptr, &m_resUAV)) &&
             SUCCEEDED(m_device->CreateShaderResourceView(m_res, nullptr, &m_resSRV)) &&
             SUCCEEDED(m_device->CreateShaderResourceView(m_colProfile, nullptr, &m_colProfileSRV)) &&
             SUCCEEDED(m_device->CreateUnorderedAccessView(m_cols, nullptr, &m_colsUAV));
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth      = sizeof(JobCB);
    cb.Usage          = D3D11_USAGE_DYNAMIC;
    cb.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(m_device->CreateBuffer(&cb, nullptr, &m_cb));
    if (!ok || !m_csRows || !m_csSad || !m_csCols || !m_csSadX) { Log("GpuTracker: resource creation failed"); Shutdown(); return false; }
    Log("GpuTracker: ready (%d slots, +/-%d rows)", kMaxSlots, kSearch);
    return true;
}

void GpuTracker::Shutdown()
{
    SafeRelease(m_resSRV); SafeRelease(m_resUAV); SafeRelease(m_res);
    SafeRelease(m_colsUAV); SafeRelease(m_cols); SafeRelease(m_colProfileSRV); SafeRelease(m_colProfile);
    SafeRelease(m_csSadX); SafeRelease(m_csCols);
    SafeRelease(m_rowsUAV); SafeRelease(m_rows);
    SafeRelease(m_profileSRV); SafeRelease(m_profile);
    SafeRelease(m_cb); SafeRelease(m_csSad); SafeRelease(m_csRows);
    m_device = nullptr; m_context = nullptr;
}

void GpuTracker::Run(ID3D11ShaderResourceView* luma, const std::vector<GpuTrackJob>& jobs)
{
    if (!m_csRows || !luma) return;
    const UINT zero[4] = { 0, 0, 0, 0 };
    m_context->ClearUnorderedAccessViewUint(m_resUAV, zero);   // unused slots: "no result"

    ID3D11ShaderResourceView* srvs[3] = { luma, m_profileSRV, m_colProfileSRV };
    ID3D11UnorderedAccessView* uavs[3] = { m_rowsUAV, m_resUAV, m_colsUAV };
    for (const GpuTrackJob& j : jobs)
    {
        if (j.slot < 0 || j.slot >= kMaxSlots || !j.profile) continue;
        const int h = (std::min)((int)j.profile->size(), kMaxRows);
        if (h < 8) continue;
        // Fingerprint rows for this slot.
        D3D11_BOX box{ (UINT)(j.slot * kMaxRows * 4), 0, 0, (UINT)((j.slot * kMaxRows + h) * 4), 1, 1 };
        m_context->UpdateSubresource(m_profile, 0, &box, j.profile->data(), 0, 0);
        const int w = j.colProfile ? (std::min)((int)j.colProfile->size(), kMaxCols) : 0;
        if (w >= 8)
        {
            D3D11_BOX cbx{ (UINT)(j.slot * kMaxCols * 4), 0, 0, (UINT)((j.slot * kMaxCols + w) * 4), 1, 1 };
            m_context->UpdateSubresource(m_colProfile, 0, &cbx, j.colProfile->data(), 0, 0);
        }

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(m_context->Map(m_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
        JobCB* c = static_cast<JobCB*>(m.pData);
        c->baseRect[0] = j.base.left; c->baseRect[1] = j.base.top; c->baseRect[2] = j.base.right; c->baseRect[3] = j.base.bottom;
        c->viewRect[0] = j.view.left; c->viewRect[1] = j.view.top; c->viewRect[2] = j.view.right; c->viewRect[3] = j.view.bottom;
        c->slot = j.slot;
        c->h    = h;
        c->x0   = (std::max)(j.base.left, j.view.left);
        c->x1   = (std::min)(j.base.right, j.view.right);
        c->w    = w;
        m_context->Unmap(m_cb, 0);

        m_context->CSSetShaderResources(0, 3, srvs);
        m_context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        m_context->CSSetConstantBuffers(0, 1, &m_cb);
        m_context->CSSetShader(m_csRows, nullptr, 0);
        m_context->Dispatch((UINT)(h + 2 * kSearch), 1, 1);
        m_context->CSSetShader(m_csSad, nullptr, 0);
        m_context->Dispatch(1, 1, 1);
        // Sideways, at the vertical position just found (read on the GPU).
        if (w >= 8)
        {
            m_context->CSSetShader(m_csCols, nullptr, 0);
            m_context->Dispatch((UINT)(w + 2 * kSearchX), 1, 1);
            m_context->CSSetShader(m_csSadX, nullptr, 0);
            m_context->Dispatch(1, 1, 1);
        }
    }
    ID3D11ShaderResourceView* nullSrv[3] = {};
    ID3D11UnorderedAccessView* nullUav[3] = {};
    m_context->CSSetShaderResources(0, 3, nullSrv);
    m_context->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
    m_context->CSSetShader(nullptr, nullptr, 0);
}
