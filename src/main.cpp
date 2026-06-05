// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "Capture.h"
#include "CaptureDXGI.h"
#include "Converter.h"
#include "Detector.h"
#include "Gui.h"
#include "Settings.h"
#include "VideoSource.h"
#include "resource.h"

#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>      // DragAcceptFiles, DragQueryFile, DragFinish
#include <cmath>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")

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
        Capture      capture;        // WGC: default; only API that can do per-window + window-exclusion
        CaptureDXGI  captureDxgi;    // Output Duplication fallback for exclusive-fullscreen sources
        Converter    converter;
        Detector     detector;
        Gui          gui;
        VideoSource  video;          // active video file source (mp4/mov/etc), if any
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
        // Quilt source (Looking Glass): cols x rows grid; view indices pick the L/R pair.
        // Defaults are the centre pair of an 8x6 quilt; auto-set on file load from a
        // "_qsCxR_" filename token, falling back to these.
        int          quiltCols      = 8;
        int          quiltRows      = 6;
        int          quiltLeftIdx   = 23;  // centre - 1 (0-based) for 8x6 = 48 views
        int          quiltRightIdx  = 24;  // centre
        // Head-tracking smoothing state for Quilt view selection. EMA on the
        // raw head.x position takes the high-frequency tracker noise out before
        // it ever drives the view choice -- so a still head stays put. View
        // BLENDING (below) replaces the old hysteresis-snap: instead of locking
        // to one integer view, we cross-fade between the two nearest views in
        // the shader using the fractional position, which is what Looking Glass
        // does between physical lenticular columns -- no jagged step.
        double       headXEMA          = 0.0;
        bool         headXEMAInit      = false;
        float        quiltLeftBlend    = 0.0f;   // L pane: blend between quiltLeftIdx and the next view
        float        quiltRightBlend   = 0.0f;   // R pane: blend between quiltRightIdx and the next view
        double       srMmPerPx         = 0.0;    // SR display physical-to-pixel scale (mm/px), 0 = unknown
        std::string  lastTestImagePath;          // last image fed to SetStereoImageFromFile
        HWND       sourceWindow   = nullptr; // tracked window in WindowOverlay mode
        HWND       lastForeground = nullptr; // last real foreground window (for "make active window 3D")
        bool       loupeInteractive = false; // looking glass: currently grabbable (not click-through)
        bool       loupeDragging  = false;   // looking glass: in a move/resize loop
        bool       loupeActive    = false;   // looking glass shown (keep its position across re-applies)
        bool       captureRebind  = false;   // re-register SRV on next frame
        RECT       srDisplayRect  = { 0, 0, 1920, 1080 };  // filled from SR SDK
        HMONITOR   sourceMonitor  = nullptr; // monitor being captured (passthrough / display picker)
        bool       foreignDisplay = false;   // capturing a NON-SR display: weave the whole frame (no crop)
        // Exclusive-fullscreen fallback state. WGC stops delivering frames when the
        // captured monitor hosts a true exclusive-fullscreen app; we switch to DXGI
        // Output Duplication for those, then back to WGC when fullscreen ends. Only
        // engaged for foreign-display monitor capture (DXGI on the SR display itself
        // would capture our own overlay -> feedback loop).
        bool       dxgiActive     = false;
        int        wgcStuck       = 0;     // consecutive iterations with no new WGC frame
        int        dxgiCheckCtr   = 0;     // polls the foreground-fullscreen state every ~30 ticks
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

    // Heuristic: is the current foreground window covering the entirety of `target`?
    // Used to decide whether to keep the DXGI Output Duplication fallback engaged.
    // Catches both true exclusive fullscreen and borderless-fullscreen; we don't
    // actually need to distinguish them -- DXGI captures both equally well, and the
    // signal that we should swap BACK to WGC is "the foreground stopped being
    // fullscreen-sized on this monitor".
    bool ForegroundCoversMonitor(HMONITOR target)
    {
        if (!target) return false;
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        if (MonitorFromWindow(fg, MONITOR_DEFAULTTONULL) != target) return false;
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(target, &mi)) return false;
        RECT wr{};
        if (!GetWindowRect(fg, &wr)) return false;
        const LONG tol = 2;   // a few px of slop for borderless windows that don't quite hit the edge
        return wr.left  <= mi.rcMonitor.left  + tol &&
               wr.top   <= mi.rcMonitor.top   + tol &&
               wr.right >= mi.rcMonitor.right - tol &&
               wr.bottom >= mi.rcMonitor.bottom - tol;
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

    // ----- Fullscreen-controls overlay --------------------------------------
    // A tiny topmost layered popup with three custom-drawn buttons (minimise /
    // switch to windowed / close) that appears in the top-right of the SR
    // display whenever the user clicks inside the fullscreen weave. Auto-hides
    // after a few seconds of inactivity. Painted with GDI on its own thread of
    // messages so it doesn't interfere with the weave path.
    // Warm-dark palette matched to Gui.cpp's "Warm Dark" theme so the floating
    // overlays sit visually with the rest of the app (Reeder-ish: warm tones,
    // low-contrast surfaces, rounded corners). Shared by both overlays.
    constexpr COLORREF kFsBg       = RGB(28, 27, 25);   // window bg
    constexpr COLORREF kFsBtnBg    = RGB(40, 38, 34);   // button surface
    constexpr COLORREF kFsBtnHover = RGB(60, 56, 50);   // button hover
    constexpr COLORREF kFsCloseHot = RGB(210, 105, 74); // warm accent (close hover)
    constexpr COLORREF kFsText     = RGB(232, 228, 220);
    constexpr COLORREF kFsDim      = RGB(160, 152, 140);

    constexpr char  kFsCtrlClass[] = "SRLoomFsControls";
    constexpr int   kFsBtnCount    = 3;          // [0] minimise, [1] switch to Windowed, [2] close
    constexpr int   kFsBtnW        = 64;
    constexpr int   kFsBtnH        = 52;
    constexpr int   kFsCtrlPad     = 10;
    constexpr int   kFsCtrlGap     = 8;
    constexpr int   kFsCtrlRadius  = 10;
    constexpr int   kFsCtrlW       = kFsBtnW * kFsBtnCount + kFsCtrlGap + kFsCtrlPad * 2;
    constexpr int   kFsCtrlH       = kFsBtnH + kFsCtrlPad * 2;
    constexpr UINT  kFsCtrlTimerId = 7;
    constexpr DWORD kFsCtrlHideMs  = 3500;

    // Rounded-rect button paint. No border; hover differentiates by fill.
    void FsPaintRoundButton(HDC dc, const RECT& r, COLORREF fill, int radius)
    {
        HBRUSH fb = CreateSolidBrush(fill);
        HBRUSH oldB = (HBRUSH)SelectObject(dc, fb);
        HPEN   pen = CreatePen(PS_NULL, 0, 0);
        HPEN   oldP = (HPEN)SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
        SelectObject(dc, oldB); DeleteObject(fb);
        SelectObject(dc, oldP); DeleteObject(pen);
    }

    HWND  g_fsCtrl          = nullptr;
    int   g_fsCtrlHover     = -1;
    DWORD g_fsCtrlLastUse   = 0;
    bool  g_fsCtrlTracking  = false;

    RECT FsBtnRect(int i)
    {
        RECT r{};
        r.left   = kFsCtrlPad + i * (kFsBtnW + kFsCtrlGap);
        r.top    = kFsCtrlPad;
        r.right  = r.left + kFsBtnW;
        r.bottom = r.top  + kFsBtnH;
        return r;
    }
    int FsHit(int x, int y)
    {
        for (int i = 0; i < kFsBtnCount; ++i)
        {
            RECT r = FsBtnRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    // Draw the standard Windows title-bar glyphs (minimise, close) using GDI
    // primitives instead of font codepoints — the codepoints depend on which
    // version of Segoe Fluent / MDL2 Assets is installed and aren't reliable.
    // Lines / rects render crisply and look identical to native chrome.
    void DrawFsGlyph(HDC dc, int which, const RECT& r, COLORREF colour)
    {
        const int cx = (r.left + r.right)  / 2;
        const int cy = (r.top  + r.bottom) / 2;
        const int s  = 9;     // half-icon size (~18x18 footprint, large + chunky)
        HPEN pen     = CreatePen(PS_SOLID, 2, colour);   // thicker stroke for visibility
        HPEN oldPen  = (HPEN)SelectObject(dc, pen);
        switch (which)
        {
        case 0:   // minimise: horizontal bar, centred
            MoveToEx(dc, cx - s, cy, nullptr); LineTo(dc, cx + s + 1, cy);
            break;
        case 1:   // restore / switch to windowed: empty rectangle outline
        {
            HBRUSH hollow = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB   = (HBRUSH)SelectObject(dc, hollow);
            Rectangle(dc, cx - s, cy - s, cx + s + 1, cy + s + 1);
            SelectObject(dc, oldB);
            break;
        }
        case 2:   // close: two diagonal lines forming X
            MoveToEx(dc, cx - s, cy - s, nullptr); LineTo(dc, cx + s + 1, cy + s + 1);
            MoveToEx(dc, cx + s, cy - s, nullptr); LineTo(dc, cx - s - 1, cy + s + 1);
            break;
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    LRESULT CALLBACK FsCtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);
            for (int i = 0; i < kFsBtnCount; ++i)
            {
                RECT r = FsBtnRect(i);
                COLORREF fill = kFsBtnBg;
                if (i == g_fsCtrlHover)
                    fill = (i == 2) ? kFsCloseHot : kFsBtnHover;   // close is index 2 now
                FsPaintRoundButton(dc, r, fill, kFsCtrlRadius);
                DrawFsGlyph(dc, i, r, kFsText);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsCtrlTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsCtrlTracking = true;
            }
            const int hit = FsHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit != g_fsCtrlHover) { g_fsCtrlHover = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            g_fsCtrlLastUse = GetTickCount();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsCtrlTracking = false;
            if (g_fsCtrlHover != -1) { g_fsCtrlHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        {
            const int hit = FsHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_fsCtrlLastUse = GetTickCount();
            if (hit >= 0 && g_app && g_app->hwnd)
                PostMessageA(g_app->hwnd, WM_APP_FS_BUTTON, (WPARAM)hit, 0);
            return 0;
        }
        case WM_TIMER:
            if (wp == kFsCtrlTimerId)
            {
                // Auto-hide when the cursor has been away from the controls for
                // a few seconds; keep visible while it's hovering a button.
                if (g_fsCtrlHover == -1 && (GetTickCount() - g_fsCtrlLastUse) > kFsCtrlHideMs)
                    ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsCtrlWindow(HINSTANCE inst)
    {
        if (g_fsCtrl) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsCtrlProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsCtrlClass;
        RegisterClassA(&wc);
        g_fsCtrl = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsCtrlClass, "", WS_POPUP,
            0, 0, kFsCtrlW, kFsCtrlH, nullptr, nullptr, inst, nullptr);
        if (g_fsCtrl)
        {
            SetLayeredWindowAttributes(g_fsCtrl, 0, 225, LWA_ALPHA);
            SetTimer(g_fsCtrl, kFsCtrlTimerId, 250, nullptr);
        }
    }

    // Position helper: top-left or top-right corner of the SR Loom main window's
    // CLIENT area. Anchoring to the window (not the SR display rect) means the
    // overlays sit correctly in BOTH fullscreen and windowed modes, and follow
    // the window when the user drags / resizes it.
    void OverlayCornerScreenPos(HWND mainHwnd, int overlayW, bool leftCorner,
                                int margin, int& outX, int& outY)
    {
        RECT cr; GetClientRect(mainHwnd, &cr);
        POINT tl{ cr.left, cr.top }, tr{ cr.right, cr.top };
        ClientToScreen(mainHwnd, &tl);
        ClientToScreen(mainHwnd, &tr);
        outY = tl.y + margin;
        outX = leftCorner ? (tl.x + margin) : (tr.x - overlayW - margin);
    }

    void ShowFsCtrlOverlay(const AppState& app)
    {
        if (!g_fsCtrl || !app.hwnd) return;
        int x = 0, y = 0;
        OverlayCornerScreenPos(app.hwnd, kFsCtrlW, /*leftCorner=*/false, 12, x, y);
        SetWindowPos(g_fsCtrl, HWND_TOPMOST, x, y, kFsCtrlW, kFsCtrlH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN rgn = CreateRoundRectRgn(0, 0, kFsCtrlW + 1, kFsCtrlH + 1,
                                      kFsCtrlRadius * 2, kFsCtrlRadius * 2);
        SetWindowRgn(g_fsCtrl, rgn, TRUE);
        g_fsCtrlLastUse = GetTickCount();
    }

    void HideFsCtrlOverlay()
    {
        if (g_fsCtrl) ShowWindow(g_fsCtrl, SW_HIDE);
    }

    // ===== Top-left settings overlay ========================================
    // Inline controls for picking an image, format, and (when format == Quilt)
    // the quilt grid cols/rows. Same visual style as the right-corner overlay,
    // but each button opens a native popup menu via TrackPopupMenu — cheap to
    // implement and looks identical to standard Windows menus.
    constexpr char kFsSetClass[]   = "SRLoomFsSettings";
    constexpr int  kFsSetBtnH      = 52;
    constexpr int  kFsSetPad       = 10;
    constexpr int  kFsSetGap       = 8;
    constexpr int  kFsSetRadius    = 10;     // rounded-corner radius
    constexpr UINT kFsSetTimerId   = 8;
    constexpr int  kFsSetBtnLoad   = 0;
    constexpr int  kFsSetBtnFormat = 1;
    constexpr int  kFsSetBtnCols   = 2;
    constexpr int  kFsSetBtnRows   = 3;
    constexpr int  kFsSetBtnAuto   = 4;
    constexpr int  kFsSetWLoad     = 180;
    constexpr int  kFsSetWFormat   = 250;
    constexpr int  kFsSetWCols     = 140;
    constexpr int  kFsSetWRows     = 140;
    constexpr int  kFsSetWAuto     = 130;
    constexpr int  kQuiltColsMax   = 12;
    constexpr int  kQuiltRowsMax   = 9;

    HWND  g_fsSet         = nullptr;
    int   g_fsSetHover    = -1;
    DWORD g_fsSetLastUse  = 0;
    bool  g_fsSetTracking = false;
    int   g_fsSetBtnCount = 2;
    RECT  g_fsSetRects[5] = {};
    int   g_fsSetCalcW    = 0;
    int   g_fsSetCalcH    = 0;

    int FsSetBtnW(int i)
    {
        switch (i) {
        case 0: return kFsSetWLoad;
        case 1: return kFsSetWFormat;
        case 2: return kFsSetWCols;
        case 3: return kFsSetWRows;
        case 4: return kFsSetWAuto;
        }
        return 60;
    }
    const RECT& FsSetBtnRect(int i) { return g_fsSetRects[i]; }

    // Lay out the buttons into rows. If everything fits on one row inside the
    // given max width, that's used; otherwise buttons wrap to additional rows
    // so the overlay stays inside the window even when the window is narrow.
    void FsSetComputeLayout(int btnCount, int maxWidth)
    {
        int x = kFsSetPad;
        int y = kFsSetPad;
        int rowMaxRight = kFsSetPad;
        int maxRightAll = kFsSetPad;
        for (int i = 0; i < btnCount; ++i)
        {
            const int w = FsSetBtnW(i);
            // Wrap when this button wouldn't fit on the current row.
            if (i > 0 && (x + w + kFsSetPad) > maxWidth)
            {
                if (rowMaxRight > maxRightAll) maxRightAll = rowMaxRight;
                x = kFsSetPad;
                y += kFsSetBtnH + kFsSetGap;
                rowMaxRight = kFsSetPad;
            }
            RECT& r = g_fsSetRects[i];
            r.left   = x;
            r.right  = x + w;
            r.top    = y;
            r.bottom = y + kFsSetBtnH;
            x += w + kFsSetGap;
            if (r.right > rowMaxRight) rowMaxRight = r.right;
        }
        if (rowMaxRight > maxRightAll) maxRightAll = rowMaxRight;
        g_fsSetCalcW = maxRightAll + kFsSetPad;
        g_fsSetCalcH = y + kFsSetBtnH + kFsSetPad;
    }

    int FsSetHit(int x, int y)
    {
        for (int i = 0; i < g_fsSetBtnCount; ++i)
        {
            RECT r = FsSetBtnRect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return i;
        }
        return -1;
    }

    const char* FsCurrentFormatLabel(StereoFormat f)
    {
        int n = 0; const StereoFormatEntry* l = StereoFormatList(n);
        for (int i = 0; i < n; ++i) if (l[i].fmt == f) return l[i].label;
        return "?";
    }

    // Open a popup menu for picking a stereo format. Sent as a WM_COMMAND to
    // the main window so it routes through the existing tray-cmd handler
    // (which also calls EnsureWeavingFormatOnly to apply the change live).
    void FsOpenFormatMenu()
    {
        if (!g_app) return;
        HMENU menu = CreatePopupMenu();
        int n = 0; const StereoFormatEntry* l = StereoFormatList(n);
        for (int i = 0; i < n; ++i)
            AppendMenuA(menu, MF_STRING | (g_app->format == l[i].fmt ? MF_CHECKED : 0),
                        ID_TRAY_FMT_BASE + (UINT)i, l[i].label);
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(g_app->hwnd);
        TrackPopupMenu(menu, TPM_LEFTBUTTON, pt.x, pt.y, 0, g_app->hwnd, nullptr);
        DestroyMenu(menu);
    }

    // Generic integer-picker popup for Quilt cols/rows. Returns the chosen
    // value (1..maxVal) or 0 if dismissed. Updates the corresponding app state.
    void FsOpenIntMenu(int currentVal, int maxVal, const char* unit,
                       int* outAppField)
    {
        if (!g_app || !outAppField) return;
        HMENU menu = CreatePopupMenu();
        for (int v = 1; v <= maxVal; ++v)
        {
            char buf[16]; sprintf_s(buf, "%d %s", v, unit);
            AppendMenuA(menu, MF_STRING | (currentVal == v ? MF_CHECKED : 0),
                        (UINT)v, buf);
        }
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(g_app->hwnd);
        int picked = TrackPopupMenu(menu, TPM_LEFTBUTTON | TPM_RETURNCMD,
                                    pt.x, pt.y, 0, g_app->hwnd, nullptr);
        DestroyMenu(menu);
        if (picked >= 1 && picked <= maxVal)
        {
            *outAppField = picked;
            // Re-centre the view pair so the new grid has a sensible default.
            const int total = g_app->quiltCols * g_app->quiltRows;
            g_app->quiltLeftIdx  = total / 2 - 1; if (g_app->quiltLeftIdx  < 0) g_app->quiltLeftIdx  = 0;
            g_app->quiltRightIdx = total / 2;     if (g_app->quiltRightIdx >= total) g_app->quiltRightIdx = total - 1;
            g_app->quiltLeftBlend = g_app->quiltRightBlend = 0.0f;
            g_app->captureRebind = true;
            if (g_fsSet) InvalidateRect(g_fsSet, nullptr, FALSE);
        }
    }

    void OpenLoadTestImageDialog(AppState& app);   // fwd: defined below near other source helpers
    void AutoDetectQuiltOnCurrent(AppState& app);  // fwd: defined below near LoadTestImage

    LRESULT CALLBACK FsSetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);

            HFONT font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kFsText);

            const StereoFormat fmt = g_app ? g_app->format : StereoFormat::FullSBS;
            const int cols = g_app ? g_app->quiltCols : 8;
            const int rows = g_app ? g_app->quiltRows : 6;

            for (int i = 0; i < g_fsSetBtnCount; ++i)
            {
                RECT r = FsSetBtnRect(i);
                FsPaintRoundButton(dc, r, i == g_fsSetHover ? kFsBtnHover : kFsBtnBg, kFsSetRadius);

                wchar_t label[64];
                switch (i)
                {
                case kFsSetBtnLoad:   wcscpy_s(label, L"Load Media..."); break;
                case kFsSetBtnFormat: swprintf_s(label, L"%hs", FsCurrentFormatLabel(fmt)); break;
                case kFsSetBtnCols:   swprintf_s(label, L"%d cols", cols); break;
                case kFsSetBtnRows:   swprintf_s(label, L"%d rows", rows); break;
                case kFsSetBtnAuto:   wcscpy_s(label, L"Auto-detect"); break;
                default:              label[0] = 0;
                }
                const bool hasChevron = (i == kFsSetBtnFormat || i == kFsSetBtnCols || i == kFsSetBtnRows);
                RECT tr = r;
                tr.left  += 16;
                tr.right -= hasChevron ? 26 : 16;
                DrawTextW(dc, label, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Right-aligned dropdown chevron on menu-opening buttons.
                if (hasChevron)
                {
                    HPEN pen = CreatePen(PS_SOLID, 2, kFsDim);
                    HPEN op  = (HPEN)SelectObject(dc, pen);
                    const int ax = r.right - 16;
                    const int ay = (r.top + r.bottom) / 2;
                    MoveToEx(dc, ax - 6, ay - 3, nullptr); LineTo(dc, ax,     ay + 3);
                    MoveToEx(dc, ax,     ay + 3, nullptr); LineTo(dc, ax + 7, ay - 3);
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
            }
            SelectObject(dc, oldFont);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsSetTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsSetTracking = true;
            }
            const int hit = FsSetHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit != g_fsSetHover) { g_fsSetHover = hit; InvalidateRect(hwnd, nullptr, FALSE); }
            g_fsSetLastUse = GetTickCount();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsSetTracking = false;
            if (g_fsSetHover != -1) { g_fsSetHover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
        {
            const int hit = FsSetHit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_fsSetLastUse = GetTickCount();
            if (!g_app) return 0;
            switch (hit)
            {
            case kFsSetBtnLoad:   OpenLoadTestImageDialog(*g_app); break;
            case kFsSetBtnFormat: FsOpenFormatMenu(); break;
            case kFsSetBtnCols:   FsOpenIntMenu(g_app->quiltCols, kQuiltColsMax, "cols", &g_app->quiltCols); break;
            case kFsSetBtnRows:   FsOpenIntMenu(g_app->quiltRows, kQuiltRowsMax, "rows", &g_app->quiltRows); break;
            case kFsSetBtnAuto:
                AutoDetectQuiltOnCurrent(*g_app);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kFsSetTimerId &&
                g_fsSetHover == -1 && (GetTickCount() - g_fsSetLastUse) > kFsCtrlHideMs)
                ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsSetWindow(HINSTANCE inst)
    {
        if (g_fsSet) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsSetProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsSetClass;
        RegisterClassA(&wc);
        g_fsSet = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsSetClass, "", WS_POPUP,
            // Initial size is placeholder; Show recomputes the layout from
            // current button-count and window-width every time it's surfaced.
            0, 0, 400, kFsSetBtnH + kFsSetPad * 2, nullptr, nullptr, inst, nullptr);
        if (g_fsSet)
        {
            SetLayeredWindowAttributes(g_fsSet, 0, 225, LWA_ALPHA);
            SetTimer(g_fsSet, kFsSetTimerId, 250, nullptr);
        }
    }

    void ShowFsSetOverlay(const AppState& app)
    {
        if (!g_fsSet || !app.hwnd) return;
        // Cols / Rows / Auto-detect only when Quilt is the active format.
        g_fsSetBtnCount = (app.format == StereoFormat::Quilt) ? 5 : 2;
        // Layout limit: try to fit inside the window's client width minus margins,
        // wrap to additional rows when too wide. Falls back to a single row if
        // the window's somehow ridiculously narrow.
        RECT cr; GetClientRect(app.hwnd, &cr);
        const int clientW   = cr.right - cr.left;
        const int sideSlack = 24;     // margin from window edge on each side
        int maxW = clientW - sideSlack * 2;
        if (maxW < 200) maxW = 1024;  // window too small to constrain -- just lay out single row
        FsSetComputeLayout(g_fsSetBtnCount, maxW);
        const int w = g_fsSetCalcW;
        const int h = g_fsSetCalcH;
        int x = 0, y = 0;
        OverlayCornerScreenPos(app.hwnd, w, /*leftCorner=*/true, 12, x, y);
        SetWindowPos(g_fsSet, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Rounded outer window region to match the buttons' rounded corners.
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, kFsSetRadius * 2, kFsSetRadius * 2);
        SetWindowRgn(g_fsSet, rgn, TRUE);
        InvalidateRect(g_fsSet, nullptr, FALSE);
        g_fsSetLastUse = GetTickCount();
    }

    void HideFsSetOverlay() { if (g_fsSet) ShowWindow(g_fsSet, SW_HIDE); }

    // ===== Video transport overlay ==========================================
    // Bottom-of-window strip with play/pause + progress bar + time text. Shows
    // when a video source is loaded; click the play glyph to toggle, click the
    // bar to seek.
    constexpr char kFsVidClass[]   = "SRLoomVideoCtrls";
    constexpr int  kFsVidH         = 64;
    constexpr int  kFsVidPadX      = 14;
    constexpr int  kFsVidPadY      = 10;
    constexpr int  kFsVidBtnW      = 48;
    constexpr int  kFsVidBtnH      = 44;
    constexpr int  kFsVidGap       = 14;
    constexpr int  kFsVidTimeW     = 140;          // room for "12:34 / 56:78"
    constexpr int  kFsVidBarH      = 12;
    constexpr int  kFsVidRadius    = 12;
    constexpr UINT kFsVidTimerId   = 9;
    constexpr DWORD kFsVidHideMs   = 3500;

    HWND  g_fsVid          = nullptr;
    bool  g_fsVidBtnHover  = false;
    bool  g_fsVidBarHover  = false;
    DWORD g_fsVidLastUse   = 0;
    bool  g_fsVidTracking  = false;

    RECT FsVidBtnRect(int clientW)
    {
        (void)clientW;
        RECT r{};
        r.left   = kFsVidPadX;
        r.top    = (kFsVidH - kFsVidBtnH) / 2;
        r.right  = r.left + kFsVidBtnW;
        r.bottom = r.top  + kFsVidBtnH;
        return r;
    }
    RECT FsVidBarRect(int clientW)
    {
        RECT r{};
        r.left   = kFsVidPadX + kFsVidBtnW + kFsVidGap;
        r.right  = clientW - kFsVidPadX - kFsVidTimeW - kFsVidGap;
        r.top    = (kFsVidH - kFsVidBarH) / 2;
        r.bottom = r.top + kFsVidBarH;
        if (r.right < r.left + 40) r.right = r.left + 40;
        return r;
    }
    RECT FsVidTimeRect(int clientW)
    {
        RECT r{};
        r.left   = clientW - kFsVidPadX - kFsVidTimeW;
        r.right  = clientW - kFsVidPadX;
        r.top    = 0;
        r.bottom = kFsVidH;
        return r;
    }

    void FsFormatHmsMmSs(long long hns, char* buf, size_t cap)
    {
        if (hns < 0) hns = 0;
        const long long totalSec = hns / 10000000LL;
        const int mm = (int)(totalSec / 60);
        const int ss = (int)(totalSec % 60);
        _snprintf_s(buf, cap, _TRUNCATE, "%d:%02d", mm, ss);
    }

    LRESULT CALLBACK FsVidProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT cr; GetClientRect(hwnd, &cr);
            HBRUSH bg = CreateSolidBrush(kFsBg);
            FillRect(dc, &cr, bg);
            DeleteObject(bg);
            const int W = cr.right;

            const RECT btn  = FsVidBtnRect(W);
            const RECT bar  = FsVidBarRect(W);
            const RECT time = FsVidTimeRect(W);

            // Play/pause button.
            FsPaintRoundButton(dc, btn, g_fsVidBtnHover ? kFsBtnHover : kFsBtnBg, 8);
            const int cx = (btn.left + btn.right)  / 2;
            const int cy = (btn.top  + btn.bottom) / 2;
            const bool playing = (g_app && g_app->video.IsOpen() && !g_app->video.IsPaused());
            HBRUSH glyphBrush = CreateSolidBrush(kFsText);
            HBRUSH oldB = (HBRUSH)SelectObject(dc, glyphBrush);
            HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
            HPEN oldP = (HPEN)SelectObject(dc, nopen);
            if (playing)
            {
                // two vertical bars (pause)
                RECT a{ cx - 9, cy - 11, cx - 3, cy + 11 };
                RECT b{ cx + 3, cy - 11, cx + 9, cy + 11 };
                FillRect(dc, &a, glyphBrush);
                FillRect(dc, &b, glyphBrush);
            }
            else
            {
                // triangle (play)
                POINT t[3] = { {cx - 7, cy - 11}, {cx + 10, cy}, {cx - 7, cy + 11} };
                Polygon(dc, t, 3);
            }
            SelectObject(dc, oldB);
            SelectObject(dc, oldP);
            DeleteObject(glyphBrush);

            // Progress bar: dim track + accent fill up to current position.
            const long long dur = (g_app && g_app->video.IsOpen()) ? g_app->video.DurationHns() : 0;
            const long long pos = (g_app && g_app->video.IsOpen()) ? g_app->video.PositionHns() : 0;
            const double frac = (dur > 0) ? std::min(1.0, (double)pos / (double)dur) : 0.0;
            HBRUSH track = CreateSolidBrush(kFsBtnBg);
            FillRect(dc, &bar, track);
            DeleteObject(track);
            RECT fill = bar;
            fill.right = bar.left + (LONG)((bar.right - bar.left) * frac + 0.5);
            HBRUSH fillB = CreateSolidBrush(kFsCloseHot);   // warm accent
            FillRect(dc, &fill, fillB);
            DeleteObject(fillB);

            // Time text.
            char posStr[16], durStr[16];
            FsFormatHmsMmSs(pos, posStr, sizeof(posStr));
            FsFormatHmsMmSs(dur, durStr, sizeof(durStr));
            char both[40];
            _snprintf_s(both, _TRUNCATE, "%s / %s", posStr, durStr);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kFsText);
            HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(dc, font);
            wchar_t wboth[40];
            MultiByteToWideChar(CP_UTF8, 0, both, -1, wboth, 40);
            RECT timeMut = time;
            DrawTextW(dc, wboth, -1, &timeMut, DT_VCENTER | DT_SINGLELINE | DT_CENTER);
            SelectObject(dc, oldFont);
            DeleteObject(font);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (!g_fsVidTracking)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_fsVidTracking = true;
            }
            RECT cr; GetClientRect(hwnd, &cr);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const RECT btn = FsVidBtnRect(cr.right);
            const RECT bar = FsVidBarRect(cr.right);
            const bool bh  = (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom);
            const bool ph  = (x >= bar.left && x < bar.right && y >= bar.top - 6 && y < bar.bottom + 6);
            if (bh != g_fsVidBtnHover || ph != g_fsVidBarHover)
            {
                g_fsVidBtnHover = bh;
                g_fsVidBarHover = ph;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            g_fsVidLastUse = GetTickCount();
            return 0;
        }
        case WM_MOUSELEAVE:
            g_fsVidTracking = false;
            if (g_fsVidBtnHover || g_fsVidBarHover)
            {
                g_fsVidBtnHover = g_fsVidBarHover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
        {
            if (!g_app || !g_app->video.IsOpen()) return 0;
            RECT cr; GetClientRect(hwnd, &cr);
            const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            const RECT btn = FsVidBtnRect(cr.right);
            const RECT bar = FsVidBarRect(cr.right);
            g_fsVidLastUse = GetTickCount();
            if (x >= btn.left && x < btn.right && y >= btn.top && y < btn.bottom)
            {
                g_app->video.TogglePause();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Seek if click lands on (or very near) the bar.
            if (y >= bar.top - 6 && y < bar.bottom + 6 && x >= bar.left && x < bar.right)
            {
                const double frac = (double)(x - bar.left) / (double)(bar.right - bar.left);
                const long long dur = g_app->video.DurationHns();
                if (dur > 0) g_app->video.Seek((long long)(frac * (double)dur));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kFsVidTimerId)
            {
                if (!g_fsVidBtnHover && !g_fsVidBarHover &&
                    (GetTickCount() - g_fsVidLastUse) > kFsVidHideMs)
                    ShowWindow(hwnd, SW_HIDE);
                else
                    InvalidateRect(hwnd, nullptr, FALSE);   // tick the progress bar
            }
            return 0;
        case WM_NCHITTEST: return HTCLIENT;
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }

    void EnsureFsVidWindow(HINSTANCE inst)
    {
        if (g_fsVid) return;
        WNDCLASSA wc{};
        wc.lpfnWndProc   = FsVidProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kFsVidClass;
        RegisterClassA(&wc);
        g_fsVid = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kFsVidClass, "", WS_POPUP,
            0, 0, 400, kFsVidH, nullptr, nullptr, inst, nullptr);
        if (g_fsVid)
        {
            SetLayeredWindowAttributes(g_fsVid, 0, 225, LWA_ALPHA);
            // Tick at ~10 Hz so the progress bar visibly advances during playback.
            SetTimer(g_fsVid, kFsVidTimerId, 100, nullptr);
        }
    }

    void ShowFsVidOverlay(const AppState& app)
    {
        if (!g_fsVid || !app.hwnd || !app.video.IsOpen()) return;
        RECT cr; GetClientRect(app.hwnd, &cr);
        POINT bl{ cr.left, cr.bottom }, br{ cr.right, cr.bottom };
        ClientToScreen(app.hwnd, &bl);
        ClientToScreen(app.hwnd, &br);
        const int clientW = br.x - bl.x;
        const int sideMargin = 20;
        int w = clientW - sideMargin * 2;
        if (w < 220) w = 220;
        if (w > 800) w = 800;
        const int x = bl.x + (clientW - w) / 2;
        const int y = bl.y - kFsVidH - 16;
        SetWindowPos(g_fsVid, HWND_TOPMOST, x, y, w, kFsVidH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, kFsVidH + 1,
                                      kFsVidRadius * 2, kFsVidRadius * 2);
        SetWindowRgn(g_fsVid, rgn, TRUE);
        InvalidateRect(g_fsVid, nullptr, FALSE);
        g_fsVidLastUse = GetTickCount();
    }

    void HideFsVidOverlay() { if (g_fsVid) ShowWindow(g_fsVid, SW_HIDE); }

    // Re-anchor visible overlays after the main window moves / resizes.
    void RepositionOverlays(const AppState& app)
    {
        if (g_fsCtrl && IsWindowVisible(g_fsCtrl)) ShowFsCtrlOverlay(app);
        if (g_fsSet  && IsWindowVisible(g_fsSet))  ShowFsSetOverlay(app);
        if (g_fsVid  && IsWindowVisible(g_fsVid))  ShowFsVidOverlay(app);
    }

    // ------------------------------------------------------------------------

    void ApplyMode(AppState& app)
    {
        if (app.mode != OutputMode::LookingGlass)
            app.loupeActive = false;   // reset so the loupe re-centres next time it's entered
        // Right (min / windowed / close) overlay is ONLY for fullscreen test-image
        // viewing; in windowed mode the native title-bar chrome already does
        // those three actions. Left (settings) overlay shows in both -- it's
        // for image/format/quilt controls, which are useful regardless of mode.
        const bool isFsTestImage = (app.source == SourceKind::TestImage &&
                                    app.mode   == OutputMode::Fullscreen);
        const bool isTestImage   = (app.source == SourceKind::TestImage);
        if (!isFsTestImage) HideFsCtrlOverlay();
        if (!isTestImage)   HideFsSetOverlay();
        if (!isTestImage || !app.video.IsOpen()) HideFsVidOverlay();

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
            // Default to a 16:9 1280x720 window. For test-image sources, size
            // to the SOURCE's per-eye / per-view aspect so the visible 3D
            // content fills the window instead of being letterboxed inside it.
            int w = 1280, h = 720;
            double aspect = 16.0 / 9.0;
            if (app.source == SourceKind::TestImage)
            {
                int sw = 0, sh = 0;
                if (app.video.IsOpen()) { sw = app.video.Width(); sh = app.video.Height(); }
                else                    { sw = app.weaver.SourceWidth(); sh = app.weaver.SourceHeight(); }
                if (sw > 0 && sh > 0)
                {
                    double aw = (double)sw, ah = (double)sh;
                    switch (app.format)
                    {
                    case StereoFormat::Quilt:
                        if (app.quiltCols > 0 && app.quiltRows > 0) {
                            aw /= app.quiltCols; ah /= app.quiltRows;
                        }
                        break;
                    case StereoFormat::FullSBS:
                    case StereoFormat::HalfSBS:           aw *= 0.5; break;
                    case StereoFormat::FullTAB:
                    case StereoFormat::HalfTAB:
                    case StereoFormat::RowInterleaved:    ah *= 0.5; break;
                    case StereoFormat::ColumnInterleaved: aw *= 0.5; break;
                    default: break;
                    }
                    if (aw > 0 && ah > 0) aspect = aw / ah;
                }
                // Aim for ~720 client height; clamp width so the window fits
                // the SR display with comfortable margin.
                h = 720;
                w = (int)(h * aspect + 0.5);
                const int maxW = (dw > 200) ? dw - 100 : 1280;
                if (w > maxW) { w = maxW; h = (int)(w / aspect + 0.5); }
                if (w < 320)  { w = 320; }
                if (h < 240)  { h = 240; }
            }
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

    void SetWeaving(AppState& app, bool enable);   // fwd decl (defined below)

    // Keep the overlay aligned with the tracked source window each frame.
    void UpdateOverlayTracking(AppState& app)
    {
        if (app.mode != OutputMode::WindowOverlay)
            return;

        HWND src = app.sourceWindow;
        if (!src || !IsWindow(src))
        {
            // The captured window has closed. Don't keep weaving its last frame —
            // tear the overlay down and go idle (turns the lens off too), so the
            // stale 3D image disappears without having to quit SR Loom.
            app.capture.Stop();
            app.sourceWindow = nullptr;
            app.source = SourceKind::CaptureMonitor;   // sane default for the next enable
            SetWeaving(app, false);
            return;
        }

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
        // Bump process priority while weaving so Windows' scheduler is less likely
        // to deschedule the render loop mid-frame -- tighter frame pacing, fewer
        // hitches when other apps (browser tab, antivirus etc.) spike. Restore to
        // normal when weaving off so we're not hogging cycles while idle in the tray.
        SetPriorityClass(GetCurrentProcess(),
                         enable ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
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
            HideFsCtrlOverlay();
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
        app.capture.SetCaptureCursor(false);   // overlay sits on the source; real cursor shows through
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
    // The monitor the SR display itself lives on (the "this display" passthrough target).
    HMONITOR SrMonitor(const AppState& app)
    {
        const RECT& d = app.srDisplayRect;
        POINT center{ (d.left + d.right) / 2, (d.top + d.bottom) / 2 };
        return MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY);
    }

    // Physical-to-pixel scale (mm per pixel, horizontal axis) for a given
    // monitor. Asks the OS via GetDeviceCaps(HORZSIZE / HORZRES). Returns 0
    // if it can't be determined; callers should treat 0 as "skip features
    // that need it" (head-position window-anchoring, primarily).
    double MonitorMmPerPx(HMONITOR mon)
    {
        if (!mon) return 0.0;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return 0.0;
        HDC hdc = CreateDCW(nullptr, mi.szDevice, nullptr, nullptr);
        if (!hdc) return 0.0;
        const int physWmm  = GetDeviceCaps(hdc, HORZSIZE);
        const int pixelW   = GetDeviceCaps(hdc, HORZRES);
        DeleteDC(hdc);
        if (physWmm <= 0 || pixelW <= 0) return 0.0;
        return (double)physWmm / (double)pixelW;
    }

    // Maximum refresh rate the given monitor reports support for, in Hz. Iterates
    // every display mode the OS lists and picks the highest dmDisplayFrequency.
    // Returns 0 on failure (e.g. monitor disconnected). Per-model: Samsung Odyssey
    // 3D reports 160-165 Hz, Acer SpatialLabs View Pro 27 reports 160 Hz, future
    // higher-refresh panels will scale automatically.
    double MaxMonitorRefreshHz(HMONITOR mon)
    {
        if (!mon) return 0.0;
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return 0.0;
        double best = 0.0;
        DEVMODEW dm{}; dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); ++i)
        {
            if ((double)dm.dmDisplayFrequency > best)
                best = (double)dm.dmDisplayFrequency;
        }
        return best;
    }

    void UsePassthrough(AppState& app)
    {
        app.sourceWindow = nullptr;
        app.capture.SetCaptureCursor(false);   // same screen as the real cursor → don't double it
        HMONITOR mon = SrMonitor(app);
        if (app.capture.StartMonitor(mon))
        {
            app.source = SourceKind::CaptureMonitor;
            app.sourceMonitor = mon;
            app.foreignDisplay = false;   // this IS the SR display → crop to the viewer region
            app.captureRebind = true;
        }
    }

    // stb_image symbols live in SRWeaver.cpp (STB_IMAGE_IMPLEMENTATION is set
    // there). Forward-declare just the two functions we need so we can load
    // pixel data here for image-content quilt-grid auto-detection.
    extern "C" unsigned char* stbi_load(char const* filename, int* x, int* y,
                                        int* channels_in_file, int desired_channels);
    extern "C" void stbi_image_free(void* retval_from_stbi_load);

    // Image-content quilt-grid auto-detect. Tries the canonical LG grids and
    // picks the one that best matches "looks like a tiled set of horizontally-
    // shifted views". Two-stage filter:
    //   (1) Strict-ish divisibility -- real LG quilts have w%cols and h%rows
    //       within a few px of zero. Reject loose fits.
    //   (2) Score = mean SAD(horizontally-adjacent cells) / mean SAD(diagonally-
    //       opposite cells). For the correct grid, horizontal-adjacent cells
    //       are almost the same scene (small parallax) and opposite-corner
    //       cells are different views (larger diff) -- ratio is small.
    //       For wrong grids, the cells span unrelated content and the ratio
    //       is close to 1. Normalising by the diagonal SAD divides out global
    //       image contrast, so the heuristic survives low- and high-contrast
    //       photos equally well.
    bool AutoDetectQuiltGrid(const char* path, int& outCols, int& outRows)
    {
        int w = 0, h = 0, ch = 0;
        unsigned char* pixels = stbi_load(path, &w, &h, &ch, 4);
        if (!pixels) return false;

        struct Cand { int c, r; };
        const Cand cands[] = {
            {8, 6},  {5, 9},  {4, 8},  {11, 8}, {11, 6},
            {13, 7}, {7, 5},  {10, 6}, {12, 9}, {6, 6},
            {9, 5},  {6, 8},  {4, 6},  {3, 4},  {10, 7},
            {12, 8}, {9, 6},  {7, 6},  {8, 5},  {6, 4},
            {5, 7},  {7, 9},  {2, 4},  {4, 4},  {5, 5},
        };

        auto rgbSum = [&](int x, int y) -> int {
            if (x < 0) x = 0; else if (x >= w) x = w - 1;
            if (y < 0) y = 0; else if (y >= h) y = h - 1;
            const unsigned char* p = pixels + ((size_t)y * w + x) * 4;
            return (int)p[0] + (int)p[1] + (int)p[2];
        };

        double bestRatio = 1e30;
        int    bestC = 0, bestR = 0;

        for (const Cand& c : cands)
        {
            const int cellW = w / c.c;
            const int cellH = h / c.r;
            if (cellW < 16 || cellH < 16) continue;
            // Strict fit: each axis must divide within 1 px. Real LG quilts are
            // mathematically clean; loose fits indicate the wrong grid.
            if (w - cellW * c.c > 1 || h - cellH * c.r > 1) continue;

            constexpr int SX = 8, SY = 8;        // 8x8 samples per cell
            long long hSad = 0;  int hN = 0;     // horizontal-neighbour SAD
            long long vSad = 0;  int vN = 0;     // vertical-neighbour SAD
            long long dSad = 0;  int dN = 0;     // diagonal-opposite SAD (baseline)

            for (int cy = 0; cy < c.r; ++cy)
                for (int cx = 0; cx < c.c; ++cx)
                    for (int sj = 0; sj < SY; ++sj)
                        for (int si = 0; si < SX; ++si)
                        {
                            const int dx = (cellW * (si * 2 + 1)) / (SX * 2);
                            const int dy = (cellH * (sj * 2 + 1)) / (SY * 2);
                            const int yy = cy * cellH + dy;
                            const int xx = cx * cellW + dx;

                            if (cx < c.c - 1)   // horizontal neighbour
                            {
                                hSad += abs(rgbSum(xx, yy) - rgbSum(xx + cellW, yy));
                                ++hN;
                            }
                            if (cy < c.r - 1)   // vertical neighbour (LG layout:
                            {                   // same scene shifted by +cols views)
                                vSad += abs(rgbSum(xx, yy) - rgbSum(xx, yy + cellH));
                                ++vN;
                            }
                            // Diagonal-opposite cell as baseline for global
                            // image variance. Same relative sample inside each.
                            const int diagCx = c.c - 1 - cx;
                            const int diagCy = c.r - 1 - cy;
                            if (diagCx != cx || diagCy != cy)
                            {
                                const int x2 = diagCx * cellW + dx;
                                const int y2 = diagCy * cellH + dy;
                                dSad += abs(rgbSum(xx, yy) - rgbSum(x2, y2));
                                ++dN;
                            }
                        }

            if (hN == 0 || vN == 0 || dN == 0) continue;
            const double hMean = (double)hSad / hN;
            const double vMean = (double)vSad / vN;
            const double dMean = (double)dSad / dN;
            if (dMean < 1.0) continue;
            // Combined ratio: BOTH horizontal AND vertical neighbours should be
            // small relative to the diagonal baseline. Either being large means
            // the grid is wrong on that axis.
            double ratio = ((hMean + vMean) * 0.5) / dMean;

            // Cell-aspect bias: real LG quilt views match one of the LG display
            // aspects (Portrait 3:4 = 0.75, 16:10 landscape = 1.6, 16:9 = 1.78).
            // Grids whose cells fit a known aspect get a small score bonus -
            // breaks ties between numerically-close candidates (e.g. 8x6 vs
            // 11x8 on a square quilt: both fit, but 8x6's 3:4 aspect matches
            // Portrait native exactly so it gets the bonus).
            const double cellAspect = (double)cellW / (double)cellH;
            const double aspects[5] = { 0.75, 1.6, 1.7777, 0.5625, 0.625 };
            double aspectErr = 1.0;
            for (double a : aspects) { double e = std::fabs(cellAspect - a); if (e < aspectErr) aspectErr = e; }
            ratio *= (1.0 + 0.25 * aspectErr);   // 0% extra at perfect match, scales with err

            if (ratio < bestRatio) { bestRatio = ratio; bestC = c.c; bestR = c.r; }
        }

        stbi_image_free(pixels);
        if (bestC == 0 || bestR == 0) return false;
        Log("AutoDetectQuiltGrid: %dx%d (horizontal/diagonal SAD ratio %.3f)",
            bestC, bestR, bestRatio);
        outCols = bestC; outRows = bestR;
        return true;
    }

    // Re-run the image-content detect on the currently-loaded test image and
    // apply the result. No-op when there's no image loaded.
    void AutoDetectQuiltOnCurrent(AppState& app)
    {
        if (app.lastTestImagePath.empty()) return;
        int qc = 0, qr = 0;
        if (!AutoDetectQuiltGrid(app.lastTestImagePath.c_str(), qc, qr)) return;
        app.format        = StereoFormat::Quilt;
        app.quiltCols     = qc;
        app.quiltRows     = qr;
        const int total   = qc * qr;
        app.quiltLeftIdx  = total / 2 - 1; if (app.quiltLeftIdx  < 0)        app.quiltLeftIdx  = 0;
        app.quiltRightIdx = total / 2;     if (app.quiltRightIdx >= total)   app.quiltRightIdx = total - 1;
        app.quiltLeftBlend = app.quiltRightBlend = 0.0f;
        app.captureRebind = true;
    }

    // Load a stereo image from disk as the SourceKind::TestImage source. If the
    // filename embeds a Looking Glass quilt token ("_qs8x6_" etc) the format is
    // auto-set to Quilt with the parsed grid + a centred view pair; otherwise
    // image-content detection runs as a fallback -- so a quilt file without the
    // naming convention still works. Non-quilt images fall through and keep the
    // currently-selected format.
    bool LoadTestImage(AppState& app, const char* path)
    {
        if (!path || !*path) return false;

        // Tear down whichever source was last loaded (video or image) so we
        // don't end up with both bound.
        app.video.Close();

        const bool isVideo = VideoSource::IsVideoFile(path);
        if (isVideo)
        {
            wchar_t wpath[MAX_PATH] = {};
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
            if (!app.video.Open(app.renderer.Device(), wpath))
            {
                ShowError("Failed to open video file.\n\n"
                          "See srweaver.log next to the executable for details. "
                          "Most common causes: codec missing, file path issue, "
                          "or Windows Media Foundation not fully installed.");
                return false;
            }
        }
        else
        {
            if (!app.weaver.SetStereoImageFromFile(app.renderer.Device(), path,
                                                   app.format, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))
                return false;
        }
        app.lastTestImagePath = path;   // remembered for AutoDetect re-run

        int qc = 0, qr = 0;
        // ONLY use the LG filename token here. Image-content detection is on a
        // user-triggered button (Auto-detect) instead -- it sometimes guesses
        // wrong, so it shouldn't override the loader silently. If the filename
        // has no token, the user's currently-selected format is kept.
        if (ParseQuiltDims(path, qc, qr))
        {
            app.format        = StereoFormat::Quilt;
            app.quiltCols     = qc;
            app.quiltRows     = qr;
            const int total   = qc * qr;
            app.quiltLeftIdx  = total / 2 - 1; if (app.quiltLeftIdx  < 0)        app.quiltLeftIdx  = 0;
            app.quiltRightIdx = total / 2;     if (app.quiltRightIdx >= total)   app.quiltRightIdx = total - 1;
            Log("LoadTestImage: quilt %dx%d (from filename; default views L=%d R=%d)",
                qc, qr, app.quiltLeftIdx, app.quiltRightIdx);
        }
        // Stop any live capture so its frames don't compete with the test image,
        // then point the source at it and force a converter rebind / pipeline rebuild.
        app.capture.Stop();
        if (app.dxgiActive) { app.captureDxgi.Stop(); app.dxgiActive = false; }
        app.source        = SourceKind::TestImage;
        app.sourceWindow  = nullptr;
        app.sourceMonitor = nullptr;
        app.foreignDisplay = false;
        app.captureRebind = true;
        app.mode          = OutputMode::Fullscreen;
        EnsureWeaving(app);
        return true;
    }

    // Tray-menu / GUI handler: pop the standard Open dialog filtered to common
    // image extensions, then hand the picked path to LoadTestImage.
    void OpenLoadTestImageDialog(AppState& app)
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = app.hwnd;
        ofn.lpstrFilter =
            "Stereo media\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv\0"
            "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
            "Videos\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv\0"
            "All files\0*.*\0";
        ofn.lpstrFile   = file;
        ofn.nMaxFile    = (DWORD)sizeof(file);
        ofn.lpstrTitle  = "Load media (stereo / quilt)";
        ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn))
            LoadTestImage(app, file);
    }

    // Weave another display (real or virtual) — picked from the GUI's display dropdown.
    // The whole captured frame is woven onto the SR display in fullscreen (no region
    // crop, since the source isn't the screen we're drawing on, so there's no feedback).
    void UseDisplay(AppState& app, HMONITOR mon)
    {
        app.capture.SetCaptureCursor(true);   // show the pointer on the captured display
        if (!mon || !app.capture.StartMonitor(mon))
            return;
        app.sourceWindow = nullptr;
        app.source = SourceKind::CaptureMonitor;
        app.sourceMonitor = mon;
        app.foreignDisplay = (mon != SrMonitor(app));
        app.captureRebind = true;
        app.mode = OutputMode::Fullscreen;   // show the whole captured display, woven
        EnsureWeaving(app);
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

    // Resolve the current source frame (test image, video, or capture).
    bool ResolveSource(AppState& app, ID3D11ShaderResourceView*& srv, int& w, int& h)
    {
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen())
            {
                app.video.Update(app.renderer.Context());
                srv = app.video.SRV(); w = app.video.Width(); h = app.video.Height();
            }
            else
            {
                srv = app.weaver.SourceSRV(); w = app.weaver.SourceWidth(); h = app.weaver.SourceHeight();
            }
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

        if (IsIconic(app.hwnd))
        {
            Sleep(10);
            return;
        }

        // Pace to the display before grabbing the newest frame (lowest latency).
        app.renderer.WaitForFrame();

        // Anti-lag pattern (BlueSkyDefender's: max-frames-in-flight=1, sample input
        // AFTER the wait): tracking source-window position / cursor hover gets read
        // here so it's the freshest state by the time the frame reaches the panel.
        // Reading them BEFORE WaitForFrame would let them go up-to-one-refresh stale.
        UpdateOverlayTracking(app);     // follow the source window in overlay mode
        UpdateLoupeInteractivity(app);  // hover the chrome to grab/move the looking glass

        // Resolve the current source frame: the test image, or a capture frame.
        ID3D11ShaderResourceView* srcSRV = nullptr;
        int srcW = 0, srcH = 0;
        bool capSizeChanged = false;
        bool gotFrame = false;   // a new capture frame arrived this iteration
        if (app.source == SourceKind::TestImage)
        {
            if (app.video.IsOpen())
            {
                // Decode the next video frame (no-op if the previous one is
                // still current) and bind its texture as this frame's source.
                if (app.video.Update(app.renderer.Context()))
                    app.captureRebind = true;   // first ever frame -> ensure weaver re-binds
                srcSRV = app.video.SRV();
                srcW   = app.video.Width();
                srcH   = app.video.Height();
            }
            else
            {
                srcSRV = app.weaver.SourceSRV();
                srcW   = app.weaver.SourceWidth();
                srcH   = app.weaver.SourceHeight();
            }
        }
        else if (app.capture.IsActive())
        {
            // SR-display passthrough: weave only the region beneath the viewer. A
            // foreign display (the picker) is woven whole — no crop.
            if (app.source == SourceKind::CaptureMonitor && !app.foreignDisplay && app.capture.FrameWidth() > 0)
            {
                RECT r = PassthroughRegion(app, app.hwnd);
                app.capture.SetSourceRegion(r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
            const bool wgcGotFrame = app.capture.Update(capSizeChanged);

            // Track whether WGC is still delivering frames. An exclusive-fullscreen
            // app on the captured monitor freezes WGC (it sees DWM's last composed
            // frame and nothing new). For FOREIGN-DISPLAY monitor capture only, fall
            // back to DXGI Output Duplication in that case. We never use DXGI for the
            // SR display itself (it would capture our own woven overlay).
            const bool dxgiEligible = (app.source == SourceKind::CaptureMonitor)
                                      && app.foreignDisplay && app.sourceMonitor;
            if (wgcGotFrame) app.wgcStuck = 0;
            else if (dxgiEligible) ++app.wgcStuck;

            if (!app.dxgiActive && dxgiEligible && app.wgcStuck > 30)   // ~500ms at typical render cadence
            {
                if (app.captureDxgi.StartMonitor(app.sourceMonitor))
                {
                    app.dxgiActive = true;
                    app.captureRebind = true;
                    app.dxgiCheckCtr = 0;
                    Log("Capture: WGC stuck for %d ticks, switched to DXGI Output Duplication", app.wgcStuck);
                }
                app.wgcStuck = 0;
            }

            if (app.dxgiActive)
            {
                // Apply the same source region as WGC for foreign-display passthrough
                // (a foreign display is woven whole today, so this is a no-op pre-crop,
                // but matched to WGC's behaviour for symmetry).
                bool dxgiSizeChanged = false;
                gotFrame = app.captureDxgi.Update(dxgiSizeChanged);
                if (dxgiSizeChanged) capSizeChanged = true;
                srcSRV = app.captureDxgi.SRV();
                srcW   = app.captureDxgi.Width();
                srcH   = app.captureDxgi.Height();

                // Periodically check whether the captured monitor's foreground is still
                // covering it. When it stops (user alt-tabbed out, game quit), DXGI is
                // no longer needed and we'd rather be back on WGC (cheaper, supports
                // window exclusion if we switch source modes).
                if (++app.dxgiCheckCtr > 30)
                {
                    app.dxgiCheckCtr = 0;
                    if (!ForegroundCoversMonitor(app.sourceMonitor))
                    {
                        app.captureDxgi.Stop();
                        app.dxgiActive = false;
                        app.captureRebind = true;
                        Log("Capture: foreground no longer fullscreen on monitor, back to WGC");
                    }
                }
            }
            else
            {
                gotFrame = wgcGotFrame;
                srcSRV = app.capture.SRV();
                srcW   = app.capture.Width();
                srcH   = app.capture.Height();
            }
        }

        // FAST PATH: a live source already in side-by-side layout (full OR half SBS,
        // no eye swap, no convergence shift) is identical to what the converter would
        // output -- so feed the captured texture STRAIGHT to the weaver and skip the
        // conversion pass entirely. Lowest latency / least GPU contention, which
        // matters for games. Works for Half SBS too because the weaver only cares about
        // per-eye-width / height in the source texture; the weaver's internal bilinear
        // sample naturally un-squeezes anamorphic half-SBS when sampling to the woven
        // output's per-eye dimensions. The capture texture updates in place each frame
        // so the weaver's input view only needs re-registering on a rebind or a resize.
        // A video test source is "live" too -- its texture's content changes
        // every frame even though source == TestImage, so the converter has to
        // re-run continuously.
        const bool liveSource  = (app.source != SourceKind::TestImage) || app.video.IsOpen();
        // Quilt is included so the converter re-runs every frame even on a
        // static test image -- its L/R view indices follow the head position.
        const bool temporalFmt = (app.format == StereoFormat::Pulfrich ||
                                  app.format == StereoFormat::FrameSequential ||
                                  app.format == StereoFormat::Quilt);
        const bool noConv      = (app.convergence < 1e-4f && app.convergence > -1e-4f);
        const bool sbsFmt      = (app.format == StereoFormat::FullSBS ||
                                  app.format == StereoFormat::HalfSBS);
        const bool identitySBS = liveSource && sbsFmt && !app.swapEyes && noConv;

        if (identitySBS)
        {
            if (srcSRV && srcW > 0 && srcH > 0 && (app.captureRebind || capSizeChanged))
            {
                const DXGI_FORMAT srvFmt = app.dxgiActive ? app.captureDxgi.SRVFormat()
                                                          : app.capture.SRVFormat();
                app.weaver.SetInputView(srcSRV, srcW / 2, srcH, srvFmt);
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
            // Quilt: pick L/R view indices from the SR head-pose tracker. Each eye
            // gets the view whose virtual-camera horizontal position matches that
            // eye's lateral position. Without tracking data (cold start), the
            // user-set defaults (centred pair) just stay in place.
            //
            // Mapping: a head sweep of ±HEAD_FULL_SWEEP_MM (~33cm half-sweep at a
            // 60cm viewing distance ≈ 58° angular range, the Looking Glass Portrait
            // native FOV) traverses the full quilt index range. Each eye's lateral
            // offset (head.x ± IOD/2 -- here we just use head.x since the tracker
            // gives us the head centre, not the eye positions; an IOD of ~64mm at
            // a typical 660mm full-sweep adds a ~5-view default baseline between
            // the eyes, which is the parallax that gives 3D depth).
            if (app.format == StereoFormat::Quilt)
            {
                double hp[3] = {}, ho[3] = {};
                if (app.weaver.GetHeadPose(hp, ho))
                {
                    const double HEAD_FULL_SWEEP_MM = 660.0;   // ±330mm covers all views
                    const double IOD_MM             = 64.0;    // average interocular distance
                    const double EMA_ALPHA          = 0.10;    // lower = smoother, slower
                    const float  BLEND_SNAP         = 0.08f;   // shimmer-kill near integer boundaries
                    const int    total              = app.quiltCols * app.quiltRows;

                    // EMA on the raw head.x removes high-frequency tracker noise.
                    if (!app.headXEMAInit) { app.headXEMA = hp[0]; app.headXEMAInit = true; }
                    else app.headXEMA = EMA_ALPHA * hp[0] + (1.0 - EMA_ALPHA) * app.headXEMA;

                    // Window-position perspective shift: when SR Loom is windowed
                    // on the SR display, the user's "looking toward" the window's
                    // SCREEN POSITION, not the panel centre. Subtract the window's
                    // centre-relative-to-panel-centre offset (in mm) from the
                    // head.x so a head centred ON the window picks view N/2,
                    // regardless of where the window sits on the panel. Skipped
                    // when we don't know the panel's mm/px scale or weren't
                    // given a sensible window position.
                    double windowOffsetMm = 0.0;
                    if (app.srMmPerPx > 0.0 && app.hwnd)
                    {
                        RECT wr{}; GetWindowRect(app.hwnd, &wr);
                        const int winCenterX  = (wr.left + wr.right) / 2;
                        const int panCenterX  = (app.srDisplayRect.left + app.srDisplayRect.right) / 2;
                        windowOffsetMm = (double)(winCenterX - panCenterX) * app.srMmPerPx;
                    }
                    const double effectiveHeadX = app.headXEMA - windowOffsetMm;

                    auto mapEyeXToFrac = [&](double x) -> double {
                        double n = x / HEAD_FULL_SWEEP_MM;     // -0.5..+0.5 typical
                        if (n < -0.5) n = -0.5;
                        if (n >  0.5) n =  0.5;
                        return (0.5 + n) * (double)(total - 1);
                    };
                    auto fracToIdxBlend = [&](double frac, int& outIdx, float& outBlend) {
                        if (frac < 0.0) frac = 0.0;
                        const double maxF = (double)(total - 1);
                        if (frac > maxF) frac = maxF;
                        const int    lo = (int)floor(frac);
                        outIdx   = lo;
                        outBlend = (float)(frac - (double)lo);
                    };
                    // Each eye gets the camera position offset by ±IOD/2 from the
                    // smoothed head centre; +x = right of display. The shader
                    // cross-fades between view[idx] and view[idx+1] by the
                    // fractional component -- so motion across a view boundary
                    // is continuous, the way Looking Glass interpolates between
                    // its physical lenticular columns.
                    fracToIdxBlend(mapEyeXToFrac(effectiveHeadX - IOD_MM * 0.5),
                                   app.quiltLeftIdx,  app.quiltLeftBlend);
                    fracToIdxBlend(mapEyeXToFrac(effectiveHeadX + IOD_MM * 0.5),
                                   app.quiltRightIdx, app.quiltRightBlend);
                    // Anti-shimmer near integer boundaries: snap tiny residual
                    // blend to 0 (still head -> locked to single view -> no
                    // cross-fade flicker); snap near-1 blend to the next view +
                    // 0 blend so the same locked behaviour holds either side.
                    auto snap = [&](int& idx, float& blend) {
                        if (blend < BLEND_SNAP) { blend = 0.0f; return; }
                        if (blend > 1.0f - BLEND_SNAP)
                        {
                            if (idx < total - 1) { ++idx; blend = 0.0f; }
                            else                 { blend = 1.0f; }
                        }
                    };
                    snap(app.quiltLeftIdx,  app.quiltLeftBlend);
                    snap(app.quiltRightIdx, app.quiltRightBlend);
                }
            }

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
                app.converter.SetFramePacking(fp.eyeFrac, fp.gapFrac, fp.eyeAlign);
            }
            app.converter.SetQuilt(app.quiltCols, app.quiltRows,
                                   app.quiltLeftIdx, app.quiltRightIdx,
                                   app.quiltLeftBlend, app.quiltRightBlend);
            // Pane = SWAP CHAIN size, not SR panel size. The weaver samples
            // the SBS pane at the output's UV, so a pane that doesn't match
            // the swap-chain aspect gets squeezed when the weaver writes its
            // lens pattern. Matching them means: fullscreen on the SR panel
            // uses the panel aspect (16:9 -> portrait quilt pillarboxed),
            // windowed at the view aspect uses that aspect (3:4 -> view fills
            // the window, NO pillarboxing).
            {
                const UINT sw = app.renderer.Width();
                const UINT sh = app.renderer.Height();
                if (sw > 0 && sh > 0)
                    app.converter.SetTargetPaneSize((int)sw, (int)sh);
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

        case WM_APP_GUI_CAPTURE_DISPLAY:  // GUI display-picker chose a monitor to weave
            if (app) UseDisplay(*app, reinterpret_cast<HMONITOR>(lParam));
            return 0;

        case WM_APP_QUILT_GRID:
            // GUI -> set quilt grid (cols in wParam, rows in lParam). Re-centre
            // the L/R view pair and refresh both the GUI snapshot and the
            // floating overlay's button labels.
            if (app)
            {
                const int c = (int)wParam, r = (int)lParam;
                if (c >= 1 && c <= 12) app->quiltCols = c;
                if (r >= 1 && r <= 9)  app->quiltRows = r;
                const int total = app->quiltCols * app->quiltRows;
                app->quiltLeftIdx  = total / 2 - 1; if (app->quiltLeftIdx  < 0)        app->quiltLeftIdx  = 0;
                app->quiltRightIdx = total / 2;     if (app->quiltRightIdx >= total)   app->quiltRightIdx = total - 1;
                app->quiltLeftBlend = app->quiltRightBlend = 0.0f;
                app->captureRebind = true;
                if (g_fsSet && IsWindowVisible(g_fsSet)) InvalidateRect(g_fsSet, nullptr, FALSE);
            }
            return 0;

        case WM_APP_QUILT_AUTODETECT:
            if (app)
            {
                AutoDetectQuiltOnCurrent(*app);
                if (g_fsSet && IsWindowVisible(g_fsSet)) ShowFsSetOverlay(*app);
            }
            return 0;

        case WM_LBUTTONDOWN:
            // Test-image overlays: settings (left) appears in both modes;
            // window-controls (right) only in fullscreen (windowed already has
            // native chrome with min/max/close); video transport (bottom)
            // only when a video file is the source. Click TOGGLES: if any of
            // the overlays are already visible, hide them all (clicking in
            // the middle of the image dismisses the UI); else show.
            if (app && app->source == SourceKind::TestImage)
            {
                const bool anyVisible =
                    (g_fsCtrl && IsWindowVisible(g_fsCtrl)) ||
                    (g_fsSet  && IsWindowVisible(g_fsSet))  ||
                    (g_fsVid  && IsWindowVisible(g_fsVid));
                if (anyVisible)
                {
                    HideFsCtrlOverlay();
                    HideFsSetOverlay();
                    HideFsVidOverlay();
                }
                else
                {
                    if (app->mode == OutputMode::Fullscreen) ShowFsCtrlOverlay(*app);
                    if (app->mode == OutputMode::Fullscreen ||
                        app->mode == OutputMode::Windowed)
                    {
                        ShowFsSetOverlay(*app);
                        if (app->video.IsOpen()) ShowFsVidOverlay(*app);
                    }
                }
            }
            return 0;

        case WM_MOVE:
            // Keep the floating overlays glued to the window's corners.
            if (app) RepositionOverlays(*app);
            break;

        case WM_DROPFILES:
        {
            HDROP drop = (HDROP)wParam;
            wchar_t path[MAX_PATH] = {};
            const UINT cnt = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            if (cnt > 0 && DragQueryFileW(drop, 0, path, MAX_PATH) > 0 && app)
            {
                char pathA[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, nullptr, nullptr);
                LoadTestImage(*app, pathA);
            }
            DragFinish(drop);
            return 0;
        }

        case WM_APP_FS_BUTTON:
            // Posted from the fs-controls overlay popup when the user clicks one
            // of its buttons: 0 = minimise (standard Windows minimise to
            // taskbar), 1 = switch to Windowed mode (movable window with native
            // chrome), 2 = close (stop weaving -- back to tray, lens / camera
            // off). Hide the overlays first so they don't linger past the
            // window state change.
            if (app)
            {
                HideFsCtrlOverlay();
                HideFsSetOverlay();
                HideFsVidOverlay();
                switch (wParam)
                {
                case 0: ShowWindow(app->hwnd, SW_MINIMIZE); break;
                case 1: app->mode = OutputMode::Windowed; ApplyMode(*app); break;
                case 2: SetWeaving(*app, false); break;
                }
            }
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
                // Re-show the test-image settings overlay so Quilt's cols/rows
                // buttons appear (or vanish) immediately on a format change.
                if (g_fsSet && IsWindowVisible(g_fsSet)) ShowFsSetOverlay(*app);
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
                // (Used to flip TestImage -> passthrough here; we keep the test
                // image so quilt / SBS test files can be viewed fullscreen on the
                // SR display.)
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
            case ID_TRAY_SRC_TESTIMAGE: OpenLoadTestImageDialog(*app); return 0;
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
            if (app)
            {
                // Minimised -> the main window is gone from the screen, so the
                // floating overlays would be orphaned. Hide them.
                if (wParam == SIZE_MINIMIZED) { HideFsCtrlOverlay(); HideFsSetOverlay(); HideFsVidOverlay(); }
                else                          RepositionOverlays(*app);
            }
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




        case WM_SYSCOMMAND:
            // Hijack the native title-bar Maximise -- a true OS maximise would
            // just fill the monitor with chrome still showing, which doesn't
            // match the SR Loom mental model of "go fullscreen". Switch to
            // OutputMode::Fullscreen instead, which removes the chrome AND
            // lays the woven content full-panel (3D works correctly there).
            // Mask off the low 4 bits per SC_* docs (they're internal use).
            if (app && (wParam & 0xFFF0) == SC_MAXIMIZE &&
                app->source == SourceKind::TestImage &&
                app->mode   == OutputMode::Windowed)
            {
                app->mode = OutputMode::Fullscreen;
                ApplyMode(*app);
                return 0;
            }
            break;   // let DefWindowProc handle every other system command

        case WM_CLOSE:
            // Closing the window just pauses weaving and hides to the tray.
            // Hide the floating overlays explicitly so they don't linger as
            // detached popups after the main window's gone.
            HideFsCtrlOverlay();
            HideFsSetOverlay();
            HideFsVidOverlay();
            if (app) SetWeaving(*app, false);
            return 0;

        case WM_DESTROY:
            HideFsCtrlOverlay();
            HideFsSetOverlay();
            HideFsVidOverlay();
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1;  // we fully draw every frame
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

// Top-level structured-exception handler: writes the exception code and the
// faulting module name+address to srweaver.log before the process dies. Lets
// us diagnose unsymbolicated crash dumps from users -- without it, the dump's
// instruction pointer means nothing without our PDB. Returns
// EXCEPTION_CONTINUE_SEARCH so the default Windows error reporter + crash
// dump generation still run.
static LONG WINAPI SrLoomCrashHandler(EXCEPTION_POINTERS* ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* const addr = ep->ExceptionRecord->ExceptionAddress;
    char modName[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod)
        GetModuleFileNameA(mod, modName, MAX_PATH);
    Log("CRASH: code=0x%08X at addr=%p (module: %s)",
        (unsigned)code, addr, modName);
    return EXCEPTION_CONTINUE_SEARCH;
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Install the crash handler first so any later init failure that
    // segfaults / access-violates leaves a useful trail in srweaver.log.
    SetUnhandledExceptionFilter(SrLoomCrashHandler);
    Log("WinMain: SR Loom v1.6 starting");

    // Per-Monitor-Aware v2: unlike v1, Windows auto-scales the NON-CLIENT area
    // (title bar) per monitor — so the GUI's title bar no longer stays huge when
    // dragged from a 4K/150% display to a 1440p/100% one. Falls back to v1.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Media Foundation: needed to decode quilt video files (mp4/mov/etc.).
    VideoSource::Startup();

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

    // Cap the render loop at the SR display's max supported refresh. Without this
    // we'd render past the panel's refresh on fast sources, wasting GPU / heat
    // for frames the panel can't show. Auto-scales per panel: Samsung Odyssey 3D
    // ~165 Hz, Acer SpatialLabs View Pro 27 ~160 Hz, future panels higher.
    if (const double maxHz = MaxMonitorRefreshHz(SrMonitor(app)); maxHz > 0.0)
    {
        app.renderer.SetTargetRefreshHz(maxHz);
        Log("WinMain: render cap set to %.1f Hz (SR display max)", maxHz);
    }
    else
    {
        Log("WinMain: SR display refresh unknown, render rate uncapped");
    }
    // SR display physical size in mm/px, needed for the quilt's window-position
    // perspective shift (windowed mode "looks toward" the window's screen
    // position relative to the head). Zero if Windows can't report it.
    app.srMmPerPx = MonitorMmPerPx(SrMonitor(app));
    Log("WinMain: SR display %.4f mm/px", app.srMmPerPx);

    // Initialize live capture (this is the default source).
    const bool capInit = app.capture.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: capture.Initialize=%d", capInit ? 1 : 0);
    // DXGI Output Duplication is the fallback for exclusive-fullscreen capture; it
    // sits idle until the render loop detects WGC isn't delivering frames for a
    // foreign-display monitor source.
    const bool dxgiInit = app.captureDxgi.Initialize(app.renderer.Device(), app.renderer.Context());
    Log("WinMain: captureDxgi.Initialize=%d", dxgiInit ? 1 : 0);

    // Initialize the format-conversion stage (capture/image -> SBS for the weaver).
    if (!app.converter.Initialize(app.renderer.Device(), app.renderer.Context()))
    {
        Log("WinMain: converter.Initialize FAILED (exit 7)");
        return 7;
    }
    Log("WinMain: converter.Initialize OK");
    // Wrap detector / gui / tray init in begin/end + try/catch -- one of these
    // dying silently between converter.Initialize OK and tray.Add was the
    // failure mode reported by the first user, and "begin" / "end" pairs plus
    // exception messages make it obvious which call is to blame.
    Log("WinMain: detector.Initialize() begin");
    try {
        app.detector.Initialize(app.renderer.Device(), app.renderer.Context());  // optional
    } catch (std::exception& e) {
        Log("WinMain: detector.Initialize() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: detector.Initialize() unknown exception");
    }
    Log("WinMain: detector.Initialize done");
    app.captureRebind = true;   // bind the weaver to the converter output on the first frame

    // Control GUI (left-click the tray icon). Shares the D3D11 device; lives in its
    // own top-level window so it can sit on any monitor with its own taskbar button.
    Log("WinMain: gui.Init() begin");
    bool guiOK = false;
    try {
        guiOK = app.gui.Init(app.hwnd, app.renderer.Device(), app.renderer.Context());
    } catch (std::exception& e) {
        Log("WinMain: gui.Init() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: gui.Init() unknown exception");
    }
    Log("WinMain: gui.Init=%d", guiOK ? 1 : 0);
    // Honor the persisted "start in tray" preference. Default is true (tray-only,
    // matches the historical behaviour); flipping it off pops the control panel
    // on launch so users who prefer that workflow see it immediately.
    if (guiOK && !Settings::ReadStartInTray())
    {
        app.gui.Toggle();
        Log("WinMain: StartInTray=false -> control panel shown");
    }

    // Tray icon + global hotkeys.
    Log("WinMain: tray.Add() begin");
    bool trayOK = false;
    try {
        trayOK = app.tray.Add(app.hwnd, "SR Loom — paused (SR off)");
    } catch (std::exception& e) {
        Log("WinMain: tray.Add() std::exception: %s", e.what());
    } catch (...) {
        Log("WinMain: tray.Add() unknown exception");
    }
    Log("WinMain: tray.Add=%d", trayOK ? 1 : 0);

    // Create the floating overlay windows now (all stay hidden until the user
    // clicks inside a test-image weave).
    EnsureFsCtrlWindow(hInstance);
    EnsureFsSetWindow(hInstance);
    EnsureFsVidWindow(hInstance);

    // Accept dropped image files anywhere on the SR Loom main window -- the
    // WM_DROPFILES handler routes through LoadTestImage with the dropped path.
    DragAcceptFiles(app.hwnd, TRUE);
    const int hk1 = RegisterHotKey(app.hwnd, kHotkeyToggle,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    const int hk2 = RegisterHotKey(app.hwnd, kHotkeyMode,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    const int hk3 = RegisterHotKey(app.hwnd, kHotkeyCapture, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    Log("WinMain: hotkeys W=%d F=%d C=%d", hk1, hk2, hk3);
    // RegisterHotKey(app.hwnd, kHotkeyDetect,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D'); // auto-detect disabled

    // Start idle: release the SR session used to query the display rect so the
    // lens and camera stay off until the user enables weaving (Ctrl+Alt+W).
    app.weavingEnabled = false;
    Log("WinMain: StopSR start");
    app.weaver.StopSR();
    Log("WinMain: StopSR done");
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
                gs.srMonitor     = SrMonitor(app);        // "this display" (excluded from the picker)
                gs.captureMonitor = app.sourceMonitor;    // currently-captured display
                gs.foreignDisplay = app.foreignDisplay;   // a picked display (vs this one) is active
                gs.quiltCols     = app.quiltCols;
                gs.quiltRows     = app.quiltRows;
                gs.hasTestImage  = !app.lastTestImagePath.empty();
                // What's being weaved, for the GUI's collapsed summary line.
                if (app.source == SourceKind::CaptureWindow && app.sourceWindow && IsWindow(app.sourceWindow))
                {
                    if (GetWindowTextA(app.sourceWindow, gs.sourceName, (int)sizeof(gs.sourceName)) <= 0)
                        strncpy_s(gs.sourceName, "Window", _TRUNCATE);
                }
                else
                    strncpy_s(gs.sourceName, app.foreignDisplay ? "Display" : "Monitor", _TRUNCATE);
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
    app.video.Close();
    app.converter.Shutdown();
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    VideoSource::Shutdown();
    g_app = nullptr;
    return 0;
}
