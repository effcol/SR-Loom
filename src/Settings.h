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

    // Whether SR Loom should turn on head-tracking output (OpenTrack UDP +
    // FreeTrack + TrackIR) automatically at launch. Default ON so the
    // common case ("I want head tracking in games") works out of the box.
    // Users who don't use head tracking can flip it off in STARTUP and
    // avoid the SR camera engaging just because SR Loom is running.
    // Stored as DWORD under HKCU\Software\SRLoom\HeadTrackingOnStartup.
    bool ReadHeadTrackingOnStartup();
    void WriteHeadTrackingOnStartup(bool enable);

    // Katanga auto-receive: when a game (or bridge, e.g. the 3D Slicer
    // extension) starts publishing frames over Katanga, SR Loom switches to
    // receiving them automatically, and restores the previous setup when it
    // stops. Default ON. HKCU\Software\SRLoom\KatangaAutoReceive.
    bool ReadKatangaAutoReceive();
    void WriteKatangaAutoReceive(bool enable);

    // Windows' app light/dark mode (HKCU ...\Themes\Personalize\
    // AppsUseLightTheme). The panel and the Looking Glass title bar follow
    // it, and re-read it on WM_SETTINGCHANGE so a live switch is picked up.
    bool ReadSystemUsesLightTheme();
}
