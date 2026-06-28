// MediaProfiles.cpp -- per-file remembered stereo settings storage.
// See MediaProfiles.h. Reuses the Profile struct + format-string
// table from Profiles.h; only the load/save shape differs (key is
// the absolute file path rather than {exe, title}).
#include "MediaProfiles.h"

#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdio>

#pragma comment(lib, "shell32.lib")

extern "C" void __cdecl Log(const char* fmt, ...);

namespace
{
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

    std::wstring MediaProfilesPath()
    {
        PWSTR base = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base)
        {
            out = base;
            CoTaskMemFree(base);
            out += L"\\SRLoom";
            SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
            out += L"\\media_profiles.ini";
        }
        return out;
    }

    // Read entire INI into a vector of (sectionName, fieldLines) blocks.
    // Comments / blanks discarded. Loader is loose -- whitespace tolerant,
    // missing fields fall back to defaults.
    struct Block { std::string name; std::vector<std::pair<std::string, std::string>> fields; };

    std::vector<Block> ReadBlocks(const std::wstring& path)
    {
        std::vector<Block> out;
        std::ifstream f(path);
        if (!f.is_open()) return out;
        std::string line;
        Block cur;
        bool inBlock = false;
        while (std::getline(f, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            if (line.front() == '[' && line.back() == ']')
            {
                if (inBlock && !cur.name.empty()) out.push_back(std::move(cur));
                cur = Block{};
                cur.name = line.substr(1, line.size() - 2);
                inBlock = true;
                continue;
            }
            if (!inBlock) continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            cur.fields.push_back({ Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)) });
        }
        if (inBlock && !cur.name.empty()) out.push_back(std::move(cur));
        return out;
    }

    // Apply a key=value pair to the Profile fields we care about for
    // media profiles. Same key set as Profiles.cpp's Load() so users
    // can hand-copy fields between the two files if they want, minus
    // the exe/title/ht fields which don't apply here.
    void ApplyKey(srw::Profile& p, const std::string& key, const std::string& val)
    {
        auto toBool = [](const std::string& v) { return v == "1" || v == "true"; };
        auto toInt  = [](const std::string& v) { return std::atoi(v.c_str()); };
        if      (key == "format")          p.format = srw::Profiles::FormatFromString(val);
        else if (key == "swap_eyes")       p.swapEyes = toBool(val);
        else if (key == "convergence")     p.convergence = (float)std::atof(val.c_str());
        else if (key == "anaglyph_combo")  p.anaglyphCombo = toInt(val);
        else if (key == "anaglyph_mode")   p.anaglyphMode  = toInt(val);
        else if (key == "pulfrich_mode")   p.pulfrichMode  = toInt(val);
        else if (key == "pulfrich_delay")  p.pulfrichDelay = toInt(val);
        else if (key == "pulfrich_nd")     p.pulfrichNd    = toInt(val);
        else if (key == "frame_pack_mode") p.framePackMode = toInt(val);
        else if (key == "quilt_cols")      p.quiltCols     = toInt(val);
        else if (key == "quilt_rows")      p.quiltRows     = toInt(val);
        else if (key == "quilt_left")      p.quiltLeftIdx  = toInt(val);
        else if (key == "quilt_right")     p.quiltRightIdx = toInt(val);
    }

    void WriteBlock(std::ofstream& f, const std::string& path, const srw::Profile& p)
    {
        char conv[32]; std::snprintf(conv, sizeof(conv), "%.3f", p.convergence);
        f << "[" << path << "]\n";
        f << "format="          << srw::Profiles::FormatToString(p.format) << "\n";
        f << "swap_eyes="       << (p.swapEyes ? "1" : "0") << "\n";
        f << "convergence="     << conv << "\n";
        f << "anaglyph_combo="  << p.anaglyphCombo << "\n";
        f << "anaglyph_mode="   << p.anaglyphMode  << "\n";
        f << "pulfrich_mode="   << p.pulfrichMode  << "\n";
        f << "pulfrich_delay="  << p.pulfrichDelay << "\n";
        f << "pulfrich_nd="     << p.pulfrichNd    << "\n";
        f << "frame_pack_mode=" << p.framePackMode << "\n";
        f << "quilt_cols="      << p.quiltCols     << "\n";
        f << "quilt_rows="      << p.quiltRows     << "\n";
        f << "quilt_left="      << p.quiltLeftIdx  << "\n";
        f << "quilt_right="     << p.quiltRightIdx << "\n\n";
    }
}

namespace srw::MediaProfiles
{
    bool LoadFor(const std::string& path, Profile& out)
    {
        if (path.empty()) return false;
        const auto blocks = ReadBlocks(MediaProfilesPath());
        const std::string key = ToLower(path);
        for (const auto& b : blocks)
        {
            if (ToLower(b.name) == key)
            {
                out = Profile{};   // start from defaults, then overlay
                for (const auto& kv : b.fields) ApplyKey(out, kv.first, kv.second);
                Log("MediaProfiles::LoadFor: hit '%s'", path.c_str());
                return true;
            }
        }
        return false;
    }

    void SaveFor(const std::string& path, const Profile& p)
    {
        if (path.empty()) return;
        const std::wstring iniPath = MediaProfilesPath();
        if (iniPath.empty()) return;

        // Read existing, replace-or-append our entry, write back. Keeps
        // the file in roughly the user's order without N^2 lookups.
        auto blocks = ReadBlocks(iniPath);
        const std::string key = ToLower(path);
        bool replaced = false;
        for (auto& b : blocks)
        {
            if (ToLower(b.name) == key)
            {
                b.name = path;          // preserve the freshest casing the user passed
                b.fields.clear();       // we'll re-emit via WriteBlock below
                replaced = true;
                break;
            }
        }

        std::ofstream f(iniPath, std::ios::trunc);
        if (!f.is_open()) { Log("MediaProfiles::SaveFor: open failed"); return; }
        f << "# SR Loom media profiles. Each [path] section remembers the\n"
             "# stereo settings last used for that image / video. Updated\n"
             "# automatically when the file is the current source and you\n"
             "# change format / swap eyes / convergence / anaglyph fields.\n"
             "# Path-match is case-insensitive (Windows filesystem rules).\n\n";
        for (const auto& b : blocks)
        {
            if (ToLower(b.name) == key) continue;   // skip the old version
            // Replay verbatim fields so we don't lose any user-edited
            // values for entries we're not touching.
            f << "[" << b.name << "]\n";
            for (const auto& kv : b.fields) f << kv.first << "=" << kv.second << "\n";
            f << "\n";
        }
        // Append (or re-write) ours at the end, fresh.
        WriteBlock(f, path, p);
        (void)replaced;
        Log("MediaProfiles::SaveFor: wrote profile for '%s'", path.c_str());
    }
}
