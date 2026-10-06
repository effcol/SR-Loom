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

    void WriteAutoPlane(bool on) { WriteDword(kSettingsKey, L"AutoPlane", on ? 1 : 0); }
    bool ReadAutoPlaneDX12() { const DWORD v = ReadDword(kSettingsKey, L"AutoPlane", 1); return v != 0 && v != 2; }   // (2: the Direct3D 11 presenter only)
    bool ReadAutoPlaneHold() { return ReadDword(kSettingsKey, L"AutoPlane", 1) == 3; }   // (3: held once reached -- an experiment, Renderer::UpdateAutoPlane)
    bool ReadWeaveRaw() { return ReadDword(kSettingsKey, L"WeaveRaw", 0) != 0; }
    bool ReadGpuRealtime() { return ReadDword(kSettingsKey, L"GpuRealtime", 0) != 0; }
    void WriteGpuRealtime(bool on) { WriteDword(kSettingsKey, L"GpuRealtime", on ? 1 : 0); }
    bool ReadAutoPlane()
    {
        return ReadDword(kSettingsKey, L"AutoPlane", 1) != 0;   // default ON
    }

    bool ReadWeavePlane()
    {
        return ReadDword(kSettingsKey, L"WeavePlane", 0) != 0;   // default OFF
    }

    int ReadHdrOutput()
    {
        const DWORD v = ReadDword(kSettingsKey, L"HdrOutput", 1);   // default 1: when Windows has HDR on for the SR display
        return v <= 2 ? (int)v : 1;
    }

    int ReadWeaverLatency()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverLatency", 0);
        return v <= 1 ? (int)v : 0;
    }

    bool ReadAsyncConvert()
    {
        return ReadDword(kSettingsKey, L"AsyncConvert", 1) != 0;   // default ON (DX12 weaver only)
    }

    int ReadAsyncEnterUs()
    {
        const DWORD v = ReadDword(kSettingsKey, L"AsyncEnterUs", 3500);
        return v >= 50 && v <= 20000 ? (int)v : 3500;
    }

    int ReadAsyncBands()
    {
        const DWORD v = ReadDword(kSettingsKey, L"AsyncBands", 4);
        return v <= 32 ? (int)v : 4;
    }

    int ReadAsyncWaitUs()
    {
        const DWORD v = ReadDword(kSettingsKey, L"AsyncWaitUs", 1200);
        return v <= 5000 ? (int)v : 1200;
    }

    bool ReadDeferRecovered()
    {
        return ReadDword(kSettingsKey, L"DeferRecovered", 0) != 0;   // default OFF: the user felt the delay
    }

    bool ReadScrollReuse()
    {
        return ReadDword(kSettingsKey, L"ScrollReuse", 0) != 0;   // default OFF (v3.1: it now costs more than it saves on ordinary pages)
    }

    int ReadWeaverAct()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverAct", 0);
        return v <= 3 ? (int)v : 0;
    }
    void WriteWeaverAct(int mode) { WriteDword(kSettingsKey, L"WeaverAct", (DWORD)(mode >= 0 && mode <= 3 ? mode : 0)); }
    int ReadWeaverContrast()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverContrast", 100);
        return v <= 200 ? (int)v : 100;
    }
    void WriteWeaverContrast(int pct) { WriteDword(kSettingsKey, L"WeaverContrast", (DWORD)(pct >= 0 && pct <= 200 ? pct : 100)); }
    int ReadWeaverActStrength()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverActStrength", 100);
        return v <= 300 ? (int)v : 100;
    }
    void WriteWeaverActStrength(int pct) { WriteDword(kSettingsKey, L"WeaverActStrength", (DWORD)(pct >= 0 && pct <= 300 ? pct : 100)); }

    int   ReadWeaverActDefault() { const DWORD v = ReadDword(kSettingsKey, L"WeaverActDefault", 99); return v <= 2 ? (int)v : -1; }
    void  WriteWeaverActDefault(int mode) { WriteDword(kSettingsKey, L"WeaverActDefault", (DWORD)mode); }
    int   ReadLfFollow()    { const DWORD v = ReadDword(kSettingsKey, L"LfFollow", 0); return v <= 2 ? (int)v : 0; }
    void  WriteLfFollow(int v) { WriteDword(kSettingsKey, L"LfFollow", (DWORD)v); }
    void  ClearLfSlant()    { WriteDword(kSettingsKey, L"LfSlant", 0); }
    float ReadLfDistance()  { const DWORD v = ReadDword(kSettingsKey, L"LfDistance", 60); return v >= 20 && v <= 300 ? (float)v : 60.0f; }
    void  WriteLfDistance(float cm) { WriteDword(kSettingsKey, L"LfDistance", (DWORD)(cm + 0.5f)); }
    bool  ReadLfPattern()   { return ReadDword(kSettingsKey, L"LfPattern", 0) != 0; }
    void  WriteLfPattern(bool on) { WriteDword(kSettingsKey, L"LfPattern", on ? 1u : 0u); }
    bool  ReadLfMeasure()   { return ReadDword(kSettingsKey, L"LfMeasure", 0) != 0; }
    void  WriteLfMeasure(bool on) { WriteDword(kSettingsKey, L"LfMeasure", on ? 1u : 0u); }
    int   ReadLfSpread()    { const DWORD v = ReadDword(kSettingsKey, L"LfSpread", 30); return v >= 1 && v <= 100 ? (int)v : 30; }
    void  WriteLfSpread(float pct) { WriteDword(kSettingsKey, L"LfSpread", (DWORD)(pct + 0.5f)); }
    bool  ReadLfCentre()    { return ReadDword(kSettingsKey, L"LfCentre", 0) != 0; }
    void  WriteLfCentre(bool on) { WriteDword(kSettingsKey, L"LfCentre", on ? 1u : 0u); }
    // RGB + depth: 0 the strength (0-100), 1 the focus plane (0-100), 2 the layout
    // (bit 0 depth on the left, bit 1 black near), 3 looking around with the head.
    static const wchar_t* const kRgbdName[4] = { L"RgbdStrength", L"RgbdFocus", L"RgbdFlags", L"RgbdLook" };
    static const DWORD kRgbdDefault[4] = { 50, 50, 4, 1 };   // (layout: found automatically, white near)
    int   ReadRgbd(int i)   { if (i < 0 || i > 3) return 0; const DWORD v = ReadDword(kSettingsKey, kRgbdName[i], kRgbdDefault[i]); return v <= 100 ? (int)v : (int)kRgbdDefault[i]; }
    void  WriteRgbd(int i, int v) { if (i >= 0 && i <= 3) WriteDword(kSettingsKey, kRgbdName[i], (DWORD)(v < 0 ? 0 : v > 100 ? 100 : v)); }
    bool  ReadLightField()  { return ReadDword(kSettingsKey, L"LightField", 0) != 0; }
    void  WriteLightField(bool on) { WriteDword(kSettingsKey, L"LightField", on ? 1u : 0u); }
    // (Stored x 100000; the slant offset by 2 so it can be negative. 0: not set.)
    float ReadLfPitch()     { return (float)ReadDword(kSettingsKey, L"LfPitch", 0) / 100000.0f; }
    void  WriteLfPitch(float v) { WriteDword(kSettingsKey, L"LfPitch", (DWORD)(v * 100000.0f + 0.5f)); }
    bool  ReadLfSlant(float& v) { const DWORD d = ReadDword(kSettingsKey, L"LfSlant", 0); if (!d) return false; v = (float)d / 100000.0f - 2.0f; return true; }
    void  WriteLfSlant(float v) { WriteDword(kSettingsKey, L"LfSlant", (DWORD)((v + 2.0f) * 100000.0f + 0.5f)); }
    float ReadLfOffset()    { return (float)ReadDword(kSettingsKey, L"LfOffset", 0) / 100000.0f; }
    void  WriteLfOffset(float v) { WriteDword(kSettingsKey, L"LfOffset", (DWORD)(v * 100000.0f + 0.5f)); }

    int ReadWeaverChoice()
    {
        const DWORD v = ReadDword(kSettingsKey, L"WeaverChoice", 0);
        // (0 the standard weaver on Direct3D 11, 4 on Direct3D 12. The legacy
        // weavers -- 1-3, 5-6 -- are no longer offered: anti-crosstalk is set on
        // the standard ones. A stored legacy choice becomes its standard one.)
        return v >= 4 && v <= 6 ? 4 : 0;
    }

    void WriteWeaverChoice(int choice)
    {
        WriteDword(kSettingsKey, L"WeaverChoice", (DWORD)(choice >= 0 && choice <= 6 ? choice : 0));
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
        return ReadDword(kSettingsKey, L"EyeOrderDetect", 0) != 0;   // default off (user's decision; see the note in Settings.h)
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

    int ReadThemeMode()
    {
        const DWORD v = ReadDword(kSettingsKey, L"ThemeMode", 0);
        return v <= 2 ? (int)v : 0;
    }
    void WriteThemeMode(int mode) { WriteDword(kSettingsKey, L"ThemeMode", (DWORD)(mode >= 0 && mode <= 2 ? mode : 0)); }

    bool ReadSystemUsesLightTheme()
    {
        // Windows' "Choose your default app mode" (Settings > Personalization
        // > Colors). Missing value = pre-1809 Windows, which was light.
        return ReadDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", 1) != 0;
    }
}
