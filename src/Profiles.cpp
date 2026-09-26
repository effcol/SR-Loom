// Profiles.cpp -- INI-like loader/saver + matching helpers for the
// per-game auto-apply profile list. See Profiles.h.
#include "Profiles.h"

#include <windows.h>
#include <shlobj.h>     // SHGetKnownFolderPath, SHCreateDirectoryExW
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <regex>
#include <unordered_map>

#pragma comment(lib, "shell32.lib")

namespace
{
    // Format <-> stable string ID. Values must NOT change between versions
    // (they're written to user profile files). Add new formats at the
    // bottom; don't reorder, don't rename existing entries.
    struct FmtEntry { srw::StereoFormat fmt; const char* id; };
    constexpr FmtEntry kFmtTable[] = {
        { srw::StereoFormat::FullSBS,           "FullSBS"           },
        { srw::StereoFormat::HalfSBS,           "HalfSBS"           },
        { srw::StereoFormat::FullTAB,           "FullTAB"           },
        { srw::StereoFormat::HalfTAB,           "HalfTAB"           },
        { srw::StereoFormat::Anaglyph,          "Anaglyph"          },
        { srw::StereoFormat::RowInterleaved,    "RowInterleaved"    },
        { srw::StereoFormat::ColumnInterleaved, "ColumnInterleaved" },
        { srw::StereoFormat::Checkerboard,      "Checkerboard"      },
        { srw::StereoFormat::FrameSequential,   "FrameSequential"   },
        { srw::StereoFormat::Pulfrich,          "Pulfrich"          },
        { srw::StereoFormat::FramePacking,      "FramePacking"      },
        { srw::StereoFormat::Quilt,             "Quilt"             },
        { srw::StereoFormat::VR180TAB,          "VR180TAB"          },
        { srw::StereoFormat::VR180SBS,          "VR180SBS"          },
        { srw::StereoFormat::VR360TAB,          "VR360TAB"          },
        { srw::StereoFormat::VR360SBS,          "VR360SBS"          },
        { srw::StereoFormat::Katanga,           "Katanga"           },
        { srw::StereoFormat::LightField,        "LightField"        },
    };

    std::string ToLower(std::string s)
    {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string Trim(std::string s)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    bool ContainsI(const std::string& haystack, const std::string& needle)
    {
        if (needle.empty()) return true;
        const std::string h = ToLower(haystack);
        const std::string n = ToLower(needle);
        return h.find(n) != std::string::npos;
    }

    // Compiled-regex cache keyed by pattern string. Matches() runs from the
    // 250ms fullscreen poll as well as the foreground hook, so compiling
    // per call would be wasteful. nullptr entry = pattern failed to compile.
    // Main-thread only (both callers run on the message loop thread), so
    // no locking. Bounded by the number of distinct title patterns; the
    // clear-at-64 just stops it growing forever as the user edits patterns.
    const std::regex* CachedRegex(const std::string& pattern)
    {
        static std::unordered_map<std::string, std::unique_ptr<std::regex>> cache;
        auto it = cache.find(pattern);
        if (it != cache.end()) return it->second.get();
        if (cache.size() >= 64) cache.clear();
        std::unique_ptr<std::regex> re;
        try
        {
            re = std::make_unique<std::regex>(pattern,
                std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        }
        catch (const std::regex_error&)
        {
            srw::Log("Profiles: title pattern '%s' is not a valid regex -- substring match only",
                pattern.c_str());
        }
        return cache.emplace(pattern, std::move(re)).first->second.get();
    }

    // Title match for the profile title= field. A case-insensitive
    // SUBSTRING match always counts -- that's the v2.1 semantics, and
    // literal titles like "Game (DX11)" or "Skyrim [SE]" are valid regexes
    // that would otherwise stop matching themselves. On top of that, the
    // pattern is tried as a regex so power users get `.+3D.+` or
    // `3D SBS Image Viewer|3D.+`. Malformed patterns just get the
    // substring half, so a typo doesn't brick the profile.
    bool RegexOrSubstringMatch(const std::string& pattern, const std::string& title)
    {
        if (pattern.empty()) return true;
        if (ContainsI(title, pattern)) return true;
        const std::regex* re = CachedRegex(pattern);
        if (!re) return false;
        try
        {
            return std::regex_search(title, *re);
        }
        catch (const std::regex_error&)
        {
            // error_complexity / error_stack on a pathological pattern.
            return false;
        }
    }

    // Lowercase + collapse every run of non-alphanumeric characters into a
    // single space, padded with a space at both ends. "Movie.Full_SBS-3D.mkv"
    // -> " movie full sbs 3d mkv ". Lets one token list cover dots,
    // underscores, hyphens and spaces as separators, and makes whole-word
    // matching a plain find(" word ").
    std::string NormaliseTitle(const std::string& s)
    {
        std::string out = " ";
        for (unsigned char c : s)
        {
            if (std::isalnum(c) && c < 0x80)
                out += (char)std::tolower(c);
            else if (out.back() != ' ')
                out += ' ';
        }
        if (out.back() != ' ') out += ' ';
        return out;
    }

    // True if `phrase` (space-separated lowercase words) appears as whole
    // words in a NormaliseTitle()'d string.
    bool HasPhrase(const std::string& norm, const char* phrase)
    {
        const std::string needle = std::string(" ") + phrase + " ";
        return norm.find(needle) != std::string::npos;
    }

    // Looking Glass quilt naming convention: "_qs<cols>x<rows>[a<aspect>]",
    // e.g. "photo_qs8x6a0.75.png". After normalisation that's a word
    // starting "qs<digits>x<digits>".
    bool HasQuiltSuffix(const std::string& norm)
    {
        for (size_t i = norm.find(" qs"); i != std::string::npos; i = norm.find(" qs", i + 1))
        {
            size_t j = i + 3;
            const size_t d1 = j;
            while (j < norm.size() && std::isdigit((unsigned char)norm[j])) ++j;
            if (j == d1 || j >= norm.size() || norm[j] != 'x') continue;
            const size_t d2 = ++j;
            while (j < norm.size() && std::isdigit((unsigned char)norm[j])) ++j;
            if (j > d2) return true;
        }
        return false;
    }

    std::wstring ProfilesPath()
    {
        PWSTR base = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
        {
            out = base;
            CoTaskMemFree(base);
            out += L"\\SRLoom";
            SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
            out += L"\\profiles.ini";
        }
        return out;
    }
}

namespace srw::Profiles
{
    const char* FormatToString(StereoFormat f)
    {
        for (const auto& e : kFmtTable)
            if (e.fmt == f) return e.id;
        return "FullSBS";   // safe default
    }

    StereoFormat FormatFromString(const std::string& s, bool* ok)
    {
        for (const auto& e : kFmtTable)
            if (s == e.id) { if (ok) *ok = true; return e.fmt; }
        if (ok) *ok = false;
        return StereoFormat::FullSBS;
    }

    bool Matches(const Profile& p, const std::string& exeBaseName,
                                   const std::string& windowTitle)
    {
        // Refuse the all-wildcard profile (would auto-apply to everything,
        // user almost certainly didn't mean that).
        if (p.exe.empty() && p.title.empty()) return false;
        // exe stays case-insensitive substring (exe basenames are short and
        // regex would just be noise). title is regex-with-substring-fallback
        // to support NTM's `.+3D.+` / `Foo|Bar.+` patterns.
        return ContainsI(exeBaseName, p.exe) &&
               RegexOrSubstringMatch(p.title, windowTitle);
    }

    StereoFormat DetectFormatFromTitle(const std::string& title, bool& detected)
    {
        detected = false;
        if (title.empty()) return StereoFormat::HalfSBS;
        // Whole-word matching on a normalised title: dots, underscores,
        // hyphens and spaces are all equivalent separators, so
        // "Movie.Full.SBS.mkv", "Full_SBS" and "full-sbs" all read as the
        // words "full sbs". Word-bounded, so "tab" never hits inside
        // "table" and "ou" never hits inside "you".
        const std::string norm = NormaliseTitle(title);

        // Short / common-word tokens ("tab" as in Chrome's "New Tab", "ou"
        // is French for "or", "mvc" as in ASP.NET MVC, ...) only count when
        // the title also says it's 3D content. Strong tokens (HSBS,
        // "full sbs", anaglyph, ...) are unambiguous on their own.
        const bool has3DContext = HasPhrase(norm, "3d") || HasPhrase(norm, "stereo") ||
                                  HasPhrase(norm, "stereoscopic");

        // Order matters: MORE SPECIFIC phrases before their broad forms
        // ("full sbs" before "sbs", "half ou" before "ou").
        struct Tok { const char* s; StereoFormat f; bool weak; };
        static const Tok kTokens[] = {
            // Half-SBS
            { "hsbs",            StereoFormat::HalfSBS,  false },
            { "halfsbs",         StereoFormat::HalfSBS,  false },
            { "half sbs",        StereoFormat::HalfSBS,  false },
            { "h sbs",           StereoFormat::HalfSBS,  false },
            { "sbs half",        StereoFormat::HalfSBS,  false },
            // Full-SBS
            { "fsbs",            StereoFormat::FullSBS,  false },
            { "fullsbs",         StereoFormat::FullSBS,  false },
            { "full sbs",        StereoFormat::FullSBS,  false },
            { "f sbs",           StereoFormat::FullSBS,  false },
            { "sbs full",        StereoFormat::FullSBS,  false },
            // Half-TAB / Half-OU
            { "htab",            StereoFormat::HalfTAB,  false },
            { "halftab",         StereoFormat::HalfTAB,  false },
            { "half tab",        StereoFormat::HalfTAB,  false },
            { "h tab",           StereoFormat::HalfTAB,  false },
            { "tab half",        StereoFormat::HalfTAB,  false },
            { "half ou",         StereoFormat::HalfTAB,  false },
            { "halfou",          StereoFormat::HalfTAB,  false },
            { "h ou",            StereoFormat::HalfTAB,  false },
            { "ou half",         StereoFormat::HalfTAB,  false },
            { "hou",             StereoFormat::HalfTAB,  true  },
            // Full-TAB / Full-OU
            { "ftab",            StereoFormat::FullTAB,  false },
            { "fulltab",         StereoFormat::FullTAB,  false },
            { "full tab",        StereoFormat::FullTAB,  false },
            { "f tab",           StereoFormat::FullTAB,  false },
            { "tab full",        StereoFormat::FullTAB,  false },
            { "full ou",         StereoFormat::FullTAB,  false },
            { "fullou",          StereoFormat::FullTAB,  false },
            { "f ou",            StereoFormat::FullTAB,  false },
            { "ou full",         StereoFormat::FullTAB,  false },
            { "fou",             StereoFormat::FullTAB,  true  },
            // Frame packing
            { "framepacking",    StereoFormat::FramePacking, false },
            { "framepack",       StereoFormat::FramePacking, false },
            { "frame packing",   StereoFormat::FramePacking, false },
            { "frame pack",      StereoFormat::FramePacking, false },
            { "hdmi3d",          StereoFormat::FramePacking, false },
            { "hdmi 3d",         StereoFormat::FramePacking, false },
            { "mvc",             StereoFormat::FramePacking, true  },
            // Anaglyph
            { "anaglyph",        StereoFormat::Anaglyph, false },
            { "redcyan",         StereoFormat::Anaglyph, false },
            { "red cyan",        StereoFormat::Anaglyph, false },
            // Interleaved / checkerboard
            { "rowinterlaced",   StereoFormat::RowInterleaved, false },
            { "row interlaced",  StereoFormat::RowInterleaved, false },
            { "interlaced",      StereoFormat::RowInterleaved, true  },
            { "checkerboard",    StereoFormat::Checkerboard,   true  },
            // Quilt ("_qs8x6a0.75" suffix handled separately below)
            { "quilt",           StereoFormat::Quilt,    true  },
            // Broad SBS/TAB/OU aliases -- LAST. Streaming/YouTube 3D almost
            // always means Half-SBS / Half-TAB even when the filename just
            // says "SBS" or "OU"; full versions are the ones that need to
            // be explicit ("FullSBS"/"FSBS"). This matches NTM's example
            // (movie.3D.HSBS -> HalfSBS, movie.3D -> defaultformat).
            { "sbs",             StereoFormat::HalfSBS,  false },
            { "sidebyside",      StereoFormat::HalfSBS,  false },
            { "side by side",    StereoFormat::HalfSBS,  false },
            { "topandbottom",    StereoFormat::HalfTAB,  false },
            { "top and bottom",  StereoFormat::HalfTAB,  false },
            { "overunder",       StereoFormat::HalfTAB,  false },
            { "over under",      StereoFormat::HalfTAB,  false },
            { "tab",             StereoFormat::HalfTAB,  true  },
            { "ou",              StereoFormat::HalfTAB,  true  },
            { "2x1",             StereoFormat::HalfSBS,  true  },
            { "1x2",             StereoFormat::HalfTAB,  true  },
        };
        for (const auto& tok : kTokens)
        {
            if (tok.weak && !has3DContext) continue;
            if (HasPhrase(norm, tok.s))
            {
                detected = true;
                return tok.f;
            }
        }

        // Leia's "_2x1" / "_1x2" filename suffixes are unambiguous with the
        // underscore even without a "3D" word (e.g. "photo_2x1.jpg"). The
        // underscore is lost in normalisation, so check the raw title.
        const std::string lower = ToLower(title);
        auto hasSuffixToken = [&](const char* tok) {
            const size_t n = std::strlen(tok);
            for (size_t i = lower.find(tok); i != std::string::npos; i = lower.find(tok, i + 1))
            {
                const size_t end = i + n;
                if (end == lower.size() || !std::isalnum((unsigned char)lower[end]))
                    return true;
            }
            return false;
        };
        if (hasSuffixToken("_2x1")) { detected = true; return StereoFormat::HalfSBS; }
        if (hasSuffixToken("_1x2")) { detected = true; return StereoFormat::HalfTAB; }
        if (HasQuiltSuffix(norm))   { detected = true; return StereoFormat::Quilt;   }

        return StereoFormat::HalfSBS;   // safe default; caller sees detected=false
    }

    std::vector<Profile> Load()
    {
        std::vector<Profile> out;
        const std::wstring path = ProfilesPath();
        if (path.empty()) return out;

        std::ifstream f(path);
        if (!f.is_open()) return out;

        Profile cur;
        bool inProfile = false;
        std::string line;
        while (std::getline(f, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            if (line.front() == '[' && line.back() == ']')
            {
                // New section -- flush previous if it had a name + any match
                if (inProfile && !cur.name.empty())
                    out.push_back(cur);
                cur = Profile{};
                cur.name = line.substr(1, line.size() - 2);
                inProfile = true;
                continue;
            }

            if (!inProfile) continue;

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq));
            const std::string val = Trim(line.substr(eq + 1));
            auto toBool = [](const std::string& v) { return v == "1" || v == "true"; };
            auto toInt  = [](const std::string& v) { return std::atoi(v.c_str()); };
            if (key == "exe")                       cur.exe = val;
            else if (key == "title")                cur.title = val;
            else if (key == "fullscreen")           cur.fullscreenOnly = toBool(val);
            else if (key == "format")
            {
                if (val == "auto" || val == "Auto" || val == "AUTO")
                    cur.useAutoFormat = true;
                else
                    cur.format = FormatFromString(val);
            }
            else if (key == "defaultformat" || key == "default_format")
                cur.defaultFormat = FormatFromString(val);
            else if (key == "swap_eyes")            cur.swapEyes = toBool(val);
            else if (key == "convergence")          cur.convergence = (float)std::atof(val.c_str());
            else if (key == "anaglyph_combo")       cur.anaglyphCombo = toInt(val);
            else if (key == "anaglyph_mode")        cur.anaglyphMode  = toInt(val);
            else if (key == "pulfrich_mode")        cur.pulfrichMode  = toInt(val);
            else if (key == "pulfrich_delay")       cur.pulfrichDelay = toInt(val);
            else if (key == "pulfrich_nd")          cur.pulfrichNd    = toInt(val);
            else if (key == "frame_pack_mode")      cur.framePackMode = toInt(val);
            else if (key == "quilt_cols")           cur.quiltCols     = toInt(val);
            else if (key == "quilt_rows")           cur.quiltRows     = toInt(val);
            else if (key == "quilt_left")           cur.quiltLeftIdx  = toInt(val);
            else if (key == "quilt_right")          cur.quiltRightIdx = toInt(val);
            else if (key == "include_head_tracking") cur.includeHeadTracking = toBool(val);
            else if (key == "ht_opentrack")         cur.htOpenTrack   = toBool(val);
            else if (key == "ht_freetrack")         cur.htFreeTrack   = toBool(val);
            else if (key == "ht_trackir")           cur.htTrackIR     = toBool(val);
            else if (key == "ht_output_mode")       cur.htOutputMode  = toInt(val);
            else if (key == "ht_invert_x")          cur.htInvertX     = toBool(val);
            else if (key == "ht_invert_y")          cur.htInvertY     = toBool(val);
            else if (key == "ht_invert_z")          cur.htInvertZ     = toBool(val);
            else if (key == "ht_invert_yaw")        cur.htInvertYaw   = toBool(val);
            else if (key == "ht_invert_pitch")      cur.htInvertPitch = toBool(val);
            else if (key == "ht_invert_roll")       cur.htInvertRoll  = toBool(val);
        }
        if (inProfile && !cur.name.empty()) out.push_back(cur);
        // format=auto profiles don't store a fixed format; keep `format`
        // in step with the fallback so turning Auto off in the GUI (or
        // reading p.format anywhere) yields the user's format, not the
        // struct default.
        for (auto& p : out)
            if (p.useAutoFormat) p.format = p.defaultFormat;
        Log("Profiles::Load: %zu profile(s)", out.size());
        return out;
    }

    void Save(const std::vector<Profile>& list)
    {
        const std::wstring path = ProfilesPath();
        if (path.empty()) { Log("Profiles::Save: no AppData path"); return; }

        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) { Log("Profiles::Save: failed to open file for write"); return; }

        f << "# SR Loom profile list. Hand-editable.\n"
             "#\n"
             "# Each [Name] section is one profile. Match keys:\n"
             "#   exe=       case-insensitive substring of the foreground window's\n"
             "#              exe basename (\"vlc.exe\", \"chrome.exe\", etc.).\n"
             "#   title=     window-title regex (ECMAScript, case-insensitive).\n"
             "#              Plain strings still work as substring matches. Examples:\n"
             "#                title=.+3D.+                       any title containing \"3D\"\n"
             "#                title=3D SBS Image Viewer|3D.+     either of two patterns\n"
             "#              Leave blank to match any title.\n"
             "#   fullscreen=1  only apply when the target window is presenting\n"
             "#                 fullscreen (media playback, browser fullscreen 3D,\n"
             "#                 etc.). Weave turns off again when it leaves\n"
             "#                 fullscreen. Default 0 = apply on any focus.\n"
             "#\n"
             "# Format keys:\n"
             "#   format=auto                    detect from title (HSBS, HTAB, _2x1,\n"
             "#                                  MVC, anaglyph, etc.); fall back to\n"
             "#                                  defaultformat if nothing recognised.\n"
             "#   format=<id>                    always use this format for this profile.\n"
             "#   defaultformat=<id>             only used when format=auto and no token\n"
             "#                                  is recognised in the current title.\n"
             "# Format ids: FullSBS HalfSBS FullTAB HalfTAB Anaglyph\n"
             "#   RowInterleaved ColumnInterleaved Checkerboard FrameSequential\n"
             "#   Pulfrich FramePacking Quilt VR180TAB VR180SBS VR360TAB VR360SBS\n"
             "#   LightField\n"
             "#\n"
             "# Only fields you customised are written. Unset fields use SR Loom's\n"
             "# defaults. Order inside a section doesn't matter.\n"
             "#\n"
             "# include_head_tracking=1 makes the profile also re-apply the\n"
             "# OpenTrack / FreeTrack / TrackIR + output-mode + per-axis invert\n"
             "# state stored in the ht_* fields below. Default 0 leaves head\n"
             "# tracking alone.\n\n";

        // Sparse writer: emit only fields that differ from Profile{} defaults.
        // Keeps the file readable + matches NTM's compact example style. The
        // header is required (name) + exe is essentially required (title-only
        // profiles work but are unusual, so always emit exe even if blank).
        const Profile def{};
        for (const auto& p : list)
        {
            f << "[" << p.name << "]\n";
            f << "exe=" << p.exe << "\n";
            if (!p.title.empty())       f << "title=" << p.title << "\n";
            if (p.fullscreenOnly)       f << "fullscreen=1\n";
            // Auto-format profiles always spell out defaultformat: it's the
            // only record of the user's fixed format while Auto is on (the
            // toggle seeds it from `format`, and turning Auto off restores
            // `format` from it), so keep it visible in the file.
            if (p.useAutoFormat)
            {
                f << "format=auto\n";
                f << "defaultformat=" << FormatToString(p.defaultFormat) << "\n";
            }
            else if (p.format != def.format)
                f << "format=" << FormatToString(p.format) << "\n";
            if (p.swapEyes)             f << "swap_eyes=1\n";
            if (p.convergence != 0.0f)
            {
                char conv[32]; std::snprintf(conv, sizeof(conv), "%.3f", p.convergence);
                f << "convergence=" << conv << "\n";
            }
            if (p.anaglyphCombo  != def.anaglyphCombo)  f << "anaglyph_combo="  << p.anaglyphCombo  << "\n";
            if (p.anaglyphMode   != def.anaglyphMode)   f << "anaglyph_mode="   << p.anaglyphMode   << "\n";
            if (p.pulfrichMode   != def.pulfrichMode)   f << "pulfrich_mode="   << p.pulfrichMode   << "\n";
            if (p.pulfrichDelay  != def.pulfrichDelay)  f << "pulfrich_delay="  << p.pulfrichDelay  << "\n";
            if (p.pulfrichNd     != def.pulfrichNd)     f << "pulfrich_nd="     << p.pulfrichNd     << "\n";
            if (p.framePackMode  != def.framePackMode)  f << "frame_pack_mode=" << p.framePackMode  << "\n";
            if (p.quiltCols      != def.quiltCols)      f << "quilt_cols="      << p.quiltCols      << "\n";
            if (p.quiltRows      != def.quiltRows)      f << "quilt_rows="      << p.quiltRows      << "\n";
            if (p.quiltLeftIdx   != def.quiltLeftIdx)   f << "quilt_left="      << p.quiltLeftIdx   << "\n";
            if (p.quiltRightIdx  != def.quiltRightIdx)  f << "quilt_right="     << p.quiltRightIdx  << "\n";
            if (p.includeHeadTracking)
            {
                f << "include_head_tracking=1\n";
                if (p.htOpenTrack   != def.htOpenTrack)   f << "ht_opentrack="    << (p.htOpenTrack ? "1" : "0") << "\n";
                if (p.htFreeTrack   != def.htFreeTrack)   f << "ht_freetrack="    << (p.htFreeTrack ? "1" : "0") << "\n";
                if (p.htTrackIR     != def.htTrackIR)     f << "ht_trackir="      << (p.htTrackIR   ? "1" : "0") << "\n";
                if (p.htOutputMode  != def.htOutputMode)  f << "ht_output_mode="  << p.htOutputMode << "\n";
                if (p.htInvertX     != def.htInvertX)     f << "ht_invert_x="     << (p.htInvertX     ? "1" : "0") << "\n";
                if (p.htInvertY     != def.htInvertY)     f << "ht_invert_y="     << (p.htInvertY     ? "1" : "0") << "\n";
                if (p.htInvertZ     != def.htInvertZ)     f << "ht_invert_z="     << (p.htInvertZ     ? "1" : "0") << "\n";
                if (p.htInvertYaw   != def.htInvertYaw)   f << "ht_invert_yaw="   << (p.htInvertYaw   ? "1" : "0") << "\n";
                if (p.htInvertPitch != def.htInvertPitch) f << "ht_invert_pitch=" << (p.htInvertPitch ? "1" : "0") << "\n";
                if (p.htInvertRoll  != def.htInvertRoll)  f << "ht_invert_roll="  << (p.htInvertRoll  ? "1" : "0") << "\n";
            }
            f << "\n";
        }

        // Reference template at the bottom -- commented out so the parser
        // skips it. Anyone hand-editing can copy-paste, uncomment, and
        // tweak. Two examples reflect the two common shapes.
        f << "# ----------------------------------------------------------\n"
             "# Example 1: media player + browser, auto-detect format from title\n"
             "# with a fallback default, only weave when fullscreen.\n"
             "#\n"
             "# [VLC]\n"
             "# exe=vlc.exe\n"
             "# title=.+3D.+\n"
             "# fullscreen=1\n"
             "# format=auto\n"
             "# defaultformat=HalfSBS\n"
             "#\n"
             "# [Chrome 3D Image Viewer]\n"
             "# exe=chrome.exe\n"
             "# title=3D SBS Image Viewer|3D.+\n"
             "# fullscreen=1\n"
             "# format=HalfSBS\n"
             "#\n"
             "# Example 2: game with fixed format + swap + convergence tweak,\n"
             "# apply on any focus (not just fullscreen).\n"
             "#\n"
             "# [My Game]\n"
             "# exe=mygame.exe\n"
             "# format=HalfSBS\n"
             "# swap_eyes=1\n"
             "# convergence=-0.150\n";

        Log("Profiles::Save: wrote %zu profile(s)", list.size());
    }
}
