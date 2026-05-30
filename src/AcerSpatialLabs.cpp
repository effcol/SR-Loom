#include "AcerSpatialLabs.h"

#include <windows.h>
#pragma comment(lib, "advapi32.lib")

namespace srw::AcerSpatialLabs
{

namespace
{
    constexpr const char* kKey        = "SOFTWARE\\Acer\\SpatialLabs";
    constexpr const char* kFocus      = "Focus_Detection";
    constexpr const char* kFullscreen = "Fullscreen_Detection";
    constexpr const char* kMonitor    = "Monitor_Detection";

    const char* ValueName(Setting s)
    {
        switch (s)
        {
            case Setting::Focus:      return kFocus;
            case Setting::Fullscreen: return kFullscreen;
            case Setting::Monitor:    return kMonitor;
        }
        return "";
    }

    int ReadDword(const char* valueName)
    {
        HKEY h{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
            return -1;
        DWORD val = 0, sz = sizeof(val), type = 0;
        LONG r = RegQueryValueExA(h, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&val), &sz);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || type != REG_DWORD) return -1;
        return static_cast<int>(val);
    }
}

bool Available()
{
    HKEY h{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

State Read()
{
    return { ReadDword(kFocus), ReadDword(kFullscreen), ReadDword(kMonitor) };
}

WriteResult Write(Setting s, bool on)
{
    HKEY h{};
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, KEY_SET_VALUE, &h);
    if (r == ERROR_ACCESS_DENIED) return WriteResult::NeedsAdmin;
    if (r != ERROR_SUCCESS)       return WriteResult::Failed;
    DWORD val = on ? 1u : 0u;
    LONG w = RegSetValueExA(h, ValueName(s), 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(h);
    if (w == ERROR_ACCESS_DENIED) return WriteResult::NeedsAdmin;
    if (w != ERROR_SUCCESS)       return WriteResult::Failed;
    return WriteResult::OK;
}

}   // namespace srw::AcerSpatialLabs
