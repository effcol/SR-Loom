// Gui.h — Dear ImGui control window for SR Weaver. A normal top-level window
// (so it can live on any monitor and gets its own taskbar button) that renders
// the controls with ImGui on the shared D3D11 device. Most controls just post
// the existing tray WM_COMMAND ids to the main window (reusing those handlers);
// the convergence slider and cursor mode are owned here and read back by main.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>

struct ImFont;   // Dear ImGui (defined in imgui.h); only used by pointer here

namespace srw
{
    // Snapshot the GUI shows, plus the two GUI-owned controls it edits in place.
    struct GuiState
    {
        bool         weaving       = false;
        OutputMode   mode          = OutputMode::Fullscreen;
        SourceKind   source        = SourceKind::CaptureMonitor;
        StereoFormat format        = StereoFormat::FullSBS;
        bool         swapEyes      = false;
        int          anaglyphCombo = 0;
        int          anaglyphMode  = 4;
        float        convergence   = 0.0f;   // GUI-owned: zero-plane horizontal shift (-1..1)
        int          pulfrichMode  = 0;      // 0 = time delay, 1 = ND filter
        int          pulfrichDelay = 1;      // 1..4 frames
        int          pulfrichNd    = 1;      // index into PulfrichNdLevels
        int          framePackMode = 0;      // index into FramePackPresets
        char         sourceName[160] = "";   // what's being weaved (window title / "Monitor")
        HMONITOR     srMonitor      = nullptr; // the SR display's monitor ("This Display")
        HMONITOR     captureMonitor = nullptr; // the monitor currently being captured
        bool         foreignDisplay = false;   // a picked (non-SR) display is the active source
    };

    class Gui
    {
    public:
        bool Init(HWND mainHwnd, ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        void Toggle();                       // left-click tray: show/hide
        bool IsVisible() const { return m_visible; }
        HWND Hwnd()      const { return m_hwnd; }

        // Build + render the panel for this frame. `state` is in/out: caller fills
        // the current values, the GUI may change convergence (and posts WM_COMMANDs
        // for the rest). Returns true if convergence changed this frame.
        bool Render(GuiState& state);

    private:
        bool EnsureSwapChain(UINT w, UINT h);
        void ReleaseSwapChain();
        void ApplyScaling();          // (re)build style + fonts for m_lightMode at m_dpiScale
        void FitHeightToContent(int clientContentH);   // shrink/grow window to fit content
        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

        HWND                     m_mainHwnd = nullptr;
        HWND                     m_hwnd     = nullptr;
        ID3D11Device*            m_device   = nullptr;
        ID3D11DeviceContext*     m_context  = nullptr;
        IDXGISwapChain1*         m_swap     = nullptr;
        ID3D11RenderTargetView*  m_rtv      = nullptr;
        UINT                     m_w = 0, m_h = 0;
        bool                     m_visible  = false;
        bool                     m_imguiReady = false;
        bool                     m_lightMode = false;   // false = warm dark, true = sepia/cream
        bool                     m_expanded  = false;   // options collapsed (just the on/off toggle) by default
        float                    m_dpiScale = 1.0f;     // current monitor's DPI scale (1.0 = 96dpi)
        bool                     m_pendingRescale = false;  // apply theme/DPI rebuild before next frame
        ImFont*                  m_fontRegular = nullptr;
        ImFont*                  m_fontLarge   = nullptr;
        ImFont*                  m_fontIcons   = nullptr;   // Segoe Fluent/MDL2 caption glyphs
        bool                     m_draggingBg = false;      // dragging the window by empty space
        POINT                    m_dragCur0{}, m_dragWin0{}; // cursor + window origin at drag start
        GuiState                 m_lastState;   // last rendered state (to repaint during a window drag)
        float                    m_sectionsH = 0.0f;   // measured height of the scrolling options
    };
}
