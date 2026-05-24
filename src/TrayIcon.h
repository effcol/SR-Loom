// TrayIcon.h — system-tray (notification area) icon and its right-click menu.
#pragma once

#include "Common.h"

namespace srw
{
    // Window message the tray icon posts to the main window for mouse events.
    constexpr UINT WM_APP_TRAY = WM_APP + 1;

    // Menu command IDs (also reused as WM_COMMAND ids from the popup menu).
    enum TrayCommand : UINT
    {
        ID_TRAY_TOGGLE_WEAVE = 40001,
        ID_TRAY_MODE_FULLSCREEN,
        ID_TRAY_MODE_WINDOWED,
        ID_TRAY_EXIT
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
        void ShowContextMenu(HWND hwnd, bool weavingEnabled, OutputMode mode);

    private:
        HWND m_hwnd  = nullptr;
        bool m_added = false;
    };
}
