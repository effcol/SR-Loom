#include "Converter.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    // Fullscreen-triangle VS + a PS that samples the source per stereo layout and
    // writes the left half / right half of the SBS output.
    const char* kShaderSrc = R"HLSL(
Texture2D    srcTex : register(t0);
SamplerState samp   : register(s0);

cbuffer Params : register(b0)
{
    int   g_format;   // 0 SBS, 1 TAB, 2 Anaglyph, 3 RowInterleaved, 4 ColInterleaved, 5 Checkerboard
    int   g_swap;     // swap left/right eyes
    float g_srcW;
    float g_srcH;
};

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);   // (0,0)(2,0)(0,2)
    o.uv  = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float2 uv = i.uv;                       // 0..1 across the SBS output
    bool right = uv.x >= 0.5;
    if (g_swap) right = !right;
    float2 e = float2(right ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);  // within-eye 0..1

    if (g_format == 1)        // Top-and-bottom: top=left, bottom=right
    {
        float2 s = float2(e.x, right ? 0.5 + e.y * 0.5 : e.y * 0.5);
        return srcTex.Sample(samp, s);
    }
    else if (g_format == 3)   // Row interleaved: even rows=left, odd=right
    {
        float row = floor(e.y * (g_srcH * 0.5)) * 2.0 + (right ? 1.0 : 0.0);
        return srcTex.Sample(samp, float2(e.x, (row + 0.5) / g_srcH));
    }
    else if (g_format == 4)   // Column interleaved: even cols=left, odd=right
    {
        float col = floor(e.x * (g_srcW * 0.5)) * 2.0 + (right ? 1.0 : 0.0);
        return srcTex.Sample(samp, float2((col + 0.5) / g_srcW, e.y));
    }
    else if (g_format == 5)   // Checkerboard
    {
        float2 sz = float2(g_srcW, g_srcH);
        float2 p  = floor(e * sz);
        int parity = ((int)p.x + (int)p.y) & 1;
        if (parity == (right ? 1 : 0))
            return srcTex.Sample(samp, (p + 0.5) / sz);
        // wanted parity is the other texel; average horizontal neighbours
        float4 a = srcTex.Sample(samp, (float2(p.x + 1, p.y) + 0.5) / sz);
        float4 b = srcTex.Sample(samp, (float2(p.x - 1, p.y) + 0.5) / sz);
        return 0.5 * (a + b);
    }
    else if (g_format == 2)   // Anaglyph (red/cyan), approximate decode
    {
        float4 c = srcTex.Sample(samp, e);
        if (!right) return float4(c.r, c.r, c.r, 1);          // left eye from red
        float gray = (c.g + c.b) * 0.5;
        return float4(gray, gray, gray, 1);                   // right eye from cyan
    }

    // Default: side-by-side. left=left half, right=right half.
    float2 s = float2(right ? 0.5 + e.x * 0.5 : e.x * 0.5, e.y);
    return srcTex.Sample(samp, s);
}
)HLSL";

    int FormatCode(StereoFormat f)
    {
        switch (f)
        {
        case StereoFormat::FullTAB:
        case StereoFormat::HalfTAB:           return 1;
        case StereoFormat::Anaglyph:          return 2;
        case StereoFormat::RowInterleaved:    return 3;
        case StereoFormat::ColumnInterleaved: return 4;
        case StereoFormat::Checkerboard:      return 5;
        default:                              return 0;  // SBS (and unimplemented)
        }
    }

    // Per-eye dimensions produced from a source of (w,h) in the given layout.
    void PerEyeSize(StereoFormat f, int w, int h, int& ew, int& eh)
    {
        switch (f)
        {
        case StereoFormat::FullSBS:
        case StereoFormat::HalfSBS:           ew = w / 2; eh = h;     break;
        case StereoFormat::FullTAB:
        case StereoFormat::HalfTAB:
        case StereoFormat::RowInterleaved:    ew = w;     eh = h / 2; break;
        case StereoFormat::ColumnInterleaved: ew = w / 2; eh = h;     break;
        default:                              ew = w;     eh = h;     break; // anaglyph/checker
        }
        if (ew < 1) ew = 1;
        if (eh < 1) eh = 1;
    }

    struct CB { int format; int swap; float srcW; float srcH; };
}

Converter::~Converter()
{
    Shutdown();
}

bool Converter::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device  = device;
    m_context = context;

    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr; ID3DBlob* err = nullptr;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, &vsBlob, &err);
    if (FAILED(hr)) { if (err) err->Release(); ShowError("Converter VS compile failed."); return false; }
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, &psBlob, &err);
    if (FAILED(hr)) { if (err) err->Release(); SAFE_RELEASE(vsBlob); ShowError("Converter PS compile failed."); return false; }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);
    SAFE_RELEASE(vsBlob); SAFE_RELEASE(psBlob);
    if (FAILED(hr)) { ShowError("Converter shader creation failed."); return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &m_sampler);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CB);
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&bd, nullptr, &m_cbuffer);

    return m_vs && m_ps && m_sampler && m_cbuffer;
}

void Converter::SetFormat(StereoFormat fmt, bool swapEyes)
{
    m_fmt  = fmt;
    m_swap = swapEyes;
}

bool Converter::EnsureOutput(int width, int height)
{
    if (m_outTex && width == m_outWidth && height == m_outHeight)
        return false;

    ReleaseOutput();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_format;   // sRGB; both RTV and SRV use it directly
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_outTex))) return false;
    if (FAILED(m_device->CreateRenderTargetView(m_outTex, nullptr, &m_outRTV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_outTex, nullptr, &m_outSRV))) { ReleaseOutput(); return false; }

    m_outWidth  = width;
    m_outHeight = height;
    return true;
}

bool Converter::Convert(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight,
                        bool& outputResized)
{
    outputResized = false;
    if (!source || !m_ps || srcWidth <= 0 || srcHeight <= 0)
        return false;

    int ew = 0, eh = 0;
    PerEyeSize(m_fmt, srcWidth, srcHeight, ew, eh);
    outputResized = EnsureOutput(ew * 2, eh);
    if (!m_outRTV)
        return false;

    CB cb{ FormatCode(m_fmt), m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight };
    m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &cb, 0, 0);

    D3D11_VIEWPORT vp{};
    vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)m_outHeight; vp.MaxDepth = 1.0f;

    m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
    m_context->RSSetViewports(1, &vp);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(m_ps, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetShaderResources(0, 1, &source);
    m_context->PSSetConstantBuffers(0, 1, &m_cbuffer);
    m_context->Draw(3, 0);

    // Unbind the source so it can be reused as a render target elsewhere.
    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSRV);
    return true;
}

void Converter::ReleaseOutput()
{
    SAFE_RELEASE(m_outSRV);
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outTex);
    m_outWidth = m_outHeight = 0;
}

void Converter::Shutdown()
{
    ReleaseOutput();
    SAFE_RELEASE(m_cbuffer);
    SAFE_RELEASE(m_sampler);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
