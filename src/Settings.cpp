// Settings.cpp — HKCU registry helpers. Two stores:
//   1. SR Loom's own settings under HKCU\Software\SRLoom (DWORDs)
//   2. The Windows-standard auto-run list under HKCU\Software\Microsoft\Windows
//      \CurrentVersion\Run, which the shell reads at login to launch entries
//      (per-user, no admin required).
#include "Settings.h"

#include <windows.h>   // WIN32_LEAN_AND_MEAN is set project-wide via CMakeLists.txt
#include <string>

namespace
{
    constexpr wchar_t kRunKey[]          = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kSettingsKey[]     = L"Software\\SRLoom";
    constexpr wchar_t kSrLoomValue[]     = L"SRLoom";
    constexpr wchar_t kStartInTrayValue[] = L"StartInTray";
    constexpr wchar_t kAutoApplyProfilesValue[]   = L"AutoApplyProfiles";
    constexpr wchar_t kHeadTrackingOnStartupValue[] = L"HeadTrackingOnStartup";

    DWORD ReadDword(const wchar_t* path, const wchar_t* name, DWORD defaultValue)
    {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &h) != ERROR_SUCCESS)
            return defaultValue;
        DWORD v = defaultValue;
        DWORD sz = sizeof(v);
        DWORD type = REG_DWORD;
        if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &sz)
                != ERROR_SUCCESS || type != REG_DWORD)
            v = defaultValue;
        RegCloseKey(h);
        return v;
    }

    void WriteDword(const wchar_t* path, const wchar_t* name, DWORD value)
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            return;
        RegSetValueExW(h, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(h);
    }
}

namespace srw::Settings
{
    bool ReadRunAtStartup()
    {
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &h) != ERROR_SUCCESS)
            return false;
        const LSTATUS s = RegQueryValueExW(h, kSrLoomValue, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(h);
        return s == ERROR_SUCCESS;
    }

    void WriteRunAtStartup(bool enable)
    {
        if (enable)
        {
            // Quote the path so the shell parses spaces correctly on launch.
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len >= MAX_PATH) return;
            std::wstring quoted; quoted.reserve(len + 3);
            quoted.push_back(L'"'); quoted.append(exePath); quoted.push_back(L'"');

            HKEY h = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS)
                return;
            RegSetValueExW(h, kSrLoomValue, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(quoted.c_str()),
                           static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(h);
        }
        else
        {
            HKEY h = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &h)
                    != ERROR_SUCCESS)
                return;
            RegDeleteValueW(h, kSrLoomValue);
            RegCloseKey(h);
        }
    }

    bool ReadStartInTray()
    {
        return ReadDword(kSettingsKey, kStartInTrayValue, 1) != 0;
    }

    void WriteStartInTray(bool enable)
    {
        WriteDword(kSettingsKey, kStartInTrayValue, enable ? 1u : 0u);
    }

    bool ReadAutoApplyProfiles()
    {
        // Default ON -- the only way this hurts is if a user has built a
        // profile list and forgotten about it. The opt-out is one menu click.
        return ReadDword(kSettingsKey, kAutoApplyProfilesValue, 1) != 0;
    }

    void WriteAutoApplyProfiles(bool enable)
    {
        WriteDword(kSettingsKey, kAutoApplyProfilesValue, enable ? 1u : 0u);
    }

    bool ReadHeadTrackingOnStartup()
    {
        // Default ON -- matches the pre-toggle behaviour so upgraders see
        // no change. Users who never touch head tracking (looking-glass /
        // fullscreen weave only) can flip it off in the STARTUP section
        // to keep the SR camera cold at launch.
        return ReadDword(kSettingsKey, kHeadTrackingOnStartupValue, 1) != 0;
    }

    void WriteHeadTrackingOnStartup(bool enable)
    {
        WriteDword(kSettingsKey, kHeadTrackingOnStartupValue, enable ? 1u : 0u);
    }

    bool ReadLateLatching()
    {
        return ReadDword(kSettingsKey, L"LateLatching", 1) != 0;   // default ON
    }

    void WriteLateLatching(bool enable)
    {
        WriteDword(kSettingsKey, L"LateLatching", enable ? 1u : 0u);
    }

    bool ReadZeroCopyCapture()
    {
        return ReadDword(kSettingsKey, L"ZeroCopyCapture", 1) != 0;   // default ON
    }

    bool ReadRenderThread()
    {
        return ReadDword(kSettingsKey, L"RenderThread", 1) != 0;   // default ON
    }

    void ReadAnaCustom(float left[3], float right[3])
    {
        const DWORD l = ReadDword(kSettingsKey, L"AnaCustomLeft", 0x0000FF), r = ReadDword(kSettingsKey, L"AnaCustomRight", 0xFFFF00);   // (0x00BBGGRR)
        for (int c = 0; c < 3; ++c) { left[c] = ((l >> (8 * c)) & 0xFF) / 255.0f; right[c] = ((r >> (8 * c)) & 0xFF) / 255.0f; }
    }

    void WriteAnaCustom(const float left[3], const float right[3])
    {
        DWORD l = 0, r = 0;
        for (int c = 0; c < 3; ++c)
        {
            l |= (DWORD)(left[c]  < 0 ? 0 : left[c]  > 1 ? 255 : (int)(left[c]  * 255.0f + 0.5f)) << (8 * c);
            r |= (DWORD)(right[c] < 0 ? 0 : right[c] > 1 ? 255 : (int)(right[c] * 255.0f + 0.5f)) << (8 * c);
        }
        WriteDword(kSettingsKey, L"AnaCustomLeft", l); WriteDword(kSettingsKey, L"AnaCustomRight", r);
    }

    int ReadAnaSaved(float pairs[][6], int max)
    {
        int n = (int)ReadDword(kSettingsKey, L"AnaSavedCount", 0);
        if (n > max) n = max;
        for (int k = 0; k < n; ++k)
        {
            wchar_t nl[32], nr[32]; swprintf_s(nl, L"AnaSaved%dL", k); swprintf_s(nr, L"AnaSaved%dR", k);
            const DWORD l = ReadDword(kSettingsKey, nl, 0x0000FF), r = ReadDword(kSettingsKey, nr, 0xFFFF00);
            for (int c = 0; c < 3; ++c) { pairs[k][c] = ((l >> (8 * c)) & 0xFF) / 255.0f; pairs[k][3 + c] = ((r >> (8 * c)) & 0xFF) / 255.0f; }
        }
        return n;
    }

    void WriteAnaSaved(const float pairs[][6], int count)
    {
        auto pack = [](const float* v) { DWORD d = 0; for (int c = 0; c < 3; ++c) d |= (DWORD)(v[c] < 0 ? 0 : v[c] > 1 ? 255 : (int)(v[c] * 255.0f + 0.5f)) << (8 * c); return d; };
        WriteDword(kSettingsKey, L"AnaSavedCount", (DWORD)count);
        for (int k = 0; k < count; ++k)
        {
            wchar_t nl[32], nr[32]; swprintf_s(nl, L"AnaSaved%dL", k); swprintf_s(nr, L"AnaSaved%dR", k);
            WriteDword(kSettingsKey, nl, pack(pairs[k])); WriteDword(kSettingsKey, nr, pack(pairs[k] + 3));
        }
    }

    bool ReadWeavePlane()
    {
        return ReadDword(kSettingsKey, L"WeavePlane", 0) != 0;   // default OFF
    }

    bool ReadDeferRecovered()
    {
        return ReadDword(kSettingsKey, L"DeferRecovered", 0) != 0;   // default OFF: the user felt the delay
    }

    bool ReadScrollReuse()
    {
        return ReadDword(kSettingsKey, L"ScrollReuse", 1) != 0;   // default ON
    }

    int ReadWeaverChoice()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverChoice", 0);
        return v <= 3 ? (int)v : 0;
    }

    void WriteWeaverChoice(int choice)
    {
        WriteDword(kSettingsKey, L"WeaverChoice", (DWORD)(choice >= 0 && choice <= 3 ? choice : 0));
    }

    void WriteDiagSkipWeave(bool enable)
    {
        WriteDword(kSettingsKey, L"DiagSkipWeave", enable ? 1u : 0u);
    }

    bool ReadPerfLog()
    {
        return ReadDword(kSettingsKey, L"PerfLog", 0) != 0;   // default off (turn on in Advanced when reporting stutter)
    }

    void WritePerfLog(bool enable)
    {
        WriteDword(kSettingsKey, L"PerfLog", enable ? 1u : 0u);
    }

    bool ReadDiagSkipWeave()
    {
        return ReadDword(kSettingsKey, L"DiagSkipWeave", 0) != 0;
    }

    bool ReadEyeOrderDetect()
    {
        return ReadDword(kSettingsKey, L"EyeOrderDetect", 1) != 0;   // default on (user's decision; see the note in Settings.h)
    }

    void WriteEyeOrderDetect(bool enable)
    {
        WriteDword(kSettingsKey, L"EyeOrderDetect", enable ? 1u : 0u);
    }

    // The pinned Stereo 3D Input: 1..n a layout (StereoFormatList index + 1),
    // 0 Automatic Detection. Nothing pinned: Side-by-Side (half) -- Automatic
    // is experimental. (Stored: Automatic as kPinnedAuto; a stored 0 is from
    // before, when Automatic was the default, and reads as nothing pinned.)
    constexpr DWORD kPinnedAuto = 1000;
    constexpr int   kDefaultHalfSbs = 2;
    int ReadDefaultInput()
    {
        const DWORD v = ReadDword(kSettingsKey, L"DefaultInput", 0);
        if (v == kPinnedAuto) return 0;
        return v == 0 ? kDefaultHalfSbs : (int)v;
    }

    void WriteDefaultInput(int input)
    {
        WriteDword(kSettingsKey, L"DefaultInput", input <= 0 ? kPinnedAuto : (DWORD)input);
    }

    bool ReadDirectComposition()
    {
        return ReadDword(kSettingsKey, L"DirectComposition", 1) != 0;   // default ON
    }

    void WriteDirectComposition(bool enable)
    {
        WriteDword(kSettingsKey, L"DirectComposition", enable ? 1u : 0u);
    }

    bool ReadKatangaAutoReceive()
    {
        return ReadDword(kSettingsKey, L"KatangaAutoReceive", 1) != 0;   // default ON
    }

    void WriteKatangaAutoReceive(bool enable)
    {
        WriteDword(kSettingsKey, L"KatangaAutoReceive", enable ? 1u : 0u);
    }

    bool ReadSystemUsesLightTheme()
    {
        // Windows' "Choose your default app mode" (Settings > Personalization
        // > Colors). Missing value = pre-1809 Windows, which was light.
        return ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", 1) != 0;
    }
}
