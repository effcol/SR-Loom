// Converter shaders: stereo layout conversion, anaglyph recovery.
// Compiled at BUILD time (CMakeLists.txt, fxc) into headers the app embeds --
// nothing is compiled when SR Loom starts.

Texture2D    srcTex  : register(t0);   // current source frame
Texture2D    srcPrev : register(t1);   // delayed source frame (Pulfrich time delay)
Texture2D    dispTex : register(t2);   // disparity map: .r=dLR .g=dRL (UV), .b=confidence
Texture2D<uint4> descTexA : register(t3);   // anaglyph recovery: packed gradient descriptors
Texture2D<uint4> descTexB : register(t4);   // (PSAnaDesc; A+B left view, C+D right view)
Texture2D<uint4> descTexC : register(t5);
Texture2D<uint4> descTexD : register(t6);
Texture2D    dispPrevTex : register(t7);   // anaglyph recovery: last frame's smoothed disparity
Texture2D    srcPrevQ    : register(t8);   // ... and last frame's 1/4 source (what changed since)
Texture2D    srcQ        : register(t9);   // anaglyph recovery: the 1/4 source again (the compose's edge-aware upsampling)
Texture2D    picRectTex  : register(t10);  // anaglyph recovery: where the 3D picture is (PSPicRect: .xy top-left, .zw bottom-right, source uv)
Texture2D    profColTex  : register(t11);  // ... (PSPicCols / PSPicRows: each column's / row's share of anaglyph fringes)
Texture2D    profRowTex  : register(t12);
SamplerState samp    : register(s0);

cbuffer Params : register(b0)
{
    int   g_format;   // 0 SBS,1 TAB,2 Anaglyph,3 Row,4 Col,5 Checker,6 Pulfrich,7 FP,8 FrameSeq,9 Quilt,99 copy
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
    float g_convergence; // convergence: per-eye horizontal shift (moves the zero plane)
    float g_dispMaxUV; // anaglyph recovery: max search disparity (fraction of width)
    float g_coarseW;   // coarse disparity-map width (px)
    float g_coarseH;   // coarse disparity-map height (px)
    float g_propStride; // colour-propagation pass: neighbour sample stride (texels)
    float g_fpEyeAlign;    // frame packing: bottom-eye vertical alignment (source rows)
    int   g_quiltCols;     // quilt: grid columns of views
    int   g_quiltRows;     // quilt: grid rows of views
    int   g_quiltLeftIdx;  // quilt: view index that feeds the LEFT  pane (integer floor)
    int   g_quiltRightIdx; // quilt: view index that feeds the RIGHT pane (integer floor)
    float g_paneW;         // SBS pane width  in px (for aspect / letterbox math)
    float g_paneH;         // SBS pane height in px
    float g_quiltLBlend;   // L pane: cross-fade from view[leftIdx]  to view[leftIdx+1]
    float g_quiltRBlend;   // R pane: cross-fade from view[rightIdx] to view[rightIdx+1]
    float g_vrYaw;         // VR viewer: yaw (radians, 0 = looking forward)
    float g_vrPitch;       // VR viewer: pitch (radians, 0 = level)
    float g_vrZoom;        // VR viewer: zoom (1 = ~90° HFOV)
    int   g_vrIs360;       // VR: 0 = 180° hemisphere, 1 = 360° sphere
    int   g_vrIsSBS;       // VR: 0 = top-and-bottom packing, 1 = side-by-side
    float g_temporal;      // anaglyph recovery: last frame's disparity is there to steady this one (1)
    float g_lvlToSrcX;     // anaglyph recovery: the disparity levels cover whole 16-px blocks, a little past
    float g_lvlToSrcY;     // the picture's edge -- a level's uv x this = the picture's uv (>= 1)
    float _pad_e;
    float _pad_f;
    float _pad_g;
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

// Anaglyph recovery: the channel matched for each view's disparity, per combo --
// one that carries only that eye's picture (red vs green for red/cyan: blue is
// left out, it adds noise). Amber/blue's left view is red+green.
float anaChanL(float3 c)
{
    if (g_anaCombo == 3 || g_anaCombo == 5) return c.g;          // Green/Magenta, Cyan/Magenta
    if (g_anaCombo == 4) return (c.r + c.g) * 0.5;               // Amber/Blue
    return c.r;                                                  // Red/Cyan, Red/Green, Red/Blue
}
float anaChanR(float3 c)
{
    if (g_anaCombo == 2 || g_anaCombo == 4) return c.b;          // Red/Blue, Amber/Blue
    if (g_anaCombo == 3 || g_anaCombo == 5) return c.r;          // Green/Magenta, Cyan/Magenta
    return c.g;                                                  // Red/Cyan, Red/Green
}

// The picture's rectangle (see PSPicRect), in the source's uv; unset -> all.
float4 picRectSrc()
{
    const float4 r = picRectTex.Load(int3(0, 0, 0));
    return (r.z > r.x && r.w > r.y) ? r : float4(0, 0, 1, 1);
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
        rr[w] = anaChanL(s); gg[w] = anaChanR(s);   // (rr: left view's channel, gg: right's)
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
            rr[i] = anaChanL(s); gg[i] = anaChanR(s);
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
    float sad = 0, ea = 0, eb = 0;
    [unroll] for (int j = 0; j < 16; ++j)
    {
        sad += abs(a16[j] - b16[j]);
        ea  += abs(a16[j]);
        eb  += abs(b16[j]);
    }
    const float cost = sad / (ea + eb + 0.15);
    // A candidate where either view has next to no structure can't be
    // checked: NEUTRAL, not bad. Scored as a mismatch, the inside of a plain
    // coloured surface (red paint is blank in the cyan view) always lost to
    // any candidate out on textured background, however poor -- the borrow
    // was pulled out of the surface into its surroundings: the blobs.
    const float evidence = saturate(min(ea, eb) / 0.6);
    return lerp(0.4, cost, evidence);
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
    // Search 6% of the PICTURE's width (see PSPicRect), in this level's uv,
    // and never outside the picture.
    const float4 pr = picRectSrc();
    const float picL = pr.x / g_lvlToSrcX, picR = pr.z / g_lvlToSrcX;
    float  maxD = g_dispMaxUV * (picR - picL);
    // Candidates every 8 source pixels (half a coarse texel), however wide
    // the area is: tying the step to the area's width searched the same
    // picture on a different candidate grid whenever the captured area
    // changed size -- and the result, blobs included, changed with it.
    const float step = 8.0 / (16.0 * max(g_coarseW, 1.0));   // (this level's uv: 16 source px per texel)
    const int N = (int)ceil(maxD / step);

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
        // (A candidate off the picture is never a match: its descriptor there is
        // the edge pixel repeated, which can look like one -- and then a whole
        // band beside the edge borrowed that single column: streaks.)
        if (uv.x + d < picL || uv.x + d > picR) continue;
        float cRed[16], cGrn[16]; buildDesc(uv + float2(d, 0), tx, ty, cRed, cGrn);
        float bias = abs(d) * 16.0 * g_coarseW * 0.0004;   // (a preference for small disparities, per pixel -- also not tied to the area's width)
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

// 4x box downsample of the bound source (t0): each output pixel is the average
// of the 4x4 source pixels under it, from 4 bilinear taps (each a 2x2 average).
// The disparity passes read these small copies instead of picking scattered
// pixels out of the full-size frame (which missed the texture cache on almost
// every read: the 1/16 search alone took ~4 ms at 4K).
float4 PSDown(VSOut i) : SV_Target
{
    // The 4x4 block of source pixels under this output pixel, found by pixel
    // position (block centre = 4 x this pixel's centre), not by uv: the level
    // is sized in whole blocks, a little past the picture's edge (clamped
    // there), so its grid never drifts against the picture.
    // Anti-aliased: a tent over the 8x8 around the block (weights 1,3,3,1
    // per axis, from 16 bilinear taps each averaging a pixel pair), not a
    // plain box average. A box average jumps as the picture moves across the
    // block grid by a pixel or two; everything downstream (descriptors,
    // matches) jumped with it -- blobs came and went while scrolling.
    uint W, H;
    srcTex.GetDimensions(W, H);
    const float2 t = 1.0 / float2(W, H);
    const float2 c = i.pos.xy * 4.0 * t;
    const float o[4] = { -3.0, -1.0, 1.0, 3.0 };
    const float wt[4] = { 1.0, 3.0, 3.0, 1.0 };
    float4 acc = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
        acc += srcTex.SampleLevel(samp, c + t * float2(o[x], o[y]), 0) * (wt[x] * wt[y]);
    return acc / 64.0;
}

// Descriptor pass: each refine-level pixel's 4-angle gradient descriptor
// (buildDesc: 16 left-view + 16 right-view values, 20 texture reads), worked
// out ONCE and stored as 32 halves in four uint4 targets. The refine below
// compares 13 candidates per direction; reading their descriptors from here
// costs 2 loads each instead of 20 samples.
struct DescOut { uint4 a : SV_Target0; uint4 b : SV_Target1; uint4 c : SV_Target2; uint4 d : SV_Target3; };
uint packH(float x, float y) { return f32tof16(x) | (f32tof16(y) << 16); }
DescOut PSAnaDesc(VSOut i)
{
    float tx = 1.0 / max(g_coarseW, 1.0);
    float ty = 1.0 / max(g_coarseH, 1.0);
    float L[16], R[16]; buildDesc(i.uv, tx, ty, L, R);
    DescOut o;
    o.a = uint4(packH(L[0], L[1]),  packH(L[2], L[3]),   packH(L[4], L[5]),   packH(L[6], L[7]));
    o.b = uint4(packH(L[8], L[9]),  packH(L[10], L[11]), packH(L[12], L[13]), packH(L[14], L[15]));
    o.c = uint4(packH(R[0], R[1]),  packH(R[2], R[3]),   packH(R[4], R[5]),   packH(R[6], R[7]));
    o.d = uint4(packH(R[8], R[9]),  packH(R[10], R[11]), packH(R[12], R[13]), packH(R[14], R[15]));
    return o;
}
void unpack8(uint4 v, out float o8[8])
{
    o8[0] = f16tof32(v.x); o8[1] = f16tof32(v.x >> 16); o8[2] = f16tof32(v.y); o8[3] = f16tof32(v.y >> 16);
    o8[4] = f16tof32(v.z); o8[5] = f16tof32(v.z >> 16); o8[6] = f16tof32(v.w); o8[7] = f16tof32(v.w >> 16);
}
void loadDescL(int2 p, out float d[16])   // left view's descriptor at a refine-level pixel
{
    float a[8], b[8];
    unpack8(descTexA.Load(int3(p, 0)), a); unpack8(descTexB.Load(int3(p, 0)), b);
    [unroll] for (int k = 0; k < 8; ++k) { d[k] = a[k]; d[k + 8] = b[k]; }
}
void loadDescR(int2 p, out float d[16])   // right view's
{
    float a[8], b[8];
    unpack8(descTexC.Load(int3(p, 0)), a); unpack8(descTexD.Load(int3(p, 0)), b);
    [unroll] for (int k = 0; k < 8; ++k) { d[k] = a[k]; d[k + 8] = b[k]; }
}

// Pyramid refine: start from a coarser level (dispTex = prior) and do a local
// 4-angle gradient search to sharpen the disparity. Outputs (dLR, dRL, 0, 0).
// Candidates sit on this level's pixel grid (the prior rounded to a whole
// pixel: the full-res pass refines to sub-pixel afterwards), so each one's
// descriptor comes from PSAnaDesc's targets.
float4 PSAnaRefine(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    int2   p  = int2(i.pos.xy);
    int    W  = (int)g_coarseW;
    float4 priorAll = dispTex.SampleLevel(samp, uv, 0.0);   // .rg disparity, .ba uniqueness
    const float4 pr = picRectSrc();                          // (the picture, in this level's px -- see PSPicRect)
    const float limL = pr.x / g_lvlToSrcX * W, lim = pr.z / g_lvlToSrcX * W;

    float refL[16], refR[16]; loadDescL(p, refL); loadDescR(p, refR);

    // Hypotheses: the coarse answer here (interpolated), searched +-6, and
    // the actual answers of the 2x2 coarse blocks around, each +-2. At an
    // object's edge the interpolated answer is a blend belonging to neither
    // side, and a thin object (a leg, a pole) narrower than a coarse block
    // often has the background's -- reachable, or not, depending on where
    // the block grid falls: blobs that came and went as the picture moved.
    uint cw, ch; dispTex.GetDimensions(cw, ch);
    const int2 b0 = int2(floor(uv * float2(cw, ch) - 0.5));
    float bestL = 1e9, dL = priorAll.r, bestR = 1e9, dR = priorAll.g;
    [loop] for (int h = 0; h < 5; ++h)
    {
        float2 pr = priorAll.rg;
        if (h > 0)
        {
            const int2 q = clamp(b0 + int2((h - 1) & 1, (h - 1) >> 1), int2(0, 0), int2(cw - 1, ch - 1));
            pr = dispTex.Load(int3(q, 0)).rg;
        }
        const int kL0 = (int)round(pr.r * g_coarseW);
        const int kR0 = (int)round(pr.g * g_coarseW);
        const int M = (h == 0) ? 6 : 2;
        [loop] for (int k = -M; k <= M; ++k)
        {
            const int rawL = p.x + kL0 + k, rawR = p.x + kR0 + k;
            const int xL = clamp(rawL, 0, W - 1);   // candidate for dLR (match the right view)
            const int xR = clamp(rawR, 0, W - 1);   // candidate for dRL (match the left view)
            float candR[16], candL[16];
            loadDescR(int2(xL, p.y), candR);
            loadDescL(int2(xR, p.y), candL);
            // (Ties -- a flat area, where every candidate costs the same -- go
            // to the interpolated coarse answer, k = 0: the loop used to keep
            // the first, far-left candidate, so flat areas got a disparity of
            // -6 texels for no reason, and smoothing mixed those into the
            // picture's own next door -- borrows landing in the wrong place.)
            const float tie = (abs((float)k) + (h > 0 ? 1.0 : 0.0)) * 0.002;
            float sadL = descCost(refL, candR) + tie;   // ref left view vs candidate right view
            float sadR = descCost(refR, candL) + tie;   // ref right view vs candidate left view
            // (Off the picture: never, see PSAnaDisp.)
            if (sadL < bestL && rawL >= limL && rawL < lim) { bestL = sadL; dL = (float)(kL0 + k) * tx; }
            if (sadR < bestR && rawR >= limL && rawR < lim) { bestR = sadR; dR = (float)(kR0 + k) * tx; }
        }
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
    // (Left-right agreement is judged in pixels -- 4 source px per texel here.
    // As a fraction of the width, a bigger captured area tolerated more
    // pixels of disagreement: another way the result changed with its size.)
    const float inv = 4.0 * g_coarseW / 38.4;
    float4 dd = dispTex.SampleLevel(samp, uv, 0.0);   // .rg disparity, .b uniqL, .a uniqR
    float2 d = dd.rg;

    float backR = dispTex.SampleLevel(samp, float2(uv.x + d.r, uv.y), 0.0).g;
    float backL = dispTex.SampleLevel(samp, float2(uv.x + d.g, uv.y), 0.0).r;
    float cons  = max(abs(d.r + backR), abs(d.g + backL));
    float conf  = saturate(1.0 - cons * inv);
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
            float  nc  = saturate(1.0 - abs(nd.r + dispTex.SampleLevel(samp, float2(nuv.x + nd.r, nuv.y), 0.0).g) * inv);
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
    float  y0 = anaChanL(c0) + anaChanR(c0);       // both views' channels: luminance proxy
    float2 myConf = dispTex.SampleLevel(samp, uv, 0.0).ba;   // per-eye confidence (.b left, .a right)

    // Weighted median (medoid) of the 5x5, not a weighted mean: the result is
    // one of the neighbours' actual disparities -- the one that agrees best
    // with the rest. A mean blended a near object's disparity with the
    // background's at every edge (a value belonging to neither), and dragged
    // a correct value toward wrong neighbours; on real stereo pairs that is
    // where ~9% of the blob pixels, right until here, were lost.
    float2 nds[25]; float nws[25];
    const int R = 2;
    [unroll] for (int dy = -R; dy <= R; ++dy)
    [unroll] for (int dx = -R; dx <= R; ++dx)
    {
        const int idx = (dy + R) * 5 + (dx + R);
        float2 nuv = uv + float2((float)dx * tx, (float)dy * ty);
        float4 nd  = dispTex.SampleLevel(samp, nuv, 0.0);
        float3 nc  = srcTex.SampleLevel(samp, nuv, 0.0).rgb;
        float  ws  = exp(-(float)(dx * dx + dy * dy) / 8.0);   // spatial
        float  wl  = exp(-abs(y0 - (anaChanL(nc) + anaChanR(nc))) * 6.0);      // luminance (edge-aware)
        nds[idx] = nd.rg; nws[idx] = ws * wl * (0.2 + max(nd.b, nd.a));        // trust confident neighbours
    }
    float2 d = nds[12]; float bestL = 1e9, bestR = 1e9;
    [loop] for (int a = 0; a < 25; ++a)
    {
        float sL = 0, sR = 0;
        [unroll] for (int b = 0; b < 25; ++b) { sL += nws[b] * abs(nds[a].x - nds[b].x); sR += nws[b] * abs(nds[a].y - nds[b].y); }
        if (sL < bestL) { bestL = sL; d.x = nds[a].x; }
        if (sR < bestR) { bestR = sR; d.y = nds[a].y; }
    }
    // Video: each frame's disparity is estimated afresh and wobbles a little,
    // which showed as shimmering borrowed colour. Where this spot looks as it
    // did last frame, keep most of last frame's disparity; where it changed
    // (motion, a cut), take the new estimate.
    if (g_temporal > 0.5)
    {
        const float3 was = srcPrevQ.SampleLevel(samp, uv, 0.0).rgb;
        const float change = dot(abs(c0 - was), float3(1.0, 1.0, 1.0)) / 3.0;
        const float keep = 0.6 * saturate(1.0 - change * 20.0);   // (~5% change: none kept)
        d = lerp(d, dispPrevTex.SampleLevel(samp, uv, 0.0).rg, keep);
    }
    return float4(d, myConf.x, myConf.y);
}

// ----- Where the picture is (anaglyph recovery) ----------------------------
// The captured area can be more than the 3D picture: a page around it, black
// bars, a desktop. The search range was 6% of the AREA's width (reaching far
// past a small picture's real depth: far matches, blobs), matches could land
// on the page, and the page itself got "recovered" -- colour borrowed into
// plain grey. These passes find the picture from the anaglyph's own
// signature: inside it the two eyes' channels disagree at many spots (the
// colour fringes of anything with depth); a page, bars or plain UI are
// neutral. Per column and row of the 1/16 picture, the share of such spots;
// the picture is the span well above the page's. The recovery then searches
// 6% of the PICTURE's width, keeps matches inside it, and leaves everything
// outside it as it is. (A whole-screen anaglyph just gives the whole screen.)
// (A spot counts as picture if the eyes' channels disagree there OR it has any
// structure -- a page, bars or plain UI are flat. Being generous matters: a
// real row left out of the rectangle is shown unrecovered.)
float anaSignature(float3 c) { return abs(anaChanL(c) - anaChanR(c)) > 0.03 ? 1.0 : 0.0; }
float picEvidence(int x, int y)
{
    const float3 c = srcTex.Load(int3(x, y, 0)).rgb;
    uint W, H; srcTex.GetDimensions(W, H);   // (neighbours clamped: past the edge reads black)
    const float3 r = srcTex.Load(int3(min(x + 1, (int)W - 1), y, 0)).rgb, d = srcTex.Load(int3(x, min(y + 1, (int)H - 1), 0)).rgb;
    const float s = max(max(abs(c.r - r.r), abs(c.g - r.g)), max(abs(c.r - d.r), abs(c.g - d.g)));
    return max(anaSignature(c), s > 0.01 ? 1.0 : 0.0);
}
// Target W16 x 1: each column's share.
float4 PSPicCols(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    const int x = (int)i.pos.x; float n = 0;
    [loop] for (int y = 0; y < (int)H; ++y) n += picEvidence(x, y);
    return float4(n / H, 0, 0, 0);
}
// Target 1 x H16: each row's share.
float4 PSPicRows(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    const int y = (int)i.pos.y; float n = 0;
    [loop] for (int x = 0; x < (int)W; ++x) n += picEvidence(x, y);
    return float4(n / W, 0, 0, 0);
}
// Target 1x1: the rectangle, in the source's uv. First and last column (row)
// above a quarter of the busiest one's share (and 2%), padded by one coarse
// texel (the picture's edge lies somewhere inside that texel). Nothing clear:
// the whole area.
float2 picSpan(Texture2D prof, int n, bool rows)
{
    float mx = 0;
    [loop] for (int k = 0; k < n; ++k) mx = max(mx, prof.Load(int3(rows ? 0 : k, rows ? k : 0, 0)).r);
    const float thr = max(0.1 * mx, 0.01);
    int a = -1, b = -1;
    [loop] for (int k2 = 0; k2 < n; ++k2)
        if (prof.Load(int3(rows ? 0 : k2, rows ? k2 : 0, 0)).r >= thr) { if (a < 0) a = k2; b = k2; }
    if (a < 0) return float2(0, n);
    return float2(a, b + 1);   // (texels [a, b] inclusive: the one holding the picture's edge has the edge's structure)
}
float4 PSPicRect(VSOut i) : SV_Target
{
    uint cw, one, rh, one2;
    profRowTex.GetDimensions(one, rh); profColTex.GetDimensions(cw, one2);
    const float2 sx = picSpan(profColTex, (int)cw, false), sy = picSpan(profRowTex, (int)rh, true);
    // (Coarse texels are 16 source px: to the source's uv.)
    const float2 k = float2(16.0 / g_srcW, 16.0 / g_srcH);
    return saturate(float4(sx.x * k.x, sy.x * k.y, sx.y * k.x, sy.y * k.y));
}
// (There was a push-pull colour fill here -- confidence-weighted colour
// pyramids filling low-confidence pixels. Its sampler never read past the
// finest level, so it only ever returned each pixel's own colour: the look
// everything was tuned to. It cost ~3 ms a frame at 4K for no change and was
// removed; enabling it for real washed the picture out.)

// ----- Lanczos-3 sampling ---------------------------------------------------
// Sinc-windowed Lanczos-3 kernel — gold-standard non-ML upscale (FSR1 / mpv /
// every quality image viewer build on this). 36-tap, sharper than bilinear or
// Catmull-Rom and the right pick when a small quilt cell has to fill a much
// larger SR panel pane. Filters in linear light: the sRGB SRV does the
// sRGB->linear conversion on Load() and the SRGB RTV does linear->sRGB on write,
// so the math here is already linear.
float lanczos3Weight(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    const float pi = 3.14159265358979;
    float px = pi * x;
    return (sin(px) * sin(px / 3.0)) / (px * px / 3.0);
}

// 6x6 Lanczos-3 sample inside a rectangular sub-region of srcTex, clamped so
// the kernel never reaches into neighbouring quilt cells (each cell is a
// different view -- bleeding would ghost cross-view content).
float3 sampleLanczos3Cell(float2 uvWithinCell, int2 cellMinPx, int2 cellSizePx)
{
    float2 px   = uvWithinCell * float2(cellSizePx) - 0.5;
    int2   ip   = int2(floor(px));
    float2 frac = px - float2(ip);
    float3 acc  = 0;
    float  wsum = 0;
    [unroll] for (int dy = -2; dy <= 3; ++dy)
    {
        float wy = lanczos3Weight((float)dy - frac.y);
        [unroll] for (int dx = -2; dx <= 3; ++dx)
        {
            float wx = lanczos3Weight((float)dx - frac.x);
            float w = wx * wy;
            int2 q = clamp(int2(ip.x + dx, ip.y + dy),
                           int2(0, 0), cellSizePx - int2(1, 1));
            acc  += srcTex.Load(int3(cellMinPx + q, 0)).rgb * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-5);
}

float4 ConvertCore(VSOut i)
{
    if (g_format == 99) return srcTex.Sample(samp, i.uv);   // 1:1 copy (history blit)

    float2 uv = i.uv;                       // 0..1 across the SBS output
    bool rightPane = uv.x >= 0.5;           // which output half we're filling
    // within-pane 0..1 (from the OUTPUT pane, always valid)
    float2 e = float2(rightPane ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);
    // which eye's content goes into this pane (eye swap only affects content)
    bool right = rightPane;
    if (g_swap) right = !right;

    // Convergence: shift each eye's sampled content horizontally in opposite
    // directions, moving the zero-disparity plane in/out of the screen.
    e.x += (right ? -g_convergence : g_convergence);

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
        if (g_anaMode == 4)   // (any colour pair: see anaChanL / anaChanR)
        {
            float px = 1.0 / g_srcW;
            float py = 1.0 / g_srcH;
            // Outside the 3D picture (a page, bars -- see PSPicRect) nothing is
            // recovered: it's shown as it is, the same in both eyes. (It used to
            // borrow colour from the picture next to it: blobs on the page.)
            const float4 picR = picRectSrc();
            if (e.x < picR.x || e.x > picR.z || e.y < picR.y || e.y > picR.w) return float4(c, 1);
            // Where both eyes see the same plain grey here -- the anaglyph flat and
            // neutral around this pixel -- what's shown already IS each eye's
            // colour: nothing to recover (a page beside the picture, plain grey
            // sky), and a borrow could only bring the wrong colour in.
            {
                float dev = max(abs(c.r - c.g), abs(c.r - c.b));
                [unroll] for (int fy = -1; fy <= 1; ++fy)
                [unroll] for (int fx = -1; fx <= 1; ++fx)
                {
                    const float3 q = srcTex.SampleLevel(samp, e + float2(fx * 3.0, fy * 3.0) * float2(1.0 / g_srcW, 1.0 / g_srcH), 0).rgb;
                    dev = max(dev, max(max(abs(q.r - c.r), abs(q.g - c.g)), abs(q.b - c.b)));
                    dev = max(dev, max(abs(q.r - q.g), abs(q.r - q.b)));
                }
                if (dev < 0.01) return float4(c, 1);
            }
            // (The disparity levels cover whole 16-px blocks past the picture's
            // edge: their uv, and their disparities, are the picture's / g_lvlToSrc.)
            // Edge-aware upsampling (joint bilateral, Kopf et al. 2007): the
            // 2x2 disparity texels around this pixel (bilinear's), each weighted by how like
            // this pixel its 4x4 block's colour is. A texel straddling an edge
            // carries the disparity of whatever dominates it; plain bilinear
            // handed that to every pixel it covered -- page or background
            // pixels beside a picture or object took ITS disparity, and their
            // borrow landed on it: the fringe blobs.
            float4 dC = 0; float jw = 0;
            {
                uint dw, dh; dispTex.GetDimensions(dw, dh);
                const float2 lp = e / float2(g_lvlToSrcX, g_lvlToSrcY) * float2(dw, dh) - 0.5;   // level texel space
                const int2 b0 = int2(floor(lp));
                [unroll] for (int jy = 0; jy <= 1; ++jy)
                [unroll] for (int jx = 0; jx <= 1; ++jx)
                {
                    const int2 q = clamp(b0 + int2(jx, jy), int2(0, 0), int2(dw - 1, dh - 1));
                    const float2 dd = abs(lp - (float2)(b0 + int2(jx, jy)));
                    const float ws = max(0.0, 1.0 - dd.x) * max(0.0, 1.0 - dd.y);        // (bilinear's own weights)
                    const float3 qc = srcQ.Load(int3(q, 0)).rgb;
                    const float wc = exp(-dot(abs(qc - c), float3(1, 1, 1)) * 8.0);
                    const float wgt = ws * wc + 1e-6 * ws;
                    dC += dispTex.Load(int3(q, 0)) * wgt; jw += wgt;
                }
                dC /= max(jw, 1e-8);
            }
            float d0 = ((eye == 0) ? dC.r : dC.g) * g_lvlToSrcX; // this eye -> the other
            // COUPLE the eyes: a region unreliable in EITHER eye is treated unreliable
            // in BOTH (min), so it's inpainted symmetrically -> no one-eye-clean /
            // other-eye-splotch rivalry. (Both pyramids are regional averages of the
            // same scene, so they fill to near-identical colour.)
            float baseConf = min(dC.b, dC.a);                    // consistency x uniqueness, coupled

            // Reference gradient descriptor at e (red for the left eye, green for the
            // right) for the full-res gradient-matching refine.
            float rgR[4], rgG[4]; gradWindow(e, px, rgR, rgG);

            // Full-res refine (±3 px) with sub-pixel parabola fit on the cost curve.
            // The 7 candidates' 5-tap windows (taps and candidates both 1 px apart)
            // cover just 11 positions: candidate r, tap w sits at d0 + (r + w - 5) px.
            // Fetch each once -- the same values as a window per candidate, from 11
            // texture reads instead of 35.
            float sr[11], sg[11];
            [unroll] for (int q = 0; q < 11; ++q)
            {
                float3 s = srcTex.SampleLevel(samp, float2(e.x + d0 + (float)(q - 5) * px, e.y), 0).rgb;
                sr[q] = anaChanL(s); sg[q] = anaChanR(s);
            }
            float sads[7];
            [unroll] for (int r = 0; r < 7; ++r)
            {
                float sd = 0.0;
                [unroll] for (int k2 = 0; k2 < 4; ++k2)
                    sd += (eye == 0) ? abs(rgR[k2] - (sg[r + k2 + 1] - sg[r + k2]))    // left: red ref vs green cand
                                     : abs(rgG[k2] - (sr[r + k2 + 1] - sr[r + k2]));   // right: green ref vs red cand
                sads[r] = sd;
            }
            // (Ties go to the centre -- the disparity from the maps -- not to the
            // first candidate, which shifted every flat area by -3 px.)
            [unroll] for (int t0 = 0; t0 < 7; ++t0) sads[t0] += abs((float)(t0 - 3)) * 0.002;
            int bi = 3; float bs = sads[3];
            [unroll] for (int t = 0; t < 7; ++t) if (sads[t] < bs) { bs = sads[t]; bi = t; }
            float dRef = d0 + (float)(bi - 3) * px;
            if (bi > 0 && bi < 6)   // parabola vertex from the two neighbouring costs
            {
                float cm = sads[bi - 1], cc0 = sads[bi], cp = sads[bi + 1];
                float den = cm - 2.0 * cc0 + cp;
                float delta = (abs(den) > 1e-5) ? 0.5 * (cm - cp) / den : 0.0;
                dRef += clamp(delta, -1.0, 1.0) * px;
            }
            // (The borrow stays inside the picture: beyond its edge is the page.)
            dRef = clamp(e.x + dRef, picR.x, picR.z - px) - e.x;

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

            // Block processing (SIRA-style) -- LUMINANCE-WEIGHTED 3x3 borrow. The
            // matched centre tap defines the "patch region"; each neighbour is
            // exponentially down-weighted by its luminance difference from the
            // centre, so taps that land across an object edge at the matched
            // location contribute almost nothing. Plain 9-tap averaging smeared
            // cross-eye colour across edges -> the persistent borrow-edge
            // marbelling. SIRA's superpixel containment serves the same purpose;
            // luminance-weighting is the shader-feasible analogue.
            float3 centreC = srcTex.SampleLevel(samp, float2(e.x + dRef, e.y), 0.0).rgb;
            float  centreY = dot(centreC, float3(0.299, 0.587, 0.114));
            float3 there = 0;
            float  tw    = 1e-4;
            [unroll] for (int by = -1; by <= 1; ++by)
            [unroll] for (int bx = -1; bx <= 1; ++bx)
            {
                float3 s = srcTex.SampleLevel(samp, float2(e.x + dRef + (float)bx * px, e.y + (float)by * py), 0.0).rgb;
                float  sY = dot(s, float3(0.299, 0.587, 0.114));
                float  w  = exp(-abs(sY - centreY) * 8.0);  // ~0.125 luma delta -> ~37% weight
                there += s * w;
                tw    += w;
            }
            there /= tw;

            // Own channel(s) as seen, the other view's from the aligned borrow
            // (red/cyan left: own red, borrowed green + blue).
            float3 ownMask = anaFilter(float3(1, 1, 1), g_anaCombo, eye);
            float3 alignedCol = c * ownMask + there * (1.0 - ownMask);
            float aY = max(dot(alignedCol, float3(0.299, 0.587, 0.114)), 1e-3);
            // (Brought to this eye's brightness -- but if that pushes a channel
            // past full, the whole colour is scaled back rather than that channel
            // cut off: clipping one channel changed the hue, red paint going
            // pink or white even where the match was right.)
            float3 scaled = alignedCol * (eyeY / aY);
            const float peak = max(max(scaled.r, scaled.g), scaled.b);
            if (peak > 1.0) scaled /= peak;
            return float4(saturate(scaled), conf);
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
    // Frame-packing decode -- NTM-3D's corrected math from 3DToElse_NTM3D.fx (3DConsoleBridge).
    // Original 3DToElse shader (CC BY 3.0, Jose Negrete "BlueSkyDefender" + NTM-3D);
    // re-implemented in HLSL here. Capture devices squeeze the native 2205-line
    // frame-packing signal into a 16:9 capture (typically 1080 lines), which squeezes
    // the blanking gap too AND can leave the two eyes off by 1 row due to rounding. The
    // OLD approach (even-split + stretch from the middle) hid the gap by stretching,
    // distorting top/bottom geometry. This approach takes explicit top/bottom line
    // counts (derived from the preset's eyeFrac/gapFrac applied to the actual captured
    // height), uses sharedUsable = min(top, bottom) so both eyes contribute identical
    // vertical geometry, places the bottom eye at its TRUE start (totalLines - bottomLines,
    // not the even-split midpoint), and samples at pixel centres so bilinear doesn't
    // bleed in the blanking rows. g_fpEyeAlign is a fractional source-pixel shift on
    // the bottom eye for residual misalignment.
    else if (g_format == 7)   // HDMI 1.4 frame packing: top eye, gap, bottom eye
    {
        float totalLines  = g_srcH;
        float topLines    = floor(g_fpEyeFrac * totalLines + 0.5);
        float gapLines    = floor(g_fpGapFrac * totalLines + 0.5);
        float bottomLines = max(1.0, totalLines - topLines - gapLines);
        float sharedUsable = max(1.0, min(topLines, bottomLines));
        float bottomStart  = totalLines - bottomLines;

        // ("line" is a reserved word in HLSL -- it's a primitive topology type.)
        float srcRow = e.y * (sharedUsable - 1.0);
        float row  = right ? clamp(bottomStart + g_fpEyeAlign + srcRow, bottomStart, totalLines - 1.0)
                           : srcRow;
        float v = (row + 0.5) / totalLines;
        return srcTex.Sample(samp, float2(e.x, v));
    }
    else if (g_format == 8)   // Frame sequential: alternating L/R frames over time
    {
        // Each eye is a FULL frame. One eye shows the current frame, the other the
        // previous frame (= the other eye in genuinely frame-sequential content).
        // e.x is the within-pane 0..1, mapping to the full source. Swap eyes flips
        // which eye is current vs previous if the parity is wrong.
        if (right) return float4(srcTex.Sample(samp, e).rgb, 1);
        return float4(srcPrev.Sample(samp, e).rgb, 1);
    }
    else if (g_format == 9)   // Quilt: cols x rows grid of views; pick a pair
    {
        // Looking Glass convention: views indexed left-to-right, BOTTOM-to-top.
        // View 0 = bottom-left cell = leftmost camera position; view (cols*rows-1)
        // = top-right cell = rightmost camera position.
        int total  = max(1, g_quiltCols * g_quiltRows);
        int viewLo = clamp(right ? g_quiltRightIdx : g_quiltLeftIdx, 0, total - 1);
        int viewHi = min(viewLo + 1, total - 1);
        float blend = saturate(right ? g_quiltRBlend : g_quiltLBlend);
        int colLo  = viewLo % g_quiltCols;
        int rowLoB = viewLo / g_quiltCols;
        int colHi  = viewHi % g_quiltCols;
        int rowHiB = viewHi / g_quiltCols;

        // Preserve native view aspect inside the pane: when the SR panel pane
        // doesn't match the view's aspect (e.g. portrait view in a landscape
        // pane), pillar/letterbox with black bars instead of stretching.
        float viewAspect = (g_srcW / (float)g_quiltCols) / max(1.0, g_srcH / (float)g_quiltRows);
        float paneAspect = (g_paneH > 0.0) ? (g_paneW / g_paneH) : viewAspect;
        float2 ev = e;
        if (viewAspect < paneAspect)         // view narrower than pane -> pillarbox
        {
            float widthFrac = viewAspect / paneAspect;
            float marginX   = (1.0 - widthFrac) * 0.5;
            if (e.x < marginX || e.x > 1.0 - marginX) return float4(0, 0, 0, 1);
            ev.x = (e.x - marginX) / widthFrac;
        }
        else if (viewAspect > paneAspect)    // view wider than pane -> letterbox
        {
            float heightFrac = paneAspect / viewAspect;
            float marginY    = (1.0 - heightFrac) * 0.5;
            if (e.y < marginY || e.y > 1.0 - marginY) return float4(0, 0, 0, 1);
            ev.y = (e.y - marginY) / heightFrac;
        }

        // Lanczos-3 sample of EACH of the two bracketing views, clamped to its
        // own cell so the kernel can't bleed into adjacent views. Cross-fading
        // between them by the head-position fractional component gives Looking
        // Glass's smooth between-views transition. We skip the second sample
        // (and its 36 texture reads) when blend rounds to zero -- the common
        // case for a perfectly-still head pinned to one view.
        int viewWpx = max(1, (int)(g_srcW / (float)g_quiltCols));
        int viewHpx = max(1, (int)(g_srcH / (float)g_quiltRows));
        int2 cellLo = int2(colLo * viewWpx, (g_quiltRows - 1 - rowLoB) * viewHpx);
        float3 colourLo = sampleLanczos3Cell(ev, cellLo, int2(viewWpx, viewHpx));
        float3 result   = colourLo;
        if (blend > 0.002 && viewHi != viewLo)
        {
            int2 cellHi = int2(colHi * viewWpx, (g_quiltRows - 1 - rowHiB) * viewHpx);
            float3 colourHi = sampleLanczos3Cell(ev, cellHi, int2(viewWpx, viewHpx));
            result = lerp(colourLo, colourHi, blend);
        }
        return float4(result, 1);
    }
    else if (g_format == 10)  // VR180 / VR360 equirectangular projection
    {
        // Build a per-eye perspective view from an equirectangular source:
        //   1. The output pane represents a flat camera with horizontal FOV
        //      controlled by g_vrZoom (zoom = 1 -> ~90° HFOV).
        //   2. Compute the 3D ray direction for this output pixel through
        //      that virtual camera.
        //   3. Rotate the ray by the viewer's yaw + pitch to "look around".
        //   4. Project the rotated ray to spherical (lat, lon) and read the
        //      equirect texel. For VR180 we limit longitude to ±90° so the
        //      back hemisphere never samples.
        //   5. Stereo: the source packs L+R either Top-and-Bottom or
        //      Side-by-Side; pick the right half based on the output eye.

        // Camera frame: tan(half-FOV) sets the field of view.
        // zoom in [0.2..3], 1 -> ~half-tan 1 (so HFOV ~90°).
        float halfTan   = 1.0 / max(0.05, g_vrZoom);
        float paneAR    = (g_paneH > 0.0) ? (g_paneW / max(1.0, g_paneH)) : 1.0;
        float2 ndc      = float2((e.x * 2.0 - 1.0) * halfTan,
                                 (1.0 - e.y * 2.0) * (halfTan / max(0.0001, paneAR)));
        float3 ray      = normalize(float3(ndc.x, ndc.y, 1.0));

        // Apply pitch (X axis) then yaw (Y axis).
        float cp = cos(g_vrPitch), sp = sin(g_vrPitch);
        float3 r1 = float3(ray.x, cp * ray.y - sp * ray.z, sp * ray.y + cp * ray.z);
        float cy = cos(g_vrYaw),   sy = sin(g_vrYaw);
        float3 r2 = float3(cy * r1.x + sy * r1.z, r1.y, -sy * r1.x + cy * r1.z);

        // Spherical projection. lon = atan2(x, z) in [-pi..pi]; lat = asin(y)
        // in [-pi/2..pi/2]. Convert to UVs of the FULL sphere.
        float lon = atan2(r2.x, r2.z);
        float lat = asin(clamp(r2.y, -1.0, 1.0));
        const float PI = 3.14159265358979;

        // VR180: behind-the-camera samples return black instead of wrapping.
        if (g_vrIs360 == 0 && (lon < -0.5 * PI || lon > 0.5 * PI))
            return float4(0, 0, 0, 1);

        // u in [0..1] over either the full 360° (VR360) or the front 180° (VR180).
        float u = (g_vrIs360 == 1) ? (lon / (2.0 * PI) + 0.5)
                                   : (lon /        PI  + 0.5);
        float v = 0.5 - lat / PI;        // 0 at the top (+pi/2), 1 at the bottom

        // Stereo packing: select the half of the source for this eye.
        // Source layout: TAB -> top half = L, bottom half = R (the YouTube /
        // most-content convention). SBS -> left half = L, right half = R.
        float2 uv;
        if (g_vrIsSBS == 1)
            uv = float2((right ? 0.5 + u * 0.5 : u * 0.5), v);
        else
            uv = float2(u, (right ? 0.5 + v * 0.5 : v * 0.5));

        return float4(srcTex.SampleLevel(samp, uv, 0).rgb, 1);
    }

    // Default: side-by-side. left=left half, right=right half.
    // FullSBS (format 11) additionally crops the source vertically to the
    // centre 50%, where 32:9 letterboxed Full-SBS content lives when
    // displayed on a 16:9 source (screen capture or aspect-fit video).
    // HalfSBS (format 0) treats the whole source as already-shaped SBS.
    float vy = e.y;
    if (g_format == 11) vy = e.y * 0.5 + 0.25;
    float2 s = float2(right ? 0.5 + e.x * 0.5 : e.x * 0.5, vy);
    return srcTex.Sample(samp, s);
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 r = ConvertCore(i);
    // (Anaglyph recovery carries its confidence in alpha; the output is opaque.)
    if (g_format == 2 && g_anaMode == 4) r.a = 1.0;
    return r;
}
