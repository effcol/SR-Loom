// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "Capture.h"
#include "Converter.h"
#include "resource.h"

#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")

using namespace srw;

namespace
{
    constexpr char  kWindowClass[] = "SRWeaverWindow";
    constexpr char  kWindowTitle[] = "SR Weaver";
    constexpr int   kHotkeyToggle  = 1;   // Ctrl+Alt+W : enable/disable weaving
    constexpr int   kHotkeyMode    = 2;   // Ctrl+Alt+F : fullscreen/windowed
    constexpr int   kHotkeyCapture = 3;   // Ctrl+Alt+C : make active window 3D
    constexpr UINT  kRenderTimer   = 1;   // drives rendering during modal move/resize

    struct AppState
    {
        HWND       hwnd          = nullptr;
        Renderer     renderer;
        SRWeaver     weaver;
        TrayIcon     tray;
        Capture      capture;
        Converter    converter;
        bool         weavingEnabled = true;
        OutputMode   mode           = OutputMode::Fullscreen;
        SourceKind   source         = SourceKind::TestImage;
        StereoFormat format         = StereoFormat::FullSBS;  // source stereo layout
        bool         swapEyes       = false;
        int          anaglyphCombo  = 0;   // 0..5 colour combination
        int          anaglyphMode   = 0;   // 0..3 decode mode (colour by default)
        HWND       sourceWindow   = nullptr; // tracked window in WindowOverlay mode
        bool       loupeInteractive = false; // looking glass: currently grabbable (not click-through)
        bool       loupeDragging  = false;   // looking glass: in a move/resize loop
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
            exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            zorder  = HWND_TOPMOST;
            rect    = { d.left, d.top, d.left + 1280, d.top + 720 };
            if (app.sourceWindow && IsWindow(app.sourceWindow))
                GetWindowRect(app.sourceWindow, &rect);
            break;

        case OutputMode::LookingGlass:
        {
            // Visible frame for dragging/resizing; click-through (locked) toggles
            // layered+transparent without changing the frame (Ctrl+Alt+G).
            const int w = 960, h = 600;
            const int x = d.left + (dw - w) / 2;
            const int y = d.top  + (dh - h) / 2;
            // Normal window chrome (title bar + resize edges). Click-through on
            // the glass by default; UpdateLoupeInteractivity makes it grabbable
            // while the cursor is over the chrome or during a move/resize.
            style   = WS_OVERLAPPEDWINDOW;
            exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT;
            zorder  = HWND_TOPMOST;
            rect    = { x, y, x + w, y + h };
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
    }

    // The looking glass passes clicks through the glass, but becomes grabbable
    // when the cursor is over its chrome (title bar / resize edges) or while a
    // move/resize is in progress — then returns to click-through. The cursor is
    // polled directly so this works even while the window is click-through.
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

        RECT r{}, cur{};
        GetWindowRect(src, &r);
        GetWindowRect(app.hwnd, &cur);
        if (!EqualRect(&r, &cur))
        {
            SetWindowPos(app.hwnd, HWND_TOPMOST, r.left, r.top,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);  // WM_SIZE resizes the swap chain
        }
        if (!IsWindowVisible(app.hwnd))
            ShowWindow(app.hwnd, SW_SHOWNOACTIVATE);
    }

    void SetWeaving(AppState& app, bool enable)
    {
        app.weavingEnabled = enable;
        if (enable)
        {
            ApplyMode(app);
            ShowWindow(app.hwnd, SW_SHOW);
        }
        else
        {
            ShowWindow(app.hwnd, SW_HIDE);
        }
        app.tray.SetTooltip(enable ? "SR Weaver — weaving" : "SR Weaver — paused");
    }

    // --- Source selection -------------------------------------------------

    void UseTestImage(AppState& app)
    {
        app.capture.Stop();
        app.sourceWindow = nullptr;
        app.weaver.SetStereoImageFromFile(app.renderer.Device(), ExePath("test_sbs.jpg").c_str(),
                                          StereoFormat::FullSBS,
                                          app.renderer.BackBufferFormat());
        app.source = SourceKind::TestImage;
        app.captureRebind = true;   // rebind the weaver to the converter output
        if (app.mode == OutputMode::WindowOverlay)   // overlay only makes sense for a window
            app.mode = OutputMode::Fullscreen;
        if (app.weavingEnabled) ApplyMode(app);
    }

    // Capture a window and present it as an in-place 3D overlay tracking that window.
    void UseWindow(AppState& app, HWND target)
    {
        if (target && app.capture.StartWindow(target))
        {
            app.source       = SourceKind::CaptureWindow;
            app.sourceWindow = target;
            app.captureRebind = true;   // re-register the SRV once frames arrive
            app.mode = OutputMode::WindowOverlay;
            if (app.weavingEnabled) ApplyMode(app);
        }
    }

    // Grab whatever window is currently focused (Ctrl+Alt+C).
    void CaptureForeground(AppState& app)
    {
        HWND fg = GetForegroundWindow();
        Log("CaptureForeground: fg=%p app.hwnd=%p", (void*)fg, (void*)app.hwnd);
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

    // Map our window's client area to a rectangle in the captured frame's pixels.
    RECT PassthroughRegion(AppState& app)
    {
        const double fw = app.capture.FrameWidth();
        const double fh = app.capture.FrameHeight();

        HMONITOR mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(mon, &mi);
        const RECT m = mi.rcMonitor;
        const double sx = fw / (m.right - m.left);
        const double sy = fh / (m.bottom - m.top);

        // Client area in screen coordinates.
        RECT c{};
        GetClientRect(app.hwnd, &c);
        POINT tl{ c.left, c.top }, br{ c.right, c.bottom };
        ClientToScreen(app.hwnd, &tl);
        ClientToScreen(app.hwnd, &br);

        RECT r;
        r.left   = (LONG)((tl.x - m.left) * sx);
        r.top    = (LONG)((tl.y - m.top) * sy);
        r.right  = (LONG)((br.x - m.left) * sx);
        r.bottom = (LONG)((br.y - m.top) * sy);
        return r;
    }

    void RenderFrame(AppState& app)
    {
        if (!app.weavingEnabled || !app.renderer.IsValid())
        {
            Sleep(10);
            return;
        }

        UpdateOverlayTracking(app);     // follow the source window in overlay mode
        UpdateLoupeInteractivity(app);  // Ctrl+Alt to grab/move the looking glass

        if (IsIconic(app.hwnd))
        {
            Sleep(10);
            return;
        }

        // Resolve the current source frame: the test image, or a capture frame.
        ID3D11ShaderResourceView* srcSRV = nullptr;
        int srcW = 0, srcH = 0;
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
                RECT r = PassthroughRegion(app);
                app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
            bool sizeChanged = false;
            app.capture.Update(sizeChanged);   // refresh the capture texture in place
            srcSRV = app.capture.SRV();
            srcW   = app.capture.Width();
            srcH   = app.capture.Height();
        }

        // Convert the source into a side-by-side texture and feed it to the weaver.
        if (srcSRV && srcW > 0 && srcH > 0)
        {
            app.converter.SetFormat(app.format, app.swapEyes, app.anaglyphCombo, app.anaglyphMode);
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
        app.renderer.Present(true);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        AppState* app = g_app;

        switch (msg)
        {
        case WM_APP_TRAY:
            if (app && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU))
                app->tray.ShowContextMenu(hwnd, app->weavingEnabled, app->mode, app->source,
                                          app->format, app->swapEyes,
                                          app->anaglyphCombo, app->anaglyphMode);
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
                if (idx < n) { app->format = fmts[idx].fmt; app->captureRebind = true; }
                return 0;
            }
            // Anaglyph colour combo / decode mode (also selects the Anaglyph format).
            if (cmd >= ID_TRAY_ANA_COMBO_BASE && cmd <= ID_TRAY_ANA_COMBO_MAX)
            {
                app->anaglyphCombo = (int)(cmd - ID_TRAY_ANA_COMBO_BASE);
                app->format = StereoFormat::Anaglyph;
                app->captureRebind = true;
                return 0;
            }
            if (cmd >= ID_TRAY_ANA_MODE_BASE && cmd <= ID_TRAY_ANA_MODE_MAX)
            {
                app->anaglyphMode = (int)(cmd - ID_TRAY_ANA_MODE_BASE);
                app->format = StereoFormat::Anaglyph;
                app->captureRebind = true;
                return 0;
            }

            switch (cmd)
            {
            case ID_TRAY_TOGGLE_WEAVE: SetWeaving(*app, !app->weavingEnabled); return 0;
            case ID_TRAY_MODE_FULLSCREEN:
                app->mode = OutputMode::Fullscreen;
                if (app->weavingEnabled) ApplyMode(*app);
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
            case ID_TRAY_SRC_TESTIMAGE: UseTestImage(*app); return 0;
            case ID_TRAY_SRC_MONITOR:
                UsePassthrough(*app);
                if (app->mode == OutputMode::WindowOverlay) app->mode = OutputMode::Fullscreen;
                if (app->weavingEnabled) ApplyMode(*app);
                return 0;
            case ID_TRAY_LOOKING_GLASS:
                UsePassthrough(*app);
                app->mode = OutputMode::LookingGlass;
                app->loupeInteractive = false;  // click-through by default; Ctrl+Alt to move
                if (app->weavingEnabled) ApplyMode(*app);
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
                app->mode = (app->mode == OutputMode::Fullscreen)
                          ? OutputMode::Windowed : OutputMode::Fullscreen;
                if (app->weavingEnabled) ApplyMode(*app);
            }
            else if (wParam == kHotkeyCapture) CaptureForeground(*app);
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

    // Set up Direct3D + the weaver + the static SBS test image.
    if (!app.renderer.Initialize(app.hwnd))
        return 4;
    if (!app.weaver.CreateWeaver(app.renderer.Context(), app.hwnd))
        return 5;
    if (!app.weaver.SetStereoImageFromFile(app.renderer.Device(), ExePath("test_sbs.jpg").c_str(),
                                           StereoFormat::FullSBS,
                                           app.renderer.BackBufferFormat()))
        return 6;

    // Initialize live capture (optional — the test image still works without it).
    const bool capInit = app.capture.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: capture.Initialize=%d", capInit ? 1 : 0);

    // Initialize the format-conversion stage (capture/image -> SBS for the weaver).
    if (!app.converter.Initialize(app.renderer.Device(), app.renderer.Context()))
        return 7;
    app.captureRebind = true;   // bind the weaver to the converter output on the first frame

    // Tray icon + global hotkeys.
    app.tray.Add(app.hwnd, "SR Weaver — weaving");
    RegisterHotKey(app.hwnd, kHotkeyToggle,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    RegisterHotKey(app.hwnd, kHotkeyMode,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(app.hwnd, kHotkeyCapture, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');

    // Show and weave.
    ApplyMode(app);

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
            RenderFrame(app);
    }

    UnregisterHotKey(app.hwnd, kHotkeyToggle);
    UnregisterHotKey(app.hwnd, kHotkeyMode);
    UnregisterHotKey(app.hwnd, kHotkeyCapture);
    app.tray.Remove();
    app.converter.Shutdown();
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    g_app = nullptr;
    return 0;
}
