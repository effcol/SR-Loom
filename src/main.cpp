// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "Capture.h"
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
        Renderer   renderer;
        SRWeaver   weaver;
        TrayIcon   tray;
        Capture    capture;
        bool       weavingEnabled = true;
        OutputMode mode           = OutputMode::Fullscreen;
        SourceKind source         = SourceKind::TestImage;
        HWND       sourceWindow   = nullptr; // tracked window in WindowOverlay mode
        bool       loupeInteractive = true;  // looking glass: draggable (true) vs click-through (false)
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
            // Borderless, click-through by default (layered+transparent). Hold
            // Ctrl+Alt to drag (anywhere) and mouse-wheel to resize; releasing
            // restores click-through.
            style   = WS_POPUP;
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

    // The looking glass is click-through by default. While Ctrl+Alt is held it
    // becomes grabbable so the frame can be dragged/resized; releasing restores
    // click-through. This keeps passthrough "always on" except while moving it.
    void UpdateLoupeInteractivity(AppState& app)
    {
        if (app.mode != OutputMode::LookingGlass) return;

        const bool grab = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                          (GetAsyncKeyState(VK_MENU)    & 0x8000);
        if (grab == app.loupeInteractive) return;   // loupeInteractive == "grabbable now"
        app.loupeInteractive = grab;

        LONG_PTR ex = WS_EX_TOPMOST | WS_EX_LAYERED | (grab ? 0 : WS_EX_TRANSPARENT);
        SetWindowLongPtr(app.hwnd, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(app.hwnd, 0, 255, LWA_ALPHA);
        SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
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

        // For passthrough, weave only the region of the monitor beneath the viewer.
        if (app.source == SourceKind::CaptureMonitor && app.capture.FrameWidth() > 0)
        {
            RECT r = PassthroughRegion(app);
            app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
        }

        // Pull the newest captured frame and (re)point the weaver at it on resize.
        if (app.source != SourceKind::TestImage && app.capture.IsActive())
        {
            bool sizeChanged = false;
            if (app.capture.Update(sizeChanged) && (sizeChanged || app.captureRebind))
            {
                Log("RenderFrame: rebind weaver to capture %dx%d",
                    app.capture.Width(), app.capture.Height());
                app.weaver.SetInputView(app.capture.SRV(), app.capture.Width() / 2,
                                        app.capture.Height(), app.capture.SRVFormat());
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
                app->tray.ShowContextMenu(hwnd, app->weavingEnabled, app->mode, app->source);
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
            SetTimer(hwnd, kRenderTimer, 8, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
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

        // While the looking glass is grabbable (Ctrl+Alt held), drag it from
        // anywhere and resize it with the mouse wheel.
        case WM_LBUTTONDOWN:
            if (app && app->mode == OutputMode::LookingGlass && app->loupeInteractive)
            {
                ReleaseCapture();
                SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (app && app->mode == OutputMode::LookingGlass && app->loupeInteractive)
            {
                RECT r{};
                GetWindowRect(hwnd, &r);
                const int w = r.right - r.left, h = r.bottom - r.top;
                const int step = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? 1 : -1;
                int nw = w + step * w / 10; if (nw < 200) nw = 200;
                int nh = h + step * h / 10; if (nh < 150) nh = 150;
                // Resize about the center.
                const int nx = r.left + (w - nw) / 2;
                const int ny = r.top  + (h - nh) / 2;
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, nw, nh, SWP_NOACTIVATE);
                return 0;
            }
            break;



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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

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
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    g_app = nullptr;
    return 0;
}
