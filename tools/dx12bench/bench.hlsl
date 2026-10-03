// dx12bench: Recovered Colour's compose kernel (Converter.hlsl anaRecoverPixel:
// the pass that is ~80% of a full redraw) on its own, to time the same maths
// compiled different ways. FT is the type of the arithmetic: float, or a true
// 16-bit float (Shader Model 6.2, -enable-16bit-types: Direct3D 12 only).
#ifdef HALF
#define FT  float16_t
#define FT3 float16_t3
#else
#define FT  float
#define FT3 float3
#endif

// FASTM: the square roots as x * rsqrt(x) and exp as exp2 -- the forms an NVIDIA
// GPU has native instructions for.
#ifdef FASTM
#define SQRT(x) ((x) * rsqrt(max((x), (FT)1e-9)))
#define EXPN(x) exp2((x) * (FT)1.442695)
#else
#define SQRT(x) sqrt(x)
#define EXPN(x) exp(x)
#endif

Texture2D    src  : register(t0);   // the anaglyph picture (sRGB view)
Texture2D    disp : register(t1);   // per-pixel disparity (quarter size)
SamplerState samp : register(s0);
cbuffer C : register(b0) { float g_srcW, g_srcH, g_outW, g_outH; };

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    const float2 uv = pos.xy / float2(g_outW, g_outH);
    const bool right = uv.x >= 0.5;
    const float2 e = float2(right ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);
    const float dRef = disp.SampleLevel(samp, e, 0.0).r * (right ? -1.0 : 1.0);
    const float px = 1.0 / g_srcW, py = 1.0 / g_srcH;

    const FT3 lumaW = FT3(0.299, 0.587, 0.114);
    const FT3 c = (FT3)src.SampleLevel(samp, e, 0.0).rgb;
    const FT  eyeY = right ? (FT)(c.g * (FT)0.7 + c.b * (FT)0.3) : c.r;    // own sharp luminance
    const FT3 centreC = (FT3)src.SampleLevel(samp, float2(e.x + dRef, e.y), 0.0).rgb;
    const FT  centreR = SQRT(max(dot(centreC, lumaW), (FT)0.0));
    FT3 there = 0;
    FT  tw    = (FT)1e-4;
    [unroll] for (int by = -1; by <= 1; ++by)
    [unroll] for (int bx = -1; bx <= 1; ++bx)
    {
        const FT3 s  = (FT3)src.SampleLevel(samp, float2(e.x + dRef + (float)bx * px, e.y + (float)by * py), 0.0).rgb;
        const FT  sY = dot(s, lumaW);
        const FT  w  = EXPN(-abs(SQRT(max(sY, (FT)0.0)) - centreR) * (FT)8.0);
        there += s * w;
        tw    += w;
    }
    there /= tw;
    const FT3 ownMask = right ? FT3(0, 1, 1) : FT3(1, 0, 0);
    const FT3 alignedCol = c * ownMask + there * ((FT)1.0 - ownMask);
    const FT  aY = max(dot(alignedCol, lumaW), (FT)1e-3);
    FT3 scaled = alignedCol * (eyeY / aY);
    const FT peak = max(max(scaled.r, scaled.g), scaled.b);
    if (peak > (FT)1.0) scaled /= peak;
    return float4(saturate((float3)scaled), 1);
}

// ---- The matching search (Converter.hlsl gradWindow + AnaSearch for both eyes:
// PSAnaPair's work, the arithmetic-heavy pass). Built with -D SEARCH as PSMain2.
FT descCurve(FT g) { const FT r = SQRT(abs(g)); return sign(g) * r * SQRT(r); }
float2 searchOne(float2 e, float d0, bool left, FT cref[4], float px)
{
    FT sv[11];
    [unroll] for (int q = 0; q < 11; ++q)
    {
        const FT3 s = (FT3)src.SampleLevel(samp, float2(e.x + d0 + (float)(q - 5) * px, e.y), 0).rgb;
        sv[q] = left ? s.g : s.r;
    }
    FT ccand[10];
    [unroll] for (int j = 0; j < 10; ++j) ccand[j] = descCurve(sv[j + 1] - sv[j]);
    FT sads[7];
    [unroll] for (int r = 0; r < 7; ++r)
    {
        FT sd = 0;
        [unroll] for (int k = 0; k < 4; ++k) sd += abs(cref[k] - ccand[r + k]);
        sads[r] = sd + (FT)abs((float)(r - 3)) * (FT)0.02;
    }
    int bi = 3; FT bs = sads[3];
    [unroll] for (int t = 0; t < 7; ++t) if (sads[t] < bs) { bs = sads[t]; bi = t; }
    FT cm = 0, cp = 0;
    [unroll] for (int t2 = 0; t2 < 7; ++t2) { cm = (t2 == bi - 1) ? sads[t2] : cm; cp = (t2 == bi + 1) ? sads[t2] : cp; }
    float dRef = d0 + (float)(bi - 3) * px;
    if (bi > 0 && bi < 6)
    {
        const FT den = cm - (FT)2.0 * bs + cp;
        const FT delta = (abs(den) > (FT)1e-4) ? (FT)0.5 * (cm - cp) / den : (FT)0.0;
        dRef += (float)clamp(delta, (FT)-1.0, (FT)1.0) * px;
    }
    return float2(dRef, (float)saturate((FT)1.0 - bs * (FT)1.2));
}
float4 PSMain2(float4 pos : SV_Position) : SV_Target
{
    const float2 e = pos.xy / float2(g_outW, g_outH);
    const float px = 1.0 / g_srcW;
    const float d0 = disp.SampleLevel(samp, e, 0.0).r;
    FT rr[5], gg[5];
    [unroll] for (int w = 0; w < 5; ++w)
    {
        const FT3 s = (FT3)src.SampleLevel(samp, e + float2((float)(w - 2) * px, 0), 0).rgb;
        rr[w] = s.r; gg[w] = s.g;
    }
    FT cR[4], cG[4];
    [unroll] for (int k = 0; k < 4; ++k) { cR[k] = descCurve(rr[k + 1] - rr[k]); cG[k] = descCurve(gg[k + 1] - gg[k]); }
    const float2 l = searchOne(e, d0, true, cR, px), r = searchOne(e, -d0, false, cG, px);
    return float4(l.x * 40.0 + 0.5, l.y, r.x * 40.0 + 0.5, 1);
}
