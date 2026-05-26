// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "Capture.h"
#include "Converter.h"
#include "Detector.h"
#include "Gui.h"
#include "resource.h"

#include <shellscalingapi.h>
#include <dwmapi.h>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace srw;

namespace
{
    constexpr char  kWindowClass[] = "SRWeaverWindow";
    constexpr char  kWindowTitle[] = "SR Loom";
    constexpr int   kHotkeyToggle  = 1;   // Ctrl+Alt+W : enable/disable weaving
    constexpr int   kHotkeyMode    = 2;   // Ctrl+Alt+F : fullscreen/windowed
    constexpr int   kHotkeyCapture = 3;   // Ctrl+Alt+C : make active window 3D
    constexpr int   kHotkeyDetect  = 5;   // Ctrl+Alt+D : auto-detect stereo format
    constexpr UINT  kRenderTimer   = 1;   // drives rendering during modal move/resize

    struct AppState
    {
        HWND       hwnd          = nullptr;
        Renderer     renderer;
        SRWeaver     weaver;
        TrayIcon     tray;
        Capture      capture;
        Converter    converter;
        Detector     detector;
        Gui          gui;
        float        convergence    = 0.0f;   // GUI convergence slider (-1..1)
        bool         weavingEnabled = true;
        OutputMode   mode           = OutputMode::Fullscreen;
        SourceKind   source         = SourceKind::CaptureMonitor;  // default: weave the screen (fullscreen SBS)
        StereoFormat format         = StereoFormat::HalfSBS;  // default: most on-screen SBS content is half-width
        bool         swapEyes       = false;
        int          anaglyphCombo  = 0;   // 0..5 colour combination
        int          anaglyphMode   = 4;   // shader mode value (4 = Recovered colour, the default)
        PulfrichMode pulfrichMode   = PulfrichMode::TimeDelay;
        int          pulfrichDelay  = 1;   // delay frames (time-delay mode)
        int          pulfrichNd     = 1;   // ND level index (default Medium)
        int          framePackMode  = 0;   // FramePackPresets index (0 = 1080p)
        HWND       sourceWindow   = nullptr; // tracked window in WindowOverlay mode
        HWND       lastForeground = nullptr; // last real foreground window (for "make active window 3D")
        bool       loupeInteractive = false; // looking glass: currently grabbable (not click-through)
        bool       loupeDragging  = false;   // looking glass: in a move/resize loop
        bool       loupeActive    = false;   // looking glass shown (keep its position across re-applies)
        bool       captureRebind  = false;   // re-register SRV on next frame
        RECT       srDisplayRect  = { 0, 0, 1920, 1080 };  // filled from SR SDK
    };

    AppState* g_app = nullptr;

    // Determine where to place the output window: the SR display if known,
    // otherwise the primary monitor.
    RECT ResolveTargetRect(SRWeaver& weaver)
    {
        RECT rc{};
        if (weaver.GetSRDisplayRect(rc))
            return rc;

        rc.left   = 0;
        rc.top    = 0;
        rc.right  = GetSystemMetrics(SM_CXSCREEN);
        rc.bottom = GetSystemMetrics(SM_CYSCREEN);
        return rc;
    }

    // Apply fullscreen (borderless on the SR display) or windowed styling.
    // The VISIBLE window rectangle. GetWindowRect includes ~7px of invisible
    // resize border on Win10/11, which made the in-place overlay sit slightly
    // larger than the window; the DWM extended frame bounds exclude it.
    RECT VisibleWindowRect(HWND hwnd)
    {
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
            GetWindowRect(hwnd, &r);
        return r;
    }

    // Click-through (layered+transparent) is wanted whenever the weave overlays
    // live content the user interacts with beneath it.
    bool WantsClickThrough(const AppState& app)
    {
        switch (app.mode)
        {
        case OutputMode::WindowOverlay: return true;
        case OutputMode::LookingGlass:  return !app.loupeInteractive;
        case OutputMode::Fullscreen:    return app.source == SourceKind::CaptureMonitor; // passthrough
        default:                        return false;
        }
    }

    void ApplyMode(AppState& app)
    {
        if (app.mode != OutputMode::LookingGlass)
            app.loupeActive = false;   // reset so the loupe re-centres next time it's entered

        const RECT& d = app.srDisplayRect;
        const int dw = d.right - d.left;
        const int dh = d.bottom - d.top;
        const HWND hwnd = app.hwnd;
        const bool ct = WantsClickThrough(app);

        DWORD    style   = WS_POPUP;
        LONG_PTR exStyle = 0;
        RECT     rect    { d.left, d.top, d.left + dw, d.top + dh };
        HWND     zorder  = HWND_TOP;

        switch (app.mode)
        {
        case OutputMode::Fullscreen:
            style   = WS_POPUP;
            exStyle = ct ? (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE) : 0;
            zorder  = ct ? HWND_TOPMOST : HWND_TOP;
            break;

        case OutputMode::WindowOverlay:
            style   = WS_POPUP;
            // NOT topmost: the overlay is pinned directly above the source window each
            // frame (UpdateOverlayTracking), so windows you alt-tab to can occlude the
            // weave. HWND_NOTOPMOST drops it out of the topmost band on entry.
            exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            zorder  = HWND_NOTOPMOST;
            rect    = { d.left, d.top, d.left + 1280, d.top + 720 };
            if (app.sourceWindow && IsWindow(app.sourceWindow))
                rect = VisibleWindowRect(app.sourceWindow);
            break;

        case OutputMode::LookingGlass:
        {
            // Normal window chrome (title bar + resize edges). Click-through on the
            // glass by default; UpdateLoupeInteractivity makes it grabbable while the
            // cursor is over the chrome or during a move/resize.
            style   = WS_OVERLAPPEDWINDOW;
            exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT;
            zorder  = HWND_TOPMOST;
            // Keep the position/size the user has set when already in loupe mode
            // (e.g. re-applying on a format change); only centre on first entry.
            // Use the FULL window rect (GetWindowRect) — VisibleWindowRect's DWM
            // extended bounds exclude the invisible resize borders, so feeding it back
            // in shrinks the loupe by those borders on every re-apply.
            if (app.loupeActive)
            {
                GetWindowRect(hwnd, &rect);
            }
            else
            {
                const int w = 960, h = 600;
                rect = { d.left + (dw - w) / 2, d.top + (dh - h) / 2,
                         d.left + (dw - w) / 2 + w, d.top + (dh - h) / 2 + h };
                app.loupeActive = true;
            }
            break;
        }

        case OutputMode::Windowed:
        default:
        {
            const int w = 1280, h = 720;
            const int x = d.left + (dw - w) / 2;
            const int y = d.top  + (dh - h) / 2;
            style   = WS_OVERLAPPEDWINDOW;
            exStyle = 0;
            zorder  = HWND_NOTOPMOST;
            rect    = { x, y, x + w, y + h };
            break;
        }
        }

        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_VISIBLE);
        if (exStyle & WS_EX_LAYERED)
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);  // fully opaque
        UINT flags = SWP_FRAMECHANGED | SWP_SHOWWINDOW;
        if (exStyle & WS_EX_NOACTIVATE) flags |= SWP_NOACTIVATE;
        SetWindowPos(hwnd, zorder, rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top, flags);

        // Match the swap-chain model to the window: flip (low-latency) when not
        // click-through, bit-blt when layered (flip can't render on layered windows).
        app.renderer.SetLayered(ct);
    }

    // The looking glass passes clicks through the glass, but becomes grabbable when
    // the cursor is over its chrome (title bar / resize edges) or during a move/
    // resize — then returns to click-through. The cursor is polled directly so this
    // works even while the window is click-through.
    void UpdateLoupeInteractivity(AppState& app)
    {
        if (app.mode != OutputMode::LookingGlass) return;

        bool interactive;
        if (app.loupeDragging)
        {
            interactive = true;   // stay grabbable for the whole move/resize
        }
        else
        {
            POINT pt{};
            GetCursorPos(&pt);
            RECT wr{}, cr{};
            GetWindowRect(app.hwnd, &wr);
            GetClientRect(app.hwnd, &cr);
            POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
            ClientToScreen(app.hwnd, &tl);
            ClientToScreen(app.hwnd, &br);
            const bool inWindow = pt.x >= wr.left && pt.x < wr.right && pt.y >= wr.top && pt.y < wr.bottom;
            const bool inGlass  = pt.x >= tl.x   && pt.x < br.x     && pt.y >= tl.y   && pt.y < br.y;
            interactive = inWindow && !inGlass;   // over the chrome
        }

        if (interactive == app.loupeInteractive) return;
        app.loupeInteractive = interactive;

        LONG_PTR ex = WS_EX_TOPMOST | WS_EX_LAYERED | (interactive ? 0 : WS_EX_TRANSPARENT);
        SetWindowLongPtr(app.hwnd, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(app.hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    // Keep the overlay aligned with the tracked source window each frame.
    void UpdateOverlayTracking(AppState& app)
    {
        if (app.mode != OutputMode::WindowOverlay)
            return;

        HWND src = app.sourceWindow;
        if (!src || !IsWindow(src))
            return;  // source gone; leave last frame on screen

        if (IsIconic(src) || !IsWindowVisible(src))
        {
            if (IsWindowVisible(app.hwnd)) ShowWindow(app.hwnd, SW_HIDE);
            return;
        }

        RECT r = VisibleWindowRect(src), cur{};
        GetWindowRect(app.hwnd, &cur);
        const bool rectChanged = !EqualRect(&r, &cur);

        // Pin the overlay DIRECTLY ABOVE the source in the z-order (not global
        // topmost), so windows the user alt-tabs to occlude the weave. Only re-pin
        // when the source has risen above us (e.g. the game was just clicked/focused),
        // not every frame — that avoids constant z-order churn and flicker.
        const HWND above = GetWindow(src, GW_HWNDPREV);   // window directly above src
        const bool zBad  = (above != app.hwnd);           // overlay isn't sitting above src

        if (rectChanged || zBad)
        {
            UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;   // WM_SIZE resizes the swap chain
            HWND insertAfter = HWND_TOP;
            if (zBad)
                insertAfter = above ? above : HWND_TOP;     // slot just above src
            else
                flags |= SWP_NOZORDER;                      // z is fine; only move/resize
            SetWindowPos(app.hwnd, insertAfter, r.left, r.top,
                         r.right - r.left, r.bottom - r.top, flags);
        }
        if (!IsWindowVisible(app.hwnd))
            ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
    }

    void UsePassthrough(AppState& app);   // fwd decl: default weave is screen passthrough

    void SetWeaving(AppState& app, bool enable)
    {
        app.weavingEnabled = enable;
        if (enable)
        {
            // Start the SR session (lens + eye-tracking) and show the output.
            if (!app.weaver.HasWeaver())
                app.weaver.StartSR(app.renderer.Context(), app.hwnd);
            // Default action: weave the screen (fullscreen SBS passthrough). If the
            // chosen source is the monitor but capture isn't running yet, start it.
            if (app.source == SourceKind::CaptureMonitor && !app.capture.IsActive())
                UsePassthrough(app);
            app.captureRebind = true;   // re-register the converter output
            ApplyMode(app);
            ShowWindow(app.hwnd, SW_SHOW);
        }
        else
        {
            // Hide and fully release SR so the lens and camera turn off.
            ShowWindow(app.hwnd, SW_HIDE);
            app.weaver.StopSR();
        }
        app.tray.SetTooltip(enable ? "SR Loom — weaving" : "SR Loom — paused (SR off)");
    }

    // Selecting a source or format turns weaving ON if it isn't already (so the 3D
    // comes on automatically); if it already is, just re-apply the current mode.
    void EnsureWeaving(AppState& app)
    {
        if (app.weavingEnabled) ApplyMode(app);
        else                    SetWeaving(app, true);
    }

    // Like EnsureWeaving, but for a stereo-format / decode-option change: it must NOT
    // re-apply the window mode. ApplyMode resizes + re-styles the window and toggles the
    // swap-chain model — in Looking Glass that shrinks the loupe and stalls the weave on
    // every format change. The new format is picked up via captureRebind in RenderFrame,
    // so we only need to switch weaving on if it was off.
    void EnsureWeavingFormatOnly(AppState& app)
    {
        if (!app.weavingEnabled) SetWeaving(app, true);
    }

    // Record the most recent "real" foreground window (not ours, the shell, or a
    // menu), so "Make active window 3D" can target it even though clicking the tray
    // menu changes the foreground. Called each loop iteration.
    void UpdateLastForeground(AppState& app)
    {
        HWND fg = GetForegroundWindow();
        if (!fg || fg == app.hwnd || !IsWindowVisible(fg))
            return;
        char cls[64] = {};
        GetClassNameA(fg, cls, (int)sizeof(cls));
        if (strcmp(cls, "SRWeaverWindow") == 0 || strcmp(cls, "SRWeaverGuiWindow") == 0 ||
            strcmp(cls, "Shell_TrayWnd") == 0 || strcmp(cls, "#32768") == 0)   // ours / shell / popup menu
            return;
        app.lastForeground = fg;
    }

    // --- Source selection -------------------------------------------------

    // Capture a window and present it as an in-place 3D overlay tracking that window.
    void UseWindow(AppState& app, HWND target)
    {
        if (target && app.capture.StartWindow(target))
        {
            app.source       = SourceKind::CaptureWindow;
            app.sourceWindow = target;
            app.captureRebind = true;   // re-register the SRV once frames arrive
            app.mode = OutputMode::WindowOverlay;
            EnsureWeaving(app);
            // Hand input focus back to the captured window. The overlay is NOACTIVATE
            // (it never takes focus), but the panel/menu we were clicked from did — so
            // without this the game sits in the background and ignores controller/key
            // input. We're the current foreground process here, so the OS lets us set
            // it. The overlay stays topmost, so the 3D remains visible over the window.
            if (IsWindow(target))
                SetForegroundWindow(target);
        }
    }

    // Make the active window 3D (tray item / Ctrl+Alt+C). GetForegroundWindow() works
    // for the hotkey, but the tray menu steals focus, so fall back to the last real
    // foreground window we tracked -> targets the user's window, not the test image.
    void CaptureForeground(AppState& app)
    {
        HWND fg = GetForegroundWindow();
        char cls[64] = {};
        if (fg) GetClassNameA(fg, cls, (int)sizeof(cls));
        const bool ours = !fg || fg == app.hwnd || !IsWindow(fg) ||
                          strcmp(cls, "SRWeaverWindow") == 0 || strcmp(cls, "SRWeaverGuiWindow") == 0 ||
                          strcmp(cls, "Shell_TrayWnd") == 0 || strcmp(cls, "#32768") == 0;
        if (ours)
            fg = app.lastForeground;   // we (GUI/menu) had focus; use the window active before us
        Log("CaptureForeground: target=%p app.hwnd=%p", (void*)fg, (void*)app.hwnd);
        if (fg && fg != app.hwnd && IsWindow(fg))
            UseWindow(app, fg);
    }

    // Start a passthrough capture of the SR display. What gets woven is the
    // region of that monitor beneath our (capture-excluded) window — full screen
    // in Fullscreen, or just the viewer's area in Windowed/LookingGlass.
    void UsePassthrough(AppState& app)
    {
        app.sourceWindow = nullptr;
        const RECT& d = app.srDisplayRect;
        POINT center{ (d.left + d.right) / 2, (d.top + d.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY);
        if (app.capture.StartMonitor(mon))
        {
            app.source = SourceKind::CaptureMonitor;
            app.captureRebind = true;
        }
    }

    // Map a window's client area to a rectangle in the captured frame's pixels.
    RECT PassthroughRegion(AppState& app, HWND hwnd)
    {
        const double fw = app.capture.FrameWidth();
        const double fh = app.capture.FrameHeight();

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(mon, &mi);
        const RECT m = mi.rcMonitor;
        const double sx = fw / (m.right - m.left);
        const double sy = fh / (m.bottom - m.top);

        // Client area in screen coordinates.
        RECT c{};
        GetClientRect(hwnd, &c);
        POINT tl{ c.left, c.top }, br{ c.right, c.bottom };
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);

        RECT r;
        r.left   = (LONG)((tl.x - m.left) * sx);
        r.top    = (LONG)((tl.y - m.top) * sy);
        r.right  = (LONG)((br.x - m.left) * sx);
        r.bottom = (LONG)((br.y - m.top) * sy);
        return r;
    }

    // Resolve the current source frame (test image or capture).
    bool ResolveSource(AppState& app, ID3D11ShaderResourceView*& srv, int& w, int& h)
    {
        if (app.source == SourceKind::TestImage)
        {
            srv = app.weaver.SourceSRV(); w = app.weaver.SourceWidth(); h = app.weaver.SourceHeight();
        }
        else if (app.capture.IsActive())
        {
            srv = app.capture.SRV(); w = app.capture.Width(); h = app.capture.Height();
        }
        else { srv = nullptr; w = h = 0; }
        return srv && w > 0 && h > 0;
    }

    // Analyze the current frame and switch to the detected stereo layout.
    void DetectFormat(AppState& app)
    {
        ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0;
        if (!ResolveSource(app, srv, w, h)) return;
        StereoFormat f;
        if (app.detector.Detect(srv, w, h, f))
        {
            app.format = f;
            app.captureRebind = true;
            app.tray.SetTooltip("SR Loom — detected a stereo layout");
        }
        else
        {
            app.tray.SetTooltip("SR Loom — no stereo layout detected");
        }
    }

    void RenderFrame(AppState& app)
    {
        if (!app.weavingEnabled || !app.renderer.IsValid())
        {
            Sleep(10);
            return;
        }

        UpdateOverlayTracking(app);     // follow the source window in overlay mode
        UpdateLoupeInteractivity(app);  // hover the chrome to grab/move the looking glass

        if (IsIconic(app.hwnd))
        {
            Sleep(10);
            return;
        }

        // Pace to the display before grabbing the newest frame (lowest latency).
        app.renderer.WaitForFrame();

        // Resolve the current source frame: the test image, or a capture frame.
        ID3D11ShaderResourceView* srcSRV = nullptr;
        int srcW = 0, srcH = 0;
        bool capSizeChanged = false;
        bool gotFrame = false;   // a new capture frame arrived this iteration
        if (app.source == SourceKind::TestImage)
        {
            srcSRV = app.weaver.SourceSRV();
            srcW   = app.weaver.SourceWidth();
            srcH   = app.weaver.SourceHeight();
        }
        else if (app.capture.IsActive())
        {
            // For passthrough, weave only the region of the monitor beneath the viewer.
            if (app.source == SourceKind::CaptureMonitor && app.capture.FrameWidth() > 0)
            {
                RECT r = PassthroughRegion(app, app.hwnd);
                app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
            gotFrame = app.capture.Update(capSizeChanged);   // refresh the capture texture in place
            srcSRV = app.capture.SRV();
            srcW   = app.capture.Width();
            srcH   = app.capture.Height();
        }

        // FAST PATH: a live source already in full side-by-side layout (no eye swap,
        // no convergence shift) is identical to what the converter would output — so
        // feed the captured texture STRAIGHT to the weaver and skip the conversion
        // pass entirely. Lowest latency / least GPU contention, which matters for
        // games. (The capture texture updates in place each frame, so the weaver's
        // input view only needs re-registering on a rebind or a resize.)
        const bool liveSource  = (app.source != SourceKind::TestImage);
        const bool temporalFmt = (app.format == StereoFormat::Pulfrich ||
                                  app.format == StereoFormat::FrameSequential);
        const bool noConv      = (app.convergence < 1e-4f && app.convergence > -1e-4f);
        const bool identitySBS = liveSource && app.format == StereoFormat::FullSBS &&
                                 !app.swapEyes && noConv;

        if (identitySBS)
        {
            if (srcSRV && srcW > 0 && srcH > 0 && (app.captureRebind || capSizeChanged))
            {
                app.weaver.SetInputView(srcSRV, srcW / 2, srcH, app.capture.SRVFormat());
                app.captureRebind = false;
            }
        }
        // Otherwise convert the source into a side-by-side texture and feed THAT to
        // the weaver. Re-run the (potentially heavy) conversion ONLY when its output
        // can change: a NEW live-capture frame arrived, Pulfrich is temporal, or a
        // setting changed (captureRebind). When the captured content is unchanged we
        // skip the convert and re-weave the cached SBS — the weave still tracks the
        // head every frame, but we don't burn GPU re-converting identical pixels.
        else if (srcSRV && srcW > 0 && srcH > 0 && ((liveSource && gotFrame) || temporalFmt || app.captureRebind))
        {
            app.converter.SetFormat(app.format, app.swapEyes, app.anaglyphCombo, app.anaglyphMode);
            app.converter.SetConvergence(app.convergence * 0.03f);   // slider -1..1 -> ±3% width per eye
            {
                int ndN = 0; const NdLevel* nd = PulfrichNdLevels(ndN);
                const float trans = nd[(app.pulfrichNd >= 0 && app.pulfrichNd < ndN) ? app.pulfrichNd : 0].transmission;
                // Affected eye is always the right pane; Swap eyes moves it to the left.
                app.converter.SetPulfrich(app.pulfrichMode, 1, trans, app.pulfrichDelay);
            }
            {
                int fpN = 0; const FramePackPreset* fps = FramePackPresets(fpN);
                const FramePackPreset& fp = fps[(app.framePackMode >= 0 && app.framePackMode < fpN) ? app.framePackMode : 0];
                app.converter.SetFramePacking(fp.eyeFrac, fp.gapFrac);
            }
            bool resized = false;
            if (app.converter.Convert(srcSRV, srcW, srcH, resized) && (resized || app.captureRebind))
            {
                app.weaver.SetInputView(app.converter.OutputSRV(), app.converter.OutputPerEyeWidth(),
                                        app.converter.OutputHeight(), app.converter.OutputFormat());
                app.captureRebind = false;
            }
        }

        app.renderer.BindAndClearBackBuffer();
        app.weaver.Weave();
        app.renderer.Present(false);   // no-vsync: lowest latency (VRR absorbs tearing)
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        AppState* app = g_app;

        switch (msg)
        {
        case WM_APP_TRAY:
            if (app && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU))
            {
                MenuState ms{ app->weavingEnabled, app->mode, app->source,
                              app->format, app->swapEyes, app->anaglyphCombo, app->anaglyphMode,
                              app->pulfrichMode, app->pulfrichDelay, app->pulfrichNd,
                              app->framePackMode };
                app->tray.ShowContextMenu(hwnd, ms);
            }
            else if (app && LOWORD(lParam) == WM_LBUTTONUP)
            {
                app->gui.Toggle();   // left-click opens/closes the control panel
            }
            return 0;

        case WM_APP_GUI_CAPTURE_WINDOW:   // GUI window-picker chose a window to make 3D
            if (app) UseWindow(*app, reinterpret_cast<HWND>(lParam));
            return 0;

        case WM_COMMAND:
        {
            if (!app) break;
            const UINT cmd = LOWORD(wParam);

            // Window-list items occupy a command-id range.
            if (cmd >= ID_TRAY_SRC_WINDOW_BASE && cmd <= ID_TRAY_SRC_WINDOW_MAX)
            {
                UseWindow(*app, app->tray.WindowAt(cmd - ID_TRAY_SRC_WINDOW_BASE));
                return 0;
            }
            // 3D-format items occupy another range.
            if (cmd >= ID_TRAY_FMT_BASE && cmd <= ID_TRAY_FMT_MAX)
            {
                int n = 0;
                const StereoFormatEntry* fmts = StereoFormatList(n);
                const int idx = (int)(cmd - ID_TRAY_FMT_BASE);
                if (idx < n) { app->format = fmts[idx].fmt; app->captureRebind = true; EnsureWeavingFormatOnly(*app); }
                return 0;
            }
            // Anaglyph colour combo / decode mode (also selects the Anaglyph format).
            if (cmd >= ID_TRAY_ANA_COMBO_BASE && cmd <= ID_TRAY_ANA_COMBO_MAX)
            {
                app->anaglyphCombo = (int)(cmd - ID_TRAY_ANA_COMBO_BASE);
                app->format = StereoFormat::Anaglyph;
                app->captureRebind = true;
                EnsureWeavingFormatOnly(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_ANA_MODE_BASE && cmd <= ID_TRAY_ANA_MODE_MAX)
            {
                int n = 0; const AnaglyphModeEntry* modes = AnaglyphModeList(n);
                const int idx = (int)(cmd - ID_TRAY_ANA_MODE_BASE);
                if (idx < n) app->anaglyphMode = modes[idx].value;   // menu index -> shader mode value
                app->format = StereoFormat::Anaglyph;
                app->captureRebind = true;
                EnsureWeavingFormatOnly(*app);
                return 0;
            }
            // Pulfrich sub-options (each also selects the Pulfrich format).
            if (cmd >= ID_TRAY_PULF_MODE_BASE && cmd <= ID_TRAY_PULF_MAX)
            {
                if (cmd >= ID_TRAY_PULF_ND_BASE)
                    app->pulfrichNd = (int)(cmd - ID_TRAY_PULF_ND_BASE);
                else if (cmd >= ID_TRAY_PULF_DELAY_BASE)
                    app->pulfrichDelay = (int)(cmd - ID_TRAY_PULF_DELAY_BASE) + 1;
                else
                    app->pulfrichMode = (cmd == ID_TRAY_PULF_MODE_BASE) ? PulfrichMode::TimeDelay
                                                                        : PulfrichMode::NDFilter;
                app->format = StereoFormat::Pulfrich;
                app->captureRebind = true;
                EnsureWeavingFormatOnly(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_FP_BASE && cmd <= ID_TRAY_FP_MAX)
            {
                app->framePackMode = (int)(cmd - ID_TRAY_FP_BASE);
                app->format = StereoFormat::FramePacking;
                app->captureRebind = true;
                EnsureWeavingFormatOnly(*app);
                return 0;
            }

            switch (cmd)
            {
            case ID_TRAY_TOGGLE_WEAVE: SetWeaving(*app, !app->weavingEnabled); return 0;
            case ID_TRAY_MODE_FULLSCREEN:
                app->mode = OutputMode::Fullscreen;
                // Fullscreen the test image is pointless — weave the actual screen
                // (passthrough). A live window capture stays its own source so it can
                // be fullscreened (the low-latency flip path).
                if (app->source == SourceKind::TestImage)
                    UsePassthrough(*app);
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_MODE_WINDOWED:
                app->mode = OutputMode::Windowed;
                if (app->weavingEnabled) ApplyMode(*app);
                return 0;
            case ID_TRAY_MODE_OVERLAY:
                app->mode = OutputMode::WindowOverlay;
                if (app->weavingEnabled) ApplyMode(*app);
                return 0;
            case ID_TRAY_CAPTURE_FOREGROUND: CaptureForeground(*app); return 0;
            case ID_TRAY_SWAP_EYES: app->swapEyes = !app->swapEyes; app->captureRebind = true; return 0;
            case ID_TRAY_DETECT: DetectFormat(*app); return 0;
            case ID_TRAY_SRC_MONITOR:
                UsePassthrough(*app);
                app->mode = OutputMode::Fullscreen;   // Monitor always means fullscreen
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_LOOKING_GLASS:
                UsePassthrough(*app);
                app->mode = OutputMode::LookingGlass;
                app->loupeInteractive = false;  // click-through by default; Ctrl+Alt to move
                EnsureWeaving(*app);
                return 0;
            case ID_TRAY_EXIT: DestroyWindow(hwnd); return 0;
            }
            break;
        }

        case WM_HOTKEY:
            if (!app) break;
            if (wParam == kHotkeyToggle) SetWeaving(*app, !app->weavingEnabled);
            else if (wParam == kHotkeyMode)
            {
                // Toggle Fullscreen <-> Looking Glass (both weave the monitor).
                if (app->source != SourceKind::CaptureMonitor) UsePassthrough(*app);
                if (app->mode == OutputMode::LookingGlass)
                    app->mode = OutputMode::Fullscreen;
                else { app->mode = OutputMode::LookingGlass; app->loupeInteractive = false; }
                EnsureWeaving(*app);
            }
            else if (wParam == kHotkeyCapture)
            {
                // Make the active window 3D; press again to turn it back off.
                if (app->mode == OutputMode::WindowOverlay && app->weavingEnabled)
                    SetWeaving(*app, false);
                else
                    CaptureForeground(*app);
            }
            // else if (wParam == kHotkeyDetect)  DetectFormat(*app); // auto-detect disabled
            return 0;

        case WM_SIZE:
            if (app && wParam != SIZE_MINIMIZED)
                app->renderer.Resize(LOWORD(lParam), HIWORD(lParam));
            return 0;

        // While the user drags/resizes the window, the modal move loop blocks our
        // render loop — which stops weave() and freezes head-tracked weaving. Keep
        // rendering from a timer during the move.
        case WM_ENTERSIZEMOVE:
            if (app) app->loupeDragging = true;
            SetTimer(hwnd, kRenderTimer, 8, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app) app->loupeDragging = false;
            KillTimer(hwnd, kRenderTimer);
            return 0;
        case WM_TIMER:
            if (app && wParam == kRenderTimer)
                RenderFrame(*app);
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 200;
            mmi->ptMinTrackSize.y = 200;
            return 0;
        }




        case WM_CLOSE:
            // Closing the window just pauses weaving and hides to the tray.
            if (app) SetWeaving(*app, false);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // we fully draw every frame
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Per-Monitor-Aware v2: unlike v1, Windows auto-scales the NON-CLIENT area
    // (title bar) per monitor — so the GUI's title bar no longer stays huge when
    // dragged from a 4K/150% display to a 1440p/100% one. Falls back to v1.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Testing aid: "-noexclude" leaves the window visible to screen capture.
    const bool excludeFromCapture = !(lpCmdLine && strstr(lpCmdLine, "-noexclude"));

    AppState app;
    g_app = &app;

    // Connect to the SR service.
    if (!app.weaver.CreateContext(10.0))
    {
        ShowError("Could not connect to the Simulated Reality service.\n"
                  "Make sure the SR Platform runtime is installed and running.");
        return 1;
    }

    // Decide where the output window lives (the SR display if available).
    app.srDisplayRect = ResolveTargetRect(app.weaver);

    // Register window class.
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon         = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_TRAY),
                                         IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExA(&wc))
    {
        ShowError("Failed to register window class.");
        return 2;
    }

    // Create the output window on the SR display.
    const RECT& d = app.srDisplayRect;
    app.hwnd = CreateWindowExA(
        0, kWindowClass, kWindowTitle, WS_POPUP,
        d.left, d.top, d.right - d.left, d.bottom - d.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!app.hwnd)
    {
        ShowError("Failed to create output window.");
        return 3;
    }

    // Exclude our own window from screen capture so passthrough / monitor weaving
    // doesn't recursively capture its own output (no feedback loop).
    if (excludeFromCapture)
        SetWindowDisplayAffinity(app.hwnd, WDA_EXCLUDEFROMCAPTURE);

    // Set up Direct3D. The weaver/SR session is started on demand when weaving is
    // enabled; the default source is the monitor (passthrough), so no initial image.
    if (!app.renderer.Initialize(app.hwnd))
        return 4;

    // Initialize live capture (this is the default source).
    const bool capInit = app.capture.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: capture.Initialize=%d", capInit ? 1 : 0);

    // Initialize the format-conversion stage (capture/image -> SBS for the weaver).
    if (!app.converter.Initialize(app.renderer.Device(), app.renderer.Context()))
    {
        Log("WinMain: converter.Initialize FAILED (exit 7)");
        return 7;
    }
    Log("WinMain: converter.Initialize OK");
    app.detector.Initialize(app.renderer.Device(), app.renderer.Context());  // optional
    app.captureRebind = true;   // bind the weaver to the converter output on the first frame

    // Control GUI (left-click the tray icon). Shares the D3D11 device; lives in its
    // own top-level window so it can sit on any monitor with its own taskbar button.
    if (!app.gui.Init(app.hwnd, app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: GUI init failed (control panel disabled)");

    // Tray icon + global hotkeys.
    app.tray.Add(app.hwnd, "SR Loom — paused (SR off)");
    RegisterHotKey(app.hwnd, kHotkeyToggle,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    RegisterHotKey(app.hwnd, kHotkeyMode,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(app.hwnd, kHotkeyCapture, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    // RegisterHotKey(app.hwnd, kHotkeyDetect,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D'); // auto-detect disabled

    // Start idle: release the SR session used to query the display rect so the
    // lens and camera stay off until the user enables weaving (Ctrl+Alt+W).
    app.weavingEnabled = false;
    app.weaver.StopSR();
    Log("WinMain: ready — idle in tray, entering main loop");

    bool running = true;
    while (running)
    {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (running)
        {
            UpdateLastForeground(app);   // remember the user's active window for "make 3D"
            RenderFrame(app);

            // Render the control panel when it's open, and pick up its convergence slider.
            if (app.gui.IsVisible())
            {
                GuiState gs;
                gs.weaving       = app.weavingEnabled;
                gs.mode          = app.mode;
                gs.source        = app.source;
                gs.format        = app.format;
                gs.swapEyes      = app.swapEyes;
                gs.anaglyphCombo = app.anaglyphCombo;
                gs.anaglyphMode  = app.anaglyphMode;
                gs.convergence   = app.convergence;
                gs.pulfrichMode  = (int)app.pulfrichMode;
                gs.pulfrichDelay = app.pulfrichDelay;
                gs.pulfrichNd    = app.pulfrichNd;
                gs.framePackMode = app.framePackMode;
                // What's being weaved, for the GUI's collapsed summary line.
                if (app.source == SourceKind::CaptureWindow && app.sourceWindow && IsWindow(app.sourceWindow))
                {
                    if (GetWindowTextA(app.sourceWindow, gs.sourceName, (int)sizeof(gs.sourceName)) <= 0)
                        strncpy_s(gs.sourceName, "Window", _TRUNCATE);
                }
                else
                    strncpy_s(gs.sourceName, "Monitor", _TRUNCATE);
                if (app.gui.Render(gs))
                {
                    app.convergence   = gs.convergence;
                    app.captureRebind = true;   // re-run the conversion with the new convergence
                }
            }
        }
    }

    UnregisterHotKey(app.hwnd, kHotkeyToggle);
    UnregisterHotKey(app.hwnd, kHotkeyMode);
    UnregisterHotKey(app.hwnd, kHotkeyCapture);
    // UnregisterHotKey(app.hwnd, kHotkeyDetect); // auto-detect disabled
    app.tray.Remove();
    app.gui.Shutdown();
    app.detector.Shutdown();
    app.converter.Shutdown();
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    g_app = nullptr;
    return 0;
}
