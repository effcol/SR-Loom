// Settings.h — small HKCU registry-backed setting store for user preferences
// that persist across runs (run-at-login, start-in-tray, etc). No file I/O,
// no third-party deps; just a couple of registry calls per setting.
#pragma once

namespace srw::Settings
{
    // Whether SR Loom is registered to launch on user login (a value in
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Run named SRLoom).
    bool ReadRunAtStartup();
    void WriteRunAtStartup(bool enable);

    // Whether SR Loom should start with the control panel hidden (just the
    // tray icon). Default true -- the natural behaviour for a tray-resident
    // utility. When false, the control panel opens on launch.
    bool ReadStartInTray();
    void WriteStartInTray(bool enable);

    // Master toggle for the per-game profile auto-apply feature (see
    // Profiles.h). Default ON when a profile list exists; off-by-default
    // is fine when no profiles are configured (nothing to apply anyway).
    // Stored as DWORD under HKCU\Software\SRLoom\AutoApplyProfiles.
    bool ReadAutoApplyProfiles();
    void WriteAutoApplyProfiles(bool enable);
}
