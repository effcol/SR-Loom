"""SIRA-faithful offline test: anaglyph -> recovered colour SBS.

A stripped-down reproduction of Kunze (2020) "Stereo Image Recovery from
Anaglyph", used as a no-shader sandbox for experimenting with quality
improvements before any of them are ported to the real-time HLSL pipeline.

Two pluggable stages:
    MATCHER   {sgbm, census}      -- how stereo disparity is computed.
    FILL      {knn,  bilateral}   -- how low-confidence pixels are colorized.
Plus an optional REPROJECTION CHECK that tightens confidence by demanding
cross-channel zero-mean patch similarity at the matched location.

Pipeline (per (matcher, fill) combination):
    1. Extract per-eye preserved channels (left = anaglyph.r, right = (g+b)/2).
    2. Run the matcher both directions; confidence = pixels passing the
       matcher's own L-R consistency check (and optionally the reprojection
       check on top).
    3. SLIC superpixels on the anaglyph (only used by the KNN fill).
    4. Borrow the missing channels for each eye from the disparity-matched
       location in the other eye's anaglyph channels.
    5. Apply the chosen fill to recover low-confidence pixels.
    6. Stack a comparison image: input | luma baseline | every (matcher, fill)
       requested, in order.

Toggles (run one combination by default, add flags to fan out):
    --census          also run the Census-Transform matcher
    --bilateral       also run the joint-bilateral fill (in addition to KNN)
    --reproj-check    enable the reprojection-check confidence gate
Variants are the cross-product of (matchers requested) x (fills requested), so
e.g. `--census --bilateral` runs four variants per image.

Setup:
    pip install numpy opencv-contrib-python   # contrib needed for SLIC + JBF

Usage:
    python tools/sira_offline.py path/to/anaglyph.png
    python tools/sira_offline.py shot.png --census --bilateral --reproj-check
    python tools/sira_offline.py shot.png --max-disp 96 --region 16 -o out/
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np

try:
    import cv2
except ImportError:
    sys.exit("Need opencv-contrib-python: pip install opencv-contrib-python")

if not hasattr(cv2, "ximgproc"):
    sys.exit("Need opencv-contrib-python (cv2.ximgproc is missing). "
             "pip install --upgrade opencv-contrib-python")


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def to_rgb(bgr): return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
def to_bgr(rgb): return cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)


def slic_segments(rgb_image, region_size=20, ruler=10.0, iters=10):
    """Return an int32 label map (H, W), one label per superpixel."""
    lab = cv2.cvtColor(rgb_image, cv2.COLOR_RGB2LAB)
    slic = cv2.ximgproc.createSuperpixelSLIC(
        lab, algorithm=cv2.ximgproc.SLICO,
        region_size=region_size, ruler=ruler)
    slic.iterate(iters)
    return slic.getLabels().astype(np.int32)


# ---------------------------------------------------------------------------
# Matchers (return d_l, d_r, conf_l, conf_r in pixel units)
# ---------------------------------------------------------------------------

def sgbm_match(leftY, rightY, num_disparities=64, **_):
    """StereoSGBM both directions, with SGBM's own L-R consistency check.

    d_l[y, x] >= 0 means leftY[y, x] matches rightY[y, x - d_l[y, x]].
    d_r[y, x] >= 0 means rightY[y, x] matches leftY[y, x + d_r[y, x]].
    """
    L = leftY.astype(np.uint8)
    R = rightY.astype(np.uint8)
    num_disp = ((num_disparities + 15) // 16) * 16

    def make():
        return cv2.StereoSGBM_create(
            minDisparity=0, numDisparities=num_disp,
            blockSize=5,
            P1=8 * 5 ** 2, P2=32 * 5 ** 2,
            disp12MaxDiff=1,
            uniquenessRatio=10,
            speckleWindowSize=100, speckleRange=2,
            mode=cv2.STEREO_SGBM_MODE_SGBM_3WAY,
        )

    d_l = make().compute(L, R).astype(np.float32) / 16.0
    d_r_flipped = make().compute(np.fliplr(R), np.fliplr(L)).astype(np.float32) / 16.0
    d_r = np.fliplr(d_r_flipped)

    conf_l = d_l >= 0
    conf_r = d_r >= 0
    d_l = np.where(conf_l, d_l, 0).astype(np.float32)
    d_r = np.where(conf_r, d_r, 0).astype(np.float32)
    return d_l, d_r, conf_l, conf_r


def census_transform(img, win=5):
    """Per-pixel Census bit-string ((win*win)-1 bits): each non-centre neighbour
    coded as 1 if BRIGHTER than the centre, else 0. Robust to cross-channel
    intensity offset (the bit pattern is preserved as long as the RANKING of
    neighbours by intensity is preserved -- which it largely is across the
    anaglyph's red and green channels even when absolute values differ wildly).

    win must be odd; (win*win)-1 must fit in uint64 -> win in {3, 5, 7}.
    """
    if win % 2 == 0 or (win * win - 1) > 64:
        raise ValueError(f"census window {win} unsupported (use 3, 5 or 7)")
    H, W = img.shape
    img_i = img.astype(np.int32)
    half = win // 2
    pad = np.pad(img_i, half, mode="edge")
    bits = np.zeros((H, W), dtype=np.uint64)
    bit_idx = 0
    for dy in range(-half, half + 1):
        for dx in range(-half, half + 1):
            if dy == 0 and dx == 0:
                continue
            n = pad[half + dy: half + dy + H, half + dx: half + dx + W]
            bits |= (n > img_i).astype(np.uint64) << np.uint64(bit_idx)
            bit_idx += 1
    return bits


def census_match(leftY, rightY, num_disparities=64, win=5, lr_tol=1, **_):
    """Hamming-distance matching on Census codes, WTA per pixel, both directions,
    with L-R consistency check.

    Cost = popcount(L_census XOR R_census_shifted). Lower is better.
    """
    L_cen = census_transform(leftY, win=win)
    R_cen = census_transform(rightY, win=win)
    H, W = leftY.shape
    BIG = np.uint8(win * win)   # max possible Hamming = (win*win)-1, plus 1 for the sentinel

    def disparity(src, ref, sign):
        """For each src pixel, find d in [0, num_disparities) minimising
        popcount(src XOR ref_shifted_by_sign*d)."""
        best_d = np.zeros((H, W), dtype=np.int32)
        best_c = np.full((H, W), BIG, dtype=np.uint8)
        for d in range(num_disparities):
            shifted = np.full_like(ref, 0)
            if sign < 0:        # match src[y, x] with ref[y, x - d]
                if d == 0:
                    shifted = ref
                else:
                    shifted[:, d:] = ref[:, :-d]
                cost = np.bitwise_count(src ^ shifted).astype(np.uint8)
                if d > 0:
                    cost[:, :d] = BIG          # disabled — outside the search
            else:               # match src[y, x] with ref[y, x + d]
                if d == 0:
                    shifted = ref
                else:
                    shifted[:, :-d] = ref[:, d:]
                cost = np.bitwise_count(src ^ shifted).astype(np.uint8)
                if d > 0:
                    cost[:, -d:] = BIG
            better = cost < best_c
            best_d[better] = d
            best_c[better] = cost[better]
        return best_d.astype(np.float32), best_c

    d_l, _ = disparity(L_cen, R_cen, sign=-1)
    d_r, _ = disparity(R_cen, L_cen, sign=+1)

    # L-R consistency: d_l at (y, x) should agree with d_r at the matched (y, x - d_l).
    ys, xs = np.indices((H, W), dtype=np.int32)
    xm_l = xs - d_l.astype(np.int32)
    in_bounds_l = (xm_l >= 0) & (xm_l < W)
    xm_l_c = np.clip(xm_l, 0, W - 1)
    conf_l = in_bounds_l & (np.abs(d_l - d_r[ys, xm_l_c]) <= lr_tol)

    xm_r = xs + d_r.astype(np.int32)
    in_bounds_r = (xm_r >= 0) & (xm_r < W)
    xm_r_c = np.clip(xm_r, 0, W - 1)
    conf_r = in_bounds_r & (np.abs(d_r - d_l[ys, xm_r_c]) <= lr_tol)

    return d_l, d_r, conf_l, conf_r


MATCHERS = {"sgbm": sgbm_match, "census": census_match}


# ---------------------------------------------------------------------------
# Coarse-to-fine pyramid wrapper around any base matcher
# ---------------------------------------------------------------------------

def _refine_disp(srcY, refY, d_init, sign, radius=2, win=5):
    """For each pixel, set d to the integer in [d_init-radius, d_init+radius] that
    minimises ZERO-MEAN patch SAD between srcY[y,x] and refY[y, x + sign*d].
    Zero-mean handles the cross-spectral offset (a red wrapper is 255 in red and
    30 in green; raw SAD would always pick a "match" that happens to have a
    bright green pixel, ZM-SAD cares only about local structure)."""
    H, W = srcY.shape
    S = srcY.astype(np.float32)
    R = refY.astype(np.float32)
    Sm = cv2.boxFilter(S, -1, (win, win))
    Rm = cv2.boxFilter(R, -1, (win, win))
    Sz = S - Sm
    Rz = R - Rm
    best_d = d_init.astype(np.float32).copy()
    best_cost = np.full((H, W), np.inf, dtype=np.float32)
    ys, xs = np.indices((H, W), dtype=np.int32)
    for offset in range(-radius, radius + 1):
        d_test = (d_init + offset).round().astype(np.int32)
        xm = xs + sign * d_test
        valid = (xm >= 0) & (xm < W)
        xm_c = np.clip(xm, 0, W - 1)
        diff = np.abs(Sz - Rz[ys, xm_c])
        cost = cv2.boxFilter(diff, -1, (win, win))
        cost = np.where(valid, cost, np.inf)
        better = cost < best_cost
        best_d = np.where(better, (d_init + offset).astype(np.float32), best_d)
        best_cost = np.where(better, cost, best_cost)
    return best_d, best_cost


def pyramid_match(base_matcher, leftY, rightY, num_disparities=64,
                  levels=3, refine_radius=2, refine_win=5, **kwargs):
    """Coarse-to-fine wrapper: match at the coarsest level with full disparity
    range, upsample 2x at each step, then SAD-refine +/-refine_radius at the
    finer level. Mirrors the shader's PSAnaDisp->PSAnaRefine flow.

    levels = total pyramid depth (3 means coarsest at 1/4 res, mid at 1/2, fine at full).
    """
    # Build pyramid (index 0 = full res, index levels-1 = coarsest)
    pyr_L = [leftY]
    pyr_R = [rightY]
    for _ in range(levels - 1):
        if pyr_L[-1].shape[0] < 4 or pyr_L[-1].shape[1] < 4:
            break
        pyr_L.append(cv2.pyrDown(pyr_L[-1]))
        pyr_R.append(cv2.pyrDown(pyr_R[-1]))
    actual_levels = len(pyr_L)

    # Coarse match: full search at the lowest resolution. Halve the disparity
    # range at each pyramid step (disparities scale with resolution).
    coarse_disp = max(8, num_disparities // (2 ** (actual_levels - 1)))
    L_c = pyr_L[-1]
    R_c = pyr_R[-1]
    print(f"      level {actual_levels-1} ({L_c.shape[1]}x{L_c.shape[0]}) "
          f"full search max_disp={coarse_disp}")
    d_l, d_r, _, _ = base_matcher(L_c, R_c, coarse_disp, **kwargs)

    # Refine at each finer level: upsample (and double) the coarser disparity,
    # then small SAD search around the predicted value.
    for level in range(actual_levels - 2, -1, -1):
        L_f = pyr_L[level]
        R_f = pyr_R[level]
        H_f, W_f = L_f.shape
        d_l = cv2.resize(d_l, (W_f, H_f), interpolation=cv2.INTER_LINEAR) * 2.0
        d_r = cv2.resize(d_r, (W_f, H_f), interpolation=cv2.INTER_LINEAR) * 2.0
        print(f"      level {level} ({W_f}x{H_f}) refine +/-{refine_radius} px")
        d_l, _ = _refine_disp(L_f, R_f, d_l, sign=-1,
                              radius=refine_radius, win=refine_win)
        d_r, _ = _refine_disp(R_f, L_f, d_r, sign=+1,
                              radius=refine_radius, win=refine_win)

    # Final L-R consistency check at full res. Tolerance is generous because the
    # upsample-then-refine path can let independent L/R disparities drift a few
    # pixels at object boundaries even when they're both "right".
    H, W = d_l.shape
    ys, xs = np.indices((H, W), dtype=np.int32)
    lr_tol = 2.5
    xm_l = xs - d_l.round().astype(np.int32)
    in_l = (xm_l >= 0) & (xm_l < W)
    xm_l_c = np.clip(xm_l, 0, W - 1)
    conf_l = in_l & (np.abs(d_l - d_r[ys, xm_l_c]) <= lr_tol)

    xm_r = xs + d_r.round().astype(np.int32)
    in_r = (xm_r >= 0) & (xm_r < W)
    xm_r_c = np.clip(xm_r, 0, W - 1)
    conf_r = in_r & (np.abs(d_r - d_l[ys, xm_r_c]) <= lr_tol)

    return d_l, d_r, conf_l, conf_r


# ---------------------------------------------------------------------------
# Reprojection-check confidence gate (Kunze §3.2, simplified)
# ---------------------------------------------------------------------------

def reproj_check(srcY, refY, disparity, sign, win=5, thresh=12.0):
    """Cross-channel zero-mean SAD between a srcY patch and the refY patch at
    the matched location. ZERO-MEAN handles the cross-spectral intensity offset
    (red wrapper vs green channel) by subtracting each patch's own mean before
    comparing. Returns a bool mask: True = passed the check.
    """
    H, W = srcY.shape
    S = srcY.astype(np.float32)
    R = refY.astype(np.float32)
    # Patch means
    Sm = cv2.boxFilter(S, -1, (win, win))
    Rm = cv2.boxFilter(R, -1, (win, win))
    # Zero-mean versions
    Sz = S - Sm
    Rz = R - Rm
    # Patch SAD between Sz at (y, x) and Rz at (y, x + sign*d)
    # We approximate with pointwise absolute difference on zero-mean values,
    # then box-filter to get a windowed mean. This catches gross local mismatch.
    ys, xs = np.indices((H, W), dtype=np.int32)
    xm = xs + sign * disparity.round().astype(np.int32)
    valid = (xm >= 0) & (xm < W)
    xm_c = np.clip(xm, 0, W - 1)
    diff = np.abs(Sz - Rz[ys, xm_c])
    diff_blur = cv2.boxFilter(diff, -1, (win, win))
    return valid & (diff_blur <= thresh)


# ---------------------------------------------------------------------------
# Fills (replace low-confidence pixels' RGB with a recovered estimate)
# ---------------------------------------------------------------------------

def knn_fill(rgb, pres, conf, segs, k=8, **_):
    """For each low-conf pixel, weighted mean of the K nearest-preserved-value
    confident pixels in the SAME SUPERPIXEL. Weights = 1 / (1 + |pres_diff|)."""
    out = rgb.astype(np.float32).copy()
    flat_seg = segs.ravel()
    flat_pres = pres.ravel().astype(np.float32)
    flat_conf = conf.ravel()
    flat_rgb = rgb.reshape(-1, 3).astype(np.float32)
    flat_out = out.reshape(-1, 3)

    order = np.argsort(flat_seg, kind="stable")
    sorted_seg = flat_seg[order]
    boundaries = np.concatenate(([0], np.where(np.diff(sorted_seg) != 0)[0] + 1,
                                 [len(sorted_seg)]))

    for s_start, s_end in zip(boundaries[:-1], boundaries[1:]):
        idx = order[s_start:s_end]
        if len(idx) == 0:
            continue
        seg_pres = flat_pres[idx]
        seg_conf = flat_conf[idx]
        seg_rgb = flat_rgb[idx]

        donor_pos = np.where(seg_conf)[0]
        recip_pos = np.where(~seg_conf)[0]
        if len(donor_pos) == 0 or len(recip_pos) == 0:
            continue

        donor_pres = seg_pres[donor_pos]
        donor_rgb = seg_rgb[donor_pos]
        recip_pres = seg_pres[recip_pos]

        dist = np.abs(recip_pres[:, None] - donor_pres[None, :])
        k_use = min(k, len(donor_pos))
        nearest = np.argpartition(dist, k_use - 1, axis=1)[:, :k_use]
        gathered_dist = np.take_along_axis(dist, nearest, axis=1)
        gathered_rgb = donor_rgb[nearest]

        w = 1.0 / (1.0 + gathered_dist)
        w_sum = w.sum(axis=1, keepdims=True)
        weighted = (gathered_rgb * w[:, :, None]).sum(axis=1) / w_sum
        flat_out[idx[recip_pos]] = weighted

    return np.clip(out, 0, 255).astype(np.uint8)


def bilateral_fill(rgb, pres, conf, segs=None, d=41, sigma_r=15.0, sigma_s=8.0,
                   iters=2, **_):
    """Confidence-aware joint-bilateral fill guided by the preserved channel.

    Linear-filter trick: filter (rgb * conf) and (conf) separately and divide,
    so each output pixel is the bilateral-weighted mean of the donor pixels
    (= those with conf > 0). cv2.ximgproc.jointBilateralFilter is a C++ inner
    loop -- way faster than a numpy 2D window.

    Iterated: each pass uses the previous pass's filled result as additional
    donors, so info propagates ~d/2 px per iteration. With iters=2 a default
    d=41 reaches ~40 px of fill, which covers most low-conf regions.
    """
    rgb_f = rgb.astype(np.float32)
    pres_f = pres.astype(np.float32)
    cur_rgb = rgb_f.copy()
    cur_conf = conf.astype(np.float32)
    cur_mask = conf.copy()

    for _it in range(iters):
        rgb_w = cur_rgb * cur_conf[..., None]
        rgb_bf = cv2.ximgproc.jointBilateralFilter(pres_f, rgb_w, d, sigma_r, sigma_s)
        conf_bf = cv2.ximgproc.jointBilateralFilter(pres_f, cur_conf, d, sigma_r, sigma_s)
        # Pixels where the bilateral found at least some donors get the filled value;
        # those with effectively zero donor weight keep their current (possibly
        # still-low-conf) value -- another iteration may reach them as the filled
        # region grows.
        has_donors = conf_bf > 1e-3
        filled = rgb_bf / np.maximum(conf_bf[..., None], 1e-4)
        # Only update pixels that were low-confidence AND now have donors in reach.
        update_mask = (~cur_mask) & has_donors
        cur_rgb = np.where(update_mask[..., None], filled, cur_rgb)
        cur_conf = np.where(update_mask, 1.0, cur_conf)
        cur_mask = cur_mask | update_mask

    # Fallback: any pixel still unfilled keeps its raw borrowed colour (better than
    # the divide-by-tiny black blob the naive single-pass would have produced).
    return np.clip(cur_rgb, 0, 255).astype(np.uint8)


def pushpull_fill(rgb, pres, conf, segs=None, levels=None, **_):
    """Push-pull pyramid colorization (the shader's PSAnaSplit + GenerateMips +
    PSAnaCompose path, in CPU). PULL: repeatedly pyrDown(premultiplied rgb, conf),
    each level a confidence-weighted average of the level below. PUSH: walk every
    level upsampled to full res; accumulate colour from each level weighted by
    its confidence and how much weight remains. Reach is GLOBAL (every pixel
    eventually gets non-zero weight from some coarse level), so unlike bilateral
    there's no notion of pixels too far from a donor.
    """
    H, W = pres.shape
    rgb_f = rgb.astype(np.float32)
    conf_f = conf.astype(np.float32)

    # Premultiplied (rgb*conf, conf) so pyrDown's box average is confidence-weighted.
    pre_rgb = rgb_f * conf_f[..., None]
    pre_a = conf_f.copy()

    # Build pyramid down to ~1 px.
    if levels is None:
        levels = int(np.ceil(np.log2(max(H, W)))) + 1
    pyr = [(pre_rgb, pre_a)]
    for _ in range(levels - 1):
        prev_rgb, prev_a = pyr[-1]
        if prev_rgb.shape[0] < 2 or prev_rgb.shape[1] < 2:
            break
        pyr.append((cv2.pyrDown(prev_rgb), cv2.pyrDown(prev_a)))

    # Compose: walk every level upsampled to full res; finer levels dominate via
    # acc_w saturation, coarser levels fill what's still empty.
    acc_rgb = np.zeros_like(rgb_f)
    acc_w = np.zeros((H, W), dtype=np.float32)
    for lvl, (lvl_rgb, lvl_a) in enumerate(pyr):
        if lvl == 0:
            up_rgb, up_a = lvl_rgb, lvl_a
        else:
            up_rgb = cv2.resize(lvl_rgb, (W, H), interpolation=cv2.INTER_LINEAR)
            up_a = cv2.resize(lvl_a, (W, H), interpolation=cv2.INTER_LINEAR)
        col = up_rgb / np.maximum(up_a, 1e-4)[..., None]
        a = np.clip(up_a, 0, 1)
        contrib = a * (1.0 - acc_w)
        acc_rgb += col * contrib[..., None]
        acc_w += contrib
        if acc_w.min() >= 0.99:
            break

    filled = acc_rgb / np.maximum(acc_w[..., None], 1e-3)
    out = np.where(conf[..., None], rgb_f, filled)
    return np.clip(out, 0, 255).astype(np.uint8)


FILLS = {"knn": knn_fill, "bilateral": bilateral_fill, "pushpull": pushpull_fill}


# ---------------------------------------------------------------------------
# Borrowing + per-eye assembly
# ---------------------------------------------------------------------------

def borrow_from(other_channels, disparity, sign):
    """For each (y, x): other_channels[y, x + sign*disparity[y, x]] (clamped).
    sign = -1 for left eye, +1 for right eye."""
    H, W = disparity.shape
    ys, xs = np.indices((H, W), dtype=np.int32)
    x_match = xs + sign * disparity.round().astype(np.int32)
    valid = (x_match >= 0) & (x_match < W)
    x_clamp = np.clip(x_match, 0, W - 1)
    return other_channels[ys, x_clamp], valid


def assemble_eyes(ana_rgb, leftY, rightY, d_l, d_r, conf_l, conf_r):
    """Build per-eye RGB (preserved channels straight from ana, missing channel(s)
    borrowed from the disparity-matched pixel of the other eye) and combined
    per-pixel confidence (matcher conf AND borrow in-frame).

    For red/cyan: left eye preserved = ana.r; right eye preserved = ana.g AND ana.b
    (BOTH preserved separately -- the rightY luma-proxy used for matching is NOT
    the right eye's green channel, those have to come from ana.g and ana.b directly).
    """
    other_for_left = ana_rgb[..., 1:3]    # (g, b) -> recovers left.g, left.b
    other_for_right = ana_rgb[..., 0:1]   # (r,)   -> recovers right.r
    left_borrowed, left_valid = borrow_from(other_for_left, d_l, sign=-1)
    right_borrowed, right_valid = borrow_from(other_for_right, d_r, sign=+1)

    left_rgb = np.dstack([leftY, left_borrowed]).astype(np.uint8)
    right_rgb = np.dstack([
        right_borrowed[..., 0],
        ana_rgb[..., 1],     # green preserved on cyan side (NOT rightY -- that's a luma proxy)
        ana_rgb[..., 2],     # blue preserved on cyan side
    ]).astype(np.uint8)
    return left_rgb, right_rgb, conf_l & left_valid, conf_r & right_valid


# ---------------------------------------------------------------------------
# Luma baseline (~ SRLoom mode 0)
# ---------------------------------------------------------------------------

def luma_decode(ana_rgb):
    ana_f = ana_rgb.astype(np.float32)
    leftY = ana_f[..., 0]
    rightY = (ana_f[..., 1] + ana_f[..., 2]) * 0.5
    blurred = cv2.boxFilter(ana_f, -1, (9, 1))
    blurY = np.maximum(
        0.299 * blurred[..., 0] + 0.587 * blurred[..., 1] + 0.114 * blurred[..., 2],
        1e-3)

    def per_eye(eyeY):
        return np.clip(blurred * (eyeY / blurY)[..., None], 0, 255).astype(np.uint8)

    return np.hstack([per_eye(leftY), per_eye(rightY)])


# ---------------------------------------------------------------------------
# Top-level pipeline for one (matcher, fill) variant
# ---------------------------------------------------------------------------

def run_variant(ana_rgb, leftY, rightY, segs, matcher_name, fill_name,
                num_disp, reproj_check_enabled, knn_k, bilateral_d,
                bilateral_sigma_r, bilateral_sigma_s, bilateral_iters,
                census_win, pyramid_levels, refine_radius):
    win_tag = f"{census_win}x{census_win}" if matcher_name == "census" else ""
    pyr_tag = f"-pyr{pyramid_levels}" if pyramid_levels > 1 else ""
    label = (f"SIRA: {matcher_name}{('-'+win_tag) if win_tag else ''}{pyr_tag} "
             f"+ {fill_name}" + (" + reproj" if reproj_check_enabled else ""))
    print(f"\n  --- {label} ---")
    matcher = MATCHERS[matcher_name]

    t = time.perf_counter()
    if pyramid_levels > 1:
        d_l, d_r, conf_l, conf_r = pyramid_match(
            matcher, leftY, rightY, num_disp,
            levels=pyramid_levels, refine_radius=refine_radius,
            win=census_win)
    else:
        d_l, d_r, conf_l, conf_r = matcher(leftY, rightY, num_disp, win=census_win)
    print(f"    matcher in {time.perf_counter()-t:.2f}s; "
          f"conf left = {conf_l.mean():.1%}, right = {conf_r.mean():.1%}")

    if reproj_check_enabled:
        t = time.perf_counter()
        ok_l = reproj_check(leftY, rightY, d_l, sign=-1)
        ok_r = reproj_check(rightY, leftY, d_r, sign=+1)
        before_l, before_r = conf_l.mean(), conf_r.mean()
        conf_l = conf_l & ok_l
        conf_r = conf_r & ok_r
        print(f"    reproj-check in {time.perf_counter()-t:.2f}s; "
              f"conf left {before_l:.1%}->{conf_l.mean():.1%}, "
              f"right {before_r:.1%}->{conf_r.mean():.1%}")

    left_rgb, right_rgb, conf_l, conf_r = assemble_eyes(
        ana_rgb, leftY, rightY, d_l, d_r, conf_l, conf_r)

    t = time.perf_counter()
    fill = FILLS[fill_name]
    fill_kwargs = dict(k=knn_k, d=bilateral_d,
                       sigma_r=bilateral_sigma_r, sigma_s=bilateral_sigma_s,
                       iters=bilateral_iters)
    left_done = fill(left_rgb, leftY, conf_l, segs, **fill_kwargs)
    right_done = fill(right_rgb, rightY, conf_r, segs, **fill_kwargs)
    print(f"    fill in {time.perf_counter()-t:.2f}s")

    return label, np.hstack([left_done, right_done]), conf_l, conf_r


# ---------------------------------------------------------------------------
# Output stacking
# ---------------------------------------------------------------------------

def stack_compare(label_imgs):
    """Vertically stack [(label, RGB), ...] with a label bar above each."""
    max_w = max(img.shape[1] for _, img in label_imgs)
    rows = []
    for label, img in label_imgs:
        H, W = img.shape[:2]
        if W != max_w:
            new_h = max(1, int(H * max_w / W))
            img = cv2.resize(img, (max_w, new_h), interpolation=cv2.INTER_AREA)
        bar = np.full((30, max_w, 3), 32, dtype=np.uint8)
        cv2.putText(bar, label, (10, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (240, 240, 240), 1, cv2.LINE_AA)
        rows.append(np.vstack([bar, img]))
    return np.vstack(rows)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path, help="anaglyph PNG/JPG (red/cyan)")
    ap.add_argument("-o", "--out-dir", type=Path, default=None,
                    help="output directory (default: alongside input)")
    ap.add_argument("--max-disp", type=int, default=64,
                    help="max disparity in pixels (default 64)")
    ap.add_argument("--region", type=int, default=20,
                    help="SLIC superpixel region size in pixels (default 20, KNN only)")
    ap.add_argument("--census", action="store_true",
                    help="also run the Census-Transform matcher")
    ap.add_argument("--bilateral", action="store_true",
                    help="also run the joint-bilateral fill")
    ap.add_argument("--pushpull", action="store_true",
                    help="also run the push-pull pyramid fill (the shader's strategy)")
    ap.add_argument("--reproj-check", action="store_true",
                    help="enable reprojection-check confidence gate")
    ap.add_argument("--pyramid-levels", type=int, default=1,
                    help="coarse-to-fine matching pyramid depth (1 = disabled; "
                         "3 = match at 1/4 then refine at 1/2 then refine at full)")
    ap.add_argument("--refine-radius", type=int, default=2,
                    help="per-level pyramid refine search radius in px (default 2)")
    ap.add_argument("--knn-k", type=int, default=8,
                    help="K neighbours for KNN fill (default 8)")
    ap.add_argument("--bilateral-d", type=int, default=41,
                    help="bilateral diameter px (default 41)")
    ap.add_argument("--bilateral-iters", type=int, default=2,
                    help="bilateral fill iterations (default 2, fills ~iters*d/2 px)")
    ap.add_argument("--census-win", type=int, default=5,
                    help="Census transform window (5, 7 or 9; default 5 -> 24 bits)")
    ap.add_argument("--bilateral-sigma-r", type=float, default=15.0,
                    help="bilateral preserved-channel sigma (default 15)")
    ap.add_argument("--bilateral-sigma-s", type=float, default=8.0,
                    help="bilateral spatial sigma (default 8)")
    args = ap.parse_args()

    bgr = cv2.imread(str(args.input), cv2.IMREAD_COLOR)
    if bgr is None:
        sys.exit(f"Could not read {args.input}")
    ana = to_rgb(bgr)
    print(f"Input: {args.input} ({ana.shape[1]}x{ana.shape[0]})")

    leftY = ana[..., 0]
    rightY = ((ana[..., 1].astype(np.uint16) + ana[..., 2].astype(np.uint16)) // 2).astype(np.uint8)

    print("Luma baseline decode...")
    luma_sbs = luma_decode(ana)

    print(f"SLIC superpixels (region={args.region})...")
    t = time.perf_counter()
    segs = slic_segments(ana, region_size=args.region)
    print(f"  {segs.max() + 1} superpixels in {time.perf_counter() - t:.2f}s")

    matchers = ["sgbm"]
    if args.census:
        matchers.append("census")
    fills = ["knn"]
    if args.bilateral:
        fills.append("bilateral")
    if args.pushpull:
        fills.append("pushpull")
    print(f"Running {len(matchers)*len(fills)} variants: "
          f"matchers={matchers}, fills={fills}, "
          f"reproj-check={args.reproj_check}, pyramid-levels={args.pyramid_levels}")

    rows = [("Input anaglyph", ana),
            ("Luma-only decode (~ SRLoom mode 0)", luma_sbs)]
    variant_outputs = {}
    for m in matchers:
        for f in fills:
            label, sbs, conf_l, conf_r = run_variant(
                ana, leftY, rightY, segs, m, f,
                args.max_disp, args.reproj_check,
                args.knn_k, args.bilateral_d,
                args.bilateral_sigma_r, args.bilateral_sigma_s,
                args.bilateral_iters, args.census_win,
                args.pyramid_levels, args.refine_radius)
            rows.append((label, sbs))
            variant_outputs[f"{m}-{f}"] = (sbs, conf_l, conf_r)

    out_dir = args.out_dir or args.input.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = args.input.stem

    def save(name, img_rgb):
        path = out_dir / f"{stem}_{name}.png"
        cv2.imwrite(str(path), to_bgr(img_rgb))
        print(f"  -> {path}")

    print("\nSaving outputs...")
    save("luma", luma_sbs)
    for variant_name, (sbs, conf_l, conf_r) in variant_outputs.items():
        save(variant_name, sbs)
        conf_vis = (np.hstack([conf_l, conf_r]).astype(np.uint8) * 255)
        save(f"{variant_name}_conf", np.dstack([conf_vis, conf_vis, conf_vis]))
    save("compare", stack_compare(rows))
    print("Done.")


if __name__ == "__main__":
    main()
