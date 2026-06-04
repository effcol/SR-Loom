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
}
