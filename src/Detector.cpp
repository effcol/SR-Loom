#include "Detector.h"
#include <d3dcompiler.h>
#include <vector>
#include <cmath>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    // Fullscreen triangle + 1:1 sample — used to bilinear-downscale the source.
    const char* kCopyShader = R"HLSL(
Texture2D    srcTex : register(t0);
SamplerState samp   : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
float4 PSMain(VSOut i) : SV_Target { return srcTex.Sample(samp, i.uv); }
)HLSL";

    inline float Luma(const unsigned char* p) // p -> BGRA or RGBA; luma is channel-order agnostic enough
    {
        return (0.299f * p[2] + 0.587f * p[1] + 0.114f * p[0]) / 255.0f;
    }
}

Detector::~Detector() { Shutdown(); }

bool Detector::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device = device; m_context = context;

    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kCopyShader, strlen(kCopyShader), "srw_detect", nullptr, nullptr,
                          "VSMain", "vs_5_0", 0, 0, &vs, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(kCopyShader, strlen(kCopyShader), "srw_detect", nullptr, nullptr,
                          "PSMain", "ps_5_0", 0, 0, &ps, &err))) { if (err) err->Release(); SAFE_RELEASE(vs); return false; }
    HRESULT hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
    SAFE_RELEASE(vs); SAFE_RELEASE(ps);
    if (FAILED(hr)) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &m_sampler);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kSize; td.Height = kSize; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_rt))) return false;
    if (FAILED(device->CreateRenderTargetView(m_rt, nullptr, &m_rtv))) return false;

    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_staging))) return false;

    return m_vs && m_ps && m_sampler && m_rt && m_rtv && m_staging;
}

bool Detector::Detect(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight,
                      StereoFormat& outFormat)
{
    if (!source || !m_rtv || srcWidth <= 0 || srcHeight <= 0)
        return false;

    // Downscale the source into the small render target.
    D3D11_VIEWPORT vp{}; vp.Width = (FLOAT)kSize; vp.Height = (FLOAT)kSize; vp.MaxDepth = 1.0f;
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(m_ps, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetShaderResources(0, 1, &source);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);

    // Read it back.
    m_context->CopyResource(m_staging, m_rt);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_context->Map(m_staging, 0, D3D11_MAP_READ, 0, &map)))
        return false;

    const int N = kSize, half = kSize / 2;
    auto px = [&](int x, int y) -> const unsigned char*
    {
        return (const unsigned char*)map.pData + (size_t)y * map.RowPitch + (size_t)x * 4;
    };

    // SBS: left half vs right half. TAB: top half vs bottom half.
    double sbs = 0, tab = 0, ana = 0;
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < half; ++x)
            sbs += std::fabs(Luma(px(x, y)) - Luma(px(x + half, y)));
    sbs /= (double)half * N;

    for (int y = 0; y < half; ++y)
        for (int x = 0; x < N; ++x)
            tab += std::fabs(Luma(px(x, y)) - Luma(px(x, y + half)));
    tab /= (double)half * N;

    // Anaglyph: red channel differs from cyan (G/B) — measure red-vs-cyan disparity.
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x)
        {
            const unsigned char* p = px(x, y);            // BGRA
            float r = p[2] / 255.0f, cyan = (p[1] + p[0]) / 510.0f;
            ana += std::fabs(r - cyan);
        }
    ana /= (double)N * N;

    m_context->Unmap(m_staging, 0);

    Log("Detector: sbs=%.3f tab=%.3f ana=%.3f", sbs, tab, ana);

    // Decide. Stereo halves are very similar (low diff); a strong red/cyan
    // disparity with no half-symmetry suggests anaglyph.
    const double kHalfThresh = 0.11;
    if (sbs < tab && sbs < kHalfThresh) { outFormat = StereoFormat::FullSBS; return true; }
    if (tab < sbs && tab < kHalfThresh) { outFormat = StereoFormat::FullTAB; return true; }
    if (ana > 0.18)                     { outFormat = StereoFormat::Anaglyph; return true; }
    return false;   // no confident stereo layout
}

void Detector::Shutdown()
{
    SAFE_RELEASE(m_staging);
    SAFE_RELEASE(m_rtv);
    SAFE_RELEASE(m_rt);
    SAFE_RELEASE(m_sampler);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
