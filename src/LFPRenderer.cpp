// LFPRenderer.cpp -- see header.
#include "LFPRenderer.h"

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <vector>
#include <cstring>
#include <cmath>

using namespace srw;

namespace
{
    // HLSL: VS emits a 3-vertex full-screen triangle from SV_VertexID.
    // PS per output pixel: figure out which eye (SBS left/right), map
    // the output UV to MLA grid coords, bilinear-blend the 4 nearest
    // microlenses' aperture samples.
    const char* kShaderSrc = R"HLSL(
cbuffer LFPCB : register(b0) {
    // x = SBS width, y = output height, z = per-eye output width, w = unused
    float4 g_outDims;
    // x = mla cols, y = mla rows, z = sensor width, w = sensor height
    float4 g_grid;
    // x = lens sample radius in sensor pixels, y unused, z = aperture-stop
    //   integration radius (in sensor pixels; 0 = single sample), w unused
    float4 g_sample;
    // xy = eye0 aperture (-1..+1), zw = eye1 aperture
    float4 g_apertureLR;
    // xy = per-eye content rect MIN in normalised per-eye UV (0..1),
    // zw = per-eye content rect MAX. Anything outside this rect outputs
    // black (letterbox / pillarbox bars). For aspect-preserving fit.
    float4 g_contentRect;
};

Texture2D<float4> sensorRGB    : register(t0);
Texture2D<float2> lensCentres  : register(t1);
SamplerState      samp         : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    // Standard fullscreen-triangle hack: 3 vertices covering the entire viewport.
    VSOut o;
    float2 t = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(t * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;   // top-left origin like image space
    o.uv = t;
    return o;
}

float4 SampleLensView(int2 lensIdx, float2 apertureUV) {
    // Clamp to valid grid range so edge lenses don't fall off the lookup.
    lensIdx.x = clamp(lensIdx.x, 0, (int)g_grid.x - 1);
    lensIdx.y = clamp(lensIdx.y, 0, (int)g_grid.y - 1);
    // Get this lens's sensor-centre (sensor pixel coords).
    float2 centre = lensCentres.Load(int3(lensIdx, 0));
    // Aperture-stop integration: optionally average a few samples around
    // the chosen aperture position to smooth per-microlens noise.
    float2 sensorPx = centre + apertureUV * g_sample.x;
    float2 baseUV   = sensorPx / g_grid.zw;
    int    integ    = (int)max(0.0, g_sample.z);   // half-radius in sensor pixels
    float2 invSensor = 1.0 / g_grid.zw;
    if (integ <= 0) {
        return sensorRGB.SampleLevel(samp, baseUV, 0);
    }
    // Integrate, weighted by alpha. ESLF PNGs have round microlens
    // discs with transparent gaps between them (alpha=0); raw-LFR
    // demosaic fills alpha=1.0 everywhere. Multiplying by alpha makes
    // the integration "see" only valid microlens samples and avoids
    // the horizontal banding the gaps would otherwise produce.
    float4 sum = float4(0, 0, 0, 0);
    float wsum = 0.0;
    [unroll(5)] for (int dy = -integ; dy <= integ; ++dy)
    [unroll(5)] for (int dx = -integ; dx <= integ; ++dx) {
        float4 s = sensorRGB.SampleLevel(samp, baseUV + float2(dx, dy) * invSensor, 0);
        sum  += float4(s.rgb * s.a, 0);
        wsum += s.a;
    }
    if (wsum < 0.01) {
        return float4(0, 0, 0, 1);
    }
    return float4(sum.rgb / wsum, 1);
}

float4 PSMain(VSOut i) : SV_Target {
    // SBS: left half = eye0, right half = eye1.
    float2 outUV;
    float2 ap;
    if (i.uv.x < 0.5) {
        ap    = g_apertureLR.xy;
        outUV = float2(i.uv.x * 2.0, i.uv.y);
    } else {
        ap    = g_apertureLR.zw;
        outUV = float2((i.uv.x - 0.5) * 2.0, i.uv.y);
    }

    // Aspect-preserving fit: anything outside the content rect is black
    // bars. Content rect is in per-eye normalised UV space [0,1].
    if (outUV.x < g_contentRect.x || outUV.x > g_contentRect.z ||
        outUV.y < g_contentRect.y || outUV.y > g_contentRect.w)
        return float4(0, 0, 0, 1);
    // Remap UV from content-rect space back to [0,1] for MLA grid lookup.
    float2 contentSz = g_contentRect.zw - g_contentRect.xy;
    outUV = (outUV - g_contentRect.xy) / contentSz;

    // Output UV -> continuous MLA grid coords.
    float2 mlaPos = outUV * g_grid.xy;
    int2   base   = int2(floor(mlaPos));
    float2 frac   = mlaPos - float2(base);

    // Bilinear blend across 4 nearest microlenses for sub-pixel quality.
    float4 c00 = SampleLensView(int2(base.x,     base.y    ), ap);
    float4 c10 = SampleLensView(int2(base.x + 1, base.y    ), ap);
    float4 c01 = SampleLensView(int2(base.x,     base.y + 1), ap);
    float4 c11 = SampleLensView(int2(base.x + 1, base.y + 1), ap);

    float4 cx0 = lerp(c00, c10, frac.x);
    float4 cx1 = lerp(c01, c11, frac.x);
    float4 c   = lerp(cx0, cx1, frac.y);
    c.a = 1.0;
    return c;
}
)HLSL";

    constexpr double kSqrt3Over2 = 0.86602540378443864676;

    // CPU-side: lens (col, row) -> sensor pixel centre (float x, float y).
    // Mirrors MicrolensCentre in LFPReader.cpp -- duplicated here to keep
    // the LFPRenderer self-contained for the lens-centres lookup build.
    void LensCentreSensor(int col, int row, const LFPCalibration& cal,
                          double& outX, double& outY)
    {
        const double pitchM = cal.mlaLensPitchM;
        double mx = col * pitchM + ((row & 1) ? 0.5 * pitchM : 0.0);
        double my = row * pitchM * kSqrt3Over2;
        mx *= cal.mlaScaleFactorX;
        my *= cal.mlaScaleFactorY;
        const double cr = cos(cal.mlaRotationRad);
        const double sr = sin(cal.mlaRotationRad);
        const double rx = cr * mx - sr * my;
        const double ry = sr * mx + cr * my;
        const double sxMeters = rx + cal.mlaSensorOffsetXM;
        const double syMeters = ry + cal.mlaSensorOffsetYM;
        outX = sxMeters / cal.pixelPitchM + 0.5 * cal.sensorWidth;
        outY = syMeters / cal.pixelPitchM + 0.5 * cal.sensorHeight;
    }

    // CBuffer layout MUST match HLSL declaration -- 16-byte aligned.
    struct LFPCB
    {
        float outDims[4];      // (sbsW, h, perEyeW, _)
        float grid[4];         // (cols, rows, sensorW, sensorH)
        float sample[4];       // (lensRadius, _, aperturStopRadius, _)
        float apertureLR[4];   // (eye0u, eye0v, eye1u, eye1v)
        float contentRect[4];  // (minU, minV, maxU, maxV) per-eye letterbox
    };
    static_assert(sizeof(LFPCB) % 16 == 0, "cbuffer must be 16-byte aligned");
}

LFPRenderer::~LFPRenderer() { Shutdown(); }

bool LFPRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device  = device;
    m_context = context;
    if (!CompileShaders()) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, &m_sampler))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(LFPCB);
    bd.Usage     = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_cbuffer))) return false;

    return true;
}

void LFPRenderer::Shutdown()
{
    ReleaseTextures();
    SAFE_RELEASE(m_cbuffer);
    SAFE_RELEASE(m_sampler);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
    m_device  = nullptr;
    m_context = nullptr;
}

bool LFPRenderer::CompileShaders()
{
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif
    ID3DBlob* err = nullptr;
    auto reportErr = [&](const char* stage) {
        std::string msg = std::string("LFPRenderer ") + stage + " compile failed.";
        if (err && err->GetBufferSize() > 0)
            msg.append("\n\n", 2),
            msg.append(reinterpret_cast<const char*>(err->GetBufferPointer()),
                       err->GetBufferSize());
        Log("%s", msg.c_str());
        ShowError(msg.c_str());
    };

    ID3DBlob* vsBlob = nullptr;
    HRESULT hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "lfp_vs",
                            nullptr, nullptr, "VSMain", "vs_5_0", flags, 0,
                            &vsBlob, &err);
    if (FAILED(hr)) { reportErr("VS"); if (err) err->Release(); return false; }
    ID3DBlob* psBlob = nullptr;
    hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "lfp_ps",
                    nullptr, nullptr, "PSMain", "ps_5_0", flags, 0,
                    &psBlob, &err);
    if (FAILED(hr)) { reportErr("PS"); if (err) err->Release(); if (vsBlob) vsBlob->Release(); return false; }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, &m_vs);
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                          nullptr, &m_ps);
    vsBlob->Release();
    psBlob->Release();
    return SUCCEEDED(hr);
}

bool LFPRenderer::LoadFromMemory(const std::vector<uint8_t>& sensorRgb,
                                  const LFPCalibration& cal,
                                  float outputAspect)
{
    if (!m_device) return false;
    ReleaseTextures();

    const int W = cal.sensorWidth, H = cal.sensorHeight;
    const size_t pixels = (size_t)W * H;
    const bool inputIsRgba = sensorRgb.size() >= pixels * 4;
    const bool inputIsRgb  = !inputIsRgba && sensorRgb.size() >= pixels * 3;
    if (!inputIsRgba && !inputIsRgb)
    {
        Log("LFPRenderer::LoadFromMemory: sensorRgb too small (got %zu, want %zu or %zu)",
            sensorRgb.size(), pixels * 3, pixels * 4);
        return false;
    }

    // Upload as RGBA8 (D3D11 doesn't natively support RGB8). Two
    // input shapes:
    //  - RGB8 (3 bytes/px): from LFPDemosaicPlenopticAware. Fill
    //    alpha=255 everywhere -- raw sensor data has no transparent
    //    regions.
    //  - RGBA8 (4 bytes/px): from LFPLoadEslfAsSensorRgb. Use the
    //    PNG's actual alpha, which is 0 in the gaps between microlens
    //    discs and 255 inside them. The shader uses alpha as a weight
    //    during aperture-stop integration so gap pixels don't bleed in.
    std::vector<uint8_t> rgba((size_t)W * H * 4);
    if (inputIsRgba)
    {
        std::memcpy(rgba.data(), sensorRgb.data(), pixels * 4);
    }
    else
    {
        for (size_t i = 0, j = 0; i < pixels; ++i, j += 3)
        {
            rgba[i * 4 + 0] = sensorRgb[j + 0];
            rgba[i * 4 + 1] = sensorRgb[j + 1];
            rgba[i * 4 + 2] = sensorRgb[j + 2];
            rgba[i * 4 + 3] = 255;
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width  = (UINT)W;
    td.Height = (UINT)H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = rgba.data();
    init.SysMemPitch = (UINT)W * 4;
    HRESULT hr = m_device->CreateTexture2D(&td, &init, &m_sensorTex);
    if (FAILED(hr)) { Log("LFPRenderer: sensor texture create failed 0x%08X", (unsigned)hr); return false; }
    hr = m_device->CreateShaderResourceView(m_sensorTex, nullptr, &m_sensorSRV);
    if (FAILED(hr)) { Log("LFPRenderer: sensor SRV create failed 0x%08X", (unsigned)hr); return false; }
    m_sensorW = W; m_sensorH = H;

    // Build the lens-centres lookup -- one (x, y) pair per microlens
    // in the usable MLA grid. Same grid the CPU SBS extractor uses
    // (LFPMicrolensGridDims + gridMin offset).
    int cols = 0, rows = 0;
    LFPMicrolensGridDims(cal, cols, rows);
    if (cols <= 0 || rows <= 0)
    {
        Log("LFPRenderer: microlens grid empty (cols=%d rows=%d)", cols, rows);
        return false;
    }
    // Same scan as LFPExtractSubApertureViewFromRgb to find gridMinCol/Row.
    int gridMinCol = 0, gridMinRow = 0;
    const double pitchPx = cal.mlaLensPitchM / cal.pixelPitchM;
    auto fits = [&](int col, int row) {
        double cx, cy;
        LensCentreSensor(col, row, cal, cx, cy);
        const double r = 0.5 * pitchPx;
        return cx > r && cx < cal.sensorWidth - r
            && cy > r && cy < cal.sensorHeight - r;
    };
    while (fits(gridMinCol - 1, 0)) --gridMinCol;
    while (fits(0, gridMinRow - 1)) --gridMinRow;

    std::vector<float> centres((size_t)cols * rows * 2);
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            double cx, cy;
            LensCentreSensor(col + gridMinCol, row + gridMinRow, cal, cx, cy);
            centres[((size_t)row * cols + col) * 2 + 0] = (float)cx;
            centres[((size_t)row * cols + col) * 2 + 1] = (float)cy;
        }
    }

    D3D11_TEXTURE2D_DESC ltd{};
    ltd.Width  = (UINT)cols;
    ltd.Height = (UINT)rows;
    ltd.MipLevels = 1;
    ltd.ArraySize = 1;
    ltd.Format    = DXGI_FORMAT_R32G32_FLOAT;
    ltd.SampleDesc.Count = 1;
    ltd.Usage     = D3D11_USAGE_DEFAULT;
    ltd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA linit{};
    linit.pSysMem     = centres.data();
    linit.SysMemPitch = (UINT)cols * 2 * sizeof(float);
    hr = m_device->CreateTexture2D(&ltd, &linit, &m_lensTex);
    if (FAILED(hr)) { Log("LFPRenderer: lens lookup create failed 0x%08X", (unsigned)hr); return false; }
    hr = m_device->CreateShaderResourceView(m_lensTex, nullptr, &m_lensSRV);
    if (FAILED(hr)) { Log("LFPRenderer: lens SRV create failed 0x%08X", (unsigned)hr); return false; }
    m_lensCols = cols;
    m_lensRows = rows;

    // Sample radius inside each microlens: match CPU extractor's 42% pitch
    // (well inside the lens to avoid the vignette ring).
    m_lensRadiusPx     = (float)(0.42 * pitchPx);
    m_apertureDiameterM = cal.ApertureDiameterM();

    // Size the output RT per eye so that:
    //   - If the SR display aspect is wider than the LFP image aspect
    //     (LFP usually taller-than-display), pad the RT WIDTH so the LFP
    //     content sits centred with pillarbox bars on the sides.
    //   - If the display is narrower than the LFP (unusual), pad RT
    //     HEIGHT for letterbox bars on top + bottom.
    // The SR weaver upscales the SBS RT 1:1 to the panel, so matching
    // its aspect here means no further crop downstream.
    const float lfpAspect = (float)cols / (float)rows;
    const float target    = (outputAspect > 0.01f) ? outputAspect : lfpAspect;
    int perEyeW = cols, perEyeH = rows;
    if (target > lfpAspect)
        perEyeW = (int)((float)rows * target + 0.5f);
    else if (target < lfpAspect)
        perEyeH = (int)((float)cols / target + 0.5f);
    if (!EnsureOutput(perEyeW, perEyeH))
    {
        Log("LFPRenderer: output RT create failed (%dx%d per eye)", perEyeW, perEyeH);
        return false;
    }

    Log("LFPRenderer: loaded sensor %dx%d, MLA %dx%d, lensRadius %.2f px, aperture %.2f mm "
        "-> per-eye RT %dx%d (target aspect %.3f, content aspect %.3f)",
        W, H, cols, rows, m_lensRadiusPx, m_apertureDiameterM * 1e3,
        perEyeW, perEyeH, target, lfpAspect);
    return true;
}

bool LFPRenderer::SetTargetAspect(float perEyeAspect)
{
    if (m_lensCols <= 0 || m_lensRows <= 0) return false;
    const float lfpAspect = (float)m_lensCols / (float)m_lensRows;
    const float target    = (perEyeAspect > 0.01f) ? perEyeAspect : lfpAspect;
    int perEyeW = m_lensCols, perEyeH = m_lensRows;
    if (target > lfpAspect)
        perEyeW = (int)((float)m_lensRows * target + 0.5f);
    else if (target < lfpAspect)
        perEyeH = (int)((float)m_lensCols / target + 0.5f);
    if (perEyeW == m_perEyeWidth && perEyeH == m_outHeight) return true;   // no change
    return EnsureOutput(perEyeW, perEyeH);
}

void LFPRenderer::Unload()
{
    ReleaseTextures();
    m_perEyeWidth = m_outHeight = 0;
}

void LFPRenderer::ReleaseTextures()
{
    SAFE_RELEASE(m_outSRV);
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outTex);
    SAFE_RELEASE(m_lensSRV);
    SAFE_RELEASE(m_lensTex);
    SAFE_RELEASE(m_sensorSRV);
    SAFE_RELEASE(m_sensorTex);
    m_sensorW = m_sensorH = 0;
    m_lensCols = m_lensRows = 0;
    m_lensRadiusPx = 0;
    m_apertureDiameterM = 0;
}

bool LFPRenderer::EnsureOutput(int perEyeWidth, int height)
{
    // Clamp the per-eye output dimensions so a high-res source (e.g. an
    // ESLF PNG at 7574x5264 matched against a 16:9 display aspect ->
    // ~9358x5264 per eye -> ~394 MB SBS RTV alone) can't trigger a
    // GPU out-of-memory. The cap is generous enough that any sensible
    // SR display still gets a 1:1 pixel match; sources beyond that
    // get a bilinear downscale at sample time, which is essentially
    // free relative to the GPU memory blowup it prevents.
    constexpr int kMaxDim = 4096;
    if (perEyeWidth > kMaxDim || height > kMaxDim)
    {
        const float sX = (float)kMaxDim / (float)(std::max)(1, perEyeWidth);
        const float sY = (float)kMaxDim / (float)(std::max)(1, height);
        const float s  = (std::min)(sX, sY);
        const int newW = (int)((float)perEyeWidth * s);
        const int newH = (int)((float)height     * s);
        Log("LFPRenderer::EnsureOutput: clamping %dx%d per-eye to %dx%d (cap %d)",
            perEyeWidth, height, newW, newH, kMaxDim);
        perEyeWidth = newW;
        height      = newH;
    }
    if (m_outTex && m_perEyeWidth == perEyeWidth && m_outHeight == height) return true;
    SAFE_RELEASE(m_outSRV);
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outTex);
    D3D11_TEXTURE2D_DESC td{};
    td.Width  = (UINT)(perEyeWidth * 2);
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = m_outFormat;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_outTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(m_outTex, nullptr, &m_outRTV);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_outTex, nullptr, &m_outSRV);
    if (FAILED(hr)) return false;
    m_perEyeWidth = perEyeWidth;
    m_outHeight   = height;
    return true;
}

void LFPRenderer::Run(float eyeLU, float eyeLV, float eyeRU, float eyeRV)
{
    if (!m_sensorSRV || !m_lensSRV || !m_outRTV || !m_vs || !m_ps) return;

    LFPCB cb{};
    cb.outDims[0]    = (float)(m_perEyeWidth * 2);
    cb.outDims[1]    = (float)m_outHeight;
    cb.outDims[2]    = (float)m_perEyeWidth;
    cb.outDims[3]    = 0;
    cb.grid[0]       = (float)m_lensCols;
    cb.grid[1]       = (float)m_lensRows;
    cb.grid[2]       = (float)m_sensorW;
    cb.grid[3]       = (float)m_sensorH;
    cb.sample[0]     = m_lensRadiusPx;
    cb.sample[1]     = 0;
    // Aperture-stop integration: average a 5x5 sensor-pixel window
    // around the aperture sample. Smooths per-microlens noise that
    // otherwise shows up as visible grain in the output. Radius 2 is
    // ~2.8x more samples than 3x3 (25 vs 9) which knocks ~40% off
    // visible noise. Cost: ~25 texture fetches per output pixel --
    // still well within budget on any GPU that can run the weaver.
    cb.sample[2]     = 2;
    cb.sample[3]     = 0;
    cb.apertureLR[0] = eyeLU;
    cb.apertureLR[1] = eyeLV;
    cb.apertureLR[2] = eyeRU;
    cb.apertureLR[3] = eyeRV;
    // Compute the centred content rect within the per-eye output. If RT
    // is wider than the LFP, content is a vertical stripe centred; if
    // taller, a horizontal stripe centred.
    const float rtAspect  = (m_outHeight > 0) ? (float)m_perEyeWidth / (float)m_outHeight : 1.0f;
    const float lfpAspect = (m_lensRows  > 0) ? (float)m_lensCols    / (float)m_lensRows  : 1.0f;
    float minU = 0, minV = 0, maxU = 1, maxV = 1;
    if (rtAspect > lfpAspect)
    {
        // Pillarbox: content takes (lfp/rt) of width.
        const float w = lfpAspect / rtAspect;
        minU = 0.5f - 0.5f * w;
        maxU = 0.5f + 0.5f * w;
    }
    else if (rtAspect < lfpAspect)
    {
        // Letterbox: content takes (rt/lfp) of height.
        const float h = rtAspect / lfpAspect;
        minV = 0.5f - 0.5f * h;
        maxV = 0.5f + 0.5f * h;
    }
    cb.contentRect[0] = minU;
    cb.contentRect[1] = minV;
    cb.contentRect[2] = maxU;
    cb.contentRect[3] = maxV;
    m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &cb, 0, 0);

    D3D11_VIEWPORT vp{};
    vp.Width  = (FLOAT)(m_perEyeWidth * 2);
    vp.Height = (FLOAT)m_outHeight;
    vp.MaxDepth = 1.0f;

    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetShader(m_ps, nullptr, 0);
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
    ID3D11ShaderResourceView* srvs[2] = { m_sensorSRV, m_lensSRV };
    m_context->PSSetShaderResources(0, 2, srvs);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cbuffer);
    m_context->Draw(3, 0);

    // Unbind SRVs so subsequent passes (e.g. weaver) can re-bind cleanly.
    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    m_context->PSSetShaderResources(0, 2, nulls);
    ID3D11RenderTargetView* nullRtv = nullptr;
    m_context->OMSetRenderTargets(1, &nullRtv, nullptr);
}
