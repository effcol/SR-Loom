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

void TrayIcon::ShowContextMenu(HWND hwnd, bool weavingEnabled, OutputMode mode)
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
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, ID_TRAY_EXIT, "Exit");

    POINT pt{};
    GetCursorPos(&pt);

    // Required so the menu dismisses correctly when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);
}
