// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
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

    struct AppState
    {
        HWND       hwnd          = nullptr;
        Renderer   renderer;
        SRWeaver   weaver;
        TrayIcon   tray;
        bool       weavingEnabled = true;
        OutputMode mode           = OutputMode::Fullscreen;
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
    void ApplyMode(AppState& app)
    {
        const RECT& d = app.srDisplayRect;
        const int dw = d.right - d.left;
        const int dh = d.bottom - d.top;

        if (app.mode == OutputMode::Fullscreen)
        {
            SetWindowLongPtr(app.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(app.hwnd, HWND_TOP, d.left, d.top, dw, dh,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            const int w = 1280, h = 720;
            const int x = d.left + (dw - w) / 2;
            const int y = d.top  + (dh - h) / 2;
            SetWindowLongPtr(app.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(app.hwnd, HWND_NOTOPMOST, x, y, w, h,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
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

    void RenderFrame(AppState& app)
    {
        if (!app.weavingEnabled || IsIconic(app.hwnd) || !app.renderer.IsValid())
        {
            Sleep(10);
            return;
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
                app->tray.ShowContextMenu(hwnd, app->weavingEnabled, app->mode);
            return 0;

        case WM_COMMAND:
            if (!app) break;
            switch (LOWORD(wParam))
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
            case ID_TRAY_EXIT: DestroyWindow(hwnd); return 0;
            }
            break;

        case WM_HOTKEY:
            if (!app) break;
            if (wParam == kHotkeyToggle) SetWeaving(*app, !app->weavingEnabled);
            else if (wParam == kHotkeyMode)
            {
                app->mode = (app->mode == OutputMode::Fullscreen)
                          ? OutputMode::Windowed : OutputMode::Fullscreen;
                if (app->weavingEnabled) ApplyMode(*app);
            }
            return 0;

        case WM_SIZE:
            if (app && wParam != SIZE_MINIMIZED)
                app->renderer.Resize(LOWORD(lParam), HIWORD(lParam));
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

    // Set up Direct3D + the weaver + the static SBS test image.
    if (!app.renderer.Initialize(app.hwnd))
        return 4;
    if (!app.weaver.CreateWeaver(app.renderer.Context(), app.hwnd))
        return 5;
    if (!app.weaver.SetStereoImageFromFile(app.renderer.Device(), "test_sbs.jpg",
                                           StereoFormat::FullSBS,
                                           app.renderer.BackBufferFormat()))
        return 6;

    // Tray icon + global hotkeys.
    app.tray.Add(app.hwnd, "SR Weaver — weaving");
    RegisterHotKey(app.hwnd, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    RegisterHotKey(app.hwnd, kHotkeyMode,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');

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
    app.tray.Remove();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    g_app = nullptr;
    return 0;
}
