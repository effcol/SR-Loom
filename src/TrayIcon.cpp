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

void TrayIcon::ShowContextMenu(HWND hwnd, const MenuState& s)
{
    const bool         weavingEnabled = s.weaving;
    const OutputMode   mode           = s.mode;
    const SourceKind   source         = s.source;
    const StereoFormat format         = s.format;
    const bool         swapEyes       = s.swapEyes;
    const int          anaglyphCombo  = s.anaglyphCombo;
    const int          anaglyphMode   = s.anaglyphMode;

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
    auto addFmt = [&](HMENU m, StereoFormat f, const char* label)
    {
        int idx = StereoFormatIndex(f);
        if (idx < 0) return;
        AppendMenuA(m, MF_STRING | (format == f ? MF_CHECKED : 0),
                    ID_TRAY_FMT_BASE + (UINT)idx, label);
    };
    auto groupChecked = [&](StereoFormat a, StereoFormat b)
    {
        return (format == a || format == b) ? (UINT)MF_CHECKED : 0u;
    };

    HMENU sbsMenu = CreatePopupMenu();
    addFmt(sbsMenu, StereoFormat::FullSBS, "Full");
    addFmt(sbsMenu, StereoFormat::HalfSBS, "Half");
    AppendMenuA(fmtMenu, MF_POPUP | groupChecked(StereoFormat::FullSBS, StereoFormat::HalfSBS),
                reinterpret_cast<UINT_PTR>(sbsMenu), "Side-by-Side");

    HMENU tabMenu = CreatePopupMenu();
    addFmt(tabMenu, StereoFormat::FullTAB, "Full");
    addFmt(tabMenu, StereoFormat::HalfTAB, "Half");
    AppendMenuA(fmtMenu, MF_POPUP | groupChecked(StereoFormat::FullTAB, StereoFormat::HalfTAB),
                reinterpret_cast<UINT_PTR>(tabMenu), "Top-and-Bottom");

    HMENU ilMenu = CreatePopupMenu();
    addFmt(ilMenu, StereoFormat::RowInterleaved,    "Row interleaved");
    addFmt(ilMenu, StereoFormat::ColumnInterleaved, "Column interleaved");
    addFmt(ilMenu, StereoFormat::Checkerboard,      "Checkerboard");
    const UINT ilChecked = (format == StereoFormat::RowInterleaved ||
                            format == StereoFormat::ColumnInterleaved ||
                            format == StereoFormat::Checkerboard) ? (UINT)MF_CHECKED : 0u;
    AppendMenuA(fmtMenu, MF_POPUP | ilChecked,
                reinterpret_cast<UINT_PTR>(ilMenu), "Interleaved / Checkerboard");

    // Anaglyph submenu: colour combinations + decode mode.
    HMENU anaMenu = CreatePopupMenu();
    int comboCount = 0, modeCount = 0;
    const char* const* combos = AnaglyphComboList(comboCount);
    const char* const* modes  = AnaglyphModeList(modeCount);
    const bool anaActive = (format == StereoFormat::Anaglyph);
    for (int i = 0; i < comboCount; ++i)
        AppendMenuA(anaMenu, MF_STRING | ((anaActive && i == anaglyphCombo) ? MF_CHECKED : 0),
                    ID_TRAY_ANA_COMBO_BASE + (UINT)i, combos[i]);
    AppendMenuA(anaMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < modeCount; ++i)
        AppendMenuA(anaMenu, MF_STRING | ((anaActive && i == anaglyphMode) ? MF_CHECKED : 0),
                    ID_TRAY_ANA_MODE_BASE + (UINT)i, modes[i]);
    AppendMenuA(fmtMenu, MF_POPUP | (anaActive ? MF_CHECKED : 0),
                reinterpret_cast<UINT_PTR>(anaMenu), "Anaglyph");

    // Pulfrich submenu: mode, affected eye, delay (time-delay), ND level.
    HMENU pulfMenu = CreatePopupMenu();
    const bool pulfActive = (format == StereoFormat::Pulfrich);
    AppendMenuA(pulfMenu, MF_STRING | ((pulfActive && s.pulfrichMode == PulfrichMode::TimeDelay) ? MF_CHECKED : 0),
                ID_TRAY_PULF_MODE_BASE + 0, "Mode: Time delay");
    AppendMenuA(pulfMenu, MF_STRING | ((pulfActive && s.pulfrichMode == PulfrichMode::NDFilter) ? MF_CHECKED : 0),
                ID_TRAY_PULF_MODE_BASE + 1, "Mode: ND filter");
    AppendMenuA(pulfMenu, MF_SEPARATOR, 0, nullptr);
    for (int d = 1; d <= 4; ++d)
    {
        char buf[32]; wsprintfA(buf, "Delay: %d frame%s", d, d == 1 ? "" : "s");
        AppendMenuA(pulfMenu, MF_STRING | ((pulfActive && s.pulfrichDelay == d) ? MF_CHECKED : 0),
                    ID_TRAY_PULF_DELAY_BASE + (UINT)(d - 1), buf);
    }
    AppendMenuA(pulfMenu, MF_SEPARATOR, 0, nullptr);
    int ndCount = 0;
    const NdLevel* nd = PulfrichNdLevels(ndCount);
    for (int i = 0; i < ndCount; ++i)
    {
        char buf[48]; wsprintfA(buf, "ND: %s", nd[i].label);
        AppendMenuA(pulfMenu, MF_STRING | ((pulfActive && s.pulfrichNd == i) ? MF_CHECKED : 0),
                    ID_TRAY_PULF_ND_BASE + (UINT)i, buf);
    }
    AppendMenuA(fmtMenu, MF_POPUP | (pulfActive ? MF_CHECKED : 0),
                reinterpret_cast<UINT_PTR>(pulfMenu), "Pulfrich Effect");

    // Frame Packing submenu: HDMI 1.4 720p / 1080p.
    HMENU fpMenu = CreatePopupMenu();
    const bool fpActive = (format == StereoFormat::FramePacking);
    int fpCount = 0;
    const FramePackPreset* fps = FramePackPresets(fpCount);
    for (int i = 0; i < fpCount; ++i)
        AppendMenuA(fpMenu, MF_STRING | ((fpActive && i == s.framePackMode) ? MF_CHECKED : 0),
                    ID_TRAY_FP_BASE + (UINT)i, fps[i].label);
    AppendMenuA(fmtMenu, MF_POPUP | (fpActive ? MF_CHECKED : 0),
                reinterpret_cast<UINT_PTR>(fpMenu), "Frame Packing (HDMI 1.4)");

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
