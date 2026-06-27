// OpenTrackBridge.h -- SR head-pose -> OneEuro filter -> OpenTrack UDP.
//
// Subscribes a SR::HeadPoseListener to a SR::SRContext (the one SRWeaver
// already created -- we don't need a second context, just a second tracker)
// and forwards every pose through opentrack_pipeline + opentrack_udp.
// Enable/disable maps directly to "register / unregister the listener" --
// when off, zero overhead.
//
// Ported from the standalone Simulated Reality OpenTrack Bridge
// (https://github.com/effcol/leia-track-app-XYZ, MIT). The standalone app
// was a console-only tuner; this is the in-app version that lives behind
// a GUI toggle and reuses the SR context SR Loom already has.
#pragma once

#include "../third_party/opentrack_pipeline.h"
#include "../third_party/opentrack_udp.h"
#include "../third_party/freetrack_shm.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace SR { class SRContext; class SwitchableLensHint; }

namespace srw
{
    class OpenTrackBridge
    {
    public:
        // Constructor and destructor are out-of-line so the unique_ptr<ListenerImpl>
        // member doesn't need ListenerImpl to be complete here.
        OpenTrackBridge();
        ~OpenTrackBridge();

        // Set which output protocols are active. Each is independent:
        //   openTrack -> UDP packets to host:port (default OpenTrack standard)
        //   freeTrack -> shared-memory "FT_SharedMem" for FreeTrack 2.0 Enhanced
        //   trackIR   -> same "FT_SharedMem" as freeTrack -- OpenTrack's
        //                NPClient.dll reads this mapping to serve TrackIR
        //                data to games. So enabling trackIR implies the
        //                memory is written; enabling freeTrack does the same.
        //                The two flags are independent for UX purposes
        //                (user may want to disable one branding without the
        //                other) but share the underlying writer.
        // If any is true the SR pose listener is set up; if all are
        // false the listener tears down. Idempotent. Returns false if
        // first-time SR context creation fails (SR Platform service
        // unavailable); the output flags then default to false.
        bool SetOutputs(bool openTrack, bool freeTrack, bool trackIR,
                        const char* host = "127.0.0.1", int port = 4242);

        // Convenience: SetOutputs(true, true, false). TrackIR off by default
        // because it requires OpenTrack + its NPClient DLLs to be installed
        // on the user's machine; caller (main.cpp) detects and enables.
        bool Enable(const char* host = "127.0.0.1", int port = 4242)
        { return SetOutputs(true, true, false, host, port); }

        // Tear down listener + all senders. Each sender sends a final
        // identity frame so any tied-in game doesn't freeze at the last
        // live pose.
        void Disable() { SetOutputs(false, false, false); }

        // True if any output is active (i.e. pose is being processed).
        bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }
        bool IsOpenTrackEnabled() const { return m_otOn.load(std::memory_order_acquire); }
        bool IsFreeTrackEnabled() const { return m_ftOn.load(std::memory_order_acquire); }
        bool IsTrackIREnabled()   const { return m_tirOn.load(std::memory_order_acquire); }

        // Live-update the pipeline config (sensitivity, modes, offsets).
        // Thread-safe: protected by the same mutex as the head-pose
        // listener so config swaps mid-frame don't tear.
        OpenTrackConfig GetConfig() const;
        void SetConfig(const OpenTrackConfig& cfg);

        // "Calibrate" -- set the current head orientation as the neutral
        // pose, applied as additive offsets in the pipeline config.
        void CalibrateNeutral();

        // Diagnostic counters (lock-free reads).
        uint64_t SentPackets() const  { return m_sent.load(std::memory_order_relaxed); }
        uint64_t FailedPackets() const{ return m_failed.load(std::memory_order_relaxed); }

    private:
        class ListenerImpl;
        std::unique_ptr<ListenerImpl> m_listener;
        OpenTrackSender               m_sender;
        FreeTrackSender               m_ftSender;
        // Bridge-owned SRContext: independent of the weaver's, so the
        // toggle works even when the weave is off. Lazily created on
        // first Enable(); held across enable/disable cycles to avoid
        // the SR Platform service rejecting a rapid recreate.
        SR::SRContext*                m_context     = nullptr;
        bool                          m_initialized = false;
        // Cooperative "we don't need the lens" vote -- the SR runtime
        // turns the lens on whenever ANY connected app wants it on, so
        // this stays passive in the presence of the weaver or 3D games.
        SR::SwitchableLensHint*       m_lensHint    = nullptr;
        std::atomic<bool>             m_enabled{ false };
        std::atomic<bool>             m_otOn{ false };
        std::atomic<bool>             m_ftOn{ false };
        std::atomic<bool>             m_tirOn{ false };
        std::atomic<uint64_t>         m_sent{ 0 };
        std::atomic<uint64_t>         m_failed{ 0 };
    };
}
