// freetrack_shm.h -- FreeTrack 2.0 Enhanced shared-memory publisher.
//
// FreeTrack is a head-tracking IPC that predates OpenTrack: games memory-map
// a named Win32 mapping called "FT_SharedMem" and read a fixed-layout struct
// holding pose data. The protocol itself isn't copyrighted -- this is a
// from-spec implementation, not a port of OpenTrack's freetrack output.
//
// This same mapping is ALSO what OpenTrack's NPClient.dll / NPClient64.dll
// reads to serve TrackIR data to games (OpenTrack's freetrack and NPClient
// modules are two consumer wrappers over one shared-memory section). That
// means a single writer here covers BOTH FreeTrack-native games AND
// TrackIR-native games -- the TrackIR pathway just requires OpenTrack to
// be installed so the NaturalPoint registry key points at its NPClient.dll.
//
// Layout follows the canonical freetrackclient/fttypes.h FTHeap (FTData
// pose + GameID + table[8] + GameID2). The OpenTrack-shipped NPClient
// reads up to FTHeap-size, so writing only FTData (the pre-NPClient layout)
// leaves the trailing fields uninitialised in shared memory -- works for
// pure FreeTrack consumers but not robust for NPClient. We zero the whole
// heap and let the game write its own GameID into the trailing fields.
//
// Unit conventions of the struct:
//   position = millimetres
//   rotation = radians
// Our OpenTrack pipeline emits cm + degrees (matching OpenTrack UDP); the
// Send() entry point takes those and converts internally so callers don't
// have to track units per output.
//
// Mutex: "FT_Mutext" (sic -- intentional typo from the original FreeTrack
// project, preserved by every consumer that synchronises on it including
// OpenTrack's NPClient). Created but not held during Send -- consumers
// poll with WaitForSingleObject(timeout=16ms).
#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace srw
{
    // FTHeap layout, natural alignment (no #pragma pack -- matches the
    // canonical freetrackclient/fttypes.h that OpenTrack's NPClient.dll
    // reads). Total = FTData (88 bytes pose) + 16 bytes trailing GameID
    // / table / GameID2 = ~104 bytes naturally aligned. Games write their
    // TrackIR identifier into GameID; OpenTrack's NPClient mirrors it
    // back into GameID2 -- we leave those untouched after the initial
    // zero so the protocol round-trip works.
    struct FreeTrackData
    {
        uint32_t DataID;        // increments per write; games read this to detect "fresh frame"
        int32_t  CamWidth;      // unused by tracking; set to a sane non-zero so the consumer doesn't divide-by-zero
        int32_t  CamHeight;
        float    Yaw;           // radians
        float    Pitch;
        float    Roll;
        float    X;             // mm
        float    Y;
        float    Z;
        float    RawYaw;        // pre-filter; we duplicate the filtered value here
        float    RawPitch;
        float    RawRoll;
        float    RawX;
        float    RawY;
        float    RawZ;
        float    X1, Y1, X2, Y2, X3, Y3, X4, Y4;   // IR points, unused
    };

    struct FreeTrackHeap
    {
        FreeTrackData data;
        int32_t       GameID;       // game writes its TrackIR ID here
        union {
            uint8_t   table[8];     // per-game data table
            int32_t   table_ints[2];
        };
        int32_t       GameID2;      // NPClient mirrors GameID -> here
    };

    class FreeTrackSender
    {
    public:
        FreeTrackSender() = default;
        ~FreeTrackSender() { Shutdown(); }

        // Idempotent. Creates / opens the "FT_SharedMem" mapping. Returns
        // false if CreateFileMappingA or MapViewOfFile fails (rare --
        // typically only on locked-down systems where global-named
        // mappings are denied).
        bool Init();

        // Sends an identity frame, unmaps, closes. Identity-on-shutdown
        // keeps any tied-in game from freezing at our last live pose.
        void Shutdown();

        // Send one pose. Input units = cm + degrees (matching the OpenTrack
        // pipeline output); converted to mm + radians internally.
        bool Send(float xCm, float yCm, float zCm,
                  float yawDeg, float pitchDeg, float rollDeg);

        // Write a zero pose (head centred, no rotation). Bumps DataID so
        // consumers see the frame as fresh.
        void SendIdentity();

    private:
        HANDLE         m_mapping = nullptr;
        HANDLE         m_mutex   = nullptr;
        FreeTrackHeap* m_view    = nullptr;
        uint32_t       m_frameId = 0;
    };

    inline bool FreeTrackSender::Init()
    {
        if (m_view) return true;
        // "Local\\..." would scope to the session; "Global\\..." needs
        // SeCreateGlobalPrivilege. The bare name lives in the calling
        // session's namespace, which is exactly what games expect.
        m_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                       PAGE_READWRITE, 0,
                                       sizeof(FreeTrackHeap),
                                       "FT_SharedMem");
        if (!m_mapping) return false;
        m_view = (FreeTrackHeap*)MapViewOfFile(m_mapping, FILE_MAP_WRITE,
                                               0, 0, sizeof(FreeTrackHeap));
        if (!m_view)
        {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
            return false;
        }
        // FT_Mutext (sic) -- consumers (notably OpenTrack's NPClient.dll)
        // wait on this with a 16ms timeout. CreateMutex returns the existing
        // handle if another process owns the name; we don't hold the lock
        // during writes (single 88-byte memcpy is atomic enough at our
        // cadence), so we just need the named object to exist.
        m_mutex = CreateMutexA(nullptr, FALSE, "FT_Mutext");
        memset(m_view, 0, sizeof(FreeTrackHeap));
        m_frameId = 0;
        return true;
    }

    inline void FreeTrackSender::Shutdown()
    {
        if (m_view)
        {
            SendIdentity();
            UnmapViewOfFile(m_view);
            m_view = nullptr;
        }
        if (m_mapping)
        {
            CloseHandle(m_mapping);
            m_mapping = nullptr;
        }
        if (m_mutex)
        {
            CloseHandle(m_mutex);
            m_mutex = nullptr;
        }
    }

    inline bool FreeTrackSender::Send(float xCm, float yCm, float zCm,
                                      float yawDeg, float pitchDeg, float rollDeg)
    {
        if (!m_view) return false;
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        // Write only the pose part (FTData). Leaves the trailing GameID /
        // table / GameID2 fields untouched -- the game writes its TrackIR
        // identifier into GameID and OpenTrack's NPClient mirrors it to
        // GameID2; we mustn't stomp the round-trip.
        //
        // Yaw + Pitch are negated to match OpenTrack's proto-ft output
        // convention (proto-ft/ftnoir_protocol_ft.cpp::pose()): games that
        // expect freetrack signals are mostly tuned against OpenTrack's
        // sign handedness rather than the original FreeTrack server's, so
        // matching OT means working out-of-box for the typical title. The
        // per-axis invert toggles in the GUI sit on top and apply equally
        // to all outputs, so users with a game that disagrees can flip.
        FreeTrackData d;
        memset(&d, 0, sizeof(d));
        d.DataID    = ++m_frameId;
        d.CamWidth  = 800;
        d.CamHeight = 600;
        d.X     = xCm * 10.0f;     // cm -> mm
        d.Y     = yCm * 10.0f;
        d.Z     = zCm * 10.0f;
        d.Yaw   = -yawDeg   * kDegToRad;   // OT-FT convention
        d.Pitch = -pitchDeg * kDegToRad;   // OT-FT convention
        d.Roll  =  rollDeg  * kDegToRad;
        d.RawX = d.X;   d.RawY = d.Y;   d.RawZ = d.Z;
        d.RawYaw = d.Yaw; d.RawPitch = d.Pitch; d.RawRoll = d.Roll;
        memcpy(&m_view->data, &d, sizeof(d));
        return true;
    }

    inline void FreeTrackSender::SendIdentity()
    {
        if (!m_view) return;
        FreeTrackData d;
        memset(&d, 0, sizeof(d));
        d.DataID    = ++m_frameId;
        d.CamWidth  = 800;
        d.CamHeight = 600;
        memcpy(&m_view->data, &d, sizeof(d));
    }
}
