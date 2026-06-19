#include "AcerSpatialLabs.h"

#include <windows.h>
#pragma comment(lib, "advapi32.lib")

namespace srw::AcerSpatialLabs
{

namespace
{
    constexpr const char* kAcerKey       = "SOFTWARE\\Acer\\SpatialLabs";
    constexpr const char* kAcerFocus     = "Focus_Detection";
    constexpr const char* kCameraKey     = "SOFTWARE\\Microsoft\\OEM\\Device\\Capture";
    constexpr const char* kCameraValue   = "NoPhysicalCameraLED";

    // Each Setting maps to its own (registry subkey, value name) under HKLM.
    // The Acer ones live under Acer\SpatialLabs, the camera popup lives in
    // Windows' own OEM capture-notification settings.
    void KeyFor(Setting s, const char*& outKey, const char*& outValue)
    {
        switch (s)
        {
            case Setting::Focus:        outKey = kAcerKey;   outValue = kAcerFocus;     return;
            case Setting::CameraPopup:  outKey = kCameraKey; outValue = kCameraValue;   return;
        }
        outKey = ""; outValue = "";
    }

    int ReadDword(const char* key, const char* valueName)
    {
        HKEY h{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
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
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kAcerKey, 0, KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

State Read()
{
    State s{};
    s.focusDetection = ReadDword(kAcerKey, kAcerFocus);
    // CameraPopup mirrors the *user-facing* state of the popup, not the raw
    // registry value: NoPhysicalCameraLED=0 means "display has an LED so
    // suppress popup" -> popup hidden (state=0). Missing key / =1 means
    // popup shown (state=1, which is also the Windows default).
    const int nplRaw = ReadDword(kCameraKey, kCameraValue);
    if      (nplRaw == 0) s.cameraPopup = 0;   // explicitly suppressed
    else if (nplRaw == 1) s.cameraPopup = 1;   // explicitly shown
    else                  s.cameraPopup = 1;   // missing -> Windows default = shown
    return s;
}

WriteResult Write(Setting s, bool on)
{
    const char* key = nullptr; const char* value = nullptr;
    KeyFor(s, key, value);
    if (!key || !*key) return WriteResult::Failed;

    // CameraPopup is user-facing inverted from the registry value:
    // toggle ON (popup shown) -> NoPhysicalCameraLED=1
    // toggle OFF (popup hidden) -> NoPhysicalCameraLED=0
    const DWORD val = on ? 1u : 0u;

    HKEY h{};
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, key, 0, nullptr, 0,
                             KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r == ERROR_ACCESS_DENIED) return WriteResult::NeedsAdmin;
    if (r != ERROR_SUCCESS)       return WriteResult::Failed;
    LONG w = RegSetValueExA(h, value, 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&val), sizeof(val));
    RegCloseKey(h);
    if (w == ERROR_ACCESS_DENIED) return WriteResult::NeedsAdmin;
    if (w != ERROR_SUCCESS)       return WriteResult::Failed;
    return WriteResult::OK;
}

}   // namespace srw::AcerSpatialLabs
