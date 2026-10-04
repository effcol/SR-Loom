// Profiles.h -- per-game auto-apply profile list.
//
// A Profile binds {foreground-window match condition} to {SR Loom settings
// to apply when that window comes forward}. The match condition is:
//   - exe basename substring (case-insensitive), and
//   - window title matched as a regex (ECMAScript, case-insensitive). Plain
//     strings still work as substring matches; power users get full regex
//     for cases like `3D SBS Image Viewer|3D.+` (browsers, media players
//     whose title depends on the file being played).
//   - optional `fullscreenOnly` gate: only apply when the target window
//     is presenting fullscreen (media player playback, browser fullscreen
//     3D viewer, etc.).
//
// Inspired by NTM's AutoCP-Launcher (github.com/NTM-3D/AutoCP-Launcher).
// v2.2 profile-schema additions were requested by NTM after v2.1 shipped
// so his 3D SBS Image Viewer, MPC-3D, VLC etc. workflows work cleanly.
//
// Storage: %LOCALAPPDATA%\SRLoom\profiles.ini -- INI-like, hand-editable.
// One [Name] section per profile, key=value pairs inside. Only fields the
// user actually customised are written (sparse serialisation) -- makes the
// file easy to hand-edit and read.
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
        std::string  title;                // window-title regex, case-insensitive ("" = any)
        bool         fullscreenOnly = false;   // only match when target window is fullscreen
        // What to apply when the match fires.
        // If useAutoFormat is true, the .format field is IGNORED at apply-
        // time; instead the current window's title is parsed for well-known
        // stereo tokens (HSBS/HTAB/SBS/TAB/_2x1/_1x2/OU/anaglyph/...) and
        // the detected format is used. If nothing matches, defaultFormat is
        // used instead. Lets one profile cover all "3D-named" files opened
        // in the same viewer/player.
        StereoFormat format         = StereoFormat::HalfSBS;
        bool         useAutoFormat  = false;
        // format=detect: SR Loom's Automatic Detection (it looks at the picture
        // itself for the layout) instead of a fixed format or the title.
        bool         useVisualAuto  = false;
        StereoFormat defaultFormat  = StereoFormat::HalfSBS;
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

        // The display's settings, where the profile gives them (-1: left as
        // they are). They are SR Loom's own settings, the ones the panel
        // shows: a profile that sets one changes it until something else does.
        int          antiCrosstalk   = -1;  // Anti-Crosstalk strength, % (0-300)
        int          crosstalkMethod = -1;  // 0 the display's default, 1 off, 2 static, 3 dynamic
        int          contrast        = -1;  // weaving contrast, % (0-200)
        int          weaver          = -1;  // 0 Direct3D 11, 1 Direct3D 12

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
        // When profiles.ini was last written (0: no file): for reading it again
        // when it is edited outside SR Loom.
        unsigned long long FileStamp();

        // Persist the list back to disk. Overwrites the file. Creates the
        // directory if missing. Logs on I/O failure. SPARSE -- writes only
        // fields that differ from Profile{}'s defaults, so hand-editing the
        // file stays tractable.
        void Save(const std::vector<Profile>& list);

        // True if exe + title match. `title` matches if it's a case-
        // insensitive substring of the window title (v2.1 semantics) OR it
        // matches as a regex (ECMAScript, case-insensitive; compiled once
        // and cached). An invalid regex just gets the substring half, so a
        // stray parenthesis doesn't brick the profile. Both patterns must
        // match; empty pattern is a wildcard.
        // NOTE: does NOT check `fullscreenOnly` -- caller checks the fullscreen
        // state itself (needs the target HWND, which this API doesn't take).
        bool Matches(const Profile& p, const std::string& exeBaseName,
                                       const std::string& windowTitle);

        // Parse a window title / filename for well-known stereo tokens.
        // Returns the detected format + sets detected=true on match; returns
        // HalfSBS + detected=false if nothing matched. Used when a profile
        // has format=auto: the detected format wins, falling back to the
        // profile's defaultFormat if this returns detected=false.
        //
        // Case-insensitive, whole-word; `.` `_` `-` and spaces are all
        // equivalent separators ("Full.SBS" == "full-sbs" == "Full_SBS").
        //   HSBS / HalfSBS / Half SBS / SBS Half     -> HalfSBS
        //   FSBS / FullSBS / Full SBS / SBS Full     -> FullSBS
        //   SBS / SideBySide / Side By Side          -> HalfSBS (streaming default)
        //   HTAB / HalfTAB / Half OU / HOU*          -> HalfTAB
        //   FTAB / FullTAB / Full OU / FOU*          -> FullTAB
        //   TopAndBottom / OverUnder / TAB* / OU*    -> HalfTAB (streaming default)
        //   FramePacking / FramePack / HDMI3D / MVC* -> FramePacking
        //   Anaglyph / RedCyan                       -> Anaglyph
        //   RowInterlaced / Interlaced*              -> RowInterleaved
        //   Checkerboard*                            -> Checkerboard
        //   Quilt* / _qs<cols>x<rows>                -> Quilt
        //   _2x1 (or 2x1*) / _1x2 (or 1x2*)          -> HalfSBS / HalfTAB (Leia)
        // * = common-word token; only counts when the title also contains
        //     "3D" / "stereo" (so Chrome's "New Tab" isn't read as TAB).
        StereoFormat DetectFormatFromTitle(const std::string& title, bool& detected);

        // Stable string IDs for StereoFormat (used in the on-disk file --
        // must stay stable across versions so existing profiles keep
        // working after an upgrade).
        const char*  FormatToString(StereoFormat f);
        StereoFormat FormatFromString(const std::string& s, bool* ok = nullptr);
    }
}
