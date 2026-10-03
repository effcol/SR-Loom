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
    // Zero-copy capture (default ON): the converter / weaver read the captured
    // frame itself instead of a copy of it (no UI; for comparing).
    // HKCU\Software\SRLoom\ZeroCopyCapture.
    bool ReadZeroCopyCapture();
    // The render loop on its own thread, apart from the windows' (default ON;
    // no UI). 0: one thread for both, as before v3.1 -- for comparing, or if
    // something misbehaves. HKCU\Software\SRLoom\RenderThread. Read at start-up.
    bool ReadRenderThread();
    // Recovered Colour while a page scrolls: what only moved keeps last frame's
    // result, moved, instead of being recovered again (default ON; no UI). 0:
    // every moved part recovered again -- for comparing.
    // HKCU\Software\SRLoom\ScrollReuse. Read at start-up.
    bool ReadScrollReuse();
    // Recovered Colour converted after each frame is presented and woven the
    // next refresh (default OFF; no UI): the weave is never held up by it, but
    // the picture is a refresh late -- felt as lag. 0: converted first, as
    // always before. HKCU\Software\SRLoom\DeferRecovered. Read at start-up.
    bool ReadDeferRecovered();
    // Experiment (default OFF; no UI): the weave's swap chain opaque and allowed to
    // tear, so Windows may give it a display plane of its own -- not composited,
    // and not making the screen capture see a change every refresh. No cut-outs
    // (taskbar, pop-ups) while on. HKCU\Software\SRLoom\WeavePlane. Read at start-up.
    bool ReadWeavePlane();
    // The Custom anaglyph pair's two picked colours (0-1 red, green, blue each;
    // default red / cyan). HKCU\Software\SRLoom\AnaCustomLeft, AnaCustomRight.
    void ReadAnaCustom(float left[3], float right[3]);
    void WriteAnaCustom(const float left[3], const float right[3]);
    // Saved Custom pairs (up to 8; each left rgb then right rgb, 0-1).
    // HKCU\Software\SRLoom\AnaSavedCount, AnaSaved0L, AnaSaved0R, ...
    int  ReadAnaSaved(float pairs[][6], int max);
    void WriteAnaSaved(const float pairs[][6], int count);
    // Which SR weaver (default 0): 0 the modern one; 1-3 the legacy one with
    // anti-crosstalk Off / Static / Dynamic (only it has those). Applies when
    // the SR session is next made. HKCU\Software\SRLoom\WeaverChoice.
    int  ReadWeaverChoice();
    void WriteWeaverChoice(int choice);

    // Diagnostics only (no UI): skip the SR weaver's weave call, to see
    // whether it paces the loop. HKCU\Software\SRLoom\DiagSkipWeave = 1.
    bool ReadDiagSkipWeave();
    void WriteDiagSkipWeave(bool enable);
    // Frame / GPU timing lines in srweaver.log every 5 s (default off: turned
    // on in Settings for a stutter report). HKCU\Software\SRLoom\PerfLog.
    bool ReadPerfLog();
    void WritePerfLog(bool enable);

    // Automatic Detection works out which half of an SBS / TAB picture is the
    // left eye (and swaps it when it's the other way round). Default on. (The
    // US patents Leia US9325964 / US9729852 claim this, to 2032 / 2031.)
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
