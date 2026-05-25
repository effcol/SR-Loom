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
Texture2D    dispTex : register(t2);   // disparity map: .r=dLR .g=dRL (UV), .b=confidence
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
)HLSL"
R"HLSL(
// Gradient descriptor for anaglyph matching (SIRA, Kunze 2020, re-implemented):
// match the DIFFERENCE between adjacent pixels along a direction, not raw intensity,
// so the red and cyan views match by STRUCTURE despite their photometric mismatch.
// Red is matched against the GREEN channel only (green carries most luminance).
// 5-tap line along `dir` -> 4 adjacent differences for red (gR) and green (gG).
void gradDir(float2 uv, float2 dir, out float gR[4], out float gG[4])
{
    float rr[5], gg[5];
    [unroll] for (int w = 0; w < 5; ++w)
    {
        float3 s = srcTex.SampleLevel(samp, uv + dir * (float)(w - 2), 0).rgb;
        rr[w] = s.r; gg[w] = s.g;
    }
    [unroll] for (int k = 0; k < 4; ++k) { gR[k] = rr[k + 1] - rr[k]; gG[k] = gg[k + 1] - gg[k]; }
}

float sad4(float a[4], float b[4])
{
    float s = 0; [unroll] for (int k = 0; k < 4; ++k) s += abs(a[k] - b[k]); return s;
}

// Coarse disparity pass (red/cyan anaglyph): low-resolution disparity between the
// L (red) and R (green) views, both directions. Matches mostly HORIZONTAL (the only
// direction anaglyph disparity actually moves) but adds two DIAGONAL checks at half
// weight to disambiguate horizontal edges. Plain weighted sum -- NO per-angle
// normalization (that amplified flat directions and caused wrong matches).
// Output: .r = dLR (left->right), .g = dRL (right->left), in UV (fraction of width).
#define ANA_DIRS(uv2, hR,hG, aR,aG, bR,bG) \
    gradDir(uv2, float2(ctx, 0),   hR, hG); \
    gradDir(uv2, float2(ctx, cty), aR, aG); \
    gradDir(uv2, float2(ctx,-cty), bR, bG);
#define ANA_COST(refH,refA,refB, cH,cA,cB) \
    (sad4(refH, cH) + 0.5 * sad4(refA, cA) + 0.5 * sad4(refB, cB))

float4 PSAnaDisp(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  ctx = 1.0 / max(g_coarseW, 1.0);
    float  cty = 1.0 / max(g_coarseH, 1.0);
    float  maxD = g_dispMaxUV;
    const int N = 24;
    float  step = maxD / (float)N;

    float hR[4],hG[4], aR[4],aG[4], bR[4],bG[4];
    ANA_DIRS(uv, hR,hG, aR,aG, bR,bG)   // reference descriptors at uv

    float bestL = 1e9, dL = 0.0, bestR = 1e9, dR = 0.0;
    [loop] for (int k = -N; k <= N; ++k)
    {
        float d = (float)k * step;
        float chR[4],chG[4], caR[4],caG[4], cbR[4],cbG[4];
        ANA_DIRS(uv + float2(d, 0), chR,chG, caR,caG, cbR,cbG)
        float bias = abs(d) / max(maxD, 1e-4) * 0.06;
        float sadL = bias + ANA_COST(hR,aR,bR, chG,caG,cbG);   // dLR: red ref vs green cand
        float sadR = bias + ANA_COST(hG,aG,bG, chR,caR,cbR);   // dRL: green ref vs red cand
        if (sadL < bestL) { bestL = sadL; dL = d; }
        if (sadR < bestR) { bestR = sadR; dR = d; }
    }
    return float4(dL, dR, 0, 0);
}

// Pyramid refine: start from a coarser level (dispTex = prior) and do a local
// horizontal+diagonal gradient search to sharpen the disparity. (dLR, dRL, 0, 0).
float4 PSAnaRefine(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  ctx = 1.0 / max(g_coarseW, 1.0);
    float  cty = 1.0 / max(g_coarseH, 1.0);
    float2 prior = dispTex.SampleLevel(samp, uv, 0.0).rg;
    const int M = 6;

    float hR[4],hG[4], aR[4],aG[4], bR[4],bG[4];
    ANA_DIRS(uv, hR,hG, aR,aG, bR,bG)

    float bestL = 1e9, dL = prior.r, bestR = 1e9, dR = prior.g;
    [loop] for (int k = -M; k <= M; ++k)
    {
        float ddL = prior.r + (float)k * ctx;
        float ddR = prior.g + (float)k * ctx;
        float lhR[4],lhG[4], laR[4],laG[4], lbR[4],lbG[4];
        ANA_DIRS(uv + float2(ddL, 0), lhR,lhG, laR,laG, lbR,lbG)
        float rhR[4],rhG[4], raR[4],raG[4], rbR[4],rbG[4];
        ANA_DIRS(uv + float2(ddR, 0), rhR,rhG, raR,raG, rbR,rbG)
        float sadL = ANA_COST(hR,aR,bR, lhG,laG,lbG);   // ref red vs candidate green
        float sadR = ANA_COST(hG,aG,bG, rhR,raR,rbR);   // ref green vs candidate red
        if (sadL < bestL) { bestL = sadL; dL = ddL; }
        if (sadR < bestR) { bestR = sadR; dR = ddR; }
    }
    return float4(dL, dR, 0, 0);
}

// Occlusion fill: flag pixels failing the left-right consistency check and fill
// their disparity from the NEAREST consistent neighbour (preferring the smaller =
// background disparity). Filling by spatial proximity (not source colour) avoids
// pulling in the wrong-coloured region. dispTex holds (dLR, dRL); adds .b = conf.
float4 PSAnaFill(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  ctx = 1.0 / max(g_coarseW, 1.0);
    float  inv = 1.0 / max(g_dispMaxUV, 1e-4);
    float2 d = dispTex.SampleLevel(samp, uv, 0.0).rg;

    float backR = dispTex.SampleLevel(samp, float2(uv.x + d.r, uv.y), 0.0).g;
    float backL = dispTex.SampleLevel(samp, float2(uv.x + d.g, uv.y), 0.0).r;
    float cons  = max(abs(d.r + backR), abs(d.g + backL));
    float conf  = saturate(1.0 - cons * inv * 4.0);
    if (conf > 0.5) return float4(d, conf, 0);

    // Among consistent neighbours, prefer the one whose GREEN (the trusted, shared
    // luminance channel) best matches ours -- colour-aware like SIRA but matched on
    // green only, so it never biases toward the red/blue (left-eye) channels and
    // can't pull spurious red into the gap. Small spatial penalty breaks ties.
    float myG = srcTex.SampleLevel(samp, uv, 0.0).g;
    float best = 1e9; float2 bestD = d; bool found = false;
    [loop] for (int s = 1; s <= 20; ++s)
    {
        [unroll] for (int sgn = 0; sgn < 2; ++sgn)
        {
            float2 nuv = float2(uv.x + (sgn == 0 ? (float)s : -(float)s) * ctx, uv.y);
            float2 nd  = dispTex.SampleLevel(samp, nuv, 0.0).rg;
            float  nc  = saturate(1.0 - abs(nd.r + dispTex.SampleLevel(samp, float2(nuv.x + nd.r, nuv.y), 0.0).g) * inv * 4.0);
            if (nc > 0.5)
            {
                float gd = abs(myG - srcTex.SampleLevel(samp, nuv, 0.0).g) + (float)s * 0.02;
                if (gd < best) { best = gd; bestD = nd; found = true; }
            }
        }
    }
    return float4(bestD, found ? 0.4 : 0.2, 0);
}

// Edge-aware disparity smoothing (shader-feasible stand-in for SIRA's superpixel
// "constant disparity within a segment"): cross-bilateral blur of the disparity
// map guided by source luminance, so disparities even out within a region but do
// NOT bleed across luminance edges. Confidence (.b) is preserved.
float4 PSAnaSmooth(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    float  ty = 1.0 / max(g_coarseH, 1.0);
    float3 c0 = srcTex.SampleLevel(samp, uv, 0.0).rgb;
    float  y0 = c0.r + c0.g;                       // red+green luminance proxy
    float  myConf = dispTex.SampleLevel(samp, uv, 0.0).b;

    float2 acc = 0; float wsum = 0;
    const int R = 2;
    [unroll] for (int dy = -R; dy <= R; ++dy)
    [unroll] for (int dx = -R; dx <= R; ++dx)
    {
        float2 nuv = uv + float2((float)dx * tx, (float)dy * ty);
        float4 nd  = dispTex.SampleLevel(samp, nuv, 0.0);
        float3 nc  = srcTex.SampleLevel(samp, nuv, 0.0).rgb;
        float  ws  = exp(-(float)(dx * dx + dy * dy) / 8.0);   // spatial
        float  wl  = exp(-abs(y0 - (nc.r + nc.g)) * 6.0);      // luminance (edge-aware)
        float  w   = ws * wl * (0.2 + nd.b);                   // trust confident neighbours
        acc += nd.rg * w; wsum += w;
    }
    return float4(acc / max(wsum, 1e-4), myConf, 0);
}
)HLSL"
R"HLSL(
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
            float py = 1.0 / g_srcH;
            float3 dC = dispTex.SampleLevel(samp, e, 0.0).rgb;   // (dLR, dRL, confidence)
            float d0 = (eye == 0) ? dC.r : dC.g;                 // this eye -> the other
            float baseConf = dC.b;                               // from the fill pass

            // Reference gradient descriptor at e (red for the left eye, green for the
            // right) for the full-res horizontal gradient-matching refine.
            float rgR[4], rgG[4]; gradDir(e, float2(px, 0), rgR, rgG);

            // Full-res refine (±3 px) with sub-pixel parabola fit on the cost curve.
            float sads[7];
            [unroll] for (int r = 0; r < 7; ++r)
            {
                float d = d0 + (float)(r - 3) * px;
                float cR[4], cG[4]; gradDir(float2(e.x + d, e.y), float2(px, 0), cR, cG);
                float sd = 0.0;
                [unroll] for (int k2 = 0; k2 < 4; ++k2)
                    sd += (eye == 0) ? abs(rgR[k2] - cG[k2])    // left: red ref vs green cand
                                     : abs(rgG[k2] - cR[k2]);   // right: green ref vs red cand
                sads[r] = sd;
            }
            int bi = 0; float bs = sads[0];
            [unroll] for (int t = 1; t < 7; ++t) if (sads[t] < bs) { bs = sads[t]; bi = t; }
            float dRef = d0 + (float)(bi - 3) * px;
            if (bi > 0 && bi < 6)   // parabola vertex from the two neighbouring costs
            {
                float cm = sads[bi - 1], cc0 = sads[bi], cp = sads[bi + 1];
                float den = cm - 2.0 * cc0 + cp;
                float delta = (abs(den) > 1e-5) ? 0.5 * (cm - cp) / den : 0.0;
                dRef += clamp(delta, -1.0, 1.0) * px;
            }

            float eyeY = anaEyeLuma(c, g_anaCombo, eye);            // own sharp luminance
            // Block processing (SIRA): borrow the aligned colour as a small patch
            // average, not a single pixel, to smooth residual fringing.
            float3 there = 0;
            [unroll] for (int by = -1; by <= 1; ++by)
            [unroll] for (int bx = -1; bx <= 1; ++bx)
                there += srcTex.SampleLevel(samp, float2(e.x + dRef + (float)bx * px, e.y + (float)by * py), 0.0).rgb;
            there /= 9.0;
            float3 alignedCol = (eye == 0) ? float3(c.r, there.g, there.b)
                                           : float3(there.r, c.g, c.b);
            float aY = max(dot(alignedCol, float3(0.299, 0.587, 0.114)), 1e-3);
            float3 aligned = saturate(alignedCol * (eyeY / aY));

            // EDGE-AWARE confidence + GREY fallback. In FLAT regions, trust the match
            // (uniform colour is correct even when the exact disparity is ambiguous)
            // so colour is preserved -- this is what v9's flat-penalizing structure
            // gate got wrong. At luminance EDGES (where fringe and the spurious-red
            // borrow live) require strict left-right consistency; where it fails, fall
            // back to GREY -- never the anaglyph's mixed chroma, so no red/cyan fringe
            // and no other-eye colour bleeds in.
            float refEnergy = 0;
            [unroll] for (int k = 0; k < 4; ++k) refEnergy += abs((eye == 0) ? rgR[k] : rgG[k]);
            float edgeFactor = saturate(refEnergy * 12.0);
            float conf = saturate(1.0 - bs * 1.5) * lerp(1.0, baseConf, edgeFactor);
            float3 col = lerp(float3(eyeY, eyeY, eyeY), aligned, conf);

            // Saturation gate: pull only the faintest residual chroma to grey.
            float lum = dot(col, float3(0.299, 0.587, 0.114));
            float sat = length(col - lum);
            col = lerp(float3(lum, lum, lum), col, smoothstep(0.02, 0.08, sat));
            return float4(col, 1);
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

    // Disparity passes (used only by the multi-scale anaglyph recovery mode).
    struct { const char* entry; ID3D11PixelShader** out; } passes[] = {
        { "PSAnaDisp",   &m_psCoarse },
        { "PSAnaRefine", &m_psRefine },
        { "PSAnaFill",   &m_psFill   },
        { "PSAnaSmooth", &m_psSmooth },
    };
    for (auto& p : passes)
    {
        ID3DBlob* csBlob = nullptr;
        hr = D3DCompile(kShaderSrc, strlen(kShaderSrc), "srw_convert", nullptr, nullptr,
                        p.entry, "ps_5_0", flags, 0, &csBlob, &err);
        if (FAILED(hr)) { reportCompileError(p.entry, err); if (err) err->Release(); return false; }
        hr = device->CreatePixelShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, p.out);
        SAFE_RELEASE(csBlob);
        if (FAILED(hr)) { ShowError("Converter disparity shader creation failed."); return false; }
    }

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

    // Multi-scale anaglyph recovery: build a coarse->fine disparity pyramid first.
    const bool anaRecover = (m_fmt == StereoFormat::Anaglyph && m_anaMode == 4 && m_anaCombo == 0);
    int w16 = 0, h16 = 0, w4 = 0, h4 = 0;
    if (anaRecover)
    {
        w16 = srcWidth  / 16; if (w16 < 1) w16 = 1;
        h16 = srcHeight / 16; if (h16 < 1) h16 = 1;
        w4  = srcWidth  / 4;  if (w4  < 1) w4  = 1;
        h4  = srcHeight / 4;  if (h4  < 1) h4  = 1;
        EnsureDispTarget(m_disp0, w16, h16);
        EnsureDispTarget(m_disp1, w4,  h4);
        EnsureDispTarget(m_disp2, w4,  h4);
        EnsureDispTarget(m_dispF, w4,  h4);
    }
    else ReleaseDisparity();

    const float dispMaxUV = 0.06f;
    auto uploadCB = [&](float coarseW, float coarseH)
    {
        CB cb{ FormatCode(m_fmt), m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
               m_anaCombo, m_anaMode, (int)m_pulfMode, m_pulfEye,
               m_ndTrans, m_fpEyeFrac, m_fpGapFrac, 0,
               dispMaxUV, coarseW, coarseH, 0 };
        m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &cb, 0, 0);
    };

    // Shared pipeline state.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cbuffer);

    // Render one disparity-pyramid level: source at t0, an optional coarser-level
    // disparity at t2, output to a DispTarget.
    auto runDispPass = [&](ID3D11PixelShader* ps, const DispTarget& rt,
                           ID3D11ShaderResourceView* prior)
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)rt.w; vp.Height = (FLOAT)rt.h; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(ps, nullptr, 0);
        m_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { source, nullptr, prior };
        m_context->PSSetShaderResources(0, 3, srvs);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    };

    if (anaRecover && m_disp0.rtv && m_disp1.rtv && m_disp2.rtv && m_dispF.rtv)
    {
        uploadCB((float)w16, (float)h16); runDispPass(m_psCoarse, m_disp0, nullptr);      // full search 1/16
        uploadCB((float)w4,  (float)h4);  runDispPass(m_psRefine, m_disp1, m_disp0.srv);  // refine 1/4
                                          runDispPass(m_psFill,   m_disp2, m_disp1.srv);  // occlusion fill 1/4
                                          runDispPass(m_psSmooth, m_dispF, m_disp2.srv);  // edge-aware smooth 1/4
    }

    // Compose: convert source (delayed frame for Pulfrich, filled disparity map for
    // anaglyph recovery) into the SBS output.
    uploadCB((float)w4, (float)h4);
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)m_outHeight; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(m_ps, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { source, delayedSRV, anaRecover ? m_dispF.srv : nullptr };
        m_context->PSSetShaderResources(0, 3, srvs);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
    }

    // Pass 2 (Pulfrich): copy the current source into the history ring for later.
    if (pulfrich && m_histRTV[m_histWrite])
    {
        CB copyCb{ 99, m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
                   m_anaCombo, m_anaMode, (int)m_pulfMode, m_pulfEye,
                   m_ndTrans, m_fpEyeFrac, m_fpGapFrac, 0,
                   dispMaxUV, 0, 0, 0 };
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

bool Converter::EnsureDispTarget(DispTarget& t, int width, int height)
{
    if (t.tex && width == t.w && height == t.h)
        return false;

    ReleaseDispTarget(t);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // dLR, dRL (UV), confidence
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &t.tex))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) { ReleaseDispTarget(t); return false; }
    t.w = width; t.h = height;
    return true;
}

void Converter::ReleaseDispTarget(DispTarget& t)
{
    SAFE_RELEASE(t.srv);
    SAFE_RELEASE(t.rtv);
    SAFE_RELEASE(t.tex);
    t.w = t.h = 0;
}

void Converter::ReleaseDisparity()
{
    ReleaseDispTarget(m_disp0);
    ReleaseDispTarget(m_disp1);
    ReleaseDispTarget(m_disp2);
    ReleaseDispTarget(m_dispF);
}

void Converter::Shutdown()
{
    ReleaseOutput();
    ReleaseHistory();
    ReleaseDisparity();
    SAFE_RELEASE(m_cbuffer);
    SAFE_RELEASE(m_sampler);
    SAFE_RELEASE(m_psSmooth);
    SAFE_RELEASE(m_psFill);
    SAFE_RELEASE(m_psRefine);
    SAFE_RELEASE(m_psCoarse);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
