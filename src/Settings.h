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
    // result, moved, instead of being recovered again (default OFF; no UI: on
    // ordinary pages its search and copies cost more than they save; it wins
    // only when a screen-filling photograph scrolls). 0:
    // every moved part recovered again -- for comparing.
    // HKCU\Software\SRLoom\ScrollReuse. Read at start-up.
    bool ReadScrollReuse();
    // Recovered Colour converted after each frame is presented and woven the
    // next refresh (default OFF; no UI): the weave is never held up by it, but
    // the picture is a refresh late -- felt as lag. 0: converted first, as
    // always before. HKCU\Software\SRLoom\DeferRecovered. Read at start-up.
    bool ReadDeferRecovered();
    // DX12 weaver, Recovered Colour: the conversion apart from the weave (default
    // ON; no UI). The weave reads the newest finished picture, so it goes out
    // every refresh; a conversion too slow for the refresh shows one refresh
    // later instead of holding the weave back. 0: the weave waits for the
    // conversion, as with DX11. HKCU\Software\SRLoom\AsyncConvert. Read at start-up.
    bool ReadAsyncConvert();
    // The weaver's pipeline latency (how far ahead it predicts the eyes): 0 the
    // SDK's own figure (default), 1 what SR Loom measures from the weave call to
    // the picture reaching the display (needs the Perf Log on: it is measured
    // there). HKCU\Software\SRLoom\WeaverLatency. Read at start-up.
    int  ReadWeaverLatency();
    // ... and how long the weave waits for a conversion under way before going out
    // with the picture it has, in microseconds (default 1200). AsyncWaitUs.

    int  ReadAsyncWaitUs();
    // ... and in how many pieces a long conversion is sent to the GPU, so the weave
    // can run between them (default 4; 0: in one go). AsyncBands.
    int  ReadAsyncBands();
    // ... and how long a conversion has to take (GPU time, microseconds; three in a
    // row) for it to be run apart from the weave (default 3500). AsyncEnterUs.
    int  ReadAsyncEnterUs();
    // Experiment (default OFF; no UI): the weave's swap chain opaque and allowed to
    // tear, so Windows may give it a display plane of its own -- not composited,
    // and not making the screen capture see a change every refresh. No cut-outs
    // (taskbar, pop-ups) while on. HKCU\Software\SRLoom\WeavePlane. Read at start-up.
    bool ReadWeavePlane();
    // The weave straight to the display (opaque swap chain, not composed by
    // Windows) whenever nothing has to show through it -- no taskbar, pointer or
    // window cut-outs -- and see-through again the moment something does (default
    // ON). About 5 ms less from the weave to the display, and the screen capture
    // then only reports what really changed. HKCUSoftwareSRLoomAutoPlane.
    bool ReadAutoPlane();
    // HDR (no UI yet): capture, conversion and the weave's swap chain 16-bit float
    // (scRGB) instead of 8-bit, so brighter-than-white picture is kept, with
    // either weaver. 1 (default): when Windows has HDR switched on for the SR
    // display; 2 always; 0 never. Only for the layouts that pass the picture
    // through (see HdrWanted in main.cpp). HKCU\Software\SRLoom\HdrOutput.
    int  ReadHdrOutput();
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
    // Anti-crosstalk through the runtime's weaver settings: 0 the display's default,
    // 1 off, 2 static, 3 dynamic; and its strength, % of the display's own amount
    // (0-300). HKCUSoftwareSRLoomWeaverAct / WeaverActStrength.
    // Light field (an experiment; Quilt sources): the Quilt's views interlaced
    // across the lens by SR Loom, all at once, with no eye tracking -- move the
    // head to look around. The lens pitch (pixels) and slant come from the SR
    // runtime unless set here, and the offset slides the views under the lens.
    // HKCU\Software\SRLoom\LightField, LfPitch, LfSlant, LfOffset.
    bool  ReadLightField();
    void  WriteLightField(bool on);
    float ReadLfPitch();
    void  WriteLfPitch(float v);
    bool  ReadLfSlant(float& v);
    void  WriteLfSlant(float v);
    float ReadLfOffset();
    void  WriteLfOffset(float v);
    void  ClearLfSlant();
    // ... the distance the views are aimed at (cm, default 60): every lens' fan
    // of views meets there, so the whole screen lines up from that distance; and
    // the alignment pattern (half the fan red, half blue) in place of the picture.
    float ReadLfDistance();
    void  WriteLfDistance(float cm);
    bool  ReadLfPattern();
    void  WriteLfPattern(bool on);
    // ... the views kept aimed at the tracked viewer sideways as well. LfCentre.
    bool  ReadLfCentre();
    void  WriteLfCentre(bool on);
    // RGB + depth pictures (the RGB + Depth input): 0 strength, 1 focus plane (both
    // 0-100), 2 layout bits (1 depth on the left, 2 black near), 3 look around with
    // the head. RgbdStrength, RgbdFocus, RgbdFlags, RgbdLook.
    int   ReadRgbd(int i);
    void  WriteRgbd(int i, int v);
    // ... and how much of the Quilt's range of views the lens' fan shows, about the
    // middle one (%, default 30): see the shader. LfSpread.
    int   ReadLfSpread();
    void  WriteLfSpread(float pct);
    // ... and a request from the panel to measure the lens (main.cpp MeasureLens),
    // cleared when taken up.
    bool  ReadLfMeasure();
    void  WriteLfMeasure(bool on);
    // ... following the viewer: the fan of views kept centred on the tracked head
    // (0 off, 1 on, 2 on with the direction reversed). LfFollow.
    int   ReadLfFollow();
    void  WriteLfFollow(int v);
    // The display's own anti-crosstalk mode (0 off, 1 static, 2 dynamic; -1 not
    // known), noted by the weaver for the panel to show. WeaverActDefault.
    int   ReadWeaverActDefault();
    void  WriteWeaverActDefault(int mode);
    int  ReadWeaverAct();
    void WriteWeaverAct(int mode);
    int  ReadWeaverActStrength();
    void WriteWeaverActStrength(int pct);
    // ... and the weaver's contrast, % of the display's own (0-200; experimental).
    // HKCU\Software\SRLoom\WeaverContrast.
    int  ReadWeaverContrast();
    void WriteWeaverContrast(int pct);

    // Diagnostics only (no UI): skip the SR weaver's weave call, to see
    // whether it paces the loop. HKCU\Software\SRLoom\DiagSkipWeave = 1.
    bool ReadDiagSkipWeave();
    void WriteDiagSkipWeave(bool enable);
    // Frame / GPU timing lines in srweaver.log every 5 s (default off: turned
    // on in Settings for a stutter report). HKCU\Software\SRLoom\PerfLog.
    bool ReadPerfLog();
    void WritePerfLog(bool enable);

    // Automatic Detection works out which half of an SBS / TAB picture is the
    // left eye (and swaps it when it's the other way round). Default off. (The
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
    // The panel's theme: 0 Auto (as Windows, the default), 1 Light, 2 Dark.
    // HKCU\Software\SRLoom\ThemeMode.
    int  ReadThemeMode();
    void WriteThemeMode(int mode);
}
