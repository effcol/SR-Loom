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
    float g_propStride; // colour-propagation pass: neighbour sample stride (texels)
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
    // Per-eye luminance at FULL brightness (e.g. red/cyan left = c.r, right = (g+b)/2).
    // All modes key off this so Mono / Half match the colour modes' brightness.
    float eyeY = anaEyeLuma(c, combo, e);
    if (mode == 0)   // Shared colour: per-eye luminance, shared anaglyph chrominance.
    {
        float anaY = max(dot(c, float3(0.299, 0.587, 0.114)), 1e-3);
        return saturate(c * (eyeY / anaY));   // same hue both eyes, eye-specific brightness
    }

    float3 col = anaFilter(c, combo, e);
    if (mode == 2)   // half colour: half saturation but FULL per-eye brightness (re-normalize
    {                // to eyeY so it isn't darker than the colour/mono modes).
        float3 h = lerp(col, eyeY.xxx, 0.5);
        float  hY = max(dot(h, float3(0.299, 0.587, 0.114)), 1e-3);
        return saturate(h * (eyeY / hY));
    }
    if (mode == 3) return eyeY.xxx;                   // mono: per-eye luminance (not the dim
                                                      // single-channel-weighted grey, which was ~3x dark)
    return col;                                       // mode 1: colour (filtered)
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
// match the DIFFERENCE between adjacent pixels, not raw intensity, so the red and
// cyan views match by STRUCTURE despite their photometric (colour) mismatch. We
// match red against the GREEN channel only (green carries most luminance; mixing
// in blue adds noise). 5-tap window -> 4 adjacent differences per channel.
void gradWindow(float2 uv, float ctx, out float gR[4], out float gG[4])
{
    float rr[5], gg[5];
    [unroll] for (int w = 0; w < 5; ++w)
    {
        float3 s = srcTex.SampleLevel(samp, uv + float2((float)(w - 2) * ctx, 0), 0).rgb;
        rr[w] = s.r; gg[w] = s.g;
    }
    [unroll] for (int k = 0; k < 4; ++k) { gR[k] = rr[k + 1] - rr[k]; gG[k] = gg[k + 1] - gg[k]; }
}

// Four-angle gradient descriptors (SIRA: 0/45/90/135 deg around a pixel) for red &
// green at uv. Each angle = a 5-tap line -> 4 adjacent differences. Packed as
// [angle*4 + diff], 16 entries per channel. Multiple angles capture 2D structure,
// making the match far more discriminative than a single horizontal line.
void buildDesc(float2 uv, float tx, float ty, out float gRed[16], out float gGrn[16])
{
    float2 dirs[4] = { float2(tx, 0), float2(0, ty), float2(tx, ty), float2(-tx, ty) };
    [unroll] for (int a = 0; a < 4; ++a)
    {
        float rr[5], gg[5];
        [unroll] for (int i = 0; i < 5; ++i)
        {
            float3 s = srcTex.SampleLevel(samp, uv + dirs[a] * (float)(i - 2), 0).rgb;
            rr[i] = s.r; gg[i] = s.g;
        }
        [unroll] for (int k = 0; k < 4; ++k)
        { gRed[a * 4 + k] = rr[k + 1] - rr[k]; gGrn[a * 4 + k] = gg[k + 1] - gg[k]; }
    }
}

// Gradient distance between two descriptors (channels already picked), normalized
// ONCE by total gradient energy across all 4 angles. A single global normalization
// (instead of per-angle) means a near-flat, low-energy direction contributes little
// to both the numerator and denominator and can't blow up / dominate the match --
// which was a source of wrong disparities (and the spurious colour borrows).
float descCost(float a16[16], float b16[16])
{
    float sad = 0, en = 1e-3;
    [unroll] for (int j = 0; j < 16; ++j)
    {
        sad += abs(a16[j] - b16[j]);
        en  += abs(a16[j]) + abs(b16[j]);
    }
    return sad / en;
}

// Coarse disparity pass (red/cyan anaglyph): low-resolution map of the horizontal
// disparity between the L (red) and R (green) views, both directions, via 4-angle
// gradient matching. Low resolution makes winner-take-all robust and smooth.
// Output: .r = dLR (left->right), .g = dRL (right->left), in UV (fraction of width).
float4 PSAnaDisp(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    float  ty = 1.0 / max(g_coarseH, 1.0);
    float  maxD = g_dispMaxUV;
    const int N = 24;
    float  step = maxD / (float)N;

    float refRed[16], refGrn[16]; buildDesc(uv, tx, ty, refRed, refGrn);

    // Track the best match AND the best RIVAL (lowest cost at least 3 steps away from
    // the best) per direction. A unique match has its rival much higher; a repetitive
    // / ambiguous one (e.g. thin text strokes) has a near-equal rival -> low
    // uniqueness, which later flags the (confidently-wrong) borrow as low-confidence.
    float bestL = 1e9, dL = 0.0, secondL = 1e9; int bkL = -999;
    float bestR = 1e9, dR = 0.0, secondR = 1e9; int bkR = -999;
    [loop] for (int k = -N; k <= N; ++k)
    {
        float d = (float)k * step;
        float cRed[16], cGrn[16]; buildDesc(uv + float2(d, 0), tx, ty, cRed, cGrn);
        float bias = abs(d) / max(maxD, 1e-4) * 0.06;
        float sadL = descCost(refRed, cGrn) + bias;   // dLR: red ref vs green candidate
        float sadR = descCost(refGrn, cRed) + bias;   // dRL: green ref vs red candidate
        if (sadL < bestL) { if (abs(k - bkL) > 2) secondL = bestL; bestL = sadL; dL = d; bkL = k; }
        else if (sadL < secondL && abs(k - bkL) > 2) secondL = sadL;
        if (sadR < bestR) { if (abs(k - bkR) > 2) secondR = bestR; bestR = sadR; dR = d; bkR = k; }
        else if (sadR < secondR && abs(k - bkR) > 2) secondR = sadR;
    }
    float uniqL = saturate((secondL - bestL) / (secondL + 1e-3) * 5.0);
    float uniqR = saturate((secondR - bestR) / (secondR + 1e-3) * 5.0);
    return float4(dL, dR, uniqL, uniqR);
}

// Pyramid refine: start from a coarser level (dispTex = prior) and do a local
// 4-angle gradient search to sharpen the disparity. Outputs (dLR, dRL, 0, 0).
float4 PSAnaRefine(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    float  ty = 1.0 / max(g_coarseH, 1.0);
    float4 priorAll = dispTex.SampleLevel(samp, uv, 0.0);   // .rg disparity, .ba uniqueness
    float2 prior = priorAll.rg;
    const int M = 6;

    float refRed[16], refGrn[16]; buildDesc(uv, tx, ty, refRed, refGrn);

    float bestL = 1e9, dL = prior.r, bestR = 1e9, dR = prior.g;
    [loop] for (int k = -M; k <= M; ++k)
    {
        float ddL = prior.r + (float)k * tx;   // candidate for dLR (match green)
        float ddR = prior.g + (float)k * tx;   // candidate for dRL (match red)
        float lRed[16], lGrn[16]; buildDesc(uv + float2(ddL, 0), tx, ty, lRed, lGrn);
        float rRed[16], rGrn[16]; buildDesc(uv + float2(ddR, 0), tx, ty, rRed, rGrn);
        float sadL = descCost(refRed, lGrn);   // ref red vs candidate green
        float sadR = descCost(refGrn, rRed);   // ref green vs candidate red
        if (sadL < bestL) { bestL = sadL; dL = ddL; }
        if (sadR < bestR) { bestR = sadR; dR = ddR; }
    }
    return float4(dL, dR, priorAll.b, priorAll.a);   // carry uniqueness through
}

// Occlusion fill: flag pixels failing the left-right consistency check, then fill
// their disparity from the consistent neighbour whose SOURCE COLOUR best matches
// (SIRA colorization-by-nearest-colour, full RGB, as in the paper), with a small
// spatial penalty. Any wrong borrow this introduces is caught downstream by the
// borrow-trust gate. dispTex holds the refined (dLR, dRL). Output adds .b = conf.
float4 PSAnaFill(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  ctx = 1.0 / max(g_coarseW, 1.0);
    float  inv = 1.0 / max(g_dispMaxUV, 1e-4);
    float4 dd = dispTex.SampleLevel(samp, uv, 0.0);   // .rg disparity, .b uniqL, .a uniqR
    float2 d = dd.rg;

    float backR = dispTex.SampleLevel(samp, float2(uv.x + d.r, uv.y), 0.0).g;
    float backL = dispTex.SampleLevel(samp, float2(uv.x + d.g, uv.y), 0.0).r;
    float cons  = max(abs(d.r + backR), abs(d.g + backL));
    float conf  = saturate(1.0 - cons * inv * 4.0);
    // Final per-eye confidence = left-right consistency x match uniqueness. Output
    // confL in .b (drives the left eye), confR in .a (right eye).
    if (conf > 0.5) return float4(d, dd.b * conf, dd.a * conf);

    float3 myCol = srcTex.SampleLevel(samp, uv, 0.0).rgb;
    float  best  = 1e9; float2 bestD = d; bool found = false;
    [loop] for (int s = 1; s <= 24; ++s)
    {
        [unroll] for (int sgn = 0; sgn < 2; ++sgn)
        {
            float  off = (sgn == 0) ? (float)s : -(float)s;
            float2 nuv = float2(uv.x + off * ctx, uv.y);
            float2 nd  = dispTex.SampleLevel(samp, nuv, 0.0).rg;
            float  nc  = saturate(1.0 - abs(nd.r + dispTex.SampleLevel(samp, float2(nuv.x + nd.r, nuv.y), 0.0).g) * inv * 4.0);
            if (nc > 0.5)
            {
                float3 ncol = srcTex.SampleLevel(samp, nuv, 0.0).rgb;
                float  cd = distance(myCol, ncol) + (float)s * 0.02;   // colour dist + slight spatial bias
                if (cd < best) { best = cd; bestD = nd; found = true; }
            }
        }
    }
    float fc = found ? 0.4 : 0.2;   // occluded -> low confidence (push-pull will fill)
    return float4(bestD, fc, fc);
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
    float2 myConf = dispTex.SampleLevel(samp, uv, 0.0).ba;   // per-eye confidence (.b left, .a right)

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
        float  w   = ws * wl * (0.2 + max(nd.b, nd.a));        // trust confident neighbours
        acc += nd.rg * w; wsum += w;
    }
    return float4(acc / max(wsum, 1e-4), myConf.x, myConf.y);
}
)HLSL"
R"HLSL(
// Push-pull colorization (SIRA's "fill invalid pixels from valid ones", done as a
// classical image-pyramid inpaint -- no neural net). Two stages:
//
// SPLIT: take one half of the recovered SBS (g_propStride = 0 left eye, 0.5 right)
// and write PREMULTIPLIED colour (rgb*conf, conf) into a per-eye texture. A box
// downsample (GenerateMips) of premultiplied data is exactly a CONFIDENCE-WEIGHTED
// average, so each coarser mip holds the average of the VALID colour beneath it.
// Per-eye textures keep the two eyes from mixing in the pyramid.
float4 PSAnaSplit(VSOut i) : SV_Target
{
    float2 uv = float2(g_propStride + i.uv.x * 0.5, i.uv.y);   // left half or right half
    float4 s = srcTex.SampleLevel(samp, uv, 0.0);              // recovered SBS; .a = confidence
    // Floor the weight so a zero-confidence pixel's colour isn't multiplied away to
    // black; un-premultiply (rgb*w)/w still recovers its colour, and valid pixels
    // (weight ~1) still dominate the confidence-weighted mip averages (~50x).
    float w = max(s.a, 0.02);
    return float4(s.rgb * w, w);
}

// COMPOSE: rebuild the SBS. For each pixel walk the eye's mip pyramid from finest to
// coarsest, accumulating colour weighted by confidence until full. A confident pixel
// uses its own (finest) colour; a low-confidence pixel (flat region / occlusion /
// spurious borrow) is filled from progressively larger averages of VALID colour ->
// real regional colour, never grey and never the raw wrong borrow. t0 = left pyramid,
// t1 = right pyramid; g_propStride = max mip level.
float4 PSAnaCompose(VSOut i) : SV_Target
{
    bool right = i.uv.x >= 0.5;
    float2 euv = float2(right ? (i.uv.x - 0.5) * 2.0 : i.uv.x * 2.0, i.uv.y);
    int maxLod = (int)(g_propStride + 0.5);

    float3 acc = 0; float w = 0;
    [loop] for (int L = 0; L <= maxLod; ++L)
    {
        if (w >= 0.99) break;
        float4 s = right ? srcPrev.SampleLevel(samp, euv, (float)L)
                         : srcTex.SampleLevel(samp, euv, (float)L);
        float a = saturate(s.a);
        float3 col = s.rgb / max(s.a, 1e-4);     // un-premultiply -> conf-weighted colour
        float contrib = a * (1.0 - w);
        acc += col * contrib; w += contrib;
    }
    return float4(acc / max(w, 1e-3), 1.0);
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
            float4 dC = dispTex.SampleLevel(samp, e, 0.0);       // (dLR, dRL, confL, confR)
            float d0 = (eye == 0) ? dC.r : dC.g;                 // this eye -> the other
            // COUPLE the eyes: a region unreliable in EITHER eye is treated unreliable
            // in BOTH (min), so it's inpainted symmetrically -> no one-eye-clean /
            // other-eye-splotch rivalry. (Both pyramids are regional averages of the
            // same scene, so they fill to near-identical colour.)
            float baseConf = min(dC.b, dC.a);                    // consistency x uniqueness, coupled

            // Reference gradient descriptor at e (red for the left eye, green for the
            // right) for the full-res gradient-matching refine.
            float rgR[4], rgG[4]; gradWindow(e, px, rgR, rgG);

            // Full-res refine (±3 px) with sub-pixel parabola fit on the cost curve.
            float sads[7];
            [unroll] for (int r = 0; r < 7; ++r)
            {
                float d = d0 + (float)(r - 3) * px;
                float cR[4], cG[4]; gradWindow(float2(e.x + d, e.y), px, cR, cG);
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

            // Borrow trust: the disparity match relies on the REFERENCE channel (red
            // for the left eye, green for the right). Where that channel is flat the
            // match is meaningless and the borrow lands anywhere -> spurious cross-eye
            // colour (the high-contrast red). Gate ONLY the borrowed channel(s) by the
            // reference's gradient energy, blending them toward this eye's luminance
            // when unreliable. Each eye's OWN channel(s) are untouched, so genuine
            // colour is preserved (this is NOT a global desaturation).
            // Confidence of this pixel's cross-eye borrow = reference-channel
            // structure x match quality. Written to ALPHA; the colour-propagation
            // pass keeps confident pixels and OVERWRITES low-confidence ones (flat
            // regions, occlusions, spurious-red borrows) with colour diffused from
            // reliable same-region neighbours -- SIRA-style colorization without
            // desaturation (real colour) or eye-mixing (no bleed).
            float refEnergy = 0;
            [unroll] for (int k = 0; k < 4; ++k) refEnergy += abs((eye == 0) ? rgR[k] : rgG[k]);
            // x baseConf folds in the disparity map's left-right consistency AND match
            // uniqueness, so a confidently-WRONG borrow (e.g. ambiguous text strokes,
            // high contrast but a rival match) is now low-confidence and gets filled.
            float conf = saturate(refEnergy * 8.0) * saturate(1.0 - bs * 1.2) * baseConf;

            // Block processing (SIRA): borrow the aligned colour as a small patch
            // average. FULL borrow here; propagation cleans up the low-confidence ones.
            float3 there = 0;
            [unroll] for (int by = -1; by <= 1; ++by)
            [unroll] for (int bx = -1; bx <= 1; ++bx)
                there += srcTex.SampleLevel(samp, float2(e.x + dRef + (float)bx * px, e.y + (float)by * py), 0.0).rgb;
            there /= 9.0;

            // PER-EYE: own channel(s) + the missing channel(s) from the aligned borrow.
            float3 alignedCol = (eye == 0) ? float3(c.r, there.g, there.b)
                                           : float3(there.r, c.g, c.b);
            float aY = max(dot(alignedCol, float3(0.299, 0.587, 0.114)), 1e-3);
            return float4(saturate(alignedCol * (eyeY / aY)), conf);
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
               float dispMaxUV; float coarseW; float coarseH; float propStride; };
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
        { "PSAnaDisp",    &m_psCoarse  },
        { "PSAnaRefine",  &m_psRefine  },
        { "PSAnaFill",    &m_psFill    },
        { "PSAnaSmooth",  &m_psSmooth  },
        { "PSAnaSplit",   &m_psSplit   },
        { "PSAnaCompose", &m_psCompose },
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

    // Per-eye colour pyramids for push-pull colorization (each half of the SBS).
    const int ew = (width / 2) < 1 ? 1 : (width / 2);
    const int eh = height < 1 ? 1 : height;
    m_ppMips = 1;
    for (int w = ew, h = eh; w > 1 || h > 1; ) { w = (w > 1) ? w / 2 : 1; h = (h > 1) ? h / 2 : 1; ++m_ppMips; }

    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = (UINT)ew; pd.Height = (UINT)eh; pd.MipLevels = (UINT)m_ppMips; pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; pd.SampleDesc.Count = 1; pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    pd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(m_device->CreateTexture2D(&pd, nullptr, &m_ppLeftTex)))  { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateRenderTargetView(m_ppLeftTex, nullptr, &m_ppLeftRTV)))  { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_ppLeftTex, nullptr, &m_ppLeftSRV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateTexture2D(&pd, nullptr, &m_ppRightTex))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateRenderTargetView(m_ppRightTex, nullptr, &m_ppRightRTV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_ppRightTex, nullptr, &m_ppRightSRV))) { ReleaseOutput(); return false; }

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

    // Push-pull colorization (anaglyph recovery): split the recovered SBS into two
    // premultiplied per-eye pyramids, GenerateMips (= confidence-weighted averages),
    // then re-compose, filling low-confidence pixels from the coarsest valid colour.
    if (anaRecover && m_ppLeftRTV && m_ppRightRTV)
    {
        auto cbProp = [&](float v) {
            CB pcb{ 0, 0, (float)srcWidth, (float)srcHeight, 0, 0, 0, 0,
                    0, 0, 0, 0, dispMaxUV, 0, 0, v };
            m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &pcb, 0, 0);
        };
        ID3D11ShaderResourceView* nul = nullptr;
        const int ew = m_outWidth / 2, eh = m_outHeight;

        // SPLIT: m_outTex left/right half -> premultiplied per-eye mip 0.
        m_context->PSSetShader(m_psSplit, nullptr, 0);
        D3D11_VIEWPORT evp{}; evp.Width = (FLOAT)ew; evp.Height = (FLOAT)eh; evp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &evp);
        cbProp(0.0f);  m_context->OMSetRenderTargets(1, &m_ppLeftRTV, nullptr);
        m_context->PSSetShaderResources(0, 1, &m_outSRV); m_context->Draw(3, 0);
        m_context->PSSetShaderResources(0, 1, &nul);
        cbProp(0.5f);  m_context->OMSetRenderTargets(1, &m_ppRightRTV, nullptr);
        m_context->PSSetShaderResources(0, 1, &m_outSRV); m_context->Draw(3, 0);
        m_context->PSSetShaderResources(0, 1, &nul);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);

        // PULL: confidence-weighted averages at every coarser level.
        m_context->GenerateMips(m_ppLeftSRV);
        m_context->GenerateMips(m_ppRightSRV);

        // COMPOSE: fill from the pyramids back into the SBS (m_outTex).
        cbProp((float)(m_ppMips - 1));
        D3D11_VIEWPORT ovp{}; ovp.Width = (FLOAT)m_outWidth; ovp.Height = (FLOAT)m_outHeight; ovp.MaxDepth = 1.0f;
        m_context->PSSetShader(m_psCompose, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
        m_context->RSSetViewports(1, &ovp);
        ID3D11ShaderResourceView* pp[2] = { m_ppLeftSRV, m_ppRightSRV };
        m_context->PSSetShaderResources(0, 2, pp);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls2[2] = { nullptr, nullptr };
        m_context->PSSetShaderResources(0, 2, nulls2);
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
    SAFE_RELEASE(m_ppLeftSRV);  SAFE_RELEASE(m_ppLeftRTV);  SAFE_RELEASE(m_ppLeftTex);
    SAFE_RELEASE(m_ppRightSRV); SAFE_RELEASE(m_ppRightRTV); SAFE_RELEASE(m_ppRightTex);
    m_ppMips = 0;
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
    SAFE_RELEASE(m_psCompose);
    SAFE_RELEASE(m_psSplit);
    SAFE_RELEASE(m_psSmooth);
    SAFE_RELEASE(m_psFill);
    SAFE_RELEASE(m_psRefine);
    SAFE_RELEASE(m_psCoarse);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
