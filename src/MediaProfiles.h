// MediaProfiles.h -- per-file remembered stereo settings for the media
// viewer. When the user loads an image or video, look up its path in
// media_profiles.ini; if a profile exists, apply its format/swap/
// convergence/anaglyph/etc settings automatically. While the file is
// the current source, any settings change re-saves the profile so the
// "remember settings for next time I open this file" UX just works.
//
// Storage: %LOCALAPPDATA%\SRLoom\media_profiles.ini -- INI format, the
// section name IS the full file path (case-insensitive lookup). Reuses
// the same Profile struct as the game-profile system so the same Save/
// Apply machinery works; the exe/title/HT fields are unused here.
//
// Kept in a separate file from the game profiles so the GUI's profiles
// dropdown isn't cluttered with one entry per opened photo, and so
// users can delete the lot by removing one file without losing their
// per-game profiles.
#pragma once

#include "Profiles.h"
#include <string>

namespace srw::MediaProfiles
{
    // Read profile for the given absolute file path. Returns true if a
    // matching profile was found; populates `out` with the saved settings.
    // Path comparison is case-insensitive (Windows filesystem convention).
    bool LoadFor(const std::string& path, Profile& out);

    // Persist `p` as the profile for the given path. Creates the file +
    // section if missing, overwrites the section if present. Path is
    // stored verbatim (we don't canonicalize); lookups compare
    // case-insensitive so D:\Foo.jpg and d:\foo.jpg match.
    void SaveFor(const std::string& path, const Profile& p);
}
