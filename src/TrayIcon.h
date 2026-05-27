// TrayIcon.h — system-tray (notification area) icon and its right-click menu.
#pragma once

#include "Common.h"
#include <vector>

namespace srw
{
    // Window message the tray icon posts to the main window for mouse events.
    constexpr UINT WM_APP_TRAY = WM_APP + 1;

    // GUI -> main window: capture the window whose HWND is in lParam (window picker).
    constexpr UINT WM_APP_GUI_CAPTURE_WINDOW = WM_APP + 2;

    // GUI -> main window: weave the display (monitor) whose HMONITOR is in lParam.
    constexpr UINT WM_APP_GUI_CAPTURE_DISPLAY = WM_APP + 3;

    // Menu command IDs (also reused as WM_COMMAND ids from the popup menu).
    enum TrayCommand : UINT
    {
        ID_TRAY_TOGGLE_WEAVE = 40001,
        ID_TRAY_MODE_FULLSCREEN,
        ID_TRAY_MODE_WINDOWED,
        ID_TRAY_MODE_OVERLAY,
        ID_TRAY_LOOKING_GLASS,
        ID_TRAY_CAPTURE_FOREGROUND,
        ID_TRAY_SRC_TESTIMAGE,
        ID_TRAY_SRC_TESTIMAGE_ANA,
        ID_TRAY_SRC_MONITOR,
        ID_TRAY_SWAP_EYES,
        ID_TRAY_DETECT,
        ID_TRAY_EXIT
    };

    // Window-list items get ids in this range; index = id - base.
    constexpr UINT ID_TRAY_SRC_WINDOW_BASE = 41000;
    constexpr UINT ID_TRAY_SRC_WINDOW_MAX  = 41999;

    // 3D-format items get ids in this range; index = id - base.
    constexpr UINT ID_TRAY_FMT_BASE = 42000;
    constexpr UINT ID_TRAY_FMT_MAX  = 42099;

    // Anaglyph colour-combo and decode-mode items.
    constexpr UINT ID_TRAY_ANA_COMBO_BASE = 43000;
    constexpr UINT ID_TRAY_ANA_COMBO_MAX  = 43049;
    constexpr UINT ID_TRAY_ANA_MODE_BASE  = 43050;
    constexpr UINT ID_TRAY_ANA_MODE_MAX   = 43099;

    // Pulfrich sub-options.
    constexpr UINT ID_TRAY_PULF_MODE_BASE  = 43100;  // +0 time delay, +1 ND filter
    constexpr UINT ID_TRAY_PULF_EYE_BASE   = 43110;  // +0 left, +1 right
    constexpr UINT ID_TRAY_PULF_DELAY_BASE = 43120;  // +0..3 -> 1..4 frames
    constexpr UINT ID_TRAY_PULF_ND_BASE    = 43130;  // +0..N ND level
    constexpr UINT ID_TRAY_PULF_MAX        = 43199;

    // Frame-packing presets (1080p / 720p).
    constexpr UINT ID_TRAY_FP_BASE = 43200;
    constexpr UINT ID_TRAY_FP_MAX  = 43219;

    // State the context menu reflects (checkmarks).
    struct MenuState
    {
        bool         weaving;
        OutputMode   mode;
        SourceKind   source;
        StereoFormat format;
        bool         swapEyes;
        int          anaglyphCombo;
        int          anaglyphMode;
        PulfrichMode pulfrichMode;
        int          pulfrichDelay;   // 1..4
        int          pulfrichNd;      // index into PulfrichNdLevels
        int          framePackMode;   // index into FramePackPresets
    };

    class TrayIcon
    {
    public:
        ~TrayIcon();

        // Register the icon; mouse events arrive at hwnd as WM_APP_TRAY.
        bool Add(HWND hwnd, const char* tooltip);
        void Remove();

        // Update the tooltip text (e.g. to reflect current state).
        void SetTooltip(const char* tooltip);

        // Build and show the right-click context menu, with current state
        // reflected as checkmarks/radio marks. Commands post as WM_COMMAND.
        // Enumerates top-level windows into the Source submenu.
        void ShowContextMenu(HWND hwnd, const MenuState& s);

        // Resolve a window-list menu index (id - ID_TRAY_SRC_WINDOW_BASE) to its
        // HWND, captured when the menu was last shown. Null if out of range.
        HWND WindowAt(size_t index) const;

    private:
        HWND              m_hwnd  = nullptr;
        bool              m_added = false;
        std::vector<HWND> m_windowList;   // mirrors the Source submenu order
    };
}
