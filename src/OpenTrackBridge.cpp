// OpenTrackBridge.cpp -- see header.
#include "OpenTrackBridge.h"
#include "Common.h"

#include "sr/management/srcontext.h"
#include "sr/utility/exception.h"
#include "sr/sense/headtracker/headposetracker.h"
#include "sr/sense/eyetracker/eyetracker.h"
#include "sr/sense/eyetracker/eyepairlistener.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/display/switchablehint.h"

#include <mutex>
#include <chrono>

namespace srw
{
    // Listener lives here (not in the header) so we don't leak SR SDK
    // includes into every translation unit that pokes the bridge.
    //
    // Position comes from a FILTERED EyeTracker (SR::EyeTracker::create()
    // returns the filtered tracker -- there's a separate createRaw() for
    // unfiltered, which we deliberately don't use). Eye-pair midpoint =
    // head position. This is the same data source SR-native head-tracked
    // games (Khazan, Lies of P, Stellar Blade, Hell is Us) consume, so
    // we get the SR runtime's own smoothing for free and match their
    // feel. Rotation continues to come from HeadPoseTracker since the
    // eye-pair stream doesn't carry orientation.
    class OpenTrackBridge::ListenerImpl : public SR::HeadPoseListener
    {
    public:
        OpenTrackBridge*                         owner    = nullptr;
        SR::HeadPoseTracker*                     tracker  = nullptr;
        SR::InputStream<SR::HeadPoseStream>      stream;
        // Filtered eye-pair source. The tracker pointer is owned by the
        // SRContext (per SDK docs); we just hold a reference.
        SR::EyeTracker*                          eyeTracker = nullptr;
        std::shared_ptr<SR::EyePairStream>       eyeStream;
        OpenTrackPipeline                        pipeline;
        std::mutex                               cfgMtx;
        std::chrono::steady_clock::time_point    epoch    = std::chrono::steady_clock::now();
        // Cached "current head orientation" for the Calibrate button --
        // updated on every accept() and read on demand by SetConfig path.
        double                                   lastOrient[3] = { 0.0, 0.0, 0.0 };
        bool                                     gotPose       = false;
        // Filtered head-position cache from the eye-pair listener.
        // Midpoint of left+right eye in millimetres, native SR coords.
        // Atomic-ish: we use a mutex so we don't tear during reads, but
        // the data is tiny so contention is negligible.
        std::mutex                               posMtx;
        double                                   eyeMidPos[3] = { 0.0, 0.0, 0.0 };
        bool                                     gotEyePair   = false;

        // Inner class so we don't add EyePairListener inheritance to the
        // outer listener (the outer is purely the HeadPose consumer).
        class EyePairCache : public SR::EyePairListener
        {
        public:
            ListenerImpl* owner = nullptr;
            void accept(const SR_eyePair& f) override
            {
                if (!owner) return;
                std::lock_guard<std::mutex> lk(owner->posMtx);
                owner->eyeMidPos[0] = (f.left.x + f.right.x) * 0.5;
                owner->eyeMidPos[1] = (f.left.y + f.right.y) * 0.5;
                owner->eyeMidPos[2] = (f.left.z + f.right.z) * 0.5;
                owner->gotEyePair   = true;
            }
        };
        EyePairCache                             eyeCache;

        void accept(const SR_headPose& f) override
        {
            // Snapshot the latest orientation for Calibrate.
            lastOrient[0] = f.orientation.x;
            lastOrient[1] = f.orientation.y;
            lastOrient[2] = f.orientation.z;
            gotPose       = true;

            if (!owner) return;

            const auto now = std::chrono::steady_clock::now();
            const float ts = std::chrono::duration<float>(now - epoch).count();

            // Position source: prefer the filtered eye-pair midpoint if
            // we've received any eye-pair frames yet. Falls back to the
            // raw head-pose position until the eye stream warms up.
            double px = f.position.x, py = f.position.y, pz = f.position.z;
            {
                std::lock_guard<std::mutex> lk(posMtx);
                if (gotEyePair)
                {
                    px = eyeMidPos[0];
                    py = eyeMidPos[1];
                    pz = eyeMidPos[2];
                }
            }

            OpenTrackResult r;
            {
                // Pipeline holds OneEuro filter state; protect against a
                // concurrent SetConfig() that may rebuild the filters.
                std::lock_guard<std::mutex> lk(cfgMtx);
                r = pipeline.Process(
                        (float)px, (float)py, (float)pz,
                        (float)f.orientation.x, (float)f.orientation.y, (float)f.orientation.z,
                        ts);
            }
            if (!r.valid) return;

            // Fan out the same filtered pose to every active output. They
            // share the pipeline result (one OneEuro pass, one set of
            // inversions) so the user's invert/calibration tweaks apply
            // identically to both protocols. The packet-count counter
            // tracks OpenTrack UDP only (it's the historic "Sending: N"
            // diagnostic that's surfaced in the GUI).
            bool anySent = false;
            if (owner->m_otOn.load(std::memory_order_relaxed))
            {
                const bool ok = owner->m_sender.Send(r.posXCm, r.posYCm, r.posZCm,
                                                     r.yawDeg, r.pitchDeg, r.rollDeg);
                if (ok) owner->m_sent.fetch_add(1, std::memory_order_relaxed);
                else    owner->m_failed.fetch_add(1, std::memory_order_relaxed);
                anySent |= ok;
            }
            // FreeTrack and TrackIR share the same FT_SharedMem mapping
            // (OpenTrack's NPClient is just a second consumer over the
            // same shared memory). One write covers both.
            if (owner->m_ftOn.load(std::memory_order_relaxed) ||
                owner->m_tirOn.load(std::memory_order_relaxed))
            {
                owner->m_ftSender.Send(r.posXCm, r.posYCm, r.posZCm,
                                       r.yawDeg, r.pitchDeg, r.rollDeg);
            }
            (void)anySent;
        }
    };

    OpenTrackBridge::OpenTrackBridge() = default;
    OpenTrackBridge::~OpenTrackBridge()
    {
        Disable();
        if (m_context)
        {
            try { SR::SRContext::deleteSRContext(m_context); } catch (...) {}
            m_context = nullptr;
        }
    }

    bool OpenTrackBridge::SetOutputs(bool openTrack, bool freeTrack, bool trackIR,
                                     const char* host, int port)
    {
        const bool wantListener = (openTrack || freeTrack || trackIR);

        // Tear-down path: all outputs disabled.
        if (!wantListener)
        {
            if (m_enabled.load(std::memory_order_acquire))
            {
                m_enabled.store(false, std::memory_order_release);
                if (m_listener)
                {
                    // Drop both streams BEFORE resetting the listener so
                    // no further accept() callbacks fire into freed memory.
                    try { m_listener->stream.set(nullptr); }
                    catch (...) { /* tracker may already be gone */ }
                    try { m_listener->eyeStream.reset(); }
                    catch (...) { /* eye stream may already be gone */ }
                    m_listener.reset();
                }
                if (m_otOn.load()) m_sender.Shutdown();
                // FT writer is shared by FreeTrack + TrackIR -- one shutdown.
                if (m_ftOn.load() || m_tirOn.load()) m_ftSender.Shutdown();
                m_otOn.store(false, std::memory_order_release);
                m_ftOn.store(false, std::memory_order_release);
                m_tirOn.store(false, std::memory_order_release);
                // Destroy the SR context too: the HeadPoseTracker is owned
                // by the context and keeps the camera engaged until the
                // context dies. Dropping just the listener leaves the tracker
                // alive and the camera light stays on. Recreating the
                // context next Enable() is cheap (~tens of ms on a healthy
                // SR Platform service); cheaper than leaving the camera on
                // forever. The lens hint and m_initialized are owned by /
                // tracked alongside the context so they reset too. NOTE:
                // if the SR weaver is currently running it has its OWN
                // SRContext with its own HeadPoseTracker (for late-latching),
                // so the camera will stay on regardless of what we do here.
                m_lensHint = nullptr;
                m_initialized = false;
                if (m_context)
                {
                    try { SR::SRContext::deleteSRContext(m_context); } catch (...) {}
                    m_context = nullptr;
                }
                Log("OpenTrackBridge: disabled (%llu OT packets sent, %llu failed) -- context released",
                    (unsigned long long)m_sent.load(),
                    (unsigned long long)m_failed.load());
            }
            return true;
        }

        // Ensure SRContext exists. Lazily created so an idle app pays nothing.
        if (!m_context)
        {
            try { m_context = SR::SRContext::create(); }
            catch (SR::ServerNotAvailableException&)
            {
                Log("OpenTrackBridge::SetOutputs: SR Platform service not available");
                return false;
            }
            catch (std::exception& e)
            {
                Log("OpenTrackBridge::SetOutputs: SRContext::create threw: %s", e.what());
                return false;
            }
            if (!m_context) { Log("OpenTrackBridge::SetOutputs: SRContext null"); return false; }
        }

        // OpenTrack UDP sender (independent transport).
        const bool wasOt = m_otOn.load();
        if (openTrack && !wasOt)
        {
            if (!m_sender.Init(host, port))
            {
                Log("OpenTrackBridge: OpenTrack UDP init failed (%s:%d)", host, port);
                openTrack = false;
            }
        }
        else if (!openTrack && wasOt)
        {
            m_sender.Shutdown();
        }
        // FreeTrack + TrackIR share the FT_SharedMem writer. Bring it up
        // if either is freshly enabled and was previously off; tear it down
        // only when both are now off.
        const bool wasFt    = m_ftOn.load();
        const bool wasTir   = m_tirOn.load();
        const bool wantFtMem = (freeTrack || trackIR);
        const bool hadFtMem  = (wasFt || wasTir);
        if (wantFtMem && !hadFtMem)
        {
            if (!m_ftSender.Init())
            {
                Log("OpenTrackBridge: FreeTrack shared-memory init failed");
                freeTrack = false;
                trackIR   = false;
            }
        }
        else if (!wantFtMem && hadFtMem)
        {
            m_ftSender.Shutdown();
        }
        // After possible failures, recompute whether we still want a listener.
        if (!openTrack && !freeTrack && !trackIR)
        {
            m_otOn.store(false, std::memory_order_release);
            m_ftOn.store(false, std::memory_order_release);
            m_tirOn.store(false, std::memory_order_release);
            return false;
        }
        m_otOn.store(openTrack, std::memory_order_release);
        m_ftOn.store(freeTrack, std::memory_order_release);
        m_tirOn.store(trackIR, std::memory_order_release);

        // Spin up the SR listener if it isn't already.
        if (!m_enabled.load(std::memory_order_acquire))
        {
            try
            {
                m_listener.reset(new ListenerImpl());
                m_listener->owner       = this;
                m_listener->eyeCache.owner = m_listener.get();
                // Filtered eye tracker -- create() returns the filtered
                // (smoothed) variant; createRaw() exists for unfiltered.
                // The SR runtime's internal smoothing here is what makes
                // SR-native games feel stable; we ride the same pipe.
                try
                {
                    m_listener->eyeTracker = SR::EyeTracker::create(*m_context);
                    if (m_listener->eyeTracker)
                        m_listener->eyeStream = m_listener->eyeTracker
                            ->openEyePairStream(&m_listener->eyeCache);
                }
                catch (...) { /* eye tracker is best-effort: position falls
                                 back to HeadPose if the stream fails */ }
                m_listener->tracker = SR::HeadPoseTracker::create(*m_context);
                m_listener->stream.set(m_listener->tracker->openHeadPoseStream(m_listener.get()));
                // SwitchableLensHint: vote "we don't need the lens on" so a
                // tracking-only session leaves the panel clear. The SR
                // runtime turns the lens on if ANY app (weaver, 3D game,
                // ApplicationSense) requests it; our hint stays passive.
                // Owned by the SRContext, freed when the context is.
                if (!m_lensHint)
                {
                    try { m_lensHint = SR::SwitchableLensHint::create(*m_context); }
                    catch (...) { m_lensHint = nullptr; }
                }
                if (m_lensHint)
                {
                    try { m_lensHint->disable(); }
                    catch (...) { /* hint not yet bound to a display -- harmless */ }
                }
                if (!m_initialized)
                {
                    try { m_context->initialize(); m_initialized = true; }
                    catch (...) { /* already initialised elsewhere -- harmless */ }
                }
            }
            catch (std::exception& e)
            {
                Log("OpenTrackBridge::SetOutputs: SR tracker create failed: %s", e.what());
                m_listener.reset();
                if (openTrack) m_sender.Shutdown();
                if (freeTrack) m_ftSender.Shutdown();
                m_otOn.store(false, std::memory_order_release);
                m_ftOn.store(false, std::memory_order_release);
                return false;
            }
            m_enabled.store(true, std::memory_order_release);
        }

        Log("OpenTrackBridge: outputs OT=%d FT=%d TIR=%d",
            (int)openTrack, (int)freeTrack, (int)trackIR);
        return true;
    }

    OpenTrackConfig OpenTrackBridge::GetConfig() const
    {
        if (!m_listener) return {};
        std::lock_guard<std::mutex> lk(m_listener->cfgMtx);
        return m_listener->pipeline.Config();
    }

    void OpenTrackBridge::SetConfig(const OpenTrackConfig& cfg)
    {
        if (!m_listener) return;
        std::lock_guard<std::mutex> lk(m_listener->cfgMtx);
        const auto& old = m_listener->pipeline.Config();
        // Rebuild OneEuro filters if their parameters changed; otherwise
        // a plain copy preserves filter history (no glitch on slider drag).
        const bool needReset =
            cfg.filterFreq         != old.filterFreq         ||
            cfg.filterRotMincutoff != old.filterRotMincutoff ||
            cfg.filterRotBeta      != old.filterRotBeta      ||
            cfg.filterPosMincutoff != old.filterPosMincutoff ||
            cfg.filterPosBeta      != old.filterPosBeta;
        m_listener->pipeline.Config() = cfg;
        if (needReset) m_listener->pipeline.ResetFilters();
    }

    void OpenTrackBridge::CalibrateNeutral()
    {
        if (!m_listener || !m_listener->gotPose) return;
        std::lock_guard<std::mutex> lk(m_listener->cfgMtx);
        auto& cfg = m_listener->pipeline.Config();
        const float toDeg = cfg.orientationRadians ? (180.0f / 3.14159265358979323846f) : 1.0f;
        cfg.yawOffset   = -(float)m_listener->lastOrient[1] * toDeg;
        cfg.pitchOffset = -(float)m_listener->lastOrient[0] * toDeg;
        cfg.rollOffset  = -(float)m_listener->lastOrient[2] * toDeg;
        Log("OpenTrackBridge: calibrated neutral (yaw %.2f, pitch %.2f, roll %.2f)",
            cfg.yawOffset, cfg.pitchOffset, cfg.rollOffset);
    }
}
