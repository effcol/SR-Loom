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
Texture2D    anaTintTex  : register(t10);  // what the picture under the anaglyph was (Converter::SetAnaTint / SetAnaBoxes): tint tables, rows 0-1 the whole picture's, 2+2i/3+2i box i's (sRGB-encoded)
Texture2D    anaBoxTex   : register(t11);  // ... the boxes: [0].x count, then per box [1+2i] its rect (source uv), [2+2i] .x kind (1 Mono, 2 tint) .y its tint row
Texture2D    anaBoxOkTex : register(t12);  // ... whether each box still holds that, this frame (PSAnaBoxCheck: .r 1)
Texture2D    anaBoxMapTex: register(t13);  // ... which of them touch each 16x16 block (PSAnaBoxMap: .xy index + 1)
Texture2D    changePrevMapTex : register(t14);  // anaglyph recovery: last frame's box map (PSChange)
Texture2D    changeTex   : register(t15);  // ... which 16x16 blocks to redraw this frame (PSChangeGrow: .r 1)
Texture2D    anaSnapTex  : register(t16);  // ... the 1/16 source when the boxes were judged (Converter::CommitAnaSnapshot)
Texture2D    anaShiftTex : register(t17);  // ... how far each box has moved since, this frame (PSAnaBoxShift: .r rows of blocks)
Texture2D    pairTex     : register(t18);  // Recovered Colour at full width: dRef / conf per pixel pair (PSAnaPair)
Texture2D    srcPrevFull : register(t19);  // anaglyph recovery, scroll reuse: last frame's source, full size
Texture2D    scrollTex   : register(t20);  // ... how far the picture scrolled since (PSScrollPick: .r rows, 1x1)
Texture2D    outPrevTex  : register(t21);  // ... last frame's output (read where a block only moved)
Texture2D    reachTex    : register(t22);  // anaglyph recovery: how far sideways each 16x16 block borrows (PSReach: .r least, .g most, source px)
Texture2D    flatTex     : register(t23);  // anaglyph recovery: the 1/4 level's plain grey texels, shrunk (PSFlatShrink: .r 1 by one texel, .g by two)
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
    float g_changeSkip;    // anaglyph recovery: 1 = redraw only the blocks changeTex marks (the rest is last frame's)
    float g_srcDecode;     // 1: srcTex is sRGB-encoded read as UNORM (the capture's own frame, SetSourceEncoded) -- decoded on read
    float g_pairRefine;    // anaglyph recovery: 1 = the compose reads dRef / conf from pairTex (PSAnaPair)
    float g_scrollOn;      // anaglyph recovery: 1 = blocks that only scrolled take last frame's output, moved (PSChangeScroll)
    float g_boxesNew;      // anaglyph recovery: 1 = the boxes were judged afresh -- every block one touches is redrawn
    float g_lfPitch;       // light field: the lens pitch in px (0 = off); the Quilt's views interlaced here
    float g_lfSlant;       // ... and its slant
    float4 g_anaMaskL;     // custom anaglyph pair (g_anaCombo 6): .rgb 1 = the channel is the left eye's
    float4 g_anaMaskR;     // ... the right eye's
    float4 g_anaWL;        // ... each eye's brightness from a pixel: dot(pixel, g_anaWL) (least squares, Common.h AnaCustomFromColours)
    float4 g_anaWR;
    float4 g_anaTL;        // ... each eye's filter colour (full brightness)
    float4 g_anaTR;
};

// Channel-filtered colour for one eye of an anaglyph combo (left: e=0, right: e=1).
float3 anaFilter(float3 c, int combo, int e)
{
    if (combo == 6) return saturate(dot(c, (e == 0) ? g_anaWL.rgb : g_anaWR.rgb)) * ((e == 0) ? g_anaTL.rgb : g_anaTR.rgb);   // Custom: the eye in its own colour
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
    if (combo == 6) return saturate(dot(c, (e == 0) ? g_anaWL.rgb : g_anaWR.rgb));   // Custom (least squares: AnaCustomFromColours)
    if (combo == 0) return (e == 0) ? c.r : (c.g + c.b) * 0.5; // Red/Cyan
    if (combo == 1) return (e == 0) ? c.r : c.g;               // Red/Green
    if (combo == 2) return (e == 0) ? c.r : c.b;               // Red/Blue
    if (combo == 3) return (e == 0) ? c.g : (c.r + c.b) * 0.5; // Green/Magenta
    if (combo == 4) return (e == 0) ? (c.r + c.g) * 0.5 : c.b; // Amber/Blue
    return                (e == 0) ? (c.g + c.b) * 0.5 : (c.r + c.b) * 0.5; // Cyan/Magenta
}

// sRGB <-> linear (the source's SRV and the output's RTV are sRGB views: the
// shader sees linear light; the tint tables are in sRGB-encoded values).
float3 srgbEncode(float3 l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055; }
float3 srgbDecode(float3 s) { return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4); }

// Reading the source (t0). Normally an _SRGB view: the hardware decodes, then
// filters. The capture's own frame (zero-copy) can only be viewed as UNORM, so
// with g_srcDecode set the same is done here: each texel decoded, then the
// bilinear blend (clamped at the edges, like the sampler) -- the same result.
// (SRC_PLAIN: compiled for a source that never needs it -- the "Plain" shaders,
// CMakeLists.txt. Recovered Colour reads the source some 50 times a pixel pair,
// and the choice alone, made at every read, was a tenth of its whole frame.)
float4 SrcLoad(int3 p)
{
    float4 c = srcTex.Load(p);
#ifndef SRC_PLAIN
    if (g_srcDecode > 0.5) c.rgb = srgbDecode(saturate(c.rgb));
#endif
    return c;
}
float4 SrcSampleLevel(SamplerState s, float2 uv, float lod)
{
#ifdef SRC_PLAIN
    return srcTex.SampleLevel(s, uv, lod);
#else
    if (g_srcDecode < 0.5) return srcTex.SampleLevel(s, uv, lod);
    uint W, H; srcTex.GetDimensions(W, H);
    const float2 p = uv * float2(W, H) - 0.5;
    const int2 mx = int2(W, H) - 1;
    // On a texel's centre (1:1 layouts: Half SBS, the TAB halves...): that
    // texel alone. (The hardware's own blend weights are 1/256 steps.)
    const float2 r = round(p);
    const bool2 on = abs(p - r) < 1.0 / 512.0;
    if (all(on)) return SrcLoad(int3(clamp((int2)r, 0, mx), 0));
    const float2 f = frac(p);
    const int2 a = clamp((int2)floor(p), 0, mx), b = clamp((int2)floor(p) + 1, 0, mx);
    // On a row's centre but between two of its texels (a Convergence shift,
    // DeAnaglyph's taps), or on a column's between two rows (frame packing):
    // those two alone -- half the texels to read and decode. Each decode is a
    // power per channel; all four at every read made DeAnaglyph five times as
    // slow on a captured screen as on a decoded picture.
    const int2 rc = clamp((int2)r, 0, mx);
    if (on.y) return lerp(SrcLoad(int3(a.x, rc.y, 0)), SrcLoad(int3(b.x, rc.y, 0)), f.x);
    if (on.x) return lerp(SrcLoad(int3(rc.x, a.y, 0)), SrcLoad(int3(rc.x, b.y, 0)), f.y);
    const float4 c00 = SrcLoad(int3(a.x, a.y, 0)), c10 = SrcLoad(int3(b.x, a.y, 0));
    const float4 c01 = SrcLoad(int3(a.x, b.y, 0)), c11 = SrcLoad(int3(b.x, b.y, 0));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
#endif
}
float4 SrcSample(SamplerState s, float2 uv) { return SrcSampleLevel(s, uv, 0); }   // (one mip level)
// Row `row` of the tint table at value v (0..1), between its 256 entries.
float4 tintLut(int row, float v)
{
    const float x = saturate(v) * 255.0;
    const int i = min((int)x, 254);
    return lerp(anaTintTex.Load(int3(i, row, 0)), anaTintTex.Load(int3(i + 1, row, 0)), x - i);
}
// A one-colour picture under the anaglyph (sepia, cyanotype, a duotone --
// AnalyseAnaPicture): its colours lie on one curve, so each eye's colour
// follows from what that eye sees. The eye seen through two channels keeps
// both and gets the third from the curve (table row+1); the other eye's one
// channel gives its whole colour (row). (Red/cyan, green/magenta, amber/blue.)
float3 tintDecode(float3 c, int eye, int row)
{
    const float3 sc = srgbEncode(saturate(c));
    const int s = g_anaCombo == 3 ? 1 : (g_anaCombo == 4 ? 2 : 0);   // the one-channel eye's channel
    const int singleEye = g_anaCombo == 4 ? 1 : 0;                    // (amber/blue: the right eye)
    if (eye == singleEye) return srgbDecode(tintLut(row, sc[s]).rgb);
    const float key = (dot(sc, 1.0) - sc[s]) * 0.5;                   // the pair's average
    const float m = tintLut(row + 1, key).r;                          // the third channel
    return srgbDecode(float3(s == 0 ? m : sc.r, s == 1 ? m : sc.g, s == 2 ? m : sc.b));
}

// Anaglyph recovery: the channel matched for each view's disparity, per combo --
// one that carries only that eye's picture (red vs green for red/cyan: blue is
// left out, it adds noise). Amber/blue's left view is red+green.
float anaChanL(float3 c)
{
    if (g_anaCombo == 6) return c[(int)g_anaWL.a];   // (Custom: the channel most its own -- AnaCustomFromColours)
    if (g_anaCombo == 3 || g_anaCombo == 5) return c.g;          // Green/Magenta, Cyan/Magenta
    if (g_anaCombo == 4) return (c.r + c.g) * 0.5;               // Amber/Blue
    return c.r;                                                  // Red/Cyan, Red/Green, Red/Blue
}
float anaChanR(float3 c)
{
    if (g_anaCombo == 6) return c[(int)g_anaWR.a];
    if (g_anaCombo == 2 || g_anaCombo == 4) return c.b;          // Red/Blue, Amber/Blue
    if (g_anaCombo == 3 || g_anaCombo == 5) return c.r;          // Green/Magenta, Cyan/Magenta
    return c.g;                                                  // Red/Cyan, Red/Green
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
    if (mode == 3 || mode == 5) return eyeY.xxx;                   // mono: per-eye luminance (not the dim
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
// (c: the source at uv, which the caller has -- the middle tap, not read again.)
void gradWindow(float2 uv, float3 c, float ctx, out float gR[4], out float gG[4])
{
    float rr[5], gg[5];
    [unroll] for (int w = 0; w < 5; ++w)
    {
        float3 s = c;
        if (w != 2) s = SrcSampleLevel(samp, uv + float2((float)(w - 2) * ctx, 0), 0).rgb;
        rr[w] = anaChanL(s); gg[w] = anaChanR(s);   // (rr: left view's channel, gg: right's)
    }
    [unroll] for (int k = 0; k < 4; ++k) { gR[k] = rr[k + 1] - rr[k]; gG[k] = gg[k + 1] - gg[k]; }
}

// A brightness difference as the descriptors hold it: its 0.75 power, sign kept.
// Plain differences let bright edges outweigh everything (a step in a bright
// area is numerically far bigger than the same visible step in a dark one), so
// dark and low-contrast detail barely counted in a match. The square root
// ("RootSIFT") evens that out more, but lifts a flat page's noise too: more
// colour blobs beside pictures. 0.75 keeps most of the gain and adds none
// (16 Middlebury scenes: error down 2-3% in dark, mid and bright areas alike).
// descCost's energy floor and evidence threshold are scaled to match.
float descCurve(float g) { const float r = sqrt(abs(g)); return sign(g) * r * sqrt(r); }

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
            float3 s = SrcSampleLevel(samp, uv + dirs[a] * (float)(i - 2), 0).rgb;
            rr[i] = anaChanL(s); gg[i] = anaChanR(s);
        }
        [unroll] for (int k = 0; k < 4; ++k)
        { gRed[a * 4 + k] = descCurve(rr[k + 1] - rr[k]); gGrn[a * 4 + k] = descCurve(gg[k + 1] - gg[k]); }
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
    const float cost = sad / (ea + eb + 0.2);
    // A candidate where either view has next to no structure can't be
    // checked: NEUTRAL, not bad. Scored as a mismatch, the inside of a plain
    // coloured surface (red paint is blank in the cyan view) always lost to
    // any candidate out on textured background, however poor -- the borrow
    // was pulled out of the surface into its surroundings: the blobs.
    const float evidence = saturate(min(ea, eb) / 0.85);
    return lerp(0.4, cost, evidence);
}

// Coarse disparity pass (red/cyan anaglyph): low-resolution map of the horizontal
// disparity between the L (red) and R (green) views, both directions, via 4-angle
// gradient matching. Low resolution makes winner-take-all robust and smooth.
// Output: .r = dLR (left->right), .g = dRL (right->left), in UV (fraction of width).
void loadDescL(int2 p, out float d[16]);   // (below: a stored descriptor, PSAnaDesc / PSAnaDescCoarse)
void loadDescR(int2 p, out float d[16]);
float4 PSAnaDisp(VSOut i) : SV_Target
{
    float2 uv  = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    float  ty = 1.0 / max(g_coarseH, 1.0);
    float  maxD = g_dispMaxUV;
    // Candidates every 8 source pixels (half a coarse texel), however wide
    // the area is: tying the step to the area's width searched the same
    // picture on a different candidate grid whenever the captured area
    // changed size -- and the result, blobs included, changed with it.
    const float step = 8.0 / (16.0 * max(g_coarseW, 1.0));   // (this level's uv: 16 source px per texel)
    const int N = (int)ceil(maxD / step);

    // Every position a candidate can be at is 8 source px from the next: their
    // descriptors are worked out once (PSAnaDescCoarse: texel j is the position
    // 8 x j px) and read here, where each used to be built again for every
    // pixel that tried it. This pixel's own is at 2x + 1.
    const int2 p = int2(i.pos.xy);
    const int j0 = 2 * p.x + 1;
    float refRed[16], refGrn[16]; loadDescL(int2(j0, p.y), refRed); loadDescR(int2(j0, p.y), refGrn);

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
        if (uv.x + d < 0.0 || uv.x + d > 1.0 / g_lvlToSrcX) continue;
        // (This candidate's descriptors, from PSAnaDescCoarse: 2 loads, not 20 samples.)
        float cRed[16], cGrn[16]; loadDescL(int2(j0 + k, p.y), cRed); loadDescR(int2(j0 + k, p.y), cGrn);
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

// The source's 4x4 blocks averaged, plainly (4 bilinear taps, each a 2x2
// average): for telling whether a frame is the one before again
// (Converter::Convert, directSrc) -- no more is asked of it.
float4 PSDownBox(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    const float2 t = 1.0 / float2(W, H);
    const float2 c = i.pos.xy * 4.0 * t;
    return 0.25 * (srcTex.SampleLevel(samp, c + float2(-1, -1) * t, 0) + srcTex.SampleLevel(samp, c + float2(1, -1) * t, 0) +
                   srcTex.SampleLevel(samp, c + float2(-1, 1) * t, 0) + srcTex.SampleLevel(samp, c + float2(1, 1) * t, 0));
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
        acc += SrcSampleLevel(samp, c + t * float2(o[x], o[y]), 0) * (wt[x] * wt[y]);
    return acc / 64.0;
}

// The same downsample, and whether the block is plain grey: every tap (each a
// 2x2 average) neutral and all alike to within the plain-grey test's 0.01
// (anaFlatAt). Target 1: 1 = plain.
static const float kFlatTone = 0.02;   // (the plain-grey test on the square root of the light: see anaFlatAt)
struct DownFlatOut { float4 avg : SV_Target0; float flat : SV_Target1; };
DownFlatOut PSDownFlat(VSOut i)
{
    uint W, H; srcTex.GetDimensions(W, H);
    const float2 t = 1.0 / float2(W, H);
    const float2 c = i.pos.xy * 4.0 * t;
    const float o[4] = { -3.0, -1.0, 1.0, 3.0 };
    const float wt[4] = { 1.0, 3.0, 3.0, 1.0 };
    float4 acc = 0; float3 lo = 1e9, hi = -1e9; float dev = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const float4 s = SrcSampleLevel(samp, c + t * float2(o[x], o[y]), 0);
        acc += s * (wt[x] * wt[y]);
        lo = min(lo, s.rgb); hi = max(hi, s.rgb);
        dev = max(dev, max(abs(s.r - s.g), abs(s.r - s.b)));
    }
    DownFlatOut o2;
    o2.avg = acc / 64.0;
    // (... and on the eye's scale: the darkest and the lightest value of all, any
    // channel, kFlatTone apart at most on their square roots -- as anaFlatAt.)
    const float vLo = min(min(lo.r, lo.g), lo.b), vHi = max(max(hi.r, hi.g), hi.b);
    o2.flat = (dev < 0.01 && all(hi - lo < 0.01) && sqrt(max(vHi, 0.0)) - sqrt(max(vLo, 0.0)) < kFlatTone) ? 1.0 : 0.0;
    return o2;
}
// Target 1/4: .r 1 where the 3x3 texels around are all plain (PSDownFlat at
// t0), .g where the 5x5 are. A pixel pair in a .r texel is plain grey for
// both eyes (PSAnaPair: no refine); a .g texel's smoothed disparity is read
// by no pixel that's recovered (PSAnaSmooth: not worked out).
float4 PSFlatShrink(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    const int2 p = int2(i.pos.xy);
    float m1 = 1, m2 = 1;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        const float f = srcTex.Load(int3(clamp(p + int2(x, y), int2(0, 0), int2(W, H) - 1), 0)).r;
        m2 = min(m2, f);
        if (abs(x) <= 1 && abs(y) <= 1) m1 = min(m1, f);
    }
    return float4(m1, m2, 0, 0);
}

// Descriptor pass: each refine-level pixel's 4-angle gradient descriptor
// (buildDesc: 16 left-view + 16 right-view values, 20 texture reads), worked
// out ONCE and stored as 32 halves in four uint4 targets. The refine below
// compares 13 candidates per direction; reading their descriptors from here
// costs 2 loads each instead of 20 samples.
struct DescOut { uint4 a : SV_Target0; uint4 b : SV_Target1; uint4 c : SV_Target2; uint4 d : SV_Target3; };
uint packH(float x, float y) { return f32tof16(x) | (f32tof16(y) << 16); }
DescOut descAt(float2 uv);
DescOut PSAnaDesc(VSOut i) { return descAt(i.uv); }
// The same for the coarse search (PSAnaDisp), whose candidates sit every 8
// source px -- half a coarse texel: a target twice the level's width plus
// one, texel j holding the descriptor at 8 x j px.
DescOut PSAnaDescCoarse(VSOut i)
{
    return descAt(float2((i.pos.x - 0.5) * 0.5 / max(g_coarseW, 1.0), i.uv.y));
}
DescOut descAt(float2 uv)
{
    float tx = 1.0 / max(g_coarseW, 1.0);
    float ty = 1.0 / max(g_coarseH, 1.0);
    float L[16], R[16]; buildDesc(uv, tx, ty, L, R);
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

float2 anaSkipQ(int2 p);   // (below, with PSChangeGrow2)
// Pyramid refine: start from a coarser level (dispTex = prior) and do a local
// 4-angle gradient search to sharpen the disparity. Outputs (dLR, dRL, 0, 0).
// Candidates sit on this level's pixel grid (the prior rounded to a whole
// pixel: the full-res pass refines to sub-pixel afterwards), so each one's
// descriptor comes from PSAnaDesc's targets.
float4 PSAnaRefine(VSOut i) : SV_Target
{
    if (anaSkipQ(int2(i.pos.xy)).r < 0.5) discard;   // (nothing redrawn reads it: PSChangeGrow2)
    float2 uv  = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    int2   p  = int2(i.pos.xy);
    int    W  = (int)g_coarseW;
    float4 priorAll = dispTex.SampleLevel(samp, uv, 0.0);   // .rg disparity, .ba uniqueness
    const float lim = W / g_lvlToSrcX;                       // (the picture's right edge, in this level's px)

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
    int kL00 = 0, kR00 = 0;
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
        // (A neighbouring block's answer within 4 of the interpolated one: its
        // +-2 are all among that one's +-6 already -- nothing new to try. Most
        // of the picture: the blocks around agree.)
        if (h == 0) { kL00 = kL0; kR00 = kR0; }
        else if (abs(kL0 - kL00) <= 4 && abs(kR0 - kR00) <= 4) continue;
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
            if (sadL < bestL && rawL >= 0 && rawL < lim) { bestL = sadL; dL = (float)(kL0 + k) * tx; }
            if (sadR < bestR && rawR >= 0 && rawR < lim) { bestR = sadR; dR = (float)(kR0 + k) * tx; }
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
    if (anaSkipQ(int2(i.pos.xy)).r < 0.5) discard;
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

    float3 myCol = SrcSampleLevel(samp, uv, 0.0).rgb;
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
                float3 ncol = SrcSampleLevel(samp, nuv, 0.0).rgb;
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
// ----- What changed since the last frame (anaglyph recovery) --------------
// A page that isn't moving is still delivered as new frames (a caret, a small
// animation, the capture simply ticking): recovering all of it every frame
// spent milliseconds on pixels that come out the same. Per 16x16 block, did
// its 4x4 averages (the 1/4 source, t0, vs last frame's at t8) or the boxes
// decoded as black-and-white / one colour (t13 vs last frame's at t14) change?
// Unchanged blocks are discarded -- the pass's occlusion predicate then skips
// the whole recovery when nothing changed at all (Converter::Convert).
// Target 1/16 size (cleared to 0): .r 1 = changed.
float4 PSChange(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy);
    float d = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        const int3 q = int3(b * 4 + int2(x, y), 0);
        const float3 a = SrcLoad(q).rgb, p = srcPrevQ.Load(q).rgb;
        d = max(d, max(max(abs(a.r - p.r), abs(a.g - p.g)), abs(a.b - p.b)));
    }
    // (Linear light, 4x4 averages: a single pixel changing by a few levels shows.)
    // (The boxes judged afresh, g_boxesNew: any block one touched or touches.)
    const float2 bmNow = anaBoxMapTex.Load(int3(b, 0)).xy, bmWas = changePrevMapTex.Load(int3(b, 0)).xy;
    const bool boxes = any(bmNow != bmWas) || (g_boxesNew > 0.5 && (any(bmNow != 0) || any(bmWas != 0)));
    if (d < 0.0005 && !boxes) discard;
    return 1;
}

// How far sideways each 16x16 block's output reaches for its colour (source
// px, .r the least offset, .g the most): the disparities it was last drawn
// with (PSReach, kept from frame to frame, following a scroll). Unbound: the
// whole search range.
float2 reachAt(int2 b, int dy, bool still, bool moved)
{
    uint rw, rh; reachTex.GetDimensions(rw, rh);
    const float all = g_dispMaxUV * g_srcW;
    if (rw == 0) return float2(-all, all);
    float2 r = float2(1e9, -1e9);
    if (still) { const float2 v = reachTex.Load(int3(b, 0)).rg; r = float2(min(r.x, v.x), max(r.y, v.y)); }
    if (moved)
    {
        // (The block's picture was dy rows away: the one or two blocks it lay in.)
        const int r0 = clamp((int)floor((b.y * 16 + dy) / 16.0), 0, (int)rh - 1), r1 = clamp((int)floor((b.y * 16 + 15 + dy) / 16.0), 0, (int)rh - 1);
        const float2 v0 = reachTex.Load(int3(b.x, r0, 0)).rg, v1 = reachTex.Load(int3(b.x, r1, 0)).rg;
        r = float2(min(r.x, min(v0.x, v1.x)), max(r.y, max(v0.y, v1.y)));
    }
    return clamp(r, -all, all);
}

// Target 1/16 size: which blocks are drawn again. A block's output is its own
// pixels and the colour it borrowed from the other view -- so far to one
// side as its disparity (reachAt), a few px more for the last alignment. It is
// redrawn if it changed itself, or anything in that reach did. (It used to be
// the whole search range either side, 6% of the width: a caret, a pointer, a
// small animation redrew a band a few hundred px wide around itself, every
// frame.) .r 1 = redrawn.
// Scroll reuse (PSChangeScroll: .g a block that's where it was, .b one that
// only moved): a block keeps last frame's output -- in place, or from where
// it was (.g 1) -- when everything in its reach did as it did. One that
// stayed beside one that moved, within reach: what it borrowed isn't beside
// it any more -- redrawn.
float4 PSChangeGrow(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy);
    uint W, H; srcTex.GetDimensions(W, H);
    const float4 own = srcTex.Load(int3(b, 0));
    if (own.r > 0.5) return 1;
    const bool scroll = g_scrollOn > 0.5;
    if (scroll && own.a > 0.5) return 1;   // (a box's block: PSChangeScroll)
    const int dy = scroll ? (int)scrollTex.Load(int3(0, 0, 0)).r : 0;
    // (A block that only moved: its reach came with it. One that could be
    // either -- a plain area: both.)
    const float2 reach = reachAt(b, dy, !scroll || own.b < 0.5, scroll && dy != 0 && own.g < 0.5);
    const int rx = (int)ceil(g_dispMaxUV * g_srcW / 16.0) + 1;
    const int x0 = max(max(0, b.x - rx), b.x + (int)floor((reach.x - 8.0) / 16.0));
    const int x1 = min(min((int)W - 1, b.x + rx), b.x + (int)floor((reach.y + 23.0) / 16.0));
    bool anyS = own.g > 0.5, anyM = own.b > 0.5;
    [loop] for (int y = max(0, b.y - 1); y <= min((int)H - 1, b.y + 1); ++y)
    [loop] for (int x = x0; x <= x1; ++x)
    {
        const float3 c = srcTex.Load(int3(x, y, 0)).rgb;
        if (c.r > 0.5) return 1;
        anyS = anyS || c.g > 0.5;
        anyM = anyM || c.b > 0.5;
    }
    if (scroll && anyS && anyM) return 1;
    return float4(0, scroll && anyM ? 1 : 0, 0, 0);
}

// Target 1/16 size: each block's reach (reachAt) after this frame. A block
// drawn again: from the disparities it was drawn with (the smoothed map, t2,
// its 4x4 texels and one more around -- the compose reads between them). One
// kept: the reach it had, from where its picture was.
float4 PSReach(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy);
    bool fresh = g_changeSkip < 0.5, moved = false;
    if (!fresh)
    {
        const float2 ch = changeTex.Load(int3(b, 0)).rg;
        fresh = ch.r > 0.5; moved = ch.g > 0.5;
    }
    if (!fresh)
    {
        const int dy = g_scrollOn > 0.5 ? (int)scrollTex.Load(int3(0, 0, 0)).r : 0;
        return float4(reachAt(b, dy, !moved, moved), 0, 1);
    }
    uint dw, dh; dispTex.GetDimensions(dw, dh);
    const float toPx = g_lvlToSrcX * g_srcW;
    float2 r = float2(1e9, -1e9);
    [unroll] for (int y = -1; y <= 4; ++y)
    [unroll] for (int x = -1; x <= 4; ++x)
    {
        const float4 d = dispTex.Load(int3(clamp(b * 4 + int2(x, y), 0, int2(dw, dh) - 1), 0));
        if (d.b < 0.0) continue;   // (not worked out this frame: PSAnaSmooth)
        const float lo = min(d.r, d.g) * toPx, hi = max(d.r, d.g) * toPx;
        r = float2(min(r.x, lo), max(r.y, hi));
    }
    const float all = g_dispMaxUV * g_srcW;
    if (r.x > r.y) r = float2(-all, all);
    return float4(r, 0, 1);
}

// Target 1/16 size, from PSChangeGrow's blocks to redraw (t0): what the
// disparity passes at 1/4 size have to work out for them. .g 1 = the smoothed
// disparity is read here (a redrawn block or one beside it); .r 1 = the
// refine and fill are (those, plus what the fill reaches for from them: 24
// texels and a disparity sideways, the smoothing's 2 texels up and down).
// The rest of each pass is skipped (anaSkipQ).
float4 PSChangeGrow2(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy);
    uint W, H; srcTex.GetDimensions(W, H);
    const int rx = (int)ceil(g_dispMaxUV * g_srcW / 16.0) + 8, ry = 2;
    bool nearby = false, beside = false;
    [loop] for (int y = max(0, b.y - ry); y <= min((int)H - 1, b.y + ry); ++y)
    [loop] for (int x = max(0, b.x - rx); x <= min((int)W - 1, b.x + rx); ++x)
        if (srcTex.Load(int3(x, y, 0)).r > 0.5)
        {
            nearby = true;
            if (abs(x - b.x) <= 1 && abs(y - b.y) <= 1) beside = true;
        }
    return float4(nearby ? 1 : 0, beside ? 1 : 0, 0, 0);
}
// (For a pass at 1/4 size, at pixel p: what PSChangeGrow2 says of its block.)
float2 anaSkipQ(int2 p)
{
    if (g_changeSkip < 0.5) return 1;
    uint cw, chh; changeTex.GetDimensions(cw, chh);
    if (cw == 0) return 1;
    return changeTex.Load(int3(clamp(p / 4, 0, int2(cw, chh) - 1), 0)).rg;
}

// For the perf log (Converter::TakeScrollStats): of PSChangeGrow's blocks
// (t0), how many are redrawn (.r) and how many take last frame's output,
// moved (.g) -- per row (target 1 x rows), then summed (1 x 1; .b the
// scroll's rows, .a 1: a frame where something changed).
float4 PSChangeStatRows(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    float2 s = 0;
    [loop] for (uint x = 0; x < W; ++x) { const float2 c = srcTex.Load(int3(x, (int)i.pos.y, 0)).rg; s += float2(c.r > 0.5 ? 1 : 0, (c.r < 0.5 && c.g > 0.5) ? 1 : 0); }
    return float4(s, 0, 0);
}
float4 PSChangeStat(VSOut i) : SV_Target
{
    uint W, H; srcTex.GetDimensions(W, H);
    float2 s = 0;
    [loop] for (uint y = 0; y < H; ++y) s += srcTex.Load(int3(0, y, 0)).rg;
    uint sw, sh; scrollTex.GetDimensions(sw, sh);
    return float4(s, (g_scrollOn > 0.5 && sw > 0) ? scrollTex.Load(int3(0, 0, 0)).r : 0, 1);
}

// ----- A page being scrolled (anaglyph recovery) --------------------------
// Scrolling moves the whole picture every frame, so every block "changed"
// and all of it was recovered again -- the costliest frames there are, just
// when smoothness shows. But a scrolled picture is last frame's, moved: the
// output for it is last frame's output, moved the same. So: how far did it
// scroll (PSScrollCost / PSScrollPick), which blocks are exactly last
// frame's source at that offset (PSChangeScroll), and only the rest -- the
// rows that came into view, anything that changed besides -- is recovered.
// All comparisons are exact: a pixel that differs at all is redrawn.
static const int kScroll = 192;   // (rows a frame, either way; Converter.cpp)

// Target (2 x kScroll + 1) x 54, a texel per offset and row of a grid of
// pixels: of those that aren't what they were last frame, .r how many are
// what was that many rows away, .g how many there are. (Per row, then summed
// -- PSScrollSum: one pixel's shader doing the whole grid ran for
// milliseconds.)
float4 PSScrollCost(VSOut i) : SV_Target
{
    const int dy = (int)i.pos.x - kScroll, gy = (int)i.pos.y;
    if (dy == 0) return 0;
    uint W, H; srcTex.GetDimensions(W, H);
    const int py = (gy * 2 + 1) * (int)H / 108, qy = py + dy;
    const bool inside = qy >= 0 && qy < (int)H;
    float match = 0, moving = 0;
    [unroll] for (int gx = 0; gx < 96; ++gx)
    {
        const int px = (gx * 2 + 1) * (int)W / 192 + (gy * 5) % 13;
        const float3 a = srcTex.Load(int3(px, py, 0)).rgb;
        const bool moved = any(a != srcPrevFull.Load(int3(px, py, 0)).rgb);
        const bool same = all(a == srcPrevFull.Load(int3(px, clamp(qy, 0, (int)H - 1), 0)).rgb);
        moving += moved ? 1.0 : 0.0;
        match += (moved && inside && same) ? 1.0 : 0.0;
    }
    return float4(match, moving, 0, 0);
}
// Target (2 x kScroll + 1) x 1: the rows summed.
float4 PSScrollSum(VSOut i) : SV_Target
{
    float2 s = 0;
    [unroll] for (int gy = 0; gy < 54; ++gy) s += srcTex.Load(int3((int)i.pos.x, gy, 0)).rg;
    return float4(s, 0, 0);
}

// Target 1 x 1 (cleared to 0): .r the offset most of the moving pixels agree
// on. None: discarded -- and the pass's occlusion predicate skips the copy of
// last frame's output (Converter::Convert).
float4 PSScrollPick(VSOut i) : SV_Target
{
    float best = 0, bestDy = 0, moving = 0;
    [loop] for (int k = 0; k <= 2 * kScroll; ++k)
    {
        const float2 c = srcTex.Load(int3(k, 0, 0)).rg;
        const float dy = (float)(k - kScroll);
        moving = max(moving, c.g);
        if (c.r > best || (c.r == best && abs(dy) < abs(bestDy))) { best = c.r; bestDy = dy; }
    }
    // (A scroll moves nearly everything that changed: a page of text matches
    // itself at many offsets, each a little less well.)
    if (best < 12.0 || best < 0.5 * moving) discard;
    return float4(bestDy, 0, 0, 1);
}

// Per 4x4 pixels of the source (t0) against last frame's (t19), target 1/4
// size: .r 1 = not what was there, .g 1 = not what was there the scroll away.
float4 PSChangeScrollQ(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy) * 4;
    uint W, H; srcTex.GetDimensions(W, H);
    const int dy = (int)scrollTex.Load(int3(0, 0, 0)).r;
    if (dy == 0) discard;   // (no scroll: PSChangeScroll judges the blocks as PSChange does)
    bool diffS = false, diffM = false;
    [unroll] for (int y = 0; y < 4; ++y)
    {
        const int sy = min(b.y + y, (int)H - 1), my = sy + dy;
        if (my < 0 || my >= (int)H) diffM = true;
        [unroll] for (int x = 0; x < 4; ++x)
        {
            const int sx = min(b.x + x, (int)W - 1);
            const float3 a = srcTex.Load(int3(sx, sy, 0)).rgb;
            if (any(a != srcPrevFull.Load(int3(sx, sy, 0)).rgb)) diffS = true;
            if (any(a != srcPrevFull.Load(int3(sx, clamp(my, 0, (int)H - 1), 0)).rgb)) diffM = true;
        }
    }
    return float4(diffS ? 1 : 0, diffM ? 1 : 0, 0, 0);
}
// Target 1/16 size, per 16x16 block (PSChangeScrollQ's 4x4 at t0): .r 1 =
// changed (neither where it was nor moved by the scroll), .g only where it
// was, .b only moved. (Both: a plain area -- all 0.)
float4 boxRect(int bi);   // (below)
float4 PSChangeScroll(VSOut i) : SV_Target
{
    const int2 b = int2(i.pos.xy) * 4;
    // (A block one of the black-and-white / one-colour boxes touches, now or
    // where its picture was last frame: decoded the box's way, and the box
    // follows a scroll in whole blocks only -- always redrawn, .a. Its
    // neighbours aren't for that: nothing in the source changed.)
    const int2 bb = int2(i.pos.xy);
    const int sdy = (int)scrollTex.Load(int3(0, 0, 0)).r;
    uint mw, mh; changePrevMapTex.GetDimensions(mw, mh);
    bool boxed = any(anaBoxMapTex.Load(int3(bb, 0)).xy != 0);
    if (mw > 0)
    {
        const int r0 = clamp((int)floor((bb.y * 16 + sdy) / 16.0), 0, (int)mh - 1), r1 = clamp((int)floor((bb.y * 16 + 15 + sdy) / 16.0), 0, (int)mh - 1);
        boxed = boxed || any(changePrevMapTex.Load(int3(bb.x, r0, 0)).xy != 0) || any(changePrevMapTex.Load(int3(bb.x, r1, 0)).xy != 0)
                      || any(changePrevMapTex.Load(int3(bb, 0)).xy != 0);
    }
    // (Except a block well inside one box, this frame and last, the boxes not
    // judged afresh: every pixel of it is decoded from its own source pixel
    // alone, the same way both frames -- last frame's output for it, moved,
    // is this frame's. Well inside: the box follows the page a block at a time.)
    if (boxed && mw > 0 && sdy != 0 && g_boxesNew < 0.5)
    {
        const float2 mN = anaBoxMapTex.Load(int3(bb, 0)).xy;
        const int q0 = clamp((int)floor((bb.y * 16 + sdy) / 16.0), 0, (int)mh - 1), q1 = clamp((int)floor((bb.y * 16 + 15 + sdy) / 16.0), 0, (int)mh - 1);
        if (mN.x != 0 && mN.y == 0 && all(changePrevMapTex.Load(int3(bb.x, q0, 0)).xy == mN) && all(changePrevMapTex.Load(int3(bb.x, q1, 0)).xy == mN))
        {
            const float4 br = boxRect((int)mN.x - 1) * float4(g_srcW, g_srcH, g_srcW, g_srcH);
            const float2 p0 = float2(bb) * 16.0, p1 = p0 + 16.0;
            if (p0.x >= br.x + 16.0 && p1.x <= br.z - 16.0 && p0.y >= br.y + 32.0 && p1.y <= br.w - 32.0) boxed = false;
        }
    }
    const float boxA = boxed ? 1 : 0;
    if (sdy == 0)
    {
        // Not scrolling: PSChange's test, on the 1/4 source (t9) and last frame's (t8).
        float dq = 0;
        [unroll] for (int qy = 0; qy < 4; ++qy)
        [unroll] for (int qx = 0; qx < 4; ++qx)
        {
            const int3 q = int3(b + int2(qx, qy), 0);
            const float3 a = srcQ.Load(q).rgb, p = srcPrevQ.Load(q).rgb;
            dq = max(dq, max(max(abs(a.r - p.r), abs(a.g - p.g)), abs(a.b - p.b)));
        }
        const float2 bmNow = anaBoxMapTex.Load(int3(bb, 0)).xy, bmWas = changePrevMapTex.Load(int3(bb, 0)).xy;
        const bool boxes = any(bmNow != bmWas) || (g_boxesNew > 0.5 && (any(bmNow != 0) || any(bmWas != 0)));
        return (dq < 0.0005 && !boxes) ? float4(0, 1, 0, 0) : float4(1, 0, 0, 0);
    }
    float2 d = 0;
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
        d = max(d, srcTex.Load(int3(b + int2(x, y), 0)).rg);
    const bool diffS = d.r > 0.5, diffM = d.g > 0.5;
    return float4(diffS && diffM ? 1 : 0, !diffS && diffM ? 1 : 0, diffS && !diffM ? 1 : 0, boxA);
}
// ----- Is each box still what it was judged? (Converter::SetAnaBoxes) -----
// The black-and-white / one-colour boxes come from a check on an earlier frame.
// Every frame this re-runs that check on the boxes' 16x16 block averages (the
// 1/16 source, t0 -- the blocks AnalyseAnaPicture measures): black-and-white,
// the pair's averages the same; one colour, on the box's curve (its tint
// table row 3+2i: .g .b the pair per key). A page scrolled or a colour picture
// moved in -- the box is off this frame, instead of greying what's there now.
// Two passes, so a big picture isn't one GPU thread's work: each row of each
// box (PSAnaBoxRows), then each box's rows summed (PSAnaBoxCheck).

// Box bi's blocks wholly inside the picture (x0, y0, x1, y1): the box itself
// reaches past it (a black-and-white one well past -- see
// UpdateManualAnaColour), and a block there can hold the colour picture next
// to it. bk.zw: that padding (uv).
// Box bi's rectangle (source uv) where its picture is THIS frame: where the
// check found it, moved by how far the page has scrolled since (PSAnaBoxShift,
// t17: .r rows of 16-px blocks; unbound -> 0).
float4 boxRect(int bi)
{
    const float4 br = anaBoxTex.Load(int3(1 + 2 * bi, 0, 0));
    const float dy = anaShiftTex.Load(int3(bi, 0, 0)).r * 16.0 / g_srcH;
    return br + float4(0, dy, 0, dy);
}
int4 boxInnerBlocks(int bi, uint W, uint H, bool moved = true)
{
    const float4 br = moved ? boxRect(bi) : anaBoxTex.Load(int3(1 + 2 * bi, 0, 0));   // (moved: where it is now)
    const float4 bk = anaBoxTex.Load(int3(2 + 2 * bi, 0, 0));
    const float4 inner = float4(br.xy + bk.zw, br.zw - bk.zw);
    return int4(max(0, (int)ceil(inner.x * g_srcW / 16.0)), max(0, (int)ceil(inner.y * g_srcH / 16.0)),
                min((int)W, (int)floor(inner.z * g_srcW / 16.0)), min((int)H, (int)floor(inner.w * g_srcH / 16.0)));
}

// How far each box's picture has scrolled since the check (which judged a
// frame a moment ago): its blocks in that frame (the snapshot, t16) against
// this frame's (t0), up or down by 0..kBoxShift rows of 16-px blocks.
// Target kMaxBoxes x (2 kBoxShift + 1): texel (box, j) -- the mean difference
// at dy = j - kBoxShift (.r), and how many blocks were compared (.g).
static const int kBoxShift = 48;   // (768 px either way: a fast scroll between checks)
float BlockLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float4 PSAnaBoxShiftCost(VSOut i) : SV_Target
{
    const int bi = (int)i.pos.x, dy = (int)i.pos.y - kBoxShift;
    if (bi >= (int)anaBoxTex.Load(int3(0, 0, 0)).x) return float4(1e4, 0, 0, 0);
    uint W, H; srcTex.GetDimensions(W, H);
    const int4 b = boxInnerBlocks(bi, W, H, false);   // (where the check found it: the snapshot's place)
    const int sx = max(1, (b.z - b.x) / 24), sy = max(1, (b.w - b.y) / 24);   // (at most 24 x 24 blocks compared)
    float d = 0, n = 0;
    [loop] for (int y = b.y; y < b.w; y += sy)
    {
        const int yy = y + dy;
        if (yy < 0 || yy >= (int)H) continue;
        [loop] for (int x = b.x; x < b.z; x += sx)
        {
            d += abs(BlockLuma(SrcLoad(int3(x, yy, 0)).rgb) - BlockLuma(anaSnapTex.Load(int3(x, y, 0)).rgb));
            n += 1;
        }
    }
    return float4(n > 0 ? d / n : 1e4, n, 0, 0);
}
// Target kMaxBoxes x 1: .r the shift (rows of blocks) with the least
// difference -- not moved unless another shift fits clearly better (a still
// page, a picture with repeating rows). (PSAnaBoxShiftCost at t2.)
float4 PSAnaBoxShift(VSOut i) : SV_Target
{
    const int bi = (int)i.pos.x;
    const float c0 = dispTex.Load(int3(bi, kBoxShift, 0)).r;
    float best = c0; int bestDy = 0;
    [loop] for (int j = 0; j <= 2 * kBoxShift; ++j)
    {
        const float2 c = dispTex.Load(int3(bi, j, 0)).rg;
        if (c.y < 4) continue;   // (too little of the box on screen at that shift)
        if (c.x < best - 0.002) { best = c.x; bestDy = j - kBoxShift; }
    }
    return float4(bestDy, 1, 0, 0);
}

// Target kMaxBoxes x (1/16 height): texel (box, row) -- that row's blocks
// in the box: .r how many say something (not plain grey), .g how many of
// those don't fit.
float4 PSAnaBoxRows(VSOut i) : SV_Target
{
    const int bi = (int)i.pos.x, y = (int)i.pos.y;
    if (bi >= (int)anaBoxTex.Load(int3(0, 0, 0)).x) return 0;
    uint W, H; srcTex.GetDimensions(W, H);
    const int4 b = boxInnerBlocks(bi, W, H);
    if (y < b.y || y >= b.w) return 0;
    const float4 bk = anaBoxTex.Load(int3(2 + 2 * bi, 0, 0));
    const int s = g_anaCombo == 3 ? 1 : (g_anaCombo == 4 ? 2 : 0);   // the one-channel eye's channel
    const bool tinted = bk.x > 1.5;
    const int row = (int)bk.y + 1;
    float busy = 0, off = 0;
    [loop] for (int x = b.x; x < b.z; ++x)
    {
        const float3 c = srgbEncode(saturate(SrcLoad(int3(x, y, 0)).rgb));
        if (max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b) < 3.0 / 255.0) continue;   // plain grey: says nothing
        busy += 1;
        // (The pair: red/cyan g, b; green/magenta r, b; amber/blue r, g.)
        const float a = s == 0 ? c.g : c.r, bb = s == 2 ? c.g : c.b;
        if (!tinted) { off += abs(a - bb) > 6.0 / 255.0 ? 1 : 0; continue; }
        const float4 cur = tintLut(row, (a + bb) * 0.5);
        off += (abs(a - cur.g) > 10.0 / 255.0 || abs(bb - cur.b) > 10.0 / 255.0) ? 1 : 0;
    }
    return float4(busy, off, 0, 0);
}

// Target kMaxBoxes x 1: .r 1 = still so. (PSAnaBoxRows' result at t2.)
float4 PSAnaBoxCheck(VSOut i) : SV_Target
{
    const int bi = (int)i.pos.x;
    if (bi >= (int)anaBoxTex.Load(int3(0, 0, 0)).x) return 0;
    uint W, H; dispTex.GetDimensions(W, H);
    float busy = 0, off = 0;
    [loop] for (int y = 0; y < (int)H; ++y) { const float2 v = dispTex.Load(int3(bi, y, 0)).rg; busy += v.x; off += v.y; }
    const bool tinted = anaBoxTex.Load(int3(2 + 2 * bi, 0, 0)).x > 1.5;
    // (Black-and-white: as AnalyseAnaPicture, under 2% apart -- a colour
    // picture is a third or more. One colour: block averages straddling an
    // edge leave the curve now and then, colour does all over.)
    // (Next to nothing to judge -- plain page now under it: a black-and-white
    // box can stay, Mono leaves plain grey as it is; a tint would paint it.)
    const bool ok = busy < 8 ? !tinted : off <= max(1.0, (tinted ? 0.2 : 0.03) * busy);
    return ok ? 1 : 0;
}

// Target 1/16 source size: which boxes (still so, PSAnaBoxCheck at t12) touch
// each 16x16 block -- .x, .y their index + 1 (0: none). The compose then
// looks at just those instead of every box for every pixel.
float4 PSAnaBoxMap(VSOut i) : SV_Target
{
    const int n = (int)anaBoxTex.Load(int3(0, 0, 0)).x;
    const float2 b0 = floor(i.pos.xy) * 16.0 / float2(g_srcW, g_srcH), b1 = b0 + 16.0 / float2(g_srcW, g_srcH);
    float2 found = 0;
    [loop] for (int bi = 0; bi < n; ++bi)
    {
        if (anaBoxOkTex.Load(int3(bi, 0, 0)).r < 0.5) continue;
        const float4 br = boxRect(bi);
        if (br.x >= b1.x || br.z <= b0.x || br.y >= b1.y || br.w <= b0.y) continue;
        if (found.x == 0) found.x = bi + 1; else { found.y = bi + 1; break; }
    }
    return float4(found, 0, 0);
}

float4 PSAnaSmooth(VSOut i) : SV_Target
{
    // (Plain grey all round: read by nothing recovered -- PSFlatShrink.)
    { uint fw, fh; flatTex.GetDimensions(fw, fh); if (fw > 0 && flatTex.Load(int3(i.pos.xy, 0)).g > 0.5) return float4(0, 0, -1, -1); }
    // (Not read by anything redrawn this frame: not worked out -- and marked so,
    // confidence -1, for the next frame's steadying below.)
    if (anaSkipQ(int2(i.pos.xy)).g < 0.5) return float4(0, 0, -1, -1);
    float2 uv = i.uv;
    float  tx = 1.0 / max(g_coarseW, 1.0);
    float  ty = 1.0 / max(g_coarseH, 1.0);
    float3 c0 = SrcSampleLevel(samp, uv, 0.0).rgb;
    float  y0 = anaChanL(c0) + anaChanR(c0);       // both views' channels: luminance proxy
    float2 myConf = dispTex.SampleLevel(samp, uv, 0.0).ba;   // per-eye confidence (.b left, .a right)

    // Weighted median (medoid) of the 5x5, not a weighted mean: the result is
    // one of the neighbours' actual disparities -- the one that agrees best
    // with the rest. A mean blended a near object's disparity with the
    // background's at every edge (a value belonging to neither), and dragged
    // a correct value toward wrong neighbours; on real stereo pairs that is
    // where ~9% of the blob pixels, right until here, were lost.
    // (Left's then right's: holding both sets of 25 at once took 43 registers
    // a pixel -- few pixels in flight. The weights are shared; the second set
    // is read again, from the cache. All reads at texel centres: Load.)
    uint dw, dh; dispTex.GetDimensions(dw, dh);
    const int2 p0 = int2(i.pos.xy), pmax = int2(dw, dh) - 1;
    float v[25]; float nws[25];
    const int R = 2;
    [unroll] for (int dy = -R; dy <= R; ++dy)
    [unroll] for (int dx = -R; dx <= R; ++dx)
    {
        const int idx = (dy + R) * 5 + (dx + R);
        const int3 q = int3(clamp(p0 + int2(dx, dy), int2(0, 0), pmax), 0);
        float4 nd  = dispTex.Load(q);
        float3 nc  = SrcLoad(q).rgb;
        float  ws  = exp(-(float)(dx * dx + dy * dy) / 8.0);   // spatial
        float  wl  = exp(-abs(y0 - (anaChanL(nc) + anaChanR(nc))) * 6.0);      // luminance (edge-aware)
        v[idx] = nd.r; nws[idx] = ws * wl * (0.2 + max(nd.b, nd.a));        // trust confident neighbours
    }
    float2 d; float best = 1e9; d.x = v[12];
    [unroll] for (int a = 0; a < 25; ++a)   // (unrolled: v / nws then stay in registers, not indexed memory)
    if (abs(a % 5 - 2) <= 1 && abs(a / 5 - 2) <= 1)   // (candidates: the middle 3x3, each weighed against all 25 -- a third of the work, the same results on the test scenes)
    {
        float s = 0;
        [unroll] for (int b = 0; b < 25; ++b) s += nws[b] * abs(v[a] - v[b]);
        if (s < best) { best = s; d.x = v[a]; }
    }
    [unroll] for (int ry = -R; ry <= R; ++ry)
    [unroll] for (int rx = -R; rx <= R; ++rx)
        v[(ry + R) * 5 + (rx + R)] = dispTex.Load(int3(clamp(p0 + int2(rx, ry), int2(0, 0), pmax), 0)).g;
    best = 1e9; d.y = v[12];
    [unroll] for (int a2 = 0; a2 < 25; ++a2)
    if (abs(a2 % 5 - 2) <= 1 && abs(a2 / 5 - 2) <= 1)
    {
        float s = 0;
        [unroll] for (int b = 0; b < 25; ++b) s += nws[b] * abs(v[a2] - v[b]);
        if (s < best) { best = s; d.y = v[a2]; }
    }
    // Video: each frame's disparity is estimated afresh and wobbles a little,
    // which showed as shimmering borrowed colour. Where this spot looks as it
    // did last frame, keep most of last frame's disparity; where it changed
    // (motion, a cut), take the new estimate.
    if (g_temporal > 0.5)
    {
        const float3 was = srcPrevQ.SampleLevel(samp, uv, 0.0).rgb;
        const float change = dot(abs(c0 - was), float3(1.0, 1.0, 1.0)) / 3.0;
        const float keep = saturate(1.0 - change * 20.0);   // (~5% change: none kept; none at all: all of last frame's -- a still picture's colour no longer flickers with the video's noise)
        const float4 pd = dispPrevTex.SampleLevel(samp, uv, 0.0);
        if (pd.b >= 0.0) d = lerp(d, pd.rg, keep);   // (-1: not worked out last frame, see above)
    }
    return float4(d, myConf.x, myConf.y);
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
    // (The 6 + 6 weights once, not one of each for all 36 taps.)
    float wxs[6], wys[6];
    [unroll] for (int k = 0; k < 6; ++k) { wxs[k] = lanczos3Weight((float)(k - 2) - frac.x); wys[k] = lanczos3Weight((float)(k - 2) - frac.y); }
    [unroll] for (int dy = -2; dy <= 3; ++dy)
    {
        float wy = wys[dy + 2];
        [unroll] for (int dx = -2; dx <= 3; ++dx)
        {
            float wx = wxs[dx + 2];
            float w = wx * wy;
            int2 q = clamp(int2(ip.x + dx, ip.y + dy),
                           int2(0, 0), cellSizePx - int2(1, 1));
            acc  += SrcLoad(int3(cellMinPx + q, 0)).rgb * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-5);
}

// Quilt in two passes. The Lanczos kernel is the product of a horizontal and a
// vertical one, so it can be applied one direction at a time with exactly
// the same result: first along each row of the view (PSQuiltH, 6 reads, at
// the output's width but only the view's few hundred rows), then down the
// columns of that (the compose, 6 reads). 12 reads an output pixel instead
// of 36 -- 24 instead of 72 while cross-fading between two views.
// Target: the output's width (both panes) by twice the view's height -- the
// nearer view's rows, then the next view's (only while cross-fading).
float4 PSQuiltH(VSOut i) : SV_Target
{
    const int ew = (int)g_paneW;
    const int ox = (int)i.pos.x, oy = (int)i.pos.y;
    const bool rightPane = ox >= ew;
    bool right = rightPane; if (g_swap) right = !right;
    float ex = ((float)(ox - (rightPane ? ew : 0)) + 0.5) / g_paneW;
    ex += (right ? -g_convergence : g_convergence);
    int total  = max(1, g_quiltCols * g_quiltRows);
    int viewLo = clamp(right ? g_quiltRightIdx : g_quiltLeftIdx, 0, total - 1);
    int viewHi = min(viewLo + 1, total - 1);
    const float blend = saturate(right ? g_quiltRBlend : g_quiltLBlend);
    const int viewWpx = max(1, (int)(g_srcW / (float)g_quiltCols));
    const int viewHpx = max(1, (int)(g_srcH / (float)g_quiltRows));
    const bool hi = oy >= viewHpx;
    if (hi && (blend <= 0.002 || viewHi == viewLo)) return 0;   // (no cross-fade: not needed)
    const int view = hi ? viewHi : viewLo, row = oy - (hi ? viewHpx : 0);
    // (Pillarboxed: as the one-pass version.)
    float viewAspect = (g_srcW / (float)g_quiltCols) / max(1.0, g_srcH / (float)g_quiltRows);
    float paneAspect = (g_paneH > 0.0) ? (g_paneW / g_paneH) : viewAspect;
    if (viewAspect < paneAspect)
    {
        const float widthFrac = viewAspect / paneAspect, marginX = (1.0 - widthFrac) * 0.5;
        if (ex < marginX || ex > 1.0 - marginX) return 0;
        ex = (ex - marginX) / widthFrac;
    }
    const int2 cell = int2((view % g_quiltCols) * viewWpx, (g_quiltRows - 1 - view / g_quiltCols) * viewHpx);
    const float p = ex * viewWpx - 0.5;
    const int ip = (int)floor(p); const float fr = p - ip;
    float3 acc = 0; float wsum = 0;
    [unroll] for (int dx = -2; dx <= 3; ++dx)
    {
        const float w = lanczos3Weight((float)dx - fr);
        acc += SrcLoad(int3(cell.x + clamp(ip + dx, 0, viewWpx - 1), cell.y + clamp(row, 0, viewHpx - 1), 0)).rgb * w;
        wsum += w;
    }
    return float4(acc / max(wsum, 1e-5), 1);
}
// ... and down the columns (the compose): view `upper` 0 the nearer, 1 the next.
float3 quiltVertical(int ox, float evy, int viewHpx, int upper)
{
    const float p = evy * viewHpx - 0.5;
    const int ip = (int)floor(p); const float fr = p - ip;
    float3 acc = 0; float wsum = 0;
    [unroll] for (int dy = -2; dy <= 3; ++dy)
    {
        const float w = lanczos3Weight((float)dy - fr);
        acc += dispTex.Load(int3(ox, upper * viewHpx + clamp(ip + dy, 0, viewHpx - 1), 0)).rgb * w;
        wsum += w;
    }
    return acc / max(wsum, 1e-5);
}

// (The disparity maps' value here, joint-bilateral upsampled: .r left->right,
// .g right->left (uv), .b/.a confidence.) Both eyes share it.
float4 AnaDispAt(float2 e, float3 c)
{
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
    return dC;
}

// ... the search for one eye, from the disparity maps' start (dC) and the
// gradient window at e (gradWindow; it gives both eyes' channels).
void AnaSearch(float2 e, int eye, float4 dC, float rgR[4], float rgG[4], float px, out float dRef, out float conf)
{
    float d0 = ((eye == 0) ? dC.r : dC.g) * g_lvlToSrcX; // this eye -> the other
    // COUPLE the eyes: a region unreliable in EITHER eye is treated unreliable
    // in BOTH (min), so it's inpainted symmetrically -> no one-eye-clean /
    // other-eye-splotch rivalry. (Both pyramids are regional averages of the
    // same scene, so they fill to near-identical colour.)
    float baseConf = min(dC.b, dC.a);                    // consistency x uniqueness, coupled


    // Full-res refine (±3 px) with sub-pixel parabola fit on the cost curve.
    // The 7 candidates' 5-tap windows (taps and candidates both 1 px apart)
    // cover just 11 positions: candidate r, tap w sits at d0 + (r + w - 5) px.
    // Fetch each once -- the same values as a window per candidate, from 11
    // texture reads instead of 35.
    float sr[11], sg[11];
    [unroll] for (int q = 0; q < 11; ++q)
    {
        float3 s = SrcSampleLevel(samp, float2(e.x + d0 + (float)(q - 5) * px, e.y), 0).rgb;
        sr[q] = anaChanL(s); sg[q] = anaChanR(s);
    }
    // (Each difference through descCurve once: the candidates' windows share them.)
    float cref[4], ccand[10];
    [unroll] for (int j = 0; j < 4; ++j) cref[j] = descCurve((eye == 0) ? rgR[j] : rgG[j]);
    [unroll] for (int j2 = 0; j2 < 10; ++j2) ccand[j2] = descCurve((eye == 0) ? sg[j2 + 1] - sg[j2] : sr[j2 + 1] - sr[j2]);   // (left: red ref vs green cand; right: green vs red)
    float sads[7];
    [unroll] for (int r = 0; r < 7; ++r)
    {
        float sd = 0.0;
        [unroll] for (int k2 = 0; k2 < 4; ++k2)
            sd += abs(cref[k2] - ccand[r + k2]);
        sads[r] = sd;
    }
    // (Ties go to the centre -- the disparity from the maps -- not to the
    // first candidate, which shifted every flat area by -3 px.)
    [unroll] for (int t0 = 0; t0 < 7; ++t0) sads[t0] += abs((float)(t0 - 3)) * 0.02;   // (strong enough that noise can't move it: flicker)
    int bi = 3; float bs = sads[3];
    [unroll] for (int t = 0; t < 7; ++t) if (sads[t] < bs) { bs = sads[t]; bi = t; }
    dRef = d0 + (float)(bi - 3) * px;
    // (The two neighbouring costs picked out without indexing an array by bi --
    // that put the array in slow memory, for every pixel pair.)
    float cm = 0, cc0 = bs, cp = 0;
    [unroll] for (int t2 = 0; t2 < 7; ++t2) { cm = (t2 == bi - 1) ? sads[t2] : cm; cp = (t2 == bi + 1) ? sads[t2] : cp; }
    if (bi > 0 && bi < 6)   // parabola vertex from the two neighbouring costs
    {
        float den = cm - 2.0 * cc0 + cp;
        float delta = (abs(den) > 1e-5) ? 0.5 * (cm - cp) / den : 0.0;
        dRef += clamp(delta, -1.0, 1.0) * px;
    }


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
    conf = saturate(refEnergy * 8.0) * saturate(1.0 - bs * 1.2) * baseConf;
}

// Recovered Colour: where this pixel's missing colour is in the other eye (dRef,
// uv x offset) and how far to trust it (conf). The disparity maps give the
// start; a +-3 px search on the gradients at full resolution pins it down.
void AnaRefine(float2 e, int eye, float3 c, float px, out float dRef, out float conf)
{
    const float4 dC = AnaDispAt(e, c);
    float rgR[4], rgG[4]; gradWindow(e, c, px, rgR, rgG);
    AnaSearch(e, eye, dC, rgR, rgG, px, dRef, conf);
}

// Recovered Colour at full-width eyes: the refine once per horizontal pixel
// pair, for both eyes (it was the costliest part of the compose, and the
// disparity it finds barely changes from one pixel to the next). Both eyes look
// at the same source pixel unless convergence shifts them apart, so what they
// share (the pixel, the disparity maps' value, the gradient window) is worked
// out once. Target: one texel per pair; .rg the left eye's dRef / conf, .ba
// the right eye's. The compose reads it.
// Both eyes see the same plain grey around here -- the anaglyph flat and
// neutral: what's shown already IS each eye's colour (a page beside the
// picture, plain grey sky), nothing to recover.
// (Plain and neutral as the eye sees it, too: the darkest and the lightest
// value round here -- any tap, any channel -- must also be within kFlatTone of
// each other on the square root of the light. On the light itself alone,
// anything dark passed -- every difference down there is under 0.01 -- so a
// dark blue or dark red area was taken for plain grey and shown as it is,
// while the pixels round its edge were recovered: a band round every dark
// coloured area, a different shade from its middle. Two square roots a
// pixel: taken per tap, the test cost 6% of a whole frame.)

bool anaFlatAt(float2 e, float3 c)
{
    const float2 pxy = 1.0 / float2(g_srcW, g_srcH);
    float dev = max(abs(c.r - c.g), abs(c.r - c.b));
    // (A coloured pixel is not plain grey, whatever is round it: nothing more
    // is read -- most of a picture. The middle tap is c itself: not read again.)
    bool flat = false;
    [branch] if (dev < 0.01)
    {
        float vLo = min(min(c.r, c.g), c.b), vHi = max(max(c.r, c.g), c.b);
        [unroll] for (int fy = -1; fy <= 1; ++fy)
        [unroll] for (int fx = -1; fx <= 1; ++fx)
        {
            if (fx == 0 && fy == 0) continue;
            const float3 q = SrcSampleLevel(samp, e + float2(fx * 3.0, fy * 3.0) * pxy, 0).rgb;
            dev = max(dev, max(max(abs(q.r - c.r), abs(q.g - c.g)), abs(q.b - c.b)));
            dev = max(dev, max(abs(q.r - q.g), abs(q.r - q.b)));
            vLo = min(vLo, min(min(q.r, q.g), q.b)); vHi = max(vHi, max(max(q.r, q.g), q.b));
        }
        flat = dev < 0.01 && sqrt(max(vHi, 0.0)) - sqrt(max(vLo, 0.0)) < kFlatTone;
    }
    return flat;
}
// (For a pixel pair: 1 the first pixel plain grey, 2 the second, 3 both.)
float anaFlatPair(float2 e, float3 c)
{
    const float2 e2 = float2(e.x + 1.0 / g_paneW, e.y);
    return (anaFlatAt(e, c) ? 1.0 : 0.0) + (anaFlatAt(e2, SrcSample(samp, e2).rgb) ? 2.0 : 0.0);
}

float4 PSAnaPair(VSOut i) : SV_Target
{
    const int ew = (int)g_paneW;
    const int xe = min((int)i.pos.x * 2, ew - 1);   // (the pair's first pixel)
    const float2 e = float2(((float)xe + 0.5) / (float)ew, i.uv.y);
    // (Each eye's place in the source, as ConvertCore works it out.)
    const float2 eL = float2(e.x + g_convergence, e.y), eR = float2(e.x - g_convergence, e.y);
    if (g_changeSkip > 0.5)   // (as the compose: unchanged blocks keep last frame's)
    {
        uint cw, chh; changeTex.GetDimensions(cw, chh);
        if (cw > 0 && changeTex.Load(int3(clamp(int2(e * float2(g_srcW, g_srcH) / 16.0), 0, int2(cw, chh) - 1), 0)).r < 0.5) discard;
    }
    const float px = 1.0 / g_srcW;
    float dL, cL, dR, cR;
    if (g_convergence == 0.0)
    {
        const float3 c = SrcSample(samp, e).rgb;
        // (Plain grey round this pair, from the 1/4 level -- PSFlatShrink: no tests.)
        {
            uint fw, fh; flatTex.GetDimensions(fw, fh);
            if (fw > 0 && flatTex.Load(int3(clamp(int2(e * float2(g_srcW, g_srcH) / 4.0), 0, int2(fw, fh) - 1), 0)).r > 0.5)
                return float4(0, 3, 0, 3);
        }
        // (Both plain grey: no refine. .g / .a tell the compose which are, and it shows them as they are.)
        const float flat = anaFlatPair(e, c);
        if (flat > 2.5) return float4(0, flat, 0, flat);
        const float4 dC = AnaDispAt(e, c);
        float rgR[4], rgG[4]; gradWindow(e, c, px, rgR, rgG);
        AnaSearch(e, 0, dC, rgR, rgG, px, dL, cL);
        AnaSearch(e, 1, dC, rgR, rgG, px, dR, cR);
        return float4(dL, flat, dR, flat);   // (.g / .a: which of the pair is plain grey)
    }
    else
    {
        const float3 cl = SrcSample(samp, eL).rgb, cr = SrcSample(samp, eR).rgb;
        const float fl = anaFlatPair(eL, cl), fr = anaFlatPair(eR, cr);
        dL = dR = 0;
        if (fl < 2.5) AnaRefine(eL, 0, cl, px, dL, cL);
        if (fr < 2.5) AnaRefine(eR, 1, cr, px, dR, cR);
        return float4(dL, fl, dR, fr);
    }
    return 0;
}

// forceFmt: compiled for one format alone (>= 0; -1: the format from the
// constant buffer). Every other format then folds away -- a much smaller shader,
// so the GPU runs more pixels at once and hides its texture reads better.
// recoveryOnly: the Recovered Colour compose alone (PSAnaCompose); its per-pixel
// refine folds away too (PSAnaPair has done it). noRecovery: the anaglyph modes
// without Recovered Colour.
// Recovered Colour, one pixel (source place e, its colour c, which eye): if a
// black-and-white / one-colour box (SetAnaBoxes) holds it, its colour decoded
// that way -- true; else false.
bool anaBoxDecode(float2 e, float3 c, int eye, out float3 o)
{
    o = c;
    uint mw, mh; anaBoxMapTex.GetDimensions(mw, mh);
    const float2 cand = mw > 0 ? anaBoxMapTex.Load(int3(min(int2(e * float2(g_srcW, g_srcH) / 16.0), int2(mw, mh) - 1), 0)).xy : 0;
    [unroll] for (int ci = 0; ci < 2; ++ci)
    {
        const int bi = (int)cand[ci] - 1;
        if (bi < 0) break;
        const float4 br = boxRect(bi);
        if (e.x < br.x || e.x > br.z || e.y < br.y || e.y > br.w) continue;
        const float4 bk = anaBoxTex.Load(int3(2 + 2 * bi, 0, 0));
        if (bk.x > 1.5)
        {
            // (Plain grey here -- page the box reaches over after a scroll: as
            // it is; the tint would paint it.)
            const float3 sc = srgbEncode(saturate(c));
            o = (max(max(sc.r, sc.g), sc.b) - min(min(sc.r, sc.g), sc.b) < 3.0 / 255.0) ? c : tintDecode(c, eye, (int)bk.y);
            return true;
        }
        if (bk.x > 0.5) { o = decodeAnaglyph(c, g_anaCombo, eye, 3); return true; }
    }
    return false;
}
// ... and recovered: its own channel(s) as seen, the other view's from where
// it's matched (dRef, uv x offset), brought to this eye's brightness.
float3 anaRecoverPixel(float2 e, float3 c, int eye, float dRef)
{
    const float px = 1.0 / g_srcW, py = 1.0 / g_srcH;
    float eyeY = anaEyeLuma(c, g_anaCombo, eye);            // own sharp luminance
    // Block processing (SIRA-style) -- LUMINANCE-WEIGHTED 3x3 borrow. The
    // matched centre tap defines the "patch region"; each neighbour is
    // exponentially down-weighted by its luminance difference from the
    // centre, so taps that land across an object edge at the matched
    // location contribute almost nothing. Plain 9-tap averaging smeared
    // cross-eye colour across edges -> the persistent borrow-edge
    // marbelling. SIRA's superpixel containment serves the same purpose;
    // luminance-weighting is the shader-feasible analogue.
    // (The nine read once: the middle one is the matched centre.)
    float3 tap[9];
    [unroll] for (int ty = -1; ty <= 1; ++ty)
    [unroll] for (int tx = -1; tx <= 1; ++tx)
        tap[(ty + 1) * 3 + tx + 1] = SrcSampleLevel(samp, float2(e.x + dRef + (float)tx * px, e.y + (float)ty * py), 0.0).rgb;
    float  centreY = dot(tap[4], float3(0.299, 0.587, 0.114));
    float3 there = 0;
    float  tw    = 1e-4;
    [unroll] for (int bt = 0; bt < 9; ++bt)
    {
        float3 s = tap[bt];
        float  sY = dot(s, float3(0.299, 0.587, 0.114));
        // (Brightness compared on its square root, as the eye sees it: a step
        // in a dark area counts as much as the same visible step in a bright one.)
        float  w  = exp(-abs(sqrt(max(sY, 0.0)) - sqrt(max(centreY, 0.0))) * 8.0);
        there += s * w;
        tw    += w;
    }
    there /= tw;
    // Own channel(s) as seen, the other view's from the aligned borrow
    // (red/cyan left: own red, borrowed green + blue). Custom: the share of
    // each channel that's this eye's.
    float3 ownMask = (g_anaCombo == 6) ? ((eye == 0) ? g_anaMaskL.rgb : g_anaMaskR.rgb) : anaFilter(float3(1, 1, 1), g_anaCombo, eye);
    float3 alignedCol = c * ownMask + there * (1.0 - ownMask);
    float aY = max(dot(alignedCol, float3(0.299, 0.587, 0.114)), 1e-3);
    // (Brought to this eye's brightness -- but if that pushes a channel
    // past full, the whole colour is scaled back rather than that channel
    // cut off: clipping one channel changed the hue, red paint going
    // pink or white even where the match was right.)
    float3 scaled = alignedCol * (eyeY / aY);
    const float peak = max(max(scaled.r, scaled.g), scaled.b);
    if (peak > 1.0) scaled /= peak;
    return saturate(scaled);
}

// Light field (an experiment): every view of a Quilt at once, spread across
// each lens. A lens covers g_lfPitch pixels and leans by g_lfSlant pixels a
// row; where a sub-pixel sits under its lens (0..1) decides which view it
// shows -- red, green and blue each a third of a pixel apart. Nothing follows
// the viewer: moving the head across the lens' fan shows the views in turn.
// (The same picture goes to both halves of the output: the SR weave of two
// equal eyes is that picture, pixel for pixel.) pos: the output pixel within
// its half; e: its place in the picture, 0..1.
// RGB + depth: a picture beside its depth map (Looking Glass layout: the
// colour on the left, the depth on the right, white near -- or the other way
// round, bits 0 and 1 of g_quiltCols). A view of it from somewhere else is
// drawn by moving each part of the picture sideways by its depth: what is
// nearer than the focus plane goes against the viewer's movement, what is
// further, with it. v: where the view is from (x in eye spacings to the
// right, y upwards). The other figures ride in constants this layout has no
// other use for: g_vrZoom the strength (picture widths of shift per eye
// spacing, nearest against furthest), g_fpEyeFrac the focus plane's depth.
// (The layout, g_quiltCols - 4: bit 0 the depth map is the left half, bit 1
// black is near, bit 2 the side is found out here.)
// Which half is the depth map, when that is to be found out: the one with no
// colour in it. Eight fixed places in each half are looked at -- the same for
// every pixel, so the whole picture decides alike. A black-and-white photo
// beside its depth map can't be told this way: the right half is taken.
bool rgbdDepthLeftFind()
{
    float cl = 0.0, cr = 0.0;
    [unroll] for (int k = 0; k < 8; ++k)
    {
        const float2 q = float2(((float)(k % 4) + 0.5) / 4.0, ((float)(k / 4) + 0.5) / 2.0);
        const float3 a = SrcSampleLevel(samp, float2(q.x * 0.5, q.y), 0).rgb;
        const float3 b = SrcSampleLevel(samp, float2(0.5 + q.x * 0.5, q.y), 0).rgb;
        cl += max(max(a.r, a.g), a.b) - min(min(a.r, a.g), a.b);
        cr += max(max(b.r, b.g), b.b) - min(min(b.r, b.g), b.b);
    }
    return cl < cr * 0.5;
}
// (Found once for the whole conversion -- a 1x1 pass, PSRgbdSide -- and read
// here from t2: it used to be worked out again at every pixel, 16 reads each,
// twice what the view itself takes.)
float4 PSRgbdSide(VSOut i) : SV_Target { return rgbdDepthLeftFind() ? 1.0 : 0.0; }
bool rgbdDepthLeft()
{
    const int rf = g_quiltCols - 4;
    if ((rf & 4) == 0) return (rf & 1) != 0;
    return dispTex.Load(int3(0, 0, 0)).r > 0.5;
}
float rgbdDepth(float2 p, bool depthLeft)
{
    const float2 uv = float2((depthLeft ? 0.0 : 0.5) + saturate(p.x) * 0.5, saturate(p.y));
    // (As the depth map stores it: its code value, not the decoded light.)
    const float d = dot(srgbEncode(saturate(SrcSampleLevel(samp, uv, 0).rgb)), float3(1.0, 1.0, 1.0) / 3.0);
    return ((g_quiltCols - 4) & 2) != 0 ? 1.0 - d : d;
}
float3 rgbdView(float2 e, float2 v, bool depthLeft)
{
    const float aspect = g_srcW * 0.5 / max(g_srcH, 1.0);
    const float2 k = float2(v.x, -v.y * aspect) * g_vrZoom;
    // (Where in the picture this pixel comes from depends on the depth there:
    // found by going round a few times.)
    float2 p = e;
    [unroll] for (int it = 0; it < 6; ++it) p = e + k * (rgbdDepth(p, depthLeft) - g_fpEyeFrac);
    return SrcSampleLevel(samp, float2((depthLeft ? 0.5 : 0.0) + saturate(p.x) * 0.5, saturate(p.y)), 0).rgb;
}

float3 lightField(float2 pos, float2 e)
{
    const int total = max(1, g_quiltCols * g_quiltRows);
    const int viewWpx = max(1, (int)(g_srcW / (float)g_quiltCols));
    const int viewHpx = max(1, (int)(g_srcH / (float)g_quiltRows));
    float3 o = 0;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        // (Measured from the middle of the panel: changing the pitch then opens
        // or closes the fans about the centre instead of sliding everything.)
        const float ph = frac((pos.x - g_paneW * 0.5 + (float)(c - 1) / 3.0 + (pos.y - g_paneH * 0.5) * g_lfSlant) / g_lfPitch + g_quiltLBlend + 0.5);
        // (Swap Eyes: the views the other way round.)
        // (View spread, g_quiltRBlend: how much of the Quilt's range of views the
        // lens' fan shows, about the middle one. A Quilt made for a wide view
        // cone squeezed whole into the fan gives each eye views far apart: too
        // much depth to fuse up close. A part of it keeps the depth natural.)
        const float pv = 0.5 + ((g_swap ? 1.0 - ph : ph) - 0.5) * max(g_quiltRBlend, 0.02);
        const int view = clamp((int)(pv * total), 0, total - 1);
        // (Looking Glass order: view 0 the bottom-left cell.)
        const float2 cell = float2((view % g_quiltCols) * viewWpx, (g_quiltRows - 1 - view / g_quiltCols) * viewHpx);
        float3 s = SrcSampleLevel(samp, (cell + saturate(e) * float2(viewWpx, viewHpx)) / float2(g_srcW, g_srcH), 0).rgb;
        // (Alignment pattern, g_quiltRightIdx > 0: the left half of the fan red,
        // the right half blue, in place of the picture. Lined up, one eye sees
        // the whole screen red and the other blue; stripes mean the pitch or the
        // slant is off.)
        if (g_quiltRightIdx > 0) s = ph < 0.5 ? float3(1.0, 0.05, 0.0) : float3(0.0, 0.15, 1.0);
        o[c] = s[c];
    }
    return o;
}

// ... the light field from an RGB + depth picture: each sub-pixel's view is
// drawn for where its light goes -- across the lens' fan, g_quiltRBlend x 4 eye
// spacings from one side to the other.
float3 lightFieldRgbd(float2 pos, float2 e, bool depthLeft)
{
    float3 o = 0;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        const float ph = frac((pos.x - g_paneW * 0.5 + (float)(c - 1) / 3.0 + (pos.y - g_paneH * 0.5) * g_lfSlant) / g_lfPitch + g_quiltLBlend + 0.5);
        float3 s = rgbdView(e, float2(((g_swap ? 1.0 - ph : ph) - 0.5) * g_quiltRBlend * 4.0, 0.0), depthLeft);
        if (g_quiltRightIdx > 0) s = ph < 0.5 ? float3(1.0, 0.05, 0.0) : float3(0.0, 0.15, 1.0);
        o[c] = s[c];
    }
    return o;
}

// DeAnaglyph's colour: the 9 pixels round e in its row, averaged (c: the source
// at e, which the caller has -- it was read here a second time).
float3 anaRowBlur(float2 e, float3 c)
{
    const float h = 1.0 / g_srcW;
    float3 acc = c;
    [unroll] for (int k = 0; k < 2; ++k)
    {
        const float o = (1.5 + 2.0 * k) * h;   // (between pixels 1,2 then 3,4 away)
        acc += 2.0 * (SrcSample(samp, float2(e.x - o, e.y)).rgb + SrcSample(samp, float2(e.x + o, e.y)).rgb);
    }
    return acc / 9.0;
}

float4 ConvertCoreImpl(VSOut i, bool recoveryOnly, int forceFmt = -1, bool noRecovery = false)
{
    const int fmt   = recoveryOnly ? 2 : forceFmt >= 0 ? forceFmt : g_format;
    const int amode = recoveryOnly ? 4 : g_anaMode;
    if (fmt == 99) return SrcSample(samp, i.uv);   // 1:1 copy (history blit)

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

    if (fmt == 1 || fmt == 12)   // Top-and-bottom: top=left, bottom=right
    {
        // (12: Full top-and-bottom on a wide source -- a screen showing a tall
        // 16:18 frame pillarboxed: the picture is the middle half of the width.)
        float2 s = float2(g_format == 12 ? 0.25 + e.x * 0.5 : e.x, right ? 0.5 + e.y * 0.5 : e.y * 0.5);
        return SrcSample(samp, s);
    }
    else if (fmt == 3)   // Row interleaved: even rows=left, odd=right
    {
        float row = floor(e.y * (g_srcH * 0.5)) * 2.0 + (right ? 1.0 : 0.0);
        return SrcSample(samp, float2(e.x, (row + 0.5) / g_srcH));
    }
    else if (fmt == 4)   // Column interleaved: even cols=left, odd=right
    {
        // (An eye is every other column: each output pixel is exactly one of
        // them -- read it as it is.)
        const int2 sz = int2(g_srcW, g_srcH);
        const int col = clamp((int)floor(e.x * (g_srcW * 0.5)) * 2 + (right ? 1 : 0), 0, sz.x - 1);
        return SrcLoad(int3(col, clamp((int)floor(e.y * g_srcH), 0, sz.y - 1), 0));
    }
    else if (fmt == 5)   // Checkerboard: (x + y) even = left, odd = right
    {
        // Each eye has half the pixels, in a diamond pattern over the whole
        // picture, so it's rebuilt at full size: its own pixels exactly, and
        // each missing one from its four neighbours -- all four the same eye's
        // -- along whichever pair (left/right or up/down) differs less, so an
        // edge stays sharp. (Taking only the eye's pixel from each column pair
        // shifted every other row by half a column: zig-zag vertical edges.)
        const int2 sz = int2(g_srcW, g_srcH);
        const int y = clamp((int)floor(e.y * g_srcH), 0, sz.y - 1);
        const int x = clamp((int)floor(e.x * g_srcW), 0, sz.x - 1);
        const int eyeBit = right ? 1 : 0;
        if (((x + y + eyeBit) & 1) == 0) return SrcLoad(int3(x, y, 0));   // (this eye's own pixel)
        // (At the picture's edge a missing neighbour falls back to the one opposite.)
        const float3 l = SrcLoad(int3(x > 0 ? x - 1 : x + 1, y, 0)).rgb;
        const float3 r = SrcLoad(int3(x < sz.x - 1 ? x + 1 : x - 1, y, 0)).rgb;
        const float3 u = SrcLoad(int3(x, y > 0 ? y - 1 : y + 1, 0)).rgb;
        const float3 d = SrcLoad(int3(x, y < sz.y - 1 ? y + 1 : y - 1, 0)).rgb;
        const float dh = dot(abs(l - r), float3(1, 1, 1)), dv = dot(abs(u - d), float3(1, 1, 1));
        const float3 o = dh < dv * 0.8 ? (l + r) * 0.5 : dv < dh * 0.8 ? (u + d) * 0.5 : (l + r + u + d) * 0.25;
        return float4(o, 1);
    }
    else if (fmt == 2)   // Anaglyph: decode per combo + mode
    {
        // (Recovery: a block nothing changed near since the last frame keeps
        // last frame's output -- see PSChange.)
        if (!noRecovery && g_changeSkip > 0.5)
        {
            uint cw, chh; changeTex.GetDimensions(cw, chh);
            if (cw > 0)
            {
                const float2 ch = changeTex.Load(int3(clamp(int2(e * float2(g_srcW, g_srcH) / 16.0), 0, int2(cw, chh) - 1), 0)).rg;
                if (ch.r < 0.5)
                {
                    // (A block that only scrolled: last frame's output for it, from where it was.)
                    if (g_scrollOn > 0.5 && ch.g > 0.5)
                        return outPrevTex.Load(int3((int)i.pos.x, (int)i.pos.y + (int)scrollTex.Load(int3(0, 0, 0)).r, 0));
                    discard;
                }
            }
        }
        int eye = right ? 1 : 0;
        float3 c = SrcSample(samp, e).rgb;

        // A one-colour picture under the whole anaglyph: its tint (tintDecode).
        if (amode == 5 && anaTintTex.Load(int3(0, 0, 0)).a > 0.5)   // (no tables bound: Mono below)
            return float4(tintDecode(c, eye, 0), 1);
        // Recovered Colour on a page of several: the pictures that were black-and-
        // white (Mono) or one colour (their tint) are decoded so inside their
        // boxes (Converter::SetAnaBoxes); the rest is recovered.
        { float3 bo; if (!noRecovery && amode == 4 && anaBoxDecode(e, c, eye, bo)) return float4(bo, 1); }

        // Multi-scale aligned recovery (red/cyan only). Reads the coarse disparity
        // map (PSAnaDisp), refines it at full resolution, checks left-right
        // consistency to flag occlusions, then borrows only the disparity-aligned
        // CHROMA (each eye keeps its own sharp luminance) -> de-fringed full colour.
        if (!noRecovery && amode == 4)   // (any colour pair: see anaChanL / anaChanR)
        {
            float px = 1.0 / g_srcW;
            float py = 1.0 / g_srcH;
            // Where both eyes see the same plain grey here (anaFlatAt): shown as
            // it is -- a borrow could only bring the wrong colour in. (With
            // PSAnaPair's result: judged there, once per pixel pair.)
            if (!recoveryOnly && g_pairRefine < 0.5 && anaFlatAt(e, c)) return float4(c, 1);
            // Where to borrow from (the other eye), and how far to trust it:
            // worked out once per pixel pair by PSAnaPair when that ran (the
            // disparity barely changes from one pixel to the next), else here.
            float dRef, conf;
            if (recoveryOnly || g_pairRefine > 0.5)
            {
                const int ew = (int)g_paneW;
                const int ox = (int)i.pos.x, pane = ox >= ew ? 1 : 0;   // (this output pixel: which half, where in it)
                const float4 pp = pairTex.Load(int3(clamp(ox - pane * ew, 0, ew - 1) / 2, (int)i.pos.y, 0));
                const float2 pr = eye == 0 ? pp.rg : pp.ba;   // (.rg the left eye's, .ba the right's)
                dRef = pr.x; conf = 0;
                // (Plain grey, as PSAnaPair judged this pixel of the pair: shown as it is.)
                const int which = (clamp(ox - pane * ew, 0, ew - 1) & 1) ? 2 : 1;
                if (((int)(pr.y + 0.5) & which) != 0) return float4(c, 1);
            }
            else if (!recoveryOnly) AnaRefine(e, eye, c, px, dRef, conf);
            else { dRef = 0; conf = 0; }   // (PSAnaCompose only runs with PSAnaPair's result)
            return float4(anaRecoverPixel(e, c, eye, dRef), conf);
        }

        if (amode == 0 || (!noRecovery && amode == 4)) // Recovered colour: per-eye luminance + shared,
        {                                      // horizontally blurred chrominance (reduces fringing).
            float eyeY = anaEyeLuma(c, g_anaCombo, eye);   // sharp per-eye luminance
            // The 9 pixels around it in the row: the centre, and four pairs each
            // read in one go -- a sample exactly between two pixels is their
            // average (bilinear) -- the same sum from 5 reads instead of 9.
            float3 cb = anaRowBlur(e, c);                  // horizontally blurred colour
            float anaY = max(dot(cb, float3(0.299, 0.587, 0.114)), 1e-3);
            return float4(saturate(cb * (eyeY / anaY)), 1);
        }
        return float4(decodeAnaglyph(c, g_anaCombo, eye, amode), 1);
    }
    else if (fmt == 6)   // Pulfrich: mono source -> per-eye delay / ND darken
    {
        float3 cur = SrcSample(samp, e).rgb;
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
    else if (fmt == 7)   // HDMI 1.4 frame packing: top eye, gap, bottom eye
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
        return SrcSample(samp, float2(e.x, v));
    }
    else if (fmt == 8)   // Frame sequential: alternating L/R frames over time
    {
        // Each eye is a FULL frame. One eye shows the current frame, the other the
        // previous frame (= the other eye in genuinely frame-sequential content).
        // e.x is the within-pane 0..1, mapping to the full source. Swap eyes flips
        // which eye is current vs previous if the parity is wrong.
        if (right) return float4(SrcSample(samp, e).rgb, 1);
        return float4(srcPrev.Sample(samp, e).rgb, 1);
    }
    else if (fmt == 13)   // RGB + depth: each eye's view drawn from the depth map
    {
        const float2 pe = float2(rightPane ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);
        const bool depthLeft = rgbdDepthLeft();
        if (g_lfPitch > 0.0)
            return float4(lightFieldRgbd(float2(rightPane ? i.pos.x - g_paneW : i.pos.x, i.pos.y), pe, depthLeft), 1);
        // (This eye half the eye separation to its side -- g_fpGapFrac scales it --
        // plus where the viewer's head is: g_vrYaw / g_vrPitch, in eye spacings.)
        return float4(rgbdView(pe, float2((right ? 0.5 : -0.5) * g_fpGapFrac + g_vrYaw, g_vrPitch), depthLeft), 1);
    }
    else if (fmt == 9)   // Quilt: cols x rows grid of views; pick a pair
    {
        if (g_lfPitch > 0.0)
            return float4(lightField(float2(rightPane ? i.pos.x - g_paneW : i.pos.x, i.pos.y), float2(rightPane ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y)), 1);
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
        // (Two passes when PSQuiltH has run -- its rows at t2: the same, cheaper.)
        {
            uint qw, qh; dispTex.GetDimensions(qw, qh);
            if (qw > 0)
            {
                const int vh = max(1, (int)(g_srcH / (float)g_quiltRows));
                float3 r2 = quiltVertical((int)i.pos.x, ev.y, vh, 0);
                if (blend > 0.002 && viewHi != viewLo) r2 = lerp(r2, quiltVertical((int)i.pos.x, ev.y, vh, 1), blend);
                return float4(r2, 1);
            }
        }
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
    else if (fmt == 10)  // VR180 / VR360 equirectangular projection
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

        return float4(SrcSampleLevel(samp, uv, 0).rgb, 1);
    }

    // Default: side-by-side. left=left half, right=right half.
    // FullSBS (format 11) additionally crops the source vertically to the
    // centre 50%, where 32:9 letterboxed Full-SBS content lives when
    // displayed on a 16:9 source (screen capture or aspect-fit video).
    // HalfSBS (format 0) treats the whole source as already-shaped SBS.
    float vy = e.y;
    if (fmt == 11) vy = e.y * 0.5 + 0.25;
    float2 s = float2(right ? 0.5 + e.x * 0.5 : e.x * 0.5, vy);
    return SrcSample(samp, s);
}

// (HDR: the picture divided down into the ordinary 0..1 range for the weave --
// g_anaTL.a, a slot the custom anaglyph colours leave free: 0 = as it is. The
// renderer multiplies it back after the weave, Renderer::HdrRestore.)
float4 Opaque(float4 c) { if (g_anaTL.a > 0.0) c.rgb *= g_anaTL.a; c.a = 1.0; return c; }

float4 PSMain(VSOut i) : SV_Target
{
    // (What the weaver is given is a picture: opaque. A capture can carry
    // other alpha, and Recovered Colour its confidence; passed on, the weave
    // came out part see-through and the renderer had to mask every frame.)
    float4 r = Opaque(ConvertCoreImpl(i, false));
    return r;
}

// Recovered Colour's compose on its own (see ConvertCoreImpl): used when
// PSAnaPair has worked out the refine per pixel pair.
float4 PSAnaCompose(VSOut i) : SV_Target
{
    float4 r = ConvertCoreImpl(i, true);
    r.a = 1.0;   // (the recovery carries its confidence in alpha; the output is opaque)
    return r;
}

// The common formats each compiled on their own (see ConvertCoreImpl): smaller
// shaders than PSMain, which holds every format. Converter::Convert picks one;
// the rest (Pulfrich, frame-sequential, Quilt, VR, the history copy) use PSMain.
float4 PSFmtHalfSBS(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 0)); }    // (and Katanga)
float4 PSFmtFullSBS(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 11)); }
float4 PSFmtTAB(VSOut i)     : SV_Target { return Opaque(ConvertCoreImpl(i, false, 1)); }
float4 PSFmtRow(VSOut i)     : SV_Target { return Opaque(ConvertCoreImpl(i, false, 3)); }
float4 PSFmtColumn(VSOut i)  : SV_Target { return Opaque(ConvertCoreImpl(i, false, 4)); }
float4 PSFmtChecker(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 5)); }
float4 PSFmtFramePack(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 7)); }
float4 PSFmtAnaglyph(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 2, true)); }   // (not Recovered Colour)
float4 PSFmtQuilt(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 9)); }
float4 PSFmtPulfrich(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 6)); }
float4 PSFmtSequential(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 8)); }
float4 PSFmtVR(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 10)); }
float4 PSFmtRgbd(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 13)); }
float4 PSFmtCopy(VSOut i) : SV_Target { return Opaque(ConvertCoreImpl(i, false, 99)); }   // (the history copy: Pulfrich's and frame sequential's earlier frames)

// ----- Both eyes from one thread (compute) ---------------------------------
// In the anaglyph modes, checkerboard and the interleaved layouts, the two
// eyes' pixels at a place come from the same source pixels. A pixel shader
// draws each eye on its own -- every source read twice. Here one thread does
// both: the reads are shared, and each eye is written to its half of the
// output (a UNORM view of it: the sRGB encode is done here, as the render
// target's view did).
RWTexture2D<float4> outU : register(u0);
void csBothEyes(uint2 id, int fmt, bool noRecovery)
{
    uint W, H; outU.GetDimensions(W, H);
    const uint ew = W / 2;
    if (id.x >= ew || id.y >= H) return;
    VSOut a; a.pos = float4(id.x + 0.5, id.y + 0.5, 0, 1); a.uv = a.pos.xy / float2(W, H);
    VSOut b = a; b.pos.x += ew; b.uv.x = b.pos.x / W;
    const float3 l = ConvertCoreImpl(a, false, fmt, noRecovery).rgb, r = ConvertCoreImpl(b, false, fmt, noRecovery).rgb;
    outU[id] = float4(srgbEncode(saturate(l)), 1);
    outU[uint2(id.x + ew, id.y)] = float4(srgbEncode(saturate(r)), 1);
}
// The anaglyph modes but Recovered Colour: with no Convergence shift both eyes
// look at the same source pixel, so it is read once for the pair -- and
// DeAnaglyph's blurred colour, the same for both, worked out once. (Filtered,
// Half Colour, Mono, a tinted picture, DeAnaglyph: all of them through here.)
void csAnaBothEyes(uint2 id)
{
    if (g_convergence != 0.0) { csBothEyes(id, 2, true); return; }
    uint W, H; outU.GetDimensions(W, H);
    const uint ew = W / 2;
    if (id.x >= ew || id.y >= H) return;
    const float2 e = float2((id.x + 0.5) / ew, (id.y + 0.5) / H);
    const float3 c = SrcSample(samp, e).rgb;
    const int eyeL = g_swap ? 1 : 0, eyeR = 1 - eyeL;   // (which eye's content each half shows)
    float3 l, r;
    if (g_anaMode == 5 && anaTintTex.Load(int3(0, 0, 0)).a > 0.5)
    {
        l = tintDecode(c, eyeL, 0); r = tintDecode(c, eyeR, 0);
    }
    else if (g_anaMode == 0)
    {
        const float3 cb = anaRowBlur(e, c);
        const float anaY = max(dot(cb, float3(0.299, 0.587, 0.114)), 1e-3);
        l = saturate(cb * (anaEyeLuma(c, g_anaCombo, eyeL) / anaY));
        r = saturate(cb * (anaEyeLuma(c, g_anaCombo, eyeR) / anaY));
    }
    else
    {
        l = decodeAnaglyph(c, g_anaCombo, eyeL, g_anaMode); r = decodeAnaglyph(c, g_anaCombo, eyeR, g_anaMode);
    }
    outU[id] = float4(srgbEncode(saturate(l)), 1);
    outU[uint2(id.x + ew, id.y)] = float4(srgbEncode(saturate(r)), 1);
}
[numthreads(16, 8, 1)] void CSFmtAnaglyph(uint3 id : SV_DispatchThreadID) { csAnaBothEyes(id.xy); }

// Recovered Colour's compose, both eyes from one thread (after PSAnaPair; no
// Convergence shift, no scroll reuse -- Converter::Convert uses PSAnaCompose
// otherwise). What the two eyes share is read once: the source pixel, whether
// its block is redrawn at all, the box it may lie in, the pair's refine. Each
// eye's borrow is its own. A block not redrawn is not written: it keeps last
// frame's, as the pixel shader's discard does.
[numthreads(16, 8, 1)] void CSAnaCompose(uint3 tid : SV_DispatchThreadID)
{
    const uint2 id = tid.xy;
    uint W, H; outU.GetDimensions(W, H);
    const uint ew = W / 2;
    if (id.x >= ew || id.y >= H) return;
    const float2 e = float2((id.x + 0.5) / ew, (id.y + 0.5) / H);
    if (g_changeSkip > 0.5)
    {
        uint cw, chh; changeTex.GetDimensions(cw, chh);
        if (cw > 0 && changeTex.Load(int3(clamp(int2(e * float2(g_srcW, g_srcH) / 16.0), 0, int2(cw, chh) - 1), 0)).r < 0.5) return;
    }
    const float3 c = SrcSample(samp, e).rgb;
    const int eyeL = g_swap ? 1 : 0, eyeR = 1 - eyeL;   // (which eye's content each half shows)
    float3 l, r;
    float3 bo;
    if (anaBoxDecode(e, c, eyeL, bo))
    {
        l = bo; anaBoxDecode(e, c, eyeR, r);
    }
    else
    {
        const float4 pp = pairTex.Load(int3(id.x / 2, id.y, 0));   // (.rg the left eye's dRef / plain flags, .ba the right's)
        const int which = (id.x & 1) ? 2 : 1;
        const float2 pl = eyeL == 0 ? pp.rg : pp.ba, pr = eyeR == 0 ? pp.rg : pp.ba;
        l = (((int)(pl.y + 0.5) & which) != 0) ? c : anaRecoverPixel(e, c, eyeL, pl.x);
        r = (((int)(pr.y + 0.5) & which) != 0) ? c : anaRecoverPixel(e, c, eyeR, pr.x);
    }
    outU[id] = float4(srgbEncode(saturate(l)), 1);
    outU[uint2(id.x + ew, id.y)] = float4(srgbEncode(saturate(r)), 1);
}
// Checkerboard, both eyes from one thread, a thread per SOURCE pixel (the eyes
// the source's size, no Convergence shift -- Converter::Convert uses
// PSFmtChecker otherwise). Each source pixel is one eye's own, and the other
// eye's is rebuilt there from the four round it: the same five reads in every
// thread. As a pixel shader, every other output pixel took the other branch --
// one read or four, in a checkerboard: the worst pattern there is for a GPU,
// which runs both sides for all of them. (The same picture as PSFmtChecker.)
[numthreads(16, 8, 1)] void CSFmtChecker(uint3 tid : SV_DispatchThreadID)
{
    uint W, H; outU.GetDimensions(W, H);
    const uint ew = W / 2;
    if (tid.x >= ew || tid.y >= H) return;
    const int2 sz = int2(g_srcW, g_srcH);
    const int x = min((int)tid.x, sz.x - 1), y = min((int)tid.y, sz.y - 1);
    const float3 c = SrcLoad(int3(x, y, 0)).rgb;
    const float3 l = SrcLoad(int3(x > 0 ? x - 1 : x + 1, y, 0)).rgb;
    const float3 r = SrcLoad(int3(x < sz.x - 1 ? x + 1 : x - 1, y, 0)).rgb;
    const float3 u = SrcLoad(int3(x, y > 0 ? y - 1 : y + 1, 0)).rgb;
    const float3 d = SrcLoad(int3(x, y < sz.y - 1 ? y + 1 : y - 1, 0)).rgb;
    const float dh = dot(abs(l - r), float3(1, 1, 1)), dv = dot(abs(u - d), float3(1, 1, 1));
    const float3 o = dh < dv * 0.8 ? (l + r) * 0.5 : dv < dh * 0.8 ? (u + d) * 0.5 : (l + r + u + d) * 0.25;
    // (The left half shows the left eye's content -- the right's with the eyes
    // swapped -- and that eye owns the pixels where x + y + its number is even.)
    const bool leftOwns = ((x + y + (g_swap ? 1 : 0)) & 1) == 0;
    outU[tid.xy] = float4(srgbEncode(saturate(leftOwns ? c : o)), 1);
    outU[uint2(tid.x + ew, tid.y)] = float4(srgbEncode(saturate(leftOwns ? o : c)), 1);
}
[numthreads(16, 8, 1)] void CSFmtColumn(uint3 id : SV_DispatchThreadID)   { csBothEyes(id.xy, 4, false); }
[numthreads(16, 8, 1)] void CSFmtRow(uint3 id : SV_DispatchThreadID)      { csBothEyes(id.xy, 3, false); }
