// Settings.h — small HKCU registry-backed setting store for user preferences
// that persist across runs (run-at-login, start-in-tray, etc). No file I/O,
// no third-party deps; just a couple of registry calls per setting.
#pragma once

#include <windows.h>   // RECT

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
    // Present the weave through DirectComposition (default ON): no bit-blt
    // copy or DwmFlush, and cut-outs (taskbar, Auto Stereo regions...) drawn
    // as see-through pixels in the same frame as the picture. Off = the
    // classic swap chains. Takes effect on restart (the weave window is
    // created for one or the other). HKCU\Software\SRLoom\DirectComposition.
    // SR weaver late latching (default ON): eye positions updated for frames
    // already in flight. Suspected of pacing the weave to the 60 Hz tracking
    // camera -- switchable to compare. HKCU\Software\SRLoom\LateLatching.
    bool ReadLateLatching();
    void WriteLateLatching(bool enable);

    // Diagnostics only (no UI): skip the SR weaver's weave call, to see
    // whether it paces the loop. HKCU\Software\SRLoom\DiagSkipWeave = 1.
    bool ReadDiagSkipWeave();
    void WriteDiagSkipWeave(bool enable);
    // Frame / GPU timing lines in srweaver.log every 5 s (default ON while
    // tuning). HKCU\Software\SRLoom\PerfLog.
    bool ReadPerfLog();
    void WritePerfLog(bool enable);

    // Automatic Detection works out which half of an SBS / TAB picture is the
    // left eye (and swaps it when it's the other way round). Default on.
    bool ReadEyeOrderDetect();
    void WriteEyeOrderDetect(bool enable);

    // The Stereo 3D Input pinned as the default (the panel's pin button):
    // 0 = Automatic Detection (the default), else StereoFormatIndex + 1.
    int  ReadDefaultInput();
    void WriteDefaultInput(int input);

    bool ReadDirectComposition();
    void WriteDirectComposition(bool enable);

    bool ReadKatangaAutoReceive();
    void WriteKatangaAutoReceive(bool enable);

    // Windows' app light/dark mode (HKCU ...\Themes\Personalize\
    // AppsUseLightTheme). The panel and the Looking Glass title bar follow
    // it, and re-read it on WM_SETTINGCHANGE so a live switch is picked up.
    bool ReadSystemUsesLightTheme();
}
