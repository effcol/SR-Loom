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

#pragma comment(lib, "shell32.lib")

extern "C" void __cdecl Log(const char* fmt, ...);   // from Common.h's logger

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
        // Both patterns must match if non-empty. Empty pattern = wildcard.
        // Refuse the all-wildcard profile (would auto-apply to everything,
        // user almost certainly didn't mean that).
        if (p.exe.empty() && p.title.empty()) return false;
        return ContainsI(exeBaseName, p.exe) && ContainsI(windowTitle, p.title);
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
            else if (key == "format")               cur.format = FormatFromString(val);
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
             "# Each [Name] section is one profile. Match keys: exe and title --\n"
             "# both case-insensitive substrings of the foreground window's exe\n"
             "# basename and window title respectively. Empty match key = wildcard.\n"
             "# title= is normally left blank (= match any title for that exe); set\n"
             "# it only when you need two profiles for the same exe that differ by\n"
             "# the window title (e.g. a launcher window vs the game window).\n"
             "#\n"
             "# Format ids: FullSBS HalfSBS FullTAB HalfTAB Anaglyph\n"
             "#   RowInterleaved ColumnInterleaved Checkerboard FrameSequential\n"
             "#   Pulfrich FramePacking Quilt VR180TAB VR180SBS VR360TAB VR360SBS\n"
             "#   LightField\n"
             "#\n"
             "# include_head_tracking=1 makes the profile also re-apply the\n"
             "# OpenTrack / FreeTrack / TrackIR + output-mode + per-axis invert\n"
             "# state stored in the ht_* fields below. =0 leaves head tracking\n"
             "# alone (use the global Head Tracking section's state).\n\n";

        for (const auto& p : list)
        {
            f << "[" << p.name << "]\n";
            f << "exe="         << p.exe   << "\n";
            f << "title="       << p.title << "\n";
            f << "format="      << FormatToString(p.format) << "\n";
            f << "swap_eyes="   << (p.swapEyes ? "1" : "0") << "\n";
            char conv[32]; std::snprintf(conv, sizeof(conv), "%.3f", p.convergence);
            f << "convergence=" << conv << "\n";
            f << "anaglyph_combo="  << p.anaglyphCombo  << "\n";
            f << "anaglyph_mode="   << p.anaglyphMode   << "\n";
            f << "pulfrich_mode="   << p.pulfrichMode   << "\n";
            f << "pulfrich_delay="  << p.pulfrichDelay  << "\n";
            f << "pulfrich_nd="     << p.pulfrichNd     << "\n";
            f << "frame_pack_mode=" << p.framePackMode  << "\n";
            f << "quilt_cols="      << p.quiltCols      << "\n";
            f << "quilt_rows="      << p.quiltRows      << "\n";
            f << "quilt_left="      << p.quiltLeftIdx   << "\n";
            f << "quilt_right="     << p.quiltRightIdx  << "\n";
            f << "include_head_tracking=" << (p.includeHeadTracking ? "1" : "0") << "\n";
            f << "ht_opentrack="    << (p.htOpenTrack ? "1" : "0") << "\n";
            f << "ht_freetrack="    << (p.htFreeTrack ? "1" : "0") << "\n";
            f << "ht_trackir="      << (p.htTrackIR   ? "1" : "0") << "\n";
            f << "ht_output_mode="  << p.htOutputMode << "\n";
            f << "ht_invert_x="     << (p.htInvertX     ? "1" : "0") << "\n";
            f << "ht_invert_y="     << (p.htInvertY     ? "1" : "0") << "\n";
            f << "ht_invert_z="     << (p.htInvertZ     ? "1" : "0") << "\n";
            f << "ht_invert_yaw="   << (p.htInvertYaw   ? "1" : "0") << "\n";
            f << "ht_invert_pitch=" << (p.htInvertPitch ? "1" : "0") << "\n";
            f << "ht_invert_roll="  << (p.htInvertRoll  ? "1" : "0") << "\n\n";
        }

        // Reference template at the bottom -- commented out so the parser
        // skips it. Anyone hand-editing can copy-paste, uncomment, and
        // tweak. Documents every supported field in one block.
        f << "# ----------------------------------------------------------\n"
             "# Manual profile template (uncomment + edit):\n"
             "#\n"
             "# [My Game]\n"
             "# exe=mygame.exe                  ; case-insensitive substring of the exe basename\n"
             "# title=                          ; window-title substring; leave blank for any title\n"
             "# format=HalfSBS                  ; stereo input format the game outputs\n"
             "# swap_eyes=0                     ; 1 if the game has L/R reversed\n"
             "# convergence=0.000               ; -1..1, zero-plane shift\n"
             "# anaglyph_combo=0                ; if format=Anaglyph, colour combo index\n"
             "# anaglyph_mode=4                 ; if format=Anaglyph, decode mode index\n"
             "# pulfrich_mode=0                 ; if format=Pulfrich, 0=time delay, 1=ND filter\n"
             "# pulfrich_delay=1                ; frames\n"
             "# pulfrich_nd=1                   ; ND-filter level index\n"
             "# frame_pack_mode=0               ; if format=FramePacking, preset index\n"
             "# include_head_tracking=0         ; 1 to also re-apply the ht_* fields below\n"
             "# ht_opentrack=1                  ; ht_* only matter when include_head_tracking=1\n"
             "# ht_freetrack=1\n"
             "# ht_trackir=0\n"
             "# ht_output_mode=1                ; 1=XYZ+Yaw+Pitch  2=XYZ  3=Y+P  4=6DOF  5=Y+P+R\n"
             "# ht_invert_x=1\n"
             "# ht_invert_y=1\n"
             "# ht_invert_z=0\n"
             "# ht_invert_yaw=1\n"
             "# ht_invert_pitch=0\n"
             "# ht_invert_roll=0\n";

        Log("Profiles::Save: wrote %zu profile(s)", list.size());
    }
}
