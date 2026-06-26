// SR Weaver — tray app that weaves a side-by-side stereo source onto a
// Simulated Reality display. Milestone 1: tray icon + a static SBS test image
// woven into a window that can toggle between fullscreen and windowed.

#include "Common.h"
#include "Renderer.h"
#include "SRWeaver.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "Capture.h"
#include "CaptureDXGI.h"
#include "Converter.h"
#include "Detector.h"
#include "Gui.h"
#include "Settings.h"
#include "VideoSource.h"
#include "KatangaSource.h"
#include "LFPReader.h"
#include "LFPRenderer.h"
#include "../third_party/one_euro_filter.h"
#include "resource.h"

#include <shellscalingapi.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <windowsx.h>
#include <shellapi.h>      // DragAcceptFiles, DragQueryFile, DragFinish
#include <cmath>
#include <vector>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comdlg32.lib")

using namespace srw;

// --- Undocumented NtQuerySystemInformation plumbing -----------------------
// Used to identify the Katanga publishing process via shared-kernel-object
// handle matching. Same technique Process Explorer / Handle.exe use. The
// struct layout has been stable since Vista; the SystemExtendedHandle-
// Information class returns 64-bit-safe PIDs/handles on Win64.
extern "C" {
    typedef LONG NTSTATUS;
    #ifndef NT_SUCCESS
    #define NT_SUCCESS(x) (((NTSTATUS)(x)) >= 0)
    #endif
    #ifndef STATUS_INFO_LENGTH_MISMATCH
    #define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
    #endif
    typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
        PVOID     Object;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR HandleValue;
        ULONG     GrantedAccess;
        USHORT    CreatorBackTraceIndex;
        USHORT    ObjectTypeIndex;
        ULONG     HandleAttributes;
        ULONG     Reserved;
    } SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;
    typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
        ULONG_PTR                          NumberOfHandles;
        ULONG_PTR                          Reserved;
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX  Handles[1];
    } SYSTEM_HANDLE_INFORMATION_EX;
}
static constexpr int kSystemExtendedHandleInformation = 64;

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
        KatangaSource katanga;       // shared-texture receiver for Bo3b Katanga (Geo-11 etc.)
        LFPRenderer  lfpRenderer;    // per-frame plenoptic SBS render (head-tracked aperture)
                                     // Bound when format==Katanga; the render loop
                                     // arms/engages based on receiver state. Game-exit
                                     // detection lives in KatangaSource itself (it
                                     // re-opens the named mapping each poll, so when
                                     // the publishing process dies the kernel object
                                     // is reclaimed and the next open fails).
        // Main window of the Katanga-publishing game, discovered via
        // NT-API handle-table lookup at reception start. Used to pin SR
        // Loom's overlay specifically ABOVE the game in Z each frame, so
        // games that self-set HWND_TOPMOST on activation can't pop above
        // our weave. Null if discovery failed or no game is publishing.
        HWND       katangaPublisherWnd = nullptr;

        // VR180 / VR360 viewer state (used by the VR converter shader path).
        // yaw / pitch in RADIANS; zoom in [0.2 .. 3.0] (1 = ~90° horizontal
        // FOV, higher = zoomed in). headLook = head-position drives a small
        // additional yaw shift so leaning to the side reveals a bit more of
        // the panorama. vrDrag tracks an in-progress mouse drag-to-look.
        // vrMouseDown / vrMoved distinguish click from drag: on LBUTTONUP
        // without enough movement, fall through to the normal click handler
        // so the user can still toggle the test-image overlays.
        float        vrYaw          = 0.0f;
        float        vrPitch        = 0.0f;
        float        vrZoom         = 1.0f;
        // Default OFF for v1.6 -- the SR head-tracker step rate (~60Hz
        // raw poses) shows up as visible per-frame jitter at the 165Hz
        // render rate that no amount of LP/Accela filtering can fully
        // hide without adding lag. Mouse-drag look-around works great;
        // users can opt in to head-look via the GUI toggle and accept
        // the trade. v1.7 plan: replace GetHeadPose() with a late-
        // latched predicted pose from the SDK.
        bool         vrHeadLook     = false;
        bool         vrMouseDown    = false;
        bool         vrMoved        = false;
        int          vrDragStartX   = 0;
        int          vrDragStartY   = 0;
        int          vrDragLastX    = 0;
        int          vrDragLastY    = 0;
        // Two-stage VR head-tracking filter, one per axis:
        //   1. OneEuro adaptive low-pass smooths the SR-tracker stream.
        //      Run at a fixed 60 Hz constructor freq -- we don't pass a
        //      timestamp at call time because GetTickCount has 15 ms
        //      granularity on Windows, which makes OneEuro's auto-freq
        //      estimate (1/dt) wildly unstable at the 165 Hz render
        //      rate (some frames see dt=0, others dt=15ms).
        //   2. Accela velocity-gain on top with deadzone=0 -- soft
        //      damping of tiny residual inputs via the gain curve
        //      (near-zero gain at small normalised values), no hard
        //      threshold that would cause "still still SNAP" stair-
        //      stepping. Real head turns hit the steep part of the
        //      curve and snap instantly.
        OneEuroFilter vrYawOneEuro    { 60.0f, 1.0f, 0.3f };
        OneEuroFilter vrPitchOneEuro  { 60.0f, 1.0f, 0.3f };
        struct AccelaAxis { double lastOutput = 0.0; DWORD lastTickMs = 0; bool init = false; };
        AccelaAxis    vrYawAccela;
        AccelaAxis    vrPitchAccela;
        bool          vrFilterInit    = false;
        float        convergence    = 0.0f;   // GUI convergence slider (-1..1)
        // Light-field parallax-scale slider value (in mm of head-lean
        // needed to drive the aperture sample to its edge). Lower =
        // more sensitive, higher = closer to the physical camera baseline.
        // Default 30 mm matches the v2 launch behaviour; minimum tracks
        // the actual aperture radius (1:1 physical) and maximum lets the
        // user crank parallax further (eg 10 mm head lean for huge effect).
        float        lfpHeadLeanMm  = 30.0f;
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
        double       headXEMA          = 0.0;   // legacy head-centre EMA (fallback)
        bool         headXEMAInit      = false;
        double       leftEyeXEMA       = 0.0;   // smoothed left-eye-X from getPredictedEyePositions
        double       rightEyeXEMA      = 0.0;
        bool         eyesEMAInit       = false;
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

        // Update checker: URL of the latest GitHub release stashed when the
        // worker thread finds one newer than kAppVersion. Used to open the
        // release page when the user clicks the toast balloon.
        std::string pendingUpdateUrl;
        std::string pendingUpdateTag;
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
        case OutputMode::Fullscreen:
            // Layered fullscreen for passthrough AND for Katanga: both put SR
            // Loom over the entire SR display, both want the bit-blt swap
            // chain. Switching between them then never recreates the swap
            // chain -- which is critical because swap-chain recreation
            // leaves the LeiaSR weaver in a state where its output never
            // reaches the panel (root cause of the post-Katanga black bug).
            return app.source == SourceKind::CaptureMonitor
                || app.format == StereoFormat::Katanga;
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
    constexpr int  kFsSetBtnLoad    = 0;
    constexpr int  kFsSetBtnFormat  = 1;
    constexpr int  kFsSetBtnCols    = 2;
    constexpr int  kFsSetBtnRows    = 3;
    constexpr int  kFsSetBtnAuto    = 4;
    constexpr int  kFsSetBtnLfpLean = 5;   // LightField: Head-lean preset picker
    constexpr int  kFsSetWLoad      = 180;
    constexpr int  kFsSetWFormat    = 250;
    constexpr int  kFsSetWCols      = 140;
    constexpr int  kFsSetWRows      = 140;
    constexpr int  kFsSetWAuto      = 130;
    constexpr int  kFsSetWLfpLean   = 160;
    constexpr int  kQuiltColsMax   = 12;
    constexpr int  kQuiltRowsMax   = 9;

    HWND  g_fsSet         = nullptr;
    int   g_fsSetHover    = -1;
    DWORD g_fsSetLastUse  = 0;
    bool  g_fsSetTracking = false;
    int   g_fsSetBtnCount = 2;
    RECT  g_fsSetRects[6] = {};
    int   g_fsSetCalcW    = 0;
    int   g_fsSetCalcH    = 0;
    // Light-field "head-lean" slider drag state (lives in the FS overlay).
    bool  g_fsSetSliderDrag    = false;
    constexpr float kLfpLeanMinMm = 1.0f;     // floor; clamped to aperture radius too
    constexpr float kLfpLeanMaxMm = 200.0f;   // user request: bumped from 100 to 200

    // Map a layout slot index (0..g_fsSetBtnCount) to the LOGICAL button
    // id (kFsSetBtn*). Slot 0 + 1 are always Load + Format. Beyond that
    // the meaning depends on the active format -- Quilt fills slots 2..4
    // with Cols / Rows / Auto, LightField fills slot 2 with the Head-lean
    // preset picker.
    int FsSetSlotId(int slot)
    {
        if (slot < 2) return slot;   // Load / Format
        const StereoFormat fmt = g_app ? g_app->format : StereoFormat::FullSBS;
        if (fmt == StereoFormat::LightField) return kFsSetBtnLfpLean;
        return slot;                  // Quilt: 2=Cols, 3=Rows, 4=Auto
    }

    int FsSetBtnW(int slot)
    {
        switch (FsSetSlotId(slot)) {
        case kFsSetBtnLoad:    return kFsSetWLoad;
        case kFsSetBtnFormat:  return kFsSetWFormat;
        case kFsSetBtnCols:    return kFsSetWCols;
        case kFsSetBtnRows:    return kFsSetWRows;
        case kFsSetBtnAuto:    return kFsSetWAuto;
        case kFsSetBtnLfpLean: return kFsSetWLfpLean;
        }
        return 60;
    }
    const RECT& FsSetBtnRect(int slot) { return g_fsSetRects[slot]; }

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

                const int id = FsSetSlotId(i);

                // LightField "head-lean" slider: draw a horizontal track
                // + handle on top of the rounded background, with the
                // current value labelled.
                if (id == kFsSetBtnLfpLean)
                {
                    const float val = g_app ? g_app->lfpHeadLeanMm : 30.0f;
                    const float minV = kLfpLeanMinMm;
                    const float maxV = kLfpLeanMaxMm;
                    const float t    = (val - minV) / (maxV - minV);
                    const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                    // Track: thin horizontal rect in the lower 1/3 of the button.
                    const int trackInset = 14;
                    const int trackY = (r.top + r.bottom) / 2 + 6;
                    RECT trackR{ r.left + trackInset, trackY - 2,
                                 r.right - trackInset, trackY + 2 };
                    HBRUSH trackBr = CreateSolidBrush(kFsDim);
                    FillRect(dc, &trackR, trackBr);
                    DeleteObject(trackBr);
                    // Handle: filled accent circle at the value position.
                    const int handleR = 7;
                    const int handleX = trackR.left + (int)(tClamp * (trackR.right - trackR.left));
                    HBRUSH hb = CreateSolidBrush(kFsCloseHot);
                    HPEN hp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
                    HBRUSH ob = (HBRUSH)SelectObject(dc, hb);
                    Ellipse(dc, handleX - handleR, trackY - handleR,
                                handleX + handleR + 1, trackY + handleR + 1);
                    SelectObject(dc, ob);
                    SelectObject(dc, hp);
                    DeleteObject(hb);
                    // Label above the track.
                    wchar_t lbl[40]; swprintf_s(lbl, L"Lean: %.0f mm", val);
                    RECT tr{ r.left + 16, r.top, r.right - 16, trackY - 6 };
                    DrawTextW(dc, lbl, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    continue;   // skip generic button-text rendering
                }

                wchar_t label[64];
                switch (id)
                {
                case kFsSetBtnLoad:    wcscpy_s(label, L"Load Media..."); break;
                case kFsSetBtnFormat:  swprintf_s(label, L"%hs", FsCurrentFormatLabel(fmt)); break;
                case kFsSetBtnCols:    swprintf_s(label, L"%d cols", cols); break;
                case kFsSetBtnRows:    swprintf_s(label, L"%d rows", rows); break;
                case kFsSetBtnAuto:    wcscpy_s(label, L"Auto-detect"); break;
                default:               label[0] = 0;
                }
                const bool hasChevron = (id == kFsSetBtnFormat || id == kFsSetBtnCols
                                       || id == kFsSetBtnRows);
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
            // Drag the LFP lean slider if started.
            if (g_fsSetSliderDrag && g_app)
            {
                // Find the slider slot (LfpLean) -- it owns the drag.
                for (int i = 0; i < g_fsSetBtnCount; ++i)
                {
                    if (FsSetSlotId(i) != kFsSetBtnLfpLean) continue;
                    const RECT r = FsSetBtnRect(i);
                    const int trackInset = 14;
                    const int trackL = r.left + trackInset;
                    const int trackR = r.right - trackInset;
                    const int x = GET_X_LPARAM(lp);
                    const float t = (float)(x - trackL) / (float)(trackR - trackL);
                    const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                    g_app->lfpHeadLeanMm = kLfpLeanMinMm + tClamp * (kLfpLeanMaxMm - kLfpLeanMinMm);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
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
            if (!g_app || hit < 0) return 0;
            const int id = FsSetSlotId(hit);
            switch (id)
            {
            case kFsSetBtnLoad:   OpenLoadTestImageDialog(*g_app); break;
            case kFsSetBtnFormat: FsOpenFormatMenu(); break;
            case kFsSetBtnCols:   FsOpenIntMenu(g_app->quiltCols, kQuiltColsMax, "cols", &g_app->quiltCols); break;
            case kFsSetBtnRows:   FsOpenIntMenu(g_app->quiltRows, kQuiltRowsMax, "rows", &g_app->quiltRows); break;
            case kFsSetBtnAuto:
                AutoDetectQuiltOnCurrent(*g_app);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            case kFsSetBtnLfpLean:
            {
                // Begin dragging the slider; jump to the clicked position.
                g_fsSetSliderDrag = true;
                SetCapture(hwnd);
                const RECT r = FsSetBtnRect(hit);
                const int trackInset = 14;
                const int trackL = r.left + trackInset;
                const int trackR = r.right - trackInset;
                const int x = GET_X_LPARAM(lp);
                const float t = (float)(x - trackL) / (float)(trackR - trackL);
                const float tClamp = t < 0 ? 0 : (t > 1 ? 1 : t);
                g_app->lfpHeadLeanMm = kLfpLeanMinMm + tClamp * (kLfpLeanMaxMm - kLfpLeanMinMm);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_fsSetSliderDrag)
            {
                g_fsSetSliderDrag = false;
                ReleaseCapture();
            }
            return 0;
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
        // Quilt gets Cols / Rows / Auto-detect; LightField gets a Head-lean
        // preset picker; everything else just Load + Format.
        if      (app.format == StereoFormat::Quilt)       g_fsSetBtnCount = 5;
        else if (app.format == StereoFormat::LightField)  g_fsSetBtnCount = 3;
        else                                              g_fsSetBtnCount = 2;
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

        // If an LFP is loaded, the renderer's RT aspect depends on mode:
        // Fullscreen wants SR-display aspect (so the renderer pillarboxes
        // and we get aspect-correct fullscreen with side bars); other
        // modes want CONTENT aspect (so the window can fit tightly with
        // no bars). Looking-glass + Window-overlay also use content
        // since they're framed by other windows / overlays.
        if (app.lfpRenderer.HasData())
        {
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 0.0f;
            const float targetAspect  = (app.mode == OutputMode::Fullscreen)
                                         ? displayAspect : 0.0f;   // 0 = content
            app.lfpRenderer.SetTargetAspect(targetAspect);
        }

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
        case OutputMode::Windowed:
        default:
        {
            // Compute the per-eye content aspect for the current source +
            // format. Used to size the initial window (so it matches the
            // 3D content shape -- no letterbox bars around the inside).
            // FullSBS crops to centre 50% vertical so per-eye is w/2 over
            // h/2; HalfSBS per-eye is w/2 over h; TAB per-eye is w over
            // h/2; etc.
            auto computeAspect = [&]() -> double {
                double aspect = 16.0 / 9.0;
                int sw = 0, sh = 0;
                if (app.source == SourceKind::TestImage)
                {
                    if (app.format == StereoFormat::LightField && app.lfpRenderer.HasData())
                    {
                        sw = app.lfpRenderer.OutputPerEyeWidth();
                        sh = app.lfpRenderer.OutputHeight();
                    }
                    else if (app.video.IsOpen()) { sw = app.video.Width(); sh = app.video.Height(); }
                    else                         { sw = app.weaver.SourceWidth(); sh = app.weaver.SourceHeight(); }
                }
                else
                {
                    // Live capture: source dims come from the active capture.
                    if (app.dxgiActive)        { sw = app.captureDxgi.Width(); sh = app.captureDxgi.Height(); }
                    else if (app.capture.IsActive()) { sw = app.capture.Width(); sh = app.capture.Height(); }
                }
                if (sw <= 0 || sh <= 0) return aspect;
                double aw = (double)sw, ah = (double)sh;
                switch (app.format)
                {
                case StereoFormat::Quilt:
                    if (app.quiltCols > 0 && app.quiltRows > 0) {
                        aw /= app.quiltCols; ah /= app.quiltRows;
                    }
                    break;
                case StereoFormat::FullSBS:
                    aw *= 0.5; ah *= 0.5; break;
                case StereoFormat::HalfSBS:           aw *= 0.5; break;
                case StereoFormat::FullTAB:
                case StereoFormat::HalfTAB:
                case StereoFormat::RowInterleaved:    ah *= 0.5; break;
                case StereoFormat::ColumnInterleaved: aw *= 0.5; break;
                default: break;
                }
                if (aw > 0 && ah > 0) aspect = aw / ah;
                return aspect;
            };

            const bool isLG = (app.mode == OutputMode::LookingGlass);
            style   = WS_OVERLAPPEDWINDOW;
            zorder  = isLG ? HWND_TOPMOST : HWND_NOTOPMOST;
            exStyle = isLG ? (WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT) : 0;

            // LG: preserve user-set size after first entry; only size on
            // initial open. Windowed: always re-size to content shape on
            // mode change.
            if (isLG && app.loupeActive)
            {
                GetWindowRect(hwnd, &rect);
            }
            else
            {
                const double aspect = computeAspect();
                int h = isLG ? 600 : 720;
                int w = (int)(h * aspect + 0.5);
                const int maxW = (dw > 200) ? dw - 100 : 1280;
                const int maxH = (dh > 200) ? dh - 100 : 720;
                if (w > maxW) { w = maxW; h = (int)(w / aspect + 0.5); }
                if (h > maxH) { h = maxH; w = (int)(h * aspect + 0.5); }
                if (w < 320)  { w = 320; }
                if (h < 240)  { h = 240; }
                const int x = d.left + (dw - w) / 2;
                const int y = d.top  + (dh - h) / 2;
                rect = { x, y, x + w, y + h };
                if (isLG) app.loupeActive = true;
            }
            break;
        }
        }

        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_VISIBLE);
        if (exStyle & WS_EX_LAYERED)
            SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);  // fully opaque
        // WindowOverlay applies a rounded-rect region per-frame in
        // UpdateOverlayTracking to match the source window's DWM corners; in
        // every other mode the overlay is a plain rectangle, so clear any
        // stale region left over from a prior WindowOverlay session.
        if (app.mode != OutputMode::WindowOverlay)
            SetWindowRgn(hwnd, nullptr, FALSE);
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

        // Match the source window's Windows-11 rounded corners. WGC captures
        // the source as a rectangular pixel grid (the OS only rounds the
        // display shape via DWM compositing, not the bitmap), so our opaque
        // overlay shows the captured corner pixels as solid black squares
        // by default. SetWindowRgn clips our overlay to a rounded-rect so
        // the desktop shows through there -- matches what the user expects.
        // Recompute only when the source's rect / corner-style changes; the
        // OS reuses an identical region as a no-op.
        DWM_WINDOW_CORNER_PREFERENCE srcCornerPref = DWMWCP_DEFAULT;
        DwmGetWindowAttribute(src, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &srcCornerPref, sizeof(srcCornerPref));
        const bool wantRound = (srcCornerPref != DWMWCP_DONOTROUND);
        const int  w         = r.right - r.left;
        const int  h         = r.bottom - r.top;
        if (wantRound && w > 0 && h > 0)
        {
            // Win11 default rounded radius is 8 DIPs; ROUNDSMALL is 4 DIPs.
            // Scale by the source window's DPI so the overlay matches what
            // DWM actually drew, not what 100% scaling would have looked like.
            const UINT dpi    = GetDpiForWindow(src);
            const int  radDip = (srcCornerPref == DWMWCP_ROUNDSMALL) ? 4 : 8;
            const int  rad    = radDip * (int)dpi / 96;
            // CreateRoundRectRgn's ellipse args are *diameters*, not radii.
            HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, rad * 2, rad * 2);
            // SetWindowRgn takes ownership of rgn -- don't DeleteObject.
            SetWindowRgn(app.hwnd, rgn, FALSE);
        }
        else
        {
            // Source has rounding disabled (DWMWCP_DONOTROUND) -- clear our
            // region so the overlay is a plain rectangle again.
            SetWindowRgn(app.hwnd, nullptr, FALSE);
        }
    }

    void UsePassthrough(AppState& app);   // fwd decl: default weave is screen passthrough

    // Velocity-aware Accela filter (one axis). Stage 2 of the VR head-
    // tracking chain (stage 1 = OneEuro). Piecewise gain curve takes
    // raw deltas to a follow-rate -- small inputs ride a near-zero gain
    // (soft damping of tracker noise), real head turns hit the steep
    // part and snap. We DON'T use Accela's deadzone here because the
    // hard "no movement below threshold" cutoff caused visible jumps
    // when input crossed the boundary. The gain curve's own first
    // segment provides the damping smoothly.
    double AccelaApply(AppState::AccelaAxis& a, double raw, double threshold)
    {
        static const struct { double x, y; } kGains[] = {
            { 0.0, 0.0 }, { 0.5, 0.4  }, { 1.0, 1.5   }, { 1.5, 8.0   },
            { 2.5, 35.0 }, { 5.0, 100.0 }, { 8.0, 200.0 }, { 9.0, 300.0 }
        };
        constexpr int kN = (int)(sizeof(kGains) / sizeof(kGains[0]));

        const DWORD now = GetTickCount();
        if (!a.init) { a.init = true; a.lastOutput = raw; a.lastTickMs = now; return raw; }
        double dt = (now - a.lastTickMs) * 0.001;
        a.lastTickMs = now;
        if (dt < 1e-5) dt = 1e-5;
        if (dt > 0.25) dt = 0.25;

        const double delta    = raw - a.lastOutput;
        const double absDelta = fabs(delta);
        const double normalized = absDelta / (threshold > 1e-9 ? threshold : 1e-9);

        double gain = kGains[kN - 1].y;
        if (normalized <= kGains[0].x) gain = kGains[0].y;
        else
            for (int i = 1; i < kN; ++i)
                if (normalized <= kGains[i].x)
                {
                    const double t = (normalized - kGains[i - 1].x) / (kGains[i].x - kGains[i - 1].x);
                    gain = kGains[i - 1].y + (kGains[i].y - kGains[i - 1].y) * t;
                    break;
                }

        double alpha = (absDelta > 1e-9) ? (dt * gain / absDelta) : 0.0;
        if (alpha > 1.0) alpha = 1.0;
        if (alpha < 0.0) alpha = 0.0;
        a.lastOutput += alpha * delta;
        return a.lastOutput;
    }

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
            // Katanga arm (format = Katanga, no game publishing yet): leave the
            // SR session alive but ask SwitchableLensHint to disable the lens.
            // No StartSR/StopSR churn, no swap-chain recreation risk (that was
            // the original source of the post-Katanga black bug). The SR
            // session is always kept alive while weavingEnabled.
            const bool katangaArmed = (app.format == StereoFormat::Katanga
                                      && !app.katanga.IsReceiving());
            if (!app.weaver.HasWeaver())
                app.weaver.StartSR(app.renderer.Context(), app.hwnd);
            if (katangaArmed) app.weaver.LensDisable();
            else              app.weaver.LensEnable();
            // Default action: weave the screen (fullscreen SBS passthrough). If the
            // chosen source is the monitor but capture isn't running yet, start it.
            if (app.source == SourceKind::CaptureMonitor && !app.capture.IsActive())
                UsePassthrough(app);
            // Bind the Katanga receiver if we're (re-)enabling weaving with
            // format == Katanga. Idempotent if already begun.
            if (app.format == StereoFormat::Katanga && !app.katanga.IsActive())
                app.katanga.Begin(app.renderer.Device());
            app.captureRebind = true;   // re-register the converter output
            ApplyMode(app);
            ShowWindow(app.hwnd, katangaArmed ? SW_HIDE : SW_SHOW);
        }
        else
        {
            // Hide and fully release SR so the lens and camera turn off.
            ShowWindow(app.hwnd, SW_HIDE);
            HideFsCtrlOverlay();
            app.weaver.LensDisable();
            app.weaver.StopSR();
        }
        app.tray.SetTooltip(enable ? "SR Loom — weaving" : "SR Loom — paused (SR off)");
    }

    // Selecting a source or format turns weaving ON if it isn't already (so the 3D
    // comes on automatically); if it already is, re-apply the current mode AND
    // restart SR if it's been torn down (e.g. switching back out of Katanga arm,
    // where the SR session was stopped while no game was publishing).
    void EnsureWeaving(AppState& app)
    {
        if (!app.weavingEnabled) { SetWeaving(app, true); return; }
        const bool katangaArmed = (app.format == StereoFormat::Katanga
                                  && !app.katanga.IsReceiving());
        if (!app.weaver.HasWeaver())
        {
            app.weaver.StartSR(app.renderer.Context(), app.hwnd);
            app.captureRebind = true;
        }
        // Lens hint matches format state: lens off during Katanga arm
        // (cooperative -- the lens still actually engages if any other SR
        // app has it on); lens on for any active weave format.
        if (katangaArmed) app.weaver.LensDisable();
        else              app.weaver.LensEnable();
        ApplyMode(app);
        if (katangaArmed) ShowWindow(app.hwnd, SW_HIDE);
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

    // Centralised format setter -- handles Katanga's lifecycle (it's the only
    // format that owns external state: the shared-texture receiver, plus an
    // arm-mode lens-off until a game starts publishing). All other formats
    // are a plain pixel-interpretation change in the converter; nothing to
    // tear down. Always sets captureRebind so the next frame re-registers
    // the SRV at the new format. Re-applies the window mode iff Katanga is
    // transitioning in or out -- WantsClickThrough's Fullscreen branch keys
    // off format==Katanga, so the swap-chain mode (flip vs bit-blt) must be
    // refreshed on those edges.
    void ChangeFormat(AppState& app, StereoFormat newFmt)
    {
        const StereoFormat oldFmt = app.format;
        const bool katangaEdge = (oldFmt == StereoFormat::Katanga)
                              != (newFmt == StereoFormat::Katanga);
        app.format = newFmt;
        app.captureRebind = true;
        // Invalidate the LookingGlass / Windowed "saved" size so the next
        // ApplyMode call re-fits the window to the new format's content
        // aspect (e.g. FullSBS 16:9 per-eye vs HalfSBS 8:9). Without this
        // the window keeps its old shape and content gets pillarboxed or
        // squished in the window for the rest of the session.
        if (oldFmt != newFmt && app.mode == OutputMode::LookingGlass)
        {
            app.loupeActive = false;
            ApplyMode(app);
        }
        if (newFmt == StereoFormat::Katanga && oldFmt != StereoFormat::Katanga)
        {
            // Entering Katanga: start the receiver, arm-mode (lens off, our
            // window hidden) until a game publishes -- the render loop's
            // Katanga branch flips them back on at the first received frame.
            app.katanga.Begin(app.renderer.Device());
            app.weaver.LensDisable();
            ShowWindow(app.hwnd, SW_HIDE);
        }
        else if (oldFmt == StereoFormat::Katanga && newFmt != StereoFormat::Katanga)
        {
            // Leaving Katanga: stop the receiver, lens back on, show our
            // window. The format change below will trigger the converter
            // path with whichever Source is still bound.
            app.katanga.End();
            app.katangaPublisherWnd = nullptr;
            app.weaver.LensEnable();
            ShowWindow(app.hwnd, SW_SHOW);
            // Drop topmost (set by the render-loop receive transition).
            SetWindowPos(app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (katangaEdge) ApplyMode(app);   // refresh layered/swap-chain state

        // LightField on-load means an LFP file is bound to the LFPRenderer.
        // If the user picks a different format while in LightField, the
        // user's effectively saying "stop using the loaded LFP" -- so
        // release the GPU resources for it. (Selecting LightField without
        // a loaded LFP is a no-op until the user loads one.)
        if (oldFmt == StereoFormat::LightField && newFmt != StereoFormat::LightField)
            app.lfpRenderer.Unload();
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

    // VR180/VR360 and LightField only make sense for static / file-loaded
    // sources (equirect images / 360° video, Lytro plenoptic photos);
    // they're meaningless on a live screen-capture source. Called from
    // each capture-source setter to drop the user back to Half SBS if
    // they were on one of these formats when switching to a live source.
    // Also tears down the LFP renderer so its stale RT doesn't keep
    // shadowing the captured frames via the `if (lfpRenderer.HasData())`
    // branch in the main render loop.
    void DemoteVRFormatForLiveCapture(AppState& app)
    {
        if (IsVRFormat(app.format) || app.format == StereoFormat::LightField)
        {
            if (app.format == StereoFormat::LightField)
                app.lfpRenderer.Unload();
            ChangeFormat(app, StereoFormat::HalfSBS);
        }
    }

    // Capture a window and present it as an in-place 3D overlay tracking that window.
    void UseWindow(AppState& app, HWND target)
    {
        DemoteVRFormatForLiveCapture(app);
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
        DemoteVRFormatForLiveCapture(app);
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
    extern "C" unsigned char* stbi_load_from_memory(unsigned char const* buffer,
                                                     int len, int* x, int* y,
                                                     int* channels_in_file,
                                                     int desired_channels);
    typedef unsigned char stbi_uc;
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
    // True if the file extension (case-insensitive) matches a Lytro
    // light-field container -- .lfp, .lfr, .lfx all use the same LFP
    // container format and resolve through LFPLoadAsStereoSBS.
    static bool IsLytroLightFieldFile(const char* path)
    {
        if (!path) return false;
        const char* dot = strrchr(path, '.');
        if (!dot) return false;
        // Quick ASCII lowercase compare.
        auto eqi = [](const char* a, const char* b) {
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
                if (ca != cb) return false;
                ++a; ++b;
            }
            return *a == 0 && *b == 0;
        };
        return eqi(dot, ".lfp") || eqi(dot, ".lfr") || eqi(dot, ".lfx");
    }

    bool LoadTestImage(AppState& app, const char* path)
    {
        if (!path || !*path) return false;

        // Tear down whichever source was last loaded (video or image) so we
        // don't end up with both bound.
        app.video.Close();

        const bool isVideo = VideoSource::IsVideoFile(path);
        const bool isLFP   = IsLytroLightFieldFile(path);
        const bool isEslf  = srw::IsLytroEslfPng(path);
        // Tear down the LFPRenderer if a non-LFP source is loaded -- the
        // render loop's `if (lfpRenderer.HasData())` branch would otherwise
        // shadow the regular image / video path with the stale LFP RT.
        if (!isLFP && !isEslf) app.lfpRenderer.Unload();
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
        else if (isLFP)
        {
            // Lytro light-field photo: full plenoptic 3D path. The
            // demosaic step inside LFPLoadDemosaicedSensor automatically
            // histogram-matches our colour output against the embedded
            // JPG preview (when present, which is every LFR/LFX) -- so
            // we get 3D parallax AND Lytro's pipeline colour science.
            std::vector<uint8_t> sensorRgb;
            srw::LFPCalibration cal;
            if (!srw::LFPLoadDemosaicedSensor(path, sensorRgb, cal))
            {
                ShowError("Failed to decode Lytro light-field file.\n\n"
                          "See srweaver.log for details. Supports .lfp / .lfr / "
                          ".lfx (Lytro cameras). The file must contain raw "
                          "plenoptic sensor data + calibration metadata.");
                return false;
            }
            const RECT& d = app.srDisplayRect;
            const int dw = d.right - d.left;
            const int dh = d.bottom - d.top;
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 1.0f;
            if (!app.lfpRenderer.LoadFromMemory(sensorRgb, cal, displayAspect))
            {
                ShowError("Failed to upload Lytro sensor data to GPU.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            app.format = StereoFormat::LightField;
        }
        else if (isEslf)
        {
            // Lytro ESLF PNG: a colour-corrected microlens-array image
            // exported from Lytro Desktop. Treats it as if it were our
            // demosaiced sensor output and feeds it through the same
            // LFPRenderer that handles raw LFRs -- so we get 3D
            // parallax AND Lytro's colour science (the PNG is the
            // result of their full render pipeline, baked in). The
            // calibration is synthesised from Illum-typical defaults.
            std::vector<uint8_t> sensorRgb;
            srw::LFPCalibration cal;
            if (!srw::LFPLoadEslfAsSensorRgb(path, sensorRgb, cal))
            {
                ShowError("Failed to load Lytro ESLF PNG.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            const RECT& d = app.srDisplayRect;
            const int dw = d.right - d.left;
            const int dh = d.bottom - d.top;
            const float displayAspect = (dh > 0) ? (float)dw / (float)dh : 1.0f;
            if (!app.lfpRenderer.LoadFromMemory(sensorRgb, cal, displayAspect))
            {
                ShowError("Failed to upload ESLF data to GPU.\n\n"
                          "See srweaver.log for details.");
                return false;
            }
            app.format = StereoFormat::LightField;
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
        // A new test image's content aspect / dimensions almost certainly
        // differ from whatever was last shown -- invalidate the LookingGlass
        // saved size so the next LG entry re-fits the window to the new
        // content instead of preserving the old shape.
        app.loupeActive   = false;
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
            "Stereo media\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.lfp;*.lfr;*.lfx\0"
            "Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0"
            "Lytro light field\0*.lfp;*.lfr;*.lfx;*_eslf.png;*_qs14x14*.png\0"
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
        DemoteVRFormatForLiveCapture(app);
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
        // The detector only ever returns SBS/TAB/Anaglyph. If the user is
        // on a format with its own input pipeline (Katanga shared texture,
        // LFP plenoptic, VR equirect, Quilt, frame-packing, Pulfrich
        // temporal) a "detection" would silently downgrade it -- skip.
        switch (app.format)
        {
        case StereoFormat::Katanga:
        case StereoFormat::LightField:
        case StereoFormat::Quilt:
        case StereoFormat::VR180TAB:
        case StereoFormat::VR180SBS:
        case StereoFormat::VR360TAB:
        case StereoFormat::VR360SBS:
        case StereoFormat::Pulfrich:
        case StereoFormat::FramePacking:
            return;
        default:
            break;
        }
        ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0;
        if (!ResolveSource(app, srv, w, h)) return;
        StereoFormat f;
        if (app.detector.Detect(srv, w, h, f))
        {
            ChangeFormat(app, f);
            app.tray.SetTooltip("SR Loom — detected a stereo layout");
        }
        else
        {
            app.tray.SetTooltip("SR Loom — no stereo layout detected");
        }
    }

    // Find the PID of the process publishing to Local\KatangaMappedFile via
    // the kernel handle table: open the mapping ourselves, look up the
    // kernel Object pointer behind our handle, then enumerate ALL handles
    // in the system and return the first PID that isn't ours sharing the
    // same Object. Same trick Process Explorer / Handle.exe use. Returns
    // 0 on any failure -- caller should fall back to generic topmost.
    DWORD FindKatangaPublisherPid()
    {
        using NtQSI_t = NTSTATUS (NTAPI*)(int, PVOID, ULONG, PULONG);
        static auto pNtQSI = (NtQSI_t)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
        if (!pNtQSI)
        {
            Log("Katanga/publisher: NtQuerySystemInformation lookup failed");
            return 0;
        }

        HANDLE myMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "Local\\KatangaMappedFile");
        if (!myMap)
        {
            Log("Katanga/publisher: OpenFileMapping(myself) failed err=%lu", GetLastError());
            return 0;
        }
        const DWORD myPid = GetCurrentProcessId();

        // Buffer-grow loop. Typical handle-table size on a desktop is ~1-4 MB.
        std::vector<uint8_t> buf(64 * 1024);
        NTSTATUS st = STATUS_INFO_LENGTH_MISMATCH;
        ULONG    retLen = 0;
        for (int tries = 0; tries < 10 && st == STATUS_INFO_LENGTH_MISMATCH; ++tries)
        {
            st = pNtQSI(kSystemExtendedHandleInformation, buf.data(),
                        (ULONG)buf.size(), &retLen);
            if (st == STATUS_INFO_LENGTH_MISMATCH)
                buf.resize(buf.size() * 2);
        }
        if (!NT_SUCCESS(st))
        {
            Log("Katanga/publisher: NtQSI status=0x%08X retLen=%lu bufSize=%zu",
                (unsigned)st, retLen, buf.size());
            CloseHandle(myMap);
            return 0;
        }
        auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(buf.data());

        // First pass: find our own entry to capture the kernel Object pointer.
        PVOID    myObject = nullptr;
        ULONG_PTR myCount = 0;   // how many of OUR handles to the mapping (sanity)
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
        {
            const auto& e = info->Handles[i];
            if (e.UniqueProcessId == myPid
                && e.HandleValue   == (ULONG_PTR)myMap)
            {
                myObject = e.Object;
                ++myCount;
            }
        }
        CloseHandle(myMap);
        if (!myObject)
        {
            Log("Katanga/publisher: own handle not found in %llu-entry table (myPid=%lu myMap=%p)",
                (unsigned long long)info->NumberOfHandles, myPid, (void*)myMap);
            return 0;
        }

        // Second pass: find another PID with a handle to the same kernel object.
        int otherMatches = 0;
        DWORD firstOther = 0;
        for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i)
        {
            const auto& e = info->Handles[i];
            if (e.Object == myObject && e.UniqueProcessId != myPid)
            {
                ++otherMatches;
                if (!firstOther) firstOther = (DWORD)e.UniqueProcessId;
            }
        }
        Log("Katanga/publisher: table=%llu handles, ourMatches=%llu obj=%p otherMatches=%d firstOther=%lu",
            (unsigned long long)info->NumberOfHandles,
            (unsigned long long)myCount, myObject, otherMatches, firstOther);
        return firstOther;
    }

    // Pick the largest visible top-level window owned by the given PID.
    // Games sometimes have a small launcher window plus a big render window;
    // largest-area is a reliable heuristic for the actual game window.
    HWND FindMainWindowOfPid(DWORD pid)
    {
        struct Ctx { DWORD pid; HWND best; LONG bestArea; };
        Ctx ctx{ pid, nullptr, 0 };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD wpid = 0;
            GetWindowThreadProcessId(h, &wpid);
            if (wpid != c->pid) return TRUE;
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER)) return TRUE;
            RECT r{};
            if (!GetWindowRect(h, &r)) return TRUE;
            const LONG area = (r.right - r.left) * (r.bottom - r.top);
            if (area > c->bestArea) { c->bestArea = area; c->best = h; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
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

        // WindowOverlay mode: another app can pop above our overlay and
        // fully hide the woven output. Skip the weave + Present when the
        // SR SDK's Window2 reports the overlay region is fully occluded --
        // saves GPU work + avoids presenting frames the user can't see.
        // (Fullscreen / Katanga modes are HWND_TOPMOST so don't get covered.)
        //
        // HYSTERESIS: during a window drag DWM transiently reports our
        // overlay as not-visible while it catches up on the z-order, which
        // would cause us to skip-then-render every few frames -- visible as
        // flicker (especially in anaglyph where the converter path is more
        // sensitive to frame skips than the identity-SBS fast path). Only
        // actually pause after several consecutive occluded reports, and
        // reset immediately on any "visible".
        static int occludedFrames = 0;
        if (app.mode == OutputMode::WindowOverlay)
        {
            RECT cr{}; GetClientRect(app.hwnd, &cr);
            if (cr.right > cr.left && cr.bottom > cr.top)
            {
                if (app.weaver.IsWindowPartVisible(app.hwnd,
                                                   cr.right - cr.left,
                                                   cr.bottom - cr.top))
                    occludedFrames = 0;
                else
                    ++occludedFrames;
                // ~10 frames @ 60fps = ~166ms before we trust the "occluded"
                // signal. Faster monitors will trip the threshold sooner in
                // wall-clock terms, which is fine -- it's still well past
                // any drag-induced transient.
                if (occludedFrames >= 10)
                {
                    Sleep(10);
                    return;
                }
            }
            else
            {
                occludedFrames = 0;   // degenerate rect -> reset
            }
        }
        else
        {
            occludedFrames = 0;   // mode change -> reset
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

        // External-source format override: Katanga publishes the woven SBS
        // directly via its shared-texture handoff. Replace whatever the
        // Source resolved to. Source/Mode still control PLACEMENT (which
        // window the overlay tracks, fullscreen on the SR display, etc).
        if (app.format == StereoFormat::Katanga)
        {
            const bool wasReceiving = (app.katanga.SRV() != nullptr);
            const bool nowReceiving = app.katanga.Update();
            if (nowReceiving != wasReceiving)
            {
                app.captureRebind = true;
                if (nowReceiving)
                {
                    // Game just started publishing -- discover the
                    // publisher's main window via the kernel handle table
                    // so we can z-pin above it, re-engage the lens (SR
                    // session has been alive the whole time, only the
                    // SwitchableLensHint was disabled during arm), show the
                    // window, and pin topmost so the game's own window
                    // can't pop above the weave when focused. Fall back to
                    // StartSR if the session was actually torn down.
                    const DWORD pubPid = FindKatangaPublisherPid();
                    app.katangaPublisherWnd = pubPid ? FindMainWindowOfPid(pubPid) : nullptr;
                    // Fallback when NT-API discovery fails (publisher in
                    // higher integrity level, protected process, etc.):
                    // use lastForeground (filtered to never be us, the
                    // shell, or a popup). Better than no Z-target at all.
                    if (!app.katangaPublisherWnd
                        && app.lastForeground && IsWindow(app.lastForeground))
                    {
                        app.katangaPublisherWnd = app.lastForeground;
                        Log("Katanga: publisher discovery failed, falling back to lastForeground=%p",
                            (void*)app.katangaPublisherWnd);
                    }
                    if (!app.weaver.HasWeaver())
                        app.weaver.StartSR(app.renderer.Context(), app.hwnd);
                    app.weaver.LensEnable();
                    ShowWindow(app.hwnd, SW_SHOW);
                    SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    Log("Katanga: reception started (%dx%d, publisher pid=%lu hwnd=%p)",
                        app.katanga.Width(), app.katanga.Height(),
                        pubPid, (void*)app.katangaPublisherWnd);
                }
                else
                {
                    // Game closed: drop back to arm-mode (lens off, window
                    // hidden, non-topmost). Render loop keeps polling the
                    // Katanga mapping so the next publishing game auto-
                    // engages without the user touching anything. Clear the
                    // backbuffer first so the panel doesn't briefly show the
                    // last woven frame as the window vanishes.
                    app.katangaPublisherWnd = nullptr;
                    app.renderer.BindAndClearBackBuffer();
                    app.renderer.Present(false);
                    app.weaver.LensDisable();
                    SetWindowPos(app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    ShowWindow(app.hwnd, SW_HIDE);
                    Log("Katanga: reception lost -> arm-mode (waiting for next game)");
                    return;
                }
            }
            srcSRV   = app.katanga.SRV();
            srcW     = app.katanga.Width();
            srcH     = app.katanga.Height();
            gotFrame = nowReceiving;
            // Pin SR Loom's overlay directly above the publishing game's
            // main window each frame. Many DirectX games SetWindowPos
            // HWND_TOPMOST on themselves when activated, which would pop
            // them above us in the topmost band on a click-into-game --
            // forcing the user to alt-tab to SR Loom to push the weave
            // back up. Inserting just above the game's HWND specifically
            // (not generic HWND_TOPMOST) wins regardless of which band
            // the game is in. SWP_NOACTIVATE means we never steal focus.
            if (nowReceiving && app.katangaPublisherWnd
                && IsWindow(app.katangaPublisherWnd))
            {
                SetWindowPos(app.hwnd, app.katangaPublisherWnd, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            else if (nowReceiving)
            {
                // Fallback when publisher discovery failed: generic topmost.
                SetWindowPos(app.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        // Lytro Light Field: per-frame head-tracked plenoptic sampler.
        // The LFPRenderer owns the SBS view -- the source pipeline below
        // is skipped entirely. The user's eye-mm positions are scaled to
        // make typical head leans cover the full (tiny) Lytro aperture --
        // physically not 1:1 but perceptually MUCH more usable since the
        // F01 aperture is just 3.4 mm vs ~62 mm IPD.
        if (app.format == StereoFormat::LightField && app.lfpRenderer.HasData())
        {
            float l[3] = {}, r[3] = {};
            float lu = 0, lv = 0, ru = 0, rv = 0;
            if (app.weaver.GetPredictedEyePositions(l, r))
            {
                // Window-position-aware view angle: SR tracker reports
                // eye position in mm relative to the SR display CENTRE.
                // If we render the LFP in a windowed sub-rect (not full
                // SR display), the user perceives the WINDOW centre, not
                // the display centre, as the "reference point". Subtract
                // the window-centre-to-display-centre offset so the
                // captured scene shifts the way it would for a real
                // window onto a real scene.
                double windowOffsetMmX = 0.0, windowOffsetMmY = 0.0;
                if (app.srMmPerPx > 0.0 && app.mode != OutputMode::Fullscreen)
                {
                    RECT wr{};
                    if (GetWindowRect(app.hwnd, &wr))
                    {
                        const double winCxPx = 0.5 * (wr.left + wr.right);
                        const double winCyPx = 0.5 * (wr.top  + wr.bottom);
                        const double dispCxPx = 0.5 * (app.srDisplayRect.left + app.srDisplayRect.right);
                        const double dispCyPx = 0.5 * (app.srDisplayRect.top  + app.srDisplayRect.bottom);
                        windowOffsetMmX = (winCxPx - dispCxPx) * app.srMmPerPx;
                        windowOffsetMmY = (winCyPx - dispCyPx) * app.srMmPerPx;
                    }
                }
                const float eyeLX = l[0] - (float)windowOffsetMmX;
                const float eyeLY = l[1] - (float)windowOffsetMmY;
                const float eyeRX = r[0] - (float)windowOffsetMmX;
                const float eyeRY = r[1] - (float)windowOffsetMmY;

                const double apertureMmRadius =
                    0.5 * app.lfpRenderer.ApertureDiameterMetres() * 1e3;
                if (apertureMmRadius > 0.0)
                {
                    // GUI-tunable: head-lean distance (mm) that drives
                    // the aperture sample to its edge. Min = aperture
                    // radius (true physical 1:1, microscopic on F01),
                    // default 30, larger = even more amplified parallax.
                    // Note: V is negated -- SR tracker reports head y
                    // positive = up; sensor / image y is positive = down.
                    // So head-up should sample the upper part of the
                    // aperture, which in image coords is negative.
                    const double leanMm = (std::max)((double)app.lfpHeadLeanMm,
                                                      apertureMmRadius);
                    auto clampUnit = [](float v) { return v >  1.0f ? 1.0f
                                                       : (v < -1.0f ? -1.0f : v); };
                    lu = clampUnit((float)( eyeLX / leanMm));
                    lv = clampUnit((float)(-eyeLY / leanMm));
                    ru = clampUnit((float)( eyeRX / leanMm));
                    rv = clampUnit((float)(-eyeRY / leanMm));
                }
            }
            else
            {
                // No eye-track data yet: static L/R sub-aperture extremes
                // so the user sees stereo immediately rather than 2 identical views.
                lu = -0.7f; ru = +0.7f;
            }
            app.lfpRenderer.Run(lu, lv, ru, rv);
            app.weaver.SetInputView(app.lfpRenderer.OutputSRV(),
                                    app.lfpRenderer.OutputPerEyeWidth(),
                                    app.lfpRenderer.OutputHeight(),
                                    app.lfpRenderer.OutputFormat());
            app.captureRebind = false;
            goto skipSourcePipeline;
        }

        {
        const bool liveSource  = (app.source != SourceKind::TestImage)
                              || app.video.IsOpen()
                              || app.format == StereoFormat::Katanga;
        // Quilt is included so the converter re-runs every frame even on a
        // static test image -- its L/R view indices follow the head position.
        const bool temporalFmt = (app.format == StereoFormat::Pulfrich ||
                                  app.format == StereoFormat::FrameSequential ||
                                  app.format == StereoFormat::Quilt);
        const bool noConv      = (app.convergence < 1e-4f && app.convergence > -1e-4f);
        // HalfSBS is identity-passable (each source half = each eye, no
        // transform). FullSBS now does a vertical centre-crop in the
        // shader so a letterboxed 32:9 source projects correctly --
        // can't skip the converter for it.
        const bool halfSbsFmt  = (app.format == StereoFormat::HalfSBS);
        const bool sbsFmt      = (app.format == StereoFormat::FullSBS ||
                                  app.format == StereoFormat::HalfSBS);
        // Katanga always publishes FullSBS-layout pixels; route it through the
        // identity fast path so the weaver's internal bilinear handles any
        // scaling between the game's render res and the output window/display.
        const bool katangaFmt  = (app.format == StereoFormat::Katanga);
        const bool identitySBS = liveSource && (halfSbsFmt || katangaFmt) && !app.swapEyes && noConv;

        if (identitySBS)
        {
            if (srcSRV && srcW > 0 && srcH > 0 && (app.captureRebind || capSizeChanged))
            {
                DXGI_FORMAT srvFmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                if      (katangaFmt)     srvFmt = app.katanga.Format();
                else if (app.dxgiActive) srvFmt = app.captureDxgi.SRVFormat();
                else                     srvFmt = app.capture.SRVFormat();
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
        else if (srcSRV && srcW > 0 && srcH > 0 && ((liveSource && gotFrame) || temporalFmt || IsVRFormat(app.format) || app.captureRebind))
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
                const double HEAD_FULL_SWEEP_MM = 660.0;   // ±330mm covers all views
                // EMA is lighter than the legacy head-centre path because
                // getPredictedEyePositions returns latency-corrected /
                // already-filtered positions -- extra smoothing here just
                // adds perceived lag. Fallback head-pose path still uses
                // the heavier filter (alpha 0.10) under the same constant
                // since head pose is noisier than weaver-predicted eyes.
                const double EMA_ALPHA_EYES = 0.30;        // per-eye (predicted)
                const double EMA_ALPHA_HEAD = 0.10;        // head-centre fallback
                const float  BLEND_SNAP     = 0.08f;       // shimmer-kill near integer boundaries
                const int    total              = app.quiltCols * app.quiltRows;

                // Prefer the weaver's predicted per-eye positions (latency-
                // corrected, real IPD -- no need to assume a 64mm IOD).
                // Falls back to head-centre +/- assumed-IOD if for any reason
                // the weaver isn't ready.
                float lEye[3] = {}, rEye[3] = {};
                double leftEyeX = 0.0, rightEyeX = 0.0;
                bool haveEyes = false;
                bool fromPredicted = false;
                if (app.weaver.GetPredictedEyePositions(lEye, rEye))
                {
                    leftEyeX  = (double)lEye[0];
                    rightEyeX = (double)rEye[0];
                    haveEyes  = true;
                    fromPredicted = true;
                }
                else
                {
                    double hp[3] = {}, ho[3] = {};
                    if (app.weaver.GetHeadPose(hp, ho))
                    {
                        const double IOD_MM_DEFAULT = 64.0;
                        leftEyeX  = hp[0] - IOD_MM_DEFAULT * 0.5;
                        rightEyeX = hp[0] + IOD_MM_DEFAULT * 0.5;
                        haveEyes  = true;
                    }
                }

                if (haveEyes)
                {
                    // EMA per eye -- kills tracker noise without flattening
                    // real IPD differences (which now flow straight through).
                    // Predicted positions are already filtered upstream, so a
                    // higher alpha (less smoothing) keeps response snappy.
                    const double ema = fromPredicted ? EMA_ALPHA_EYES : EMA_ALPHA_HEAD;
                    if (!app.eyesEMAInit)
                    {
                        app.leftEyeXEMA  = leftEyeX;
                        app.rightEyeXEMA = rightEyeX;
                        app.eyesEMAInit  = true;
                    }
                    else
                    {
                        app.leftEyeXEMA  = ema * leftEyeX  + (1.0 - ema) * app.leftEyeXEMA;
                        app.rightEyeXEMA = ema * rightEyeX + (1.0 - ema) * app.rightEyeXEMA;
                    }

                    // Diagnostic: log the predicted eye positions + measured
                    // IPD ~once a second so we can confirm the runtime is
                    // returning sensible values + tell whether the user's
                    // tracked IPD differs from the 64mm fallback assumption.
                    if (fromPredicted)
                    {
                        static DWORD lastEyeLogTick = 0;
                        const DWORD now = GetTickCount();
                        if (now - lastEyeLogTick > 1000)
                        {
                            lastEyeLogTick = now;
                            const float ipd = rEye[0] - lEye[0];
                            Log("Quilt eyes: L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) "
                                "IPD=%.1fmm (smoothed L.x=%.1f R.x=%.1f)",
                                lEye[0], lEye[1], lEye[2],
                                rEye[0], rEye[1], rEye[2], ipd,
                                app.leftEyeXEMA, app.rightEyeXEMA);
                        }
                    }

                    // Window-position perspective shift: when SR Loom is windowed
                    // on the SR display, the user's "looking toward" the window's
                    // SCREEN POSITION, not the panel centre. Subtract the window's
                    // centre-relative-to-panel-centre offset (in mm) from each
                    // eye-x so a head centred ON the window picks views around
                    // N/2 regardless of where the window sits on the panel.
                    double windowOffsetMm = 0.0;
                    if (app.srMmPerPx > 0.0 && app.hwnd)
                    {
                        RECT wr{}; GetWindowRect(app.hwnd, &wr);
                        const int winCenterX  = (wr.left + wr.right) / 2;
                        const int panCenterX  = (app.srDisplayRect.left + app.srDisplayRect.right) / 2;
                        windowOffsetMm = (double)(winCenterX - panCenterX) * app.srMmPerPx;
                    }

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
                    // Each eye picks its own view from its OWN absolute
                    // position (no IOD assumption needed). Shader cross-fades
                    // between view[idx] and view[idx+1] by the fractional
                    // component for continuous motion across view boundaries.
                    fracToIdxBlend(mapEyeXToFrac(app.leftEyeXEMA  - windowOffsetMm),
                                   app.quiltLeftIdx,  app.quiltLeftBlend);
                    fracToIdxBlend(mapEyeXToFrac(app.rightEyeXEMA - windowOffsetMm),
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
            // VR view -- if Headlook is on, fold the user's head ORIENTATION
            // (not position) into the view direction. LeiaSR ho[] mapping
            // (matches leia-track-app-XYZ's track_pipeline.h):
            //   ho[0] = pitch (rad), ho[1] = yaw (rad), ho[2] = roll (ignored)
            // Two-stage filter: OneEuro low-pass first, then Accela's
            // gain curve (deadzone=0) on top for soft sub-degree damping
            // without the "still still SNAP" jumps a hard deadzone gives.
            // Roll is intentionally never applied -- it makes 360 viewing
            // nauseating.
            float vrYawEffective   = app.vrYaw;
            float vrPitchEffective = app.vrPitch;
            if (IsVRFormat(app.format) && app.vrHeadLook)
            {
                double hp[3] = {}, ho[3] = {};
                if (app.weaver.GetHeadPose(hp, ho))
                {
                    // Stage 1: OneEuro (no timestamp -> uses fixed 60Hz
                    // constructor freq, more stable than GetTickCount's
                    // 15ms granularity at 165Hz render rate).
                    const float yaw1   = app.vrYawOneEuro  .filter((float)ho[1]);
                    const float pitch1 = app.vrPitchOneEuro.filter((float)ho[0]);
                    // Stage 2: Accela gain curve, threshold 0.025 rad
                    // (~1.4°). Tiny inputs sit in the curve's near-zero
                    // first segment (soft damping); real head turns
                    // exceed normalised 1.0 and hit the steep snap zone.
                    const double yawF   = AccelaApply(app.vrYawAccela,   yaw1,   0.025);
                    const double pitchF = AccelaApply(app.vrPitchAccela, pitch1, 0.025);
                    app.vrFilterInit = true;
                    // Negate so "head right -> view right" (raw yaw/pitch
                    // are opposite the viewer's expected direction; matches
                    // leia-track-app's invert_yaw default).
                    vrYawEffective   -= (float)yawF;
                    vrPitchEffective -= (float)pitchF;
                }
            }
            else if (app.vrFilterInit)
            {
                app.vrYawOneEuro  .reset();
                app.vrPitchOneEuro.reset();
                app.vrYawAccela  .init = false;
                app.vrPitchAccela.init = false;
                app.vrFilterInit = false;
            }
            app.converter.SetVRView(vrYawEffective, vrPitchEffective, app.vrZoom);
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
        }   // matches the "{" introduced before the liveSource block by the LFPRenderer branch

        skipSourcePipeline:
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
            else if (app && LOWORD(lParam) == NIN_BALLOONUSERCLICK
                          && !app->pendingUpdateUrl.empty())
            {
                // User clicked the "update available" toast -> open the release page.
                ShellExecuteA(nullptr, "open", app->pendingUpdateUrl.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;

        case WM_APP_UPDATE_RESULT:
            if (app && wParam)
            {
                auto* info = reinterpret_cast<ReleaseInfo*>(wParam);
                char title[128], body[256];
                DWORD infoFlags = NIIF_INFO;
                switch (info->status)
                {
                case ReleaseInfo::Available:
                    app->pendingUpdateUrl = info->url;
                    app->pendingUpdateTag = info->tag;
                    _snprintf_s(title, _TRUNCATE, "SR Loom update available");
                    _snprintf_s(body, _TRUNCATE,
                                "Version %s is out (you have %s). Click to open the release page.",
                                info->tag.c_str(), kAppVersion);
                    Log("UpdateChecker: notified -- latest %s, current %s",
                        info->tag.c_str(), kAppVersion);
                    break;
                case ReleaseInfo::UpToDate:
                    app->pendingUpdateUrl.clear();
                    app->pendingUpdateTag.clear();
                    _snprintf_s(title, _TRUNCATE, "SR Loom is up to date");
                    _snprintf_s(body, _TRUNCATE,
                                "You're on the latest release (%s).", kAppVersion);
                    Log("UpdateChecker: user-checked, already up to date");
                    break;
                case ReleaseInfo::Failed:
                default:
                    app->pendingUpdateUrl.clear();
                    app->pendingUpdateTag.clear();
                    _snprintf_s(title, _TRUNCATE, "Update check failed");
                    _snprintf_s(body, _TRUNCATE,
                                "Couldn't reach the update server. Check your network and try again.");
                    infoFlags = NIIF_WARNING;
                    Log("UpdateChecker: user-checked, fetch failed");
                    break;
                }
                NOTIFYICONDATAA nid{ sizeof(nid) };
                nid.hWnd   = hwnd;
                nid.uID    = 1;
                nid.uFlags = NIF_INFO;
                nid.dwInfoFlags = infoFlags;
                _snprintf_s(nid.szInfoTitle, _TRUNCATE, "%s", title);
                _snprintf_s(nid.szInfo,      _TRUNCATE, "%s", body);
                Shell_NotifyIconA(NIM_MODIFY, &nid);
                delete info;
            }
            return 0;

        case WM_APP_CHECK_UPDATES:
            // User-forced update check from the About popup. Skips the
            // 6-hour throttle and always posts a WM_APP_UPDATE_RESULT so
            // the user sees a balloon either way.
            UpdateChecker::StartAsync(hwnd, WM_APP_UPDATE_RESULT, true);
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
            // VR viewer: record the press but don't commit to "drag" yet --
            // any movement past the threshold turns it into a drag-to-look;
            // a release within the threshold falls through to the overlay
            // toggle so the user can still pop the test-image controls.
            if (app && app->source == SourceKind::TestImage && IsVRFormat(app->format))
            {
                app->vrMouseDown  = true;
                app->vrMoved      = false;
                app->vrDragStartX = app->vrDragLastX = GET_X_LPARAM(lParam);
                app->vrDragStartY = app->vrDragLastY = GET_Y_LPARAM(lParam);
                SetCapture(hwnd);
                return 0;
            }
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

        case WM_MOUSEMOVE:
            if (app && app->vrMouseDown && (wParam & MK_LBUTTON))
            {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                if (!app->vrMoved)
                {
                    const int dx = x - app->vrDragStartX;
                    const int dy = y - app->vrDragStartY;
                    // 5-pixel drag threshold separates an accidental jitter
                    // (= still a click) from an intentional drag-to-look.
                    if (dx * dx + dy * dy > 25) app->vrMoved = true;
                }
                if (app->vrMoved)
                {
                    // px-to-radians: 1800 px ~= 180° -- comfortable viewer
                    // feel, scales down further at higher zoom.
                    const float kPxToRad = 3.14159265f / 1800.0f;
                    const float zoomDiv = (app->vrZoom > 0.1f) ? app->vrZoom : 0.1f;
                    app->vrYaw   -= (x - app->vrDragLastX) * kPxToRad / zoomDiv;
                    app->vrPitch -= (y - app->vrDragLastY) * kPxToRad / zoomDiv;
                    if (app->vrPitch >  1.5f) app->vrPitch =  1.5f;
                    if (app->vrPitch < -1.5f) app->vrPitch = -1.5f;
                    app->vrDragLastX = x;
                    app->vrDragLastY = y;
                    app->captureRebind = true;
                }
                return 0;
            }
            return 0;

        case WM_LBUTTONUP:
            if (app && app->vrMouseDown)
            {
                const bool wasClick = !app->vrMoved;
                app->vrMouseDown = false;
                app->vrMoved     = false;
                ReleaseCapture();
                if (!wasClick) return 0;   // real drag -- consume
                // Quick click on the SR Loom window: toggle the test-image
                // overlays the same way the non-VR click path does.
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
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (app && app->source == SourceKind::TestImage && IsVRFormat(app->format))
            {
                const float delta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
                app->vrZoom *= powf(1.12f, delta);   // ~12% per notch
                if (app->vrZoom < 0.2f) app->vrZoom = 0.2f;
                if (app->vrZoom > 3.0f) app->vrZoom = 3.0f;
                app->captureRebind = true;
                return 0;
            }
            break;

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
                if (idx < n) { ChangeFormat(*app, fmts[idx].fmt); EnsureWeavingFormatOnly(*app); }
                // Re-show the test-image settings overlay so Quilt's cols/rows
                // buttons appear (or vanish) immediately on a format change.
                if (g_fsSet && IsWindowVisible(g_fsSet)) ShowFsSetOverlay(*app);
                return 0;
            }
            // Anaglyph colour combo / decode mode (also selects the Anaglyph format).
            if (cmd >= ID_TRAY_ANA_COMBO_BASE && cmd <= ID_TRAY_ANA_COMBO_MAX)
            {
                app->anaglyphCombo = (int)(cmd - ID_TRAY_ANA_COMBO_BASE);
                ChangeFormat(*app, StereoFormat::Anaglyph);
                EnsureWeavingFormatOnly(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_ANA_MODE_BASE && cmd <= ID_TRAY_ANA_MODE_MAX)
            {
                int n = 0; const AnaglyphModeEntry* modes = AnaglyphModeList(n);
                const int idx = (int)(cmd - ID_TRAY_ANA_MODE_BASE);
                if (idx < n) app->anaglyphMode = modes[idx].value;   // menu index -> shader mode value
                ChangeFormat(*app, StereoFormat::Anaglyph);
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
                ChangeFormat(*app, StereoFormat::Pulfrich);
                EnsureWeavingFormatOnly(*app);
                return 0;
            }
            if (cmd >= ID_TRAY_FP_BASE && cmd <= ID_TRAY_FP_MAX)
            {
                app->framePackMode = (int)(cmd - ID_TRAY_FP_BASE);
                ChangeFormat(*app, StereoFormat::FramePacking);
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

        case WM_SIZING:
        {
            // Lock the windowed aspect when displaying a test image with a
            // known per-eye aspect (LFP, video, image). Without this the
            // user could resize the window to a random shape and the
            // image would stretch / leave large bars. Adjust the rect on
            // the EDGE the user is dragging so it feels natural.
            if (!app || app->mode != OutputMode::Windowed
                || app->source != SourceKind::TestImage)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            // Compute the same per-eye aspect ApplyMode uses.
            int sw = 0, sh = 0;
            if (app->format == StereoFormat::LightField && app->lfpRenderer.HasData())
            { sw = app->lfpRenderer.OutputPerEyeWidth(); sh = app->lfpRenderer.OutputHeight(); }
            else if (app->video.IsOpen()) { sw = app->video.Width(); sh = app->video.Height(); }
            else                          { sw = app->weaver.SourceWidth(); sh = app->weaver.SourceHeight(); }
            if (sw <= 0 || sh <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);
            double aw = (double)sw, ah = (double)sh;
            switch (app->format)
            {
            case StereoFormat::Quilt:
                if (app->quiltCols > 0 && app->quiltRows > 0)
                { aw /= app->quiltCols; ah /= app->quiltRows; }
                break;
            case StereoFormat::FullSBS:
            case StereoFormat::HalfSBS:           aw *= 0.5; break;
            case StereoFormat::FullTAB:
            case StereoFormat::HalfTAB:
            case StereoFormat::RowInterleaved:    ah *= 0.5; break;
            case StereoFormat::ColumnInterleaved: aw *= 0.5; break;
            default: break;
            }
            if (aw <= 0 || ah <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);
            const double aspect = aw / ah;

            // We're adjusting the WINDOW rect, not the client rect. Subtract
            // the chrome (frame + caption) so the *client* keeps the
            // correct aspect, then re-add.
            RECT* r = reinterpret_cast<RECT*>(lParam);
            RECT chrome{};
            ::AdjustWindowRectEx(&chrome, (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE),
                                 FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
            const int chromeW = chrome.right  - chrome.left;
            const int chromeH = chrome.bottom - chrome.top;
            const int curW = (r->right  - r->left) - chromeW;
            const int curH = (r->bottom - r->top)  - chromeH;
            if (curW <= 0 || curH <= 0)
                return DefWindowProc(hwnd, msg, wParam, lParam);

            // wParam tells us which edge / corner is being dragged. Width-
            // adjusts override (drag left/right edge), height-adjusts on
            // top/bottom, corners follow whichever side has more change.
            const WPARAM edge = wParam;
            const bool widthEdge  = (edge == WMSZ_LEFT || edge == WMSZ_RIGHT);
            const bool heightEdge = (edge == WMSZ_TOP  || edge == WMSZ_BOTTOM);
            int targetW = curW, targetH = curH;
            if (widthEdge)        targetH = (int)(targetW / aspect + 0.5);
            else if (heightEdge)  targetW = (int)(targetH * aspect + 0.5);
            else { /* corner: use the larger relative change */
                const double byW = (double)targetW / aspect;   // implied H from W
                const double byH = (double)targetH * aspect;   // implied W from H
                if (std::abs(byW - targetH) < std::abs(byH - targetW))
                    targetH = (int)(targetW / aspect + 0.5);
                else
                    targetW = (int)(targetH * aspect + 0.5);
            }

            // Apply the new rect, anchoring on the dragged edge.
            if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                r->left  = r->right  - (targetW + chromeW);
            else
                r->right = r->left   + (targetW + chromeW);
            if (edge == WMSZ_TOP  || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                r->top    = r->bottom - (targetH + chromeH);
            else
                r->bottom = r->top    + (targetH + chromeH);
            return TRUE;
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
    Log("WinMain: SR Loom v%s starting", kAppVersion);
    {
        const char* sr = SRWeaver::GetSRPlatformVersion();
        Log("WinMain: SR Platform runtime version=%s", (sr && *sr) ? sr : "(unknown)");
    }

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

    // Diagnostic snapshot: what does the runtime report for the SR display,
    // and is it Windows' primary monitor? Lets us verify whether SR Loom
    // works on a non-primary SR display setup (the runtime + getLocation()
    // pattern should support it on Windows 11; see docs/sr-non-primary-research.md).
    {
        const RECT& d = app.srDisplayRect;
        const POINT p{ (d.left + d.right) / 2, (d.top + d.bottom) / 2 };
        HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
        MONITORINFOEXA mi{}; mi.cbSize = sizeof(mi);
        const bool gotMi = mon && GetMonitorInfoA(mon, &mi);
        const bool isPrimary = gotMi && (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        Log("WinMain: SR display rect=(%ld,%ld %ld,%ld) %ldx%ld",
            d.left, d.top, d.right, d.bottom,
            d.right - d.left, d.bottom - d.top);
        Log("WinMain: SR display monitor=%p name='%s' primary=%d",
            (void*)mon, gotMi ? mi.szDevice : "(unknown)", isPrimary ? 1 : 0);
        if (!isPrimary)
            Log("WinMain: SR display is NOT Windows primary -- testing non-primary support."
                " If weave doesn't engage, see docs/sr-non-primary-research.md.");
    }

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
    // LFPRenderer is optional -- only used when an LFP file is loaded.
    // If shader compile fails, log and continue without the feature.
    if (!app.lfpRenderer.Initialize(app.renderer.Device(), app.renderer.Context()))
        Log("WinMain: lfpRenderer.Initialize FAILED (LFP files will fall back to CPU SBS)");
    else
        Log("WinMain: lfpRenderer.Initialize OK");
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

    // Kick off a once-per-launch (throttled to once per 6h) background poll of
    // GitHub Releases. If a newer tag than kAppVersion exists the worker
    // PostMessages WM_APP_UPDATE_RESULT so the WndProc can pop a balloon
    // (auto-check is silent on up-to-date / failure -- only the user-
    // forced check from the About popup yields balloons for those).
    UpdateChecker::StartAsync(app.hwnd, WM_APP_UPDATE_RESULT, false);
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
                gs.vrHeadLook        = app.vrHeadLook;
                gs.vrZoom            = app.vrZoom;
                gs.vrResetView       = false;
                gs.vrResetZoom       = false;
                gs.vrHeadLookChanged = false;
                // SR Platform runtime version (e.g. "1.34.10.17449") for
                // the About popup. Static helper; pull it fresh each frame
                // so a runtime hot-swap (rare) updates the GUI display.
                const char* sr = SRWeaver::GetSRPlatformVersion();
                strncpy_s(gs.srPlatformVersion, sr ? sr : "", _TRUNCATE);
                // Light-field parallax-scale state for the GUI slider.
                gs.lfpHeadLeanMm      = app.lfpHeadLeanMm;
                gs.lfpApertureMm      = (float)(app.lfpRenderer.ApertureDiameterMetres() * 1e3);
                gs.lfpHeadLeanChanged = false;
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
                // VR controls.
                if (gs.vrHeadLookChanged)
                {
                    app.vrHeadLook = gs.vrHeadLook;
                    app.captureRebind = true;
                }
                if (gs.lfpHeadLeanChanged)
                {
                    app.lfpHeadLeanMm = gs.lfpHeadLeanMm;
                }
                if (gs.vrResetView)
                {
                    app.vrYaw   = 0.0f;
                    app.vrPitch = 0.0f;
                    app.captureRebind = true;
                }
                if (gs.vrResetZoom)
                {
                    app.vrZoom = 1.0f;
                    app.captureRebind = true;
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
    app.katanga.End();
    app.lfpRenderer.Shutdown();
    app.converter.Shutdown();
    app.capture.Shutdown();
    app.weaver.Shutdown();
    app.renderer.Shutdown();
    VideoSource::Shutdown();
    g_app = nullptr;
    return 0;
}
