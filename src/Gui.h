// Gui.h — Dear ImGui control window for SR Weaver. A normal top-level window
// (so it can live on any monitor and gets its own taskbar button) that renders
// the controls with ImGui on the shared D3D11 device. Most controls just post
// the existing tray WM_COMMAND ids to the main window (reusing those handlers);
// the convergence slider and cursor mode are owned here and read back by main.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>

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
        int          quiltCols      = 8;       // current quilt grid columns (1..12)
        int          quiltRows      = 6;       // current quilt grid rows    (1..9)
        bool         hasTestImage   = false;   // true once a TestImage has been loaded
        // VR viewer (read+write): GUI shows Headlook toggle + Reset View /
        // Reset Zoom buttons. Headlook toggles in place; reset buttons just
        // set the respective flag so the main loop zeros the right field.
        bool         vrHeadLook     = true;
        float        vrZoom         = 1.0f;
        bool         vrResetView    = false;
        bool         vrResetZoom    = false;
        bool         vrHeadLookChanged = false;
        // SR Platform runtime version string (e.g. "1.34.10.17449"), shown in
        // the About popup. Filled once at startup by main from
        // SRWeaver::GetSRPlatformVersion(); empty if unavailable.
        char         srPlatformVersion[80] = "";
        // Light-field parallax-scale slider (mm of head-lean per full
        // aperture sweep). The GUI shows + edits this when format is
        // LightField. Main writes the initial value; GUI may change it
        // and main reads it back each frame.
        float        lfpHeadLeanMm   = 30.0f;
        float        lfpApertureMm   = 0.0f;   // physical aperture diameter (for slider min)
        bool         lfpHeadLeanChanged = false;
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
        bool                     m_acerNeedsAdmin = false;  // last Acer SpatialLabs write was ACCESS_DENIED
        bool                     m_acerSectionOpen = false; // ACER SPATIALLABS section expanded?
        bool                     m_startupSectionOpen = false; // STARTUP section expanded?
        // STARTUP-section state, cached from HKCU at Init() so toggling doesn't
        // read the registry per frame.
        bool                     m_runAtStartup = false;
        bool                     m_startInTray  = true;
        // Inline state for the About popup's "Check for updates" link.
        // Idle by default; switches to Checking on click, then settles to
        // UpToDate / Available / Failed when WM_APP_UPDATE_RESULT lands.
        enum class UpdateCheck { Idle, Checking, UpToDate, Available, Failed };
        UpdateCheck              m_updateCheck    = UpdateCheck::Idle;
        std::string              m_updateTag;        // populated for Available / UpToDate
        std::string              m_updateUrl;        // populated for Available
        GuiState                 m_lastState;   // last rendered state (to repaint during a window drag)
        float                    m_sectionsH = 0.0f;   // measured height of the scrolling options
    };
}
