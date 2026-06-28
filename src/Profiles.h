// Profiles.h -- per-game auto-apply profile list.
//
// A Profile binds {foreground-window match condition} to {SR Loom settings
// to apply when that window comes forward}. The match condition is a
// case-insensitive substring test on the foreground window's exe basename
// AND/OR its window title. An empty string in either field means "any".
//
// Inspired by NTM's AutoCP-Launcher (github.com/NTM-3D/AutoCP-Launcher):
// users build a list of {game -> right stereo format} once, SR Loom flips
// the format automatically when the game is in focus. Removes the manual
// "open the tray menu and pick the format every time you alt-tab" friction
// for the 3DMigoto / Geo-11 / fixer-mod audience.
//
// Storage: %LOCALAPPDATA%\SRLoom\profiles.ini -- INI-like, hand-editable.
// One [Name] section per profile, key=value pairs inside.
#pragma once

#include "Common.h"
#include <string>
#include <vector>

namespace srw
{
    struct Profile
    {
        std::string  name;                 // user-visible label
        std::string  exe;                  // basename substring, case-insensitive ("" = any)
        std::string  title;                // window-title substring, case-insensitive ("" = any)
        // What to apply when the match fires:
        StereoFormat format       = StereoFormat::FullSBS;
        bool         swapEyes     = false;
        float        convergence  = 0.0f;
        // Format-specific sub-options. Only meaningful when `format` is
        // the matching parent (e.g. anaglyph fields only apply if format
        // == Anaglyph). Saved for every profile so flipping a profile's
        // format later doesn't drop the related state.
        int          anaglyphCombo  = 0;
        int          anaglyphMode   = 4;
        int          pulfrichMode   = 0;   // 0=TimeDelay, 1=NDFilter
        int          pulfrichDelay  = 1;   // frames
        int          pulfrichNd     = 1;   // index into PulfrichNdLevels
        int          framePackMode  = 0;   // index into FramePackPresets
        int          quiltCols      = 8;
        int          quiltRows      = 6;
        int          quiltLeftIdx   = -1;  // -1 = auto (centre pair from cols*rows)
        int          quiltRightIdx  = -1;

        // Head-tracking settings -- applied only when includeHeadTracking
        // is true. Default off so legacy profiles (or freshly saved ones
        // where the user doesn't care about HT) leave HT alone on apply.
        bool         includeHeadTracking = false;
        bool         htOpenTrack   = true;
        bool         htFreeTrack   = true;
        bool         htTrackIR     = false;
        int          htOutputMode  = 1;
        bool         htInvertX     = true;
        bool         htInvertY     = true;
        bool         htInvertZ     = false;
        bool         htInvertYaw   = true;
        bool         htInvertPitch = false;
        bool         htInvertRoll  = false;
    };

    namespace Profiles
    {
        // Load the profile list from %LOCALAPPDATA%\SRLoom\profiles.ini.
        // Returns empty vector if the file doesn't exist or fails to parse.
        std::vector<Profile> Load();

        // Persist the list back to disk. Overwrites the file. Creates the
        // directory if missing. Logs on I/O failure.
        void Save(const std::vector<Profile>& list);

        // True if the given exe basename + window title match the profile's
        // patterns. Both fields case-insensitive substring; empty pattern
        // always matches.
        bool Matches(const Profile& p, const std::string& exeBaseName,
                                       const std::string& windowTitle);

        // Stable string IDs for StereoFormat (used in the on-disk file --
        // must stay stable across versions so existing profiles keep
        // working after an upgrade).
        const char*  FormatToString(StereoFormat f);
        StereoFormat FormatFromString(const std::string& s, bool* ok = nullptr);
    }
}
