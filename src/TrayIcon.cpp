#include "TrayIcon.h"
#include "resource.h"
#include <shellapi.h>

using namespace srw;

static constexpr UINT kIconId = 1;

// Load the embedded Dimenco logo icon at the system's small-icon size,
// falling back to the default application icon if it is unavailable.
static HICON LoadTrayIcon()
{
    HICON icon = (HICON)LoadImageA(
        GetModuleHandle(nullptr), MAKEINTRESOURCEA(IDI_TRAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR);
    return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

TrayIcon::~TrayIcon()
{
    Remove();
}

bool TrayIcon::Add(HWND hwnd, const char* tooltip)
{
    m_hwnd = hwnd;

    NOTIFYICONDATAA nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = hwnd;
    nid.uID              = kIconId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon            = LoadTrayIcon();
    lstrcpynA(nid.szTip, tooltip ? tooltip : "SR Weaver", (int)ARRAYSIZE(nid.szTip));

    m_added = Shell_NotifyIconA(NIM_ADD, &nid) != FALSE;
    return m_added;
}

void TrayIcon::Remove()
{
    if (!m_added) return;

    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = kIconId;
    Shell_NotifyIconA(NIM_DELETE, &nid);
    m_added = false;
}

void TrayIcon::SetTooltip(const char* tooltip)
{
    if (!m_added) return;

    NOTIFYICONDATAA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = kIconId;
    nid.uFlags = NIF_TIP;
    lstrcpynA(nid.szTip, tooltip ? tooltip : "SR Weaver", (int)ARRAYSIZE(nid.szTip));
    Shell_NotifyIconA(NIM_MODIFY, &nid);
}

namespace
{
    struct EnumCtx { std::vector<HWND>* list; HWND owner; };

    BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
    {
        auto* ctx = reinterpret_cast<EnumCtx*>(lp);

        if (hwnd == ctx->owner)                    return TRUE;  // skip our own window
        if (!IsWindowVisible(hwnd))                return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr)  return TRUE;  // skip owned popups
        if (GetWindowTextLengthA(hwnd) == 0)       return TRUE;

        const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW)                 return TRUE;  // skip tool windows

        ctx->list->push_back(hwnd);
        return TRUE;
    }
}

HWND TrayIcon::WindowAt(size_t index) const
{
    return (index < m_windowList.size()) ? m_windowList[index] : nullptr;
}

void TrayIcon::ShowContextMenu(HWND hwnd, bool weavingEnabled, OutputMode mode,
                               SourceKind source, StereoFormat format, bool swapEyes)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuA(menu, MF_STRING | (weavingEnabled ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_TOGGLE_WEAVE, "Weaving enabled");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING | (mode == OutputMode::Fullscreen ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_MODE_FULLSCREEN, "Fullscreen");
    AppendMenuA(menu, MF_STRING | (mode == OutputMode::Windowed ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_MODE_WINDOWED, "Windowed");
    AppendMenuA(menu, MF_STRING | (mode == OutputMode::WindowOverlay ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_MODE_OVERLAY, "Overlay source window (in-place 3D)");
    AppendMenuA(menu, MF_STRING | (mode == OutputMode::LookingGlass ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_LOOKING_GLASS, "Looking glass (passthrough; drag title bar / edges to move/resize)");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_CAPTURE_FOREGROUND, "Make active window 3D\tCtrl+Alt+C");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);

    // Source submenu: test image, monitor, then live window list.
    HMENU srcMenu = CreatePopupMenu();
    AppendMenuA(srcMenu, MF_STRING | (source == SourceKind::TestImage ? MF_CHECKED : 0),
                ID_TRAY_SRC_TESTIMAGE, "Test image");
    AppendMenuA(srcMenu, MF_STRING | (source == SourceKind::CaptureMonitor ? MF_CHECKED : 0),
                ID_TRAY_SRC_MONITOR, "Simulated Reality Monitor (passthrough)");
    AppendMenuA(srcMenu, MF_SEPARATOR, 0, nullptr);

    m_windowList.clear();
    EnumCtx ctx{ &m_windowList, hwnd };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    for (size_t i = 0; i < m_windowList.size() && i <= (ID_TRAY_SRC_WINDOW_MAX - ID_TRAY_SRC_WINDOW_BASE); ++i)
    {
        char title[128];
        GetWindowTextA(m_windowList[i], title, (int)ARRAYSIZE(title));
        AppendMenuA(srcMenu, MF_STRING, ID_TRAY_SRC_WINDOW_BASE + (UINT)i, title);
    }

    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(srcMenu), "Source");

    // 3D format submenu (source stereo layout) + eye swap.
    HMENU fmtMenu = CreatePopupMenu();
    int fmtCount = 0;
    const StereoFormatEntry* fmts = StereoFormatList(fmtCount);
    for (int i = 0; i < fmtCount; ++i)
        AppendMenuA(fmtMenu, MF_STRING | (fmts[i].fmt == format ? MF_CHECKED : 0),
                    ID_TRAY_FMT_BASE + (UINT)i, fmts[i].label);
    AppendMenuA(fmtMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(fmtMenu, MF_STRING | (swapEyes ? MF_CHECKED : 0), ID_TRAY_SWAP_EYES, "Swap eyes");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmtMenu), "3D format");

    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");

    POINT pt{};
    GetCursorPos(&pt);

    // Required so the menu dismisses correctly when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);  // also destroys the submenu
}
