// opentrack_pipeline.h -- Leia head pose to OpenTrack mapping
// Pipeline: orientation input -> OneEuro filter -> sensitivity/offset -> clamp.
//
// Ported into SR Loom from the Simulated Reality OpenTrack Bridge
// (https://github.com/effcol/leia-track-app-XYZ, MIT). The Bridge has both
// OneEuro and SMA pipelines (SMA was for VRto3D / SteamVR target); SR Loom
// targets OpenTrack only, so we keep just the OneEuro path.
#pragma once

#include "one_euro_filter.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace srw
{
    struct OpenTrackConfig {
        // OneEuro filter parameters. Position is slower/smoother; rotation is
        // faster/responsive. The Bridge's runtime-tuned defaults sit at
        // mincutoff 0.001 / beta 0.001 for rotation -- "minimal smoothing,
        // very responsive". Stage 2 Accela (below) provides the head-still
        // damping so OneEuro can stay fast.
        float filterFreq          = 60.0f;
        float filterPosMincutoff  = 0.08f;
        float filterPosBeta       = 0.08f;
        float filterRotMincutoff  = 0.001f;
        float filterRotBeta       = 0.001f;
        float angleDeadzoneDeg    = 0.2f;

        // Stage 2: OpenTrack-style Accela. Velocity-aware piecewise gain
        // curve applied AFTER OneEuro. Small inputs (head-still tracker
        // noise) get heavily damped; real head turns hit the steep part
        // of the curve and pass through. Roughly replicates what the
        // OpenTrack desktop app does as its output smoothing stage, so
        // games receive equivalent pose whether the user is bridging
        // via OpenTrack or via SR Loom directly. ON by default.
        // Threshold 0.5 deg is the middle setting. If the wobble persists
        // at all threshold values it's not Accela's gain curve causing it
        // -- likely the interaction between OneEuro's adaptive cutoff at
        // rest and Accela's slow low-norm gain, or noise from the SR
        // head tracker itself. Try accelaEnabled = false to diagnose.
        bool  accelaEnabled       = true;
        float accelaRotThresholdDeg = 0.5f;

        // Lytro/LeiaSR orientation values are radians. Conversion to degrees
        // happens inside the pipeline.
        bool  orientationRadians  = true;

        // Per-axis sensitivity (degrees in -> degrees out) and offset (added
        // before sensitivity).
        float sensYaw             = 1.0f;
        float sensPitch           = 1.0f;
        float sensRoll            = 1.0f;
        float yawOffset           = 0.0f;
        float pitchOffset         = 0.0f;
        float rollOffset          = 0.0f;

        // Output clamp (degrees).
        float maxYaw              = 70.0f;
        float maxPitch            = 70.0f;
        float maxRoll             = 70.0f;

        // Send raw mm-from-monitor position scaled to cm; OpenTrack expects
        // cm. Defaults: X + Y + Yaw inverted. X + Yaw matches OpenTrack's
        // left-handed convention; Y matches the eye-tracker's downward-positive
        // axis flipping to the game's upward-positive convention (only
        // surfaces when sourcing position from the filtered eye-pair stream
        // instead of raw head pose, which we switched to in v1.7). The
        // remaining three are user-tweakable per-game.
        bool  passthroughTranslation = true;
        bool  invertX             = true;
        bool  invertY             = true;
        bool  invertZ             = false;
        bool  invertYaw           = true;
        bool  invertPitch         = false;
        bool  invertRoll          = false;

        // Output mode (ranked by stability):
        //   1: XYZ + Yaw/Pitch    (default -- position + look direction.
        //                         Covers the largest swath of titles
        //                         (sims, racing, action games with head-
        //                         look). Position bleed from head rotation
        //                         is small and consistent with what SR-
        //                         native games (The First Berserker:
        //                         Khazan, Lies of P, Stellar Blade, Hell
        //                         is Us) produce themselves -- those
        //                         games read POSITION only but the bleed
        //                         is geometric reality, not our error.)
        //   2: XYZ only           (position-only tracking; useful when a
        //                         game double-applies rotation from its
        //                         own gamepad/mouse input on top of ours)
        //   3: Yaw/Pitch only     (rotation without roll, gimbal-lock-free)
        //   4: X/Y/Z + Y/P/R      (full 6DOF; jittery on quick head turns)
        //   5: Yaw/Pitch/Roll only (rotation-only tracking)
        int   outputMode          = 1;
    };

    struct OpenTrackResult {
        float yawDeg    = 0.0f;
        float pitchDeg  = 0.0f;
        float rollDeg   = 0.0f;
        float posXCm    = 0.0f;
        float posYCm    = 0.0f;
        float posZCm    = 0.0f;
        float rawYawDeg = 0.0f;
        float rawPitchDeg = 0.0f;
        float rawRollDeg  = 0.0f;
        bool  valid     = false;
    };

    // Per-axis Accela state (last output, last timestamp, init flag).
    // OpenTrack's accela filter: piecewise-linear gain curve mapping
    // |delta| / threshold -> follow-rate, so small jitter is heavily
    // damped and real motion passes through near-instantly.
    struct AccelaState {
        double lastOutput = 0.0;
        float  lastTs     = 0.0f;
        bool   init       = false;
    };

    // Apply one Accela step. `raw` and return value in degrees.
    // `threshold` is the half-deadband (1.0x of threshold = "1 unit of
    // motion"); curve below is the OpenTrack-shape we've used elsewhere
    // in SR Loom (kGains from the VR head-look filter), scaled by dt
    // for frame-rate independence.
    inline double AccelaStep(AccelaState& a, double raw, float ts, double threshold)
    {
        static const struct { double x, y; } kGains[] = {
            { 0.0, 0.0 }, { 0.5, 0.4  }, { 1.0, 1.5   }, { 1.5, 8.0   },
            { 2.5, 35.0 }, { 5.0, 100.0 }, { 8.0, 200.0 }, { 9.0, 300.0 }
        };
        constexpr int kN = (int)(sizeof(kGains) / sizeof(kGains[0]));
        if (!a.init) { a.init = true; a.lastOutput = raw; a.lastTs = ts; return raw; }
        double dt = (double)(ts - a.lastTs);
        a.lastTs = ts;
        if (dt < 1e-5) dt = 1e-5;
        if (dt > 0.25) dt = 0.25;
        const double delta    = raw - a.lastOutput;
        const double absDelta = std::fabs(delta);
        const double thr      = (threshold > 1e-9) ? threshold : 1e-9;
        const double norm     = absDelta / thr;
        double gain = kGains[kN - 1].y;
        if (norm <= kGains[0].x) gain = kGains[0].y;
        else
            for (int i = 1; i < kN; ++i)
                if (norm <= kGains[i].x)
                {
                    const double t = (norm - kGains[i - 1].x) / (kGains[i].x - kGains[i - 1].x);
                    gain = kGains[i - 1].y + (kGains[i].y - kGains[i - 1].y) * t;
                    break;
                }
        double alpha = (absDelta > 1e-9) ? (dt * gain / absDelta) : 0.0;
        if (alpha > 1.0) alpha = 1.0;
        if (alpha < 0.0) alpha = 0.0;
        a.lastOutput += delta * alpha;
        return a.lastOutput;
    }

    class OpenTrackPipeline
    {
        OneEuroFilter fYaw_, fPitch_, fRoll_;
        AccelaState   aYaw_, aPitch_, aRoll_;
        OpenTrackConfig cfg_;

    public:
        OpenTrackPipeline() { ResetFilters(); }
        explicit OpenTrackPipeline(const OpenTrackConfig& cfg) : cfg_(cfg) { ResetFilters(); }

        void ResetFilters()
        {
            fYaw_   = OneEuroFilter(cfg_.filterFreq, cfg_.filterRotMincutoff, cfg_.filterRotBeta);
            fPitch_ = OneEuroFilter(cfg_.filterFreq, cfg_.filterRotMincutoff, cfg_.filterRotBeta);
            fRoll_  = OneEuroFilter(cfg_.filterFreq, cfg_.filterRotMincutoff, cfg_.filterRotBeta);
            aYaw_   = aPitch_ = aRoll_ = AccelaState{};
        }

        OpenTrackResult Process(float posXmm, float posYmm, float posZmm,
                                float orientX, float orientY, float orientZ,
                                float timestampSec)
        {
            OpenTrackResult r;
            if (!std::isfinite(orientX) || !std::isfinite(orientY) || !std::isfinite(orientZ))
                return r;

            const float toDeg = cfg_.orientationRadians
                                    ? (180.0f / static_cast<float>(M_PI))
                                    : 1.0f;
            r.rawPitchDeg = orientX * toDeg;
            r.rawYawDeg   = orientY * toDeg;
            r.rawRollDeg  = orientZ * toDeg;

            float yawF   = fYaw_  .filter(r.rawYawDeg,   timestampSec);
            float pitchF = fPitch_.filter(r.rawPitchDeg, timestampSec);
            float rollF  = fRoll_ .filter(r.rawRollDeg,  timestampSec);

            // Stage 2: Accela on each rotation axis. Reduces the residual
            // micro-jitter that survives OneEuro at our aggressive responsive
            // mincutoff/beta defaults, matching what the OpenTrack desktop app
            // does on its output stage.
            if (cfg_.accelaEnabled)
            {
                yawF   = (float)AccelaStep(aYaw_,   yawF,   timestampSec, cfg_.accelaRotThresholdDeg);
                pitchF = (float)AccelaStep(aPitch_, pitchF, timestampSec, cfg_.accelaRotThresholdDeg);
                rollF  = (float)AccelaStep(aRoll_,  rollF,  timestampSec, cfg_.accelaRotThresholdDeg);
            }

            if (cfg_.invertYaw)   yawF   = -yawF;
            if (cfg_.invertPitch) pitchF = -pitchF;
            if (cfg_.invertRoll)  rollF  = -rollF;

            r.yawDeg   = std::clamp((yawF   + cfg_.yawOffset)   * cfg_.sensYaw,
                                    -cfg_.maxYaw,   cfg_.maxYaw);
            r.pitchDeg = std::clamp((pitchF + cfg_.pitchOffset) * cfg_.sensPitch,
                                    -cfg_.maxPitch, cfg_.maxPitch);
            r.rollDeg  = std::clamp((rollF  + cfg_.rollOffset)  * cfg_.sensRoll,
                                    -cfg_.maxRoll,  cfg_.maxRoll);

            // Deadzone (after sensitivity so it's user-perceptible).
            auto dz = [&](float v) {
                return (std::fabs(v) < cfg_.angleDeadzoneDeg) ? 0.0f : v;
            };
            r.yawDeg   = dz(r.yawDeg);
            r.pitchDeg = dz(r.pitchDeg);
            r.rollDeg  = dz(r.rollDeg);

            if (!std::isfinite(r.yawDeg))   r.yawDeg   = 0.0f;
            if (!std::isfinite(r.pitchDeg)) r.pitchDeg = 0.0f;
            if (!std::isfinite(r.rollDeg))  r.rollDeg  = 0.0f;

            if (cfg_.passthroughTranslation)
            {
                float xCm = posXmm / 10.0f;
                float yCm = posYmm / 10.0f;
                float zCm = posZmm / 10.0f;
                if (cfg_.invertX) xCm = -xCm;
                if (cfg_.invertY) yCm = -yCm;
                if (cfg_.invertZ) zCm = -zCm;
                r.posXCm = xCm;
                r.posYCm = yCm;
                r.posZCm = zCm;
            }

            // Zero out axes the user's chosen output mode doesn't expose.
            switch (cfg_.outputMode)
            {
            case 1: r.rollDeg = 0.0f; break;                                       // XYZ + Y/P
            case 2: r.yawDeg = r.pitchDeg = r.rollDeg = 0.0f; break;               // XYZ only
            case 3: r.posXCm = r.posYCm = r.posZCm = 0.0f; r.rollDeg = 0.0f; break;// Y/P only
            case 4: break;                                                          // 6DOF
            case 5: r.posXCm = r.posYCm = r.posZCm = 0.0f; break;                  // Y/P/R only
            }

            r.valid = true;
            return r;
        }

        OpenTrackConfig& Config()             { return cfg_; }
        const OpenTrackConfig& Config() const { return cfg_; }
    };
}
