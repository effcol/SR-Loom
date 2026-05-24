#include "Converter.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using namespace srw;

namespace
{
    // Fullscreen-triangle VS + a PS that samples the source per stereo layout and
    // writes the left half / right half of the SBS output.
    const char* kShaderSrc = R"HLSL(
Texture2D    srcTex  : register(t0);   // current source frame
Texture2D    srcPrev : register(t1);   // delayed source frame (Pulfrich time delay)
Texture2D    dispTex : register(t2);   // coarse L<->R disparity map (UV units): .r=dLR .g=dRL
SamplerState samp    : register(s0);

cbuffer Params : register(b0)
{
    int   g_format;   // 0 SBS,1 TAB,2 Anaglyph,3 Row,4 Col,5 Checker,6 Pulfrich,99 copy
    int   g_swap;     // swap left/right eyes
    float g_srcW;
    float g_srcH;
    int   g_anaCombo; // 0..5 colour combination
    int   g_anaMode;  // 0 colour-filtered, 1 half-colour, 2 mono
    int   g_pulfMode; // 0 time-delay, 1 ND filter
    int   g_pulfEye;  // affected eye: 0 left, 1 right
    float g_ndTrans;  // ND transmission (affected eye brightness)
    float g_fpEyeFrac; // frame packing: each eye's fraction of the source height
    float g_fpGapFrac; // frame packing: blanking gap fraction
    float g_pad2;
    float g_dispMaxUV; // anaglyph recovery: max search disparity (fraction of width)
    float g_coarseW;   // coarse disparity-map width (px)
    float g_coarseH;   // coarse disparity-map height (px)
    float g_pad3;
};

// Channel-filtered colour for one eye of an anaglyph combo (left: e=0, right: e=1).
float3 anaFilter(float3 c, int combo, int e)
{
    if (combo == 0) return (e == 0) ? float3(c.r, 0, 0)   : float3(0, c.g, c.b); // Red/Cyan
    if (combo == 1) return (e == 0) ? float3(c.r, 0, 0)   : float3(0, c.g, 0);   // Red/Green
    if (combo == 2) return (e == 0) ? float3(c.r, 0, 0)   : float3(0, 0, c.b);   // Red/Blue
    if (combo == 3) return (e == 0) ? float3(0, c.g, 0)   : float3(c.r, 0, c.b); // Green/Magenta
    if (combo == 4) return (e == 0) ? float3(c.r, c.g, 0) : float3(0, 0, c.b);   // Amber/Blue
    return                (e == 0) ? float3(0, c.g, c.b) : float3(c.r, 0, c.b);  // Cyan/Magenta
}

// Per-eye luminance from an anaglyph combo (carries that eye's view / disparity).
float anaEyeLuma(float3 c, int combo, int e)
{
    if (combo == 0) return (e == 0) ? c.r : (c.g + c.b) * 0.5; // Red/Cyan
    if (combo == 1) return (e == 0) ? c.r : c.g;               // Red/Green
    if (combo == 2) return (e == 0) ? c.r : c.b;               // Red/Blue
    if (combo == 3) return (e == 0) ? c.g : (c.r + c.b) * 0.5; // Green/Magenta
    if (combo == 4) return (e == 0) ? (c.r + c.g) * 0.5 : c.b; // Amber/Blue
    return                (e == 0) ? (c.g + c.b) * 0.5 : (c.r + c.b) * 0.5; // Cyan/Magenta
}

float3 decodeAnaglyph(float3 c, int combo, int e, int mode)
{
    if (mode == 0)   // Shared colour: per-eye luminance, shared anaglyph chrominance.
    {
        float anaY = max(dot(c, float3(0.299, 0.587, 0.114)), 1e-3);
        float eyeY = anaEyeLuma(c, combo, e);
        return saturate(c * (eyeY / anaY));   // same hue both eyes, eye-specific brightness
    }

    float3 col = anaFilter(c, combo, e);
    float  g   = dot(col, float3(0.299, 0.587, 0.114));
    if (mode == 2) return lerp(col, g.xxx, 0.5);   // half colour
    if (mode == 3) return g.xxx;                   // mono
    return col;                                    // mode 1: colour (filtered)
}

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);   // (0,0)(2,0)(0,2)
    o.uv  = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// Coarse disparity pass (red/cyan anaglyph): renders a low-resolution map of the
// horizontal disparity between the L (red) and R (cyan = (g+b)/2) views, in both
// directions. Low resolution makes winner-take-all matching robust and smooth.
// Output: .r = dLR (left->right), .g = dRL (right->left), in UV (fraction of width).
float4 PSAnaDisp(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  ctx = 1.0 / max(g_coarseW, 1.0);   // one coarse texel in UV (match window step)
    float  maxD = g_dispMaxUV;
    const int N = 24;                          // search steps each direction
    const int W = 2;                           // window half-width (5 taps)
    float  step = maxD / (float)N;

    // Reference windows (L=red, R=cyan luma) at uv, plus their means. ZERO-MEAN
    // matching removes the per-channel brightness offset, so red can be matched
    // against cyan by STRUCTURE even where the two views differ in colour.
    float refL[5], refR[5], mRefL = 0, mRefR = 0;
    [unroll] for (int w = -W; w <= W; ++w)
    {
        float3 s = srcTex.SampleLevel(samp, uv + float2((float)w * ctx, 0), 0).rgb;
        float rcy = (s.g + s.b) * 0.5;
        refL[w + W] = s.r; refR[w + W] = rcy;
        mRefL += s.r; mRefR += rcy;
    }
    mRefL *= 0.2; mRefR *= 0.2;

    float bestL = 1e9, dL = 0.0, bestR = 1e9, dR = 0.0;
    [loop] for (int k = -N; k <= N; ++k)
    {
        float d = (float)k * step;
        float cL[5], cR[5], mcL = 0, mcR = 0;
        [unroll] for (int w = -W; w <= W; ++w)
        {
            float3 s = srcTex.SampleLevel(samp, uv + float2(d + (float)w * ctx, 0), 0).rgb;
            float rcy = (s.g + s.b) * 0.5;
            cL[w + W] = s.r; cR[w + W] = rcy;
            mcL += s.r; mcR += rcy;
        }
        mcL *= 0.2; mcR *= 0.2;
        float bias = abs(d) / max(maxD, 1e-4) * 0.06;   // gentle small-disparity tie-break
        float sadL = bias, sadR = bias;
        [unroll] for (int w = 0; w < 5; ++w)
        {
            sadL += abs((refL[w] - mRefL) - (cR[w] - mcR));   // L red vs candidate cyan
            sadR += abs((refR[w] - mRefR) - (cL[w] - mcL));   // R cyan vs candidate red
        }
        if (sadL < bestL) { bestL = sadL; dL = d; }
        if (sadR < bestR) { bestR = sadR; dR = d; }
    }
    return float4(dL, dR, 0, 0);
}

float4 PSMain(VSOut i) : SV_Target
{
    if (g_format == 99) return srcTex.Sample(samp, i.uv);   // 1:1 copy (history blit)

    float2 uv = i.uv;                       // 0..1 across the SBS output
    bool rightPane = uv.x >= 0.5;           // which output half we're filling
    // within-pane 0..1 (from the OUTPUT pane, always valid)
    float2 e = float2(rightPane ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);
    // which eye's content goes into this pane (eye swap only affects content)
    bool right = rightPane;
    if (g_swap) right = !right;

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
    else if (g_format == 2)   // Anaglyph: decode per combo + mode
    {
        int eye = right ? 1 : 0;
        float3 c = srcTex.Sample(samp, e).rgb;

        // Multi-scale aligned recovery (red/cyan only). Reads the coarse disparity
        // map (PSAnaDisp), refines it at full resolution, checks left-right
        // consistency to flag occlusions, then borrows only the disparity-aligned
        // CHROMA (each eye keeps its own sharp luminance) -> de-fringed full colour.
        if (g_anaMode == 4 && g_anaCombo == 0)
        {
            float px = 1.0 / g_srcW;
            float2 disp = dispTex.SampleLevel(samp, e, 0.0).rg;   // (dLR, dRL) in UV
            float d0 = (eye == 0) ? disp.r : disp.g;              // this eye -> the other

            // Reference window (3-tap) + mean for zero-mean matching (same cross-
            // channel robustness as the coarse pass).
            float refY[3], mRef = 0;
            [unroll] for (int w = -1; w <= 1; ++w)
            {
                float3 cc = srcTex.SampleLevel(samp, float2(e.x + (float)w * px, e.y), 0.0).rgb;
                refY[w + 1] = (eye == 0) ? cc.r : (cc.g + cc.b) * 0.5;
                mRef += refY[w + 1];
            }
            mRef /= 3.0;

            // Full-res refine: small local search around the coarse estimate.
            float bestSAD = 1e9, dRef = d0;
            [loop] for (int r = -3; r <= 3; ++r)
            {
                float d = d0 + (float)r * px;
                float cand[3], mc = 0;
                [unroll] for (int w = -1; w <= 1; ++w)
                {
                    float3 cb = srcTex.SampleLevel(samp, float2(e.x + d + (float)w * px, e.y), 0.0).rgb;
                    cand[w + 1] = (eye == 0) ? (cb.g + cb.b) * 0.5 : cb.r;
                    mc += cand[w + 1];
                }
                mc /= 3.0;
                float sad = 0.0;
                [unroll] for (int w = 0; w < 3; ++w) sad += abs((refY[w] - mRef) - (cand[w] - mc));
                if (sad < bestSAD) { bestSAD = sad; dRef = d; }
            }

            // Left-right consistency: the reverse disparity at the matched point
            // should cancel ours; if not, this pixel is occluded -> low confidence.
            float2 d2  = dispTex.SampleLevel(samp, float2(e.x + dRef, e.y), 0.0).rg;
            float  back = (eye == 0) ? d2.g : d2.r;
            float  consistency = saturate(1.0 - abs(dRef + back) / max(g_dispMaxUV, 1e-4) * 4.0);

            float eyeY = anaEyeLuma(c, g_anaCombo, eye);            // own sharp luminance
            float3 there = srcTex.SampleLevel(samp, float2(e.x + dRef, e.y), 0.0).rgb;
            float3 alignedCol = (eye == 0) ? float3(c.r, there.g, there.b)
                                           : float3(there.r, c.g, c.b);
            float aY = max(dot(alignedCol, float3(0.299, 0.587, 0.114)), 1e-3);
            float3 aligned = saturate(alignedCol * (eyeY / aY));

            // Occlusion / low-confidence fallback: shared, horizontally-blurred chroma.
            float3 acc = 0;
            [unroll] for (int k = -4; k <= 4; ++k)
                acc += srcTex.SampleLevel(samp, float2(e.x + (float)k * px, e.y), 0.0).rgb;
            float3 blurCol = acc / 9.0;
            float bY = max(dot(blurCol, float3(0.299, 0.587, 0.114)), 1e-3);
            float3 sharedCol = saturate(blurCol * (eyeY / bY));

            float conf = saturate(1.0 - bestSAD * 1.5) * consistency;
            return float4(lerp(sharedCol, aligned, conf), 1);
        }

        if (g_anaMode == 0 || g_anaMode == 4) // Recovered colour: per-eye luminance + shared,
        {                                      // horizontally blurred chrominance (reduces fringing).
            float eyeY = anaEyeLuma(c, g_anaCombo, eye);   // sharp per-eye luminance
            float3 acc = 0;
            [unroll] for (int k = -4; k <= 4; ++k)
                acc += srcTex.Sample(samp, float2(e.x + (float)k / g_srcW, e.y)).rgb;
            float3 cb = acc / 9.0;                         // horizontally blurred colour
            float anaY = max(dot(cb, float3(0.299, 0.587, 0.114)), 1e-3);
            return float4(saturate(cb * (eyeY / anaY)), 1);
        }
        return float4(decodeAnaglyph(c, g_anaCombo, eye, g_anaMode), 1);
    }
    else if (g_format == 6)   // Pulfrich: mono source -> per-eye delay / ND darken
    {
        float3 cur = srcTex.Sample(samp, e).rgb;
        int eyeIdx = right ? 1 : 0;
        if (eyeIdx != g_pulfEye) return float4(cur, 1);          // unaffected eye = current
        if (g_pulfMode == 1)    return float4(cur * g_ndTrans, 1); // ND: darken this eye
        return float4(srcPrev.Sample(samp, e).rgb, 1);           // time delay: older frame
    }
    // Frame-packing decode informed by 3DToElse / 3DToElse_NTM3D
    // (CC BY 3.0, Jose Negrete "BlueSkyDefender" + NTM3D); re-implemented here.
    else if (g_format == 7)   // HDMI 1.4 frame packing: top eye, gap, bottom eye
    {
        float v = right ? (g_fpEyeFrac + g_fpGapFrac + e.y * g_fpEyeFrac)
                        : (e.y * g_fpEyeFrac);
        return srcTex.Sample(samp, float2(e.x, v));
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
        case StereoFormat::Pulfrich:          return 6;
        case StereoFormat::FramePacking:      return 7;
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
        default:                              ew = w;     eh = h;     break; // anaglyph/checker/pulfrich
        }
        if (ew < 1) ew = 1;
        if (eh < 1) eh = 1;
    }

    struct CB { int format; int swap; float srcW; float srcH;
               int anaCombo; int anaMode; int pulfMode; int pulfEye;
               float ndTrans; float fpEyeFrac; float fpGapFrac; float pad2;
               float dispMaxUV; float coarseW; float coarseH; float pad3; };
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
    auto reportCompileError = [](const char* stage, ID3DBlob* errBlob) {
        std::string msg = std::string("Converter ") + stage + " compile failed.";
        if (errBlob && errBlob->GetBufferSize() > 0)
        {
            msg += "\n\n";
            msg.append(reinterpret_cast<const char*>(errBlob->GetBufferPointer()),
                       errBlob->GetBufferSize());
        }
        Log("%s", msg.c_str());
        ShowError(msg.c_str());
    };

    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                            "VSMain", "vs_5_0", flags, 0, &vsBlob, &err);
    if (FAILED(hr)) { reportCompileError("VS", err); if (err) err->Release(); return false; }
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                    "PSMain", "ps_5_0", flags, 0, &psBlob, &err);
    if (FAILED(hr)) { reportCompileError("PS", err); if (err) err->Release(); SAFE_RELEASE(vsBlob); return false; }

    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (SUCCEEDED(hr))
        hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);
    SAFE_RELEASE(vsBlob); SAFE_RELEASE(psBlob);
    if (FAILED(hr)) { ShowError("Converter shader creation failed."); return false; }

    // Coarse disparity pass (used only by the multi-scale anaglyph recovery mode).
    ID3DBlob* csBlob = nullptr;
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                    "PSAnaDisp", "ps_5_0", flags, 0, &csBlob, &err);
    if (FAILED(hr)) { reportCompileError("PSAnaDisp", err); if (err) err->Release(); return false; }
    hr = device->CreatePixelShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &m_psCoarse);
    SAFE_RELEASE(csBlob);
    if (FAILED(hr)) { ShowError("Converter coarse shader creation failed."); return false; }

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

void Converter::SetFormat(StereoFormat fmt, bool swapEyes, int anaCombo, int anaMode)
{
    m_fmt      = fmt;
    m_swap     = swapEyes;
    m_anaCombo = anaCombo;
    m_anaMode  = anaMode;
}

void Converter::SetPulfrich(PulfrichMode mode, int affectedEye, float ndTransmission, int delayFrames)
{
    m_pulfMode  = mode;
    m_pulfEye   = affectedEye;
    m_ndTrans   = ndTransmission;
    m_pulfDelay = delayFrames < 1 ? 1 : (delayFrames > kHistory - 1 ? kHistory - 1 : delayFrames);
}

void Converter::SetFramePacking(float eyeFrac, float gapFrac)
{
    m_fpEyeFrac = eyeFrac;
    m_fpGapFrac = gapFrac;
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

    const bool pulfrich = (m_fmt == StereoFormat::Pulfrich);
    if (!pulfrich) ReleaseHistory();   // free the ring when not needed

    int ew = 0, eh = 0;
    PerEyeSize(m_fmt, srcWidth, srcHeight, ew, eh);
    if (m_fmt == StereoFormat::FramePacking)   // each eye is eyeFrac of the source height
        eh = (int)(srcHeight * m_fpEyeFrac + 0.5f);
    if (eh < 1) eh = 1;
    outputResized = EnsureOutput(ew * 2, eh);
    if (!m_outRTV)
        return false;

    ID3D11ShaderResourceView* delayedSRV = nullptr;
    if (pulfrich)
    {
        EnsureHistory(srcWidth, srcHeight);
        const int delayed = (m_histWrite - m_pulfDelay + kHistory) % kHistory;
        delayedSRV = m_histSRV[delayed];
    }

    // Multi-scale anaglyph recovery needs a coarse disparity map first.
    const bool anaRecover = (m_fmt == StereoFormat::Anaglyph && m_anaMode == 4 && m_anaCombo == 0);
    int cw = 0, ch = 0;
    if (anaRecover)
    {
        cw = srcWidth  / 4; if (cw < 1) cw = 1;
        ch = srcHeight / 4; if (ch < 1) ch = 1;
        EnsureDisparity(cw, ch);
    }
    else ReleaseDisparity();

    CB cb{ FormatCode(m_fmt), m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
           m_anaCombo, m_anaMode, (int)m_pulfMode, m_pulfEye,
           m_ndTrans, m_fpEyeFrac, m_fpGapFrac, 0,
           0.06f /*dispMaxUV*/, (float)cw, (float)ch, 0 };
    m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &cb, 0, 0);

    // Shared pipeline state.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cbuffer);

    // Pass 0 (anaglyph recovery): render the coarse L<->R disparity map.
    if (anaRecover && m_dispRTV)
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_dispW; vp.Height = (FLOAT)m_dispH; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(m_psCoarse, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_dispRTV, nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // Pass 1: convert source (delayed frame for Pulfrich, disparity map for anaglyph
    // recovery) into the SBS output.
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)m_outHeight; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(m_ps, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { source, delayedSRV, anaRecover ? m_dispSRV : nullptr };
        m_context->PSSetShaderResources(0, 3, srvs);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
    }

    // Pass 2 (Pulfrich): copy the current source into the history ring for later.
    if (pulfrich && m_histRTV[m_histWrite])
    {
        CB copyCb = cb; copyCb.format = 99;
        m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &copyCb, 0, 0);
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_histW; vp.Height = (FLOAT)m_histH; vp.MaxDepth = 1.0f;
        m_context->OMSetRenderTargets(1, &m_histRTV[m_histWrite], nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
        m_histWrite = (m_histWrite + 1) % kHistory;
    }
    return true;
}

bool Converter::EnsureHistory(int width, int height)
{
    if (m_hist[0] && width == m_histW && height == m_histH)
        return false;

    ReleaseHistory();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1; td.Format = m_format;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kHistory; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_hist[i]))) { ReleaseHistory(); return false; }
        if (FAILED(m_device->CreateRenderTargetView(m_hist[i], nullptr, &m_histRTV[i]))) { ReleaseHistory(); return false; }
        if (FAILED(m_device->CreateShaderResourceView(m_hist[i], nullptr, &m_histSRV[i]))) { ReleaseHistory(); return false; }
    }
    m_histW = width; m_histH = height; m_histWrite = 0;
    return true;
}

void Converter::ReleaseHistory()
{
    for (int i = 0; i < kHistory; ++i)
    {
        SAFE_RELEASE(m_histSRV[i]);
        SAFE_RELEASE(m_histRTV[i]);
        SAFE_RELEASE(m_hist[i]);
    }
    m_histW = m_histH = 0;
    m_histWrite = 0;
}

void Converter::ReleaseOutput()
{
    SAFE_RELEASE(m_outSRV);
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outTex);
    m_outWidth = m_outHeight = 0;
}

bool Converter::EnsureDisparity(int width, int height)
{
    if (m_dispTex && width == m_dispW && height == m_dispH)
        return false;

    ReleaseDisparity();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;   // signed disparities (dLR, dRL) in UV
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_dispTex))) { ReleaseDisparity(); return false; }
    if (FAILED(m_device->CreateRenderTargetView(m_dispTex, nullptr, &m_dispRTV))) { ReleaseDisparity(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_dispTex, nullptr, &m_dispSRV))) { ReleaseDisparity(); return false; }
    m_dispW = width; m_dispH = height;
    return true;
}

void Converter::ReleaseDisparity()
{
    SAFE_RELEASE(m_dispSRV);
    SAFE_RELEASE(m_dispRTV);
    SAFE_RELEASE(m_dispTex);
    m_dispW = m_dispH = 0;
}

void Converter::Shutdown()
{
    ReleaseOutput();
    ReleaseHistory();
    ReleaseDisparity();
    SAFE_RELEASE(m_cbuffer);
    SAFE_RELEASE(m_sampler);
    SAFE_RELEASE(m_psCoarse);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
