#include "Gui.h"
#include "TrayIcon.h"   // ID_TRAY_* command ids (reused by the GUI)
#include "AcerSpatialLabs.h"
#include "UpdateChecker.h"   // user-forced check from the About popup
#include "Settings.h"   // run-at-startup / start-in-tray persistence
#include "resource.h"   // IDI_TRAY (app icon)

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <dwmapi.h>
#include <shellapi.h>    // ShellExecuteA (open the GitHub link)
#include <string>
#include <vector>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

using namespace srw;

namespace
{
    // Look up an embedded RCDATA resource (an Inter font) → pointer + size. The memory
    // is owned by the module and valid for the process lifetime, so ImGui must NOT free
    // it (callers pass ImFontConfig::FontDataOwnedByAtlas = false).
    const void* EmbeddedResource(int id, size_t& size)
    {
        size = 0;
        HMODULE mod = GetModuleHandleA(nullptr);
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
        if (!res) return nullptr;
        HGLOBAL h = LoadResource(mod, res);
        if (!h) return nullptr;
        size = SizeofResource(mod, res);
        return LockResource(h);
    }
}

// ImGui's Win32 message handler (defined in imgui_impl_win32.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    constexpr char kGuiClass[] = "SRWeaverGuiWindow";
    constexpr char kGithubUrl[] = "https://github.com/effcol/SR-Loom";
    Gui* g_gui = nullptr;   // single instance, for the static WndProc

    // DWMWINDOWATTRIBUTE values (Win11 22000+) — used by number to avoid depending
    // on the SDK enum being present. Recolour the native title bar to match the app.
    constexpr DWORD kDwmImmersiveDark = 20, kDwmCornerPref = 33, kDwmBorderColor = 34,
                    kDwmCaptionColor  = 35, kDwmTextColor   = 36;

    // Palette colours the layout helpers reach for (set by ApplyTheme).
    ImVec4 g_accent, g_accentText, g_surface, g_dim, g_trackOff, g_knob, g_text;

    inline ImVec4 Col(int r, int g, int b, float a = 1.0f)
    {
        return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
    }

    // Two warm "Reeder-like" palettes (no blue, flat surfaces, restrained accent),
    // chosen by `light`: false = Warm Dark, true = Sepia / Cream.
    void ApplyTheme(bool light)
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding       = 12.0f;
        s.ChildRounding        = 10.0f;
        s.FrameRounding        = 7.0f;
        s.GrabRounding         = 7.0f;
        s.PopupRounding        = 8.0f;
        s.WindowPadding        = ImVec2(12, 9);
        s.FramePadding         = ImVec2(12, 8);
        s.ItemSpacing          = ImVec2(8, 7);
        s.ItemInnerSpacing     = ImVec2(8, 6);
        s.ScrollbarRounding    = 9.0f;
        s.ScrollbarSize        = 11.0f;
        s.GrabMinSize          = 18.0f;
        s.FrameBorderSize      = 0.0f;
        s.WindowBorderSize     = 0.0f;
        s.PopupBorderSize      = 0.0f;

        ImVec4* c = s.Colors;
        ImVec4 bg, surf, surfH, surfA, text, dim, accent, accentH, sep;
        if (!light)   // Warm Dark
        {
            bg    = Col(28, 27, 25);  surf  = Col(40, 38, 34);
            surfH = Col(50, 47, 42);  surfA = Col(60, 56, 50);
            text  = Col(232, 228, 220); dim = Col(138, 132, 122);
            accent = Col(210, 105, 74); accentH = Col(224, 124, 94);
            sep    = Col(54, 50, 45);
            g_accentText = Col(250, 247, 243);
            g_trackOff   = Col(66, 62, 56);
            g_knob       = Col(238, 234, 226);
        }
        else          // Sepia / Cream
        {
            bg    = Col(242, 235, 221); surf  = Col(231, 221, 200);
            surfH = Col(222, 210, 186); surfA = Col(210, 196, 168);
            text  = Col(74, 68, 58);    dim   = Col(146, 134, 114);
            accent = Col(178, 94, 59);  accentH = Col(160, 82, 50);
            sep    = Col(214, 202, 180);
            g_accentText = Col(248, 243, 233);
            g_trackOff   = Col(202, 190, 170);
            g_knob       = Col(255, 255, 255);
        }
        g_accent = accent; g_surface = surf; g_dim = dim; g_text = text;

        c[ImGuiCol_WindowBg]         = bg;
        c[ImGuiCol_ChildBg]          = light ? Col(236, 229, 214) : Col(33, 31, 28);
        c[ImGuiCol_PopupBg]          = light ? Col(238, 231, 217) : Col(40, 38, 34);
        c[ImGuiCol_Text]             = text;
        c[ImGuiCol_TextDisabled]     = dim;
        c[ImGuiCol_Button]           = surf;
        c[ImGuiCol_ButtonHovered]    = surfH;
        c[ImGuiCol_ButtonActive]     = surfA;
        c[ImGuiCol_FrameBg]          = surf;
        c[ImGuiCol_FrameBgHovered]   = surfH;
        c[ImGuiCol_FrameBgActive]    = surfA;
        c[ImGuiCol_Header]           = surfH;
        c[ImGuiCol_HeaderHovered]    = surfH;
        c[ImGuiCol_HeaderActive]     = surfA;
        c[ImGuiCol_SliderGrab]       = accent;
        c[ImGuiCol_SliderGrabActive] = accentH;
        c[ImGuiCol_CheckMark]        = accent;
        c[ImGuiCol_Separator]        = sep;
        c[ImGuiCol_SeparatorHovered] = accent;
        c[ImGuiCol_Border]           = Col(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarBg]      = Col(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]    = surfH;
        c[ImGuiCol_ScrollbarGrabHovered] = surfA;
        c[ImGuiCol_ScrollbarGrabActive]  = accent;
    }

    // A dim, small-caps section label with breathing room (no heavy separator line).
    void Section(const char* label)
    {
        ImGui::Dummy(ImVec2(0, 5));
        ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 1));
    }

    // A segmented control: a tight row of equal-width cells, the active one filled
    // with the accent. Returns the clicked index, or -1.
    int Segmented(const char* id, const char* const* labels, int count, int current)
    {
        int clicked = -1;
        const float gap = 3.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float w = (avail - gap * (count - 1)) / count;
        ImGui::PushID(id);
        for (int i = 0; i < count; ++i)
        {
            if (i) ImGui::SameLine(0, gap);
            const bool active = (i == current);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button,        g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  g_accent);
                ImGui::PushStyleColor(ImGuiCol_Text,          g_accentText);
            }
            if (ImGui::Button(labels[i], ImVec2(w, 0))) clicked = i;
            if (active) ImGui::PopStyleColor(4);
        }
        ImGui::PopID();
        return clicked;
    }

    // An iOS-style sliding toggle. `heightScale` makes the hero (weaving) one big.
    // Returns true when clicked. Width is 1.8x its height.
    bool ToggleSwitch(const char* id, bool on, float heightScale = 1.0f)
    {
        const float h = ImGui::GetFrameHeight() * heightScale;
        const float w = h * 1.8f;
        const float r = h * 0.5f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool clicked = ImGui::IsItemClicked();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(on ? g_accent : g_trackOff), r);
        const float kx = on ? (p.x + w - r) : (p.x + r);
        dl->AddCircleFilled(ImVec2(kx, p.y + r), r - h * 0.13f, ImGui::GetColorU32(g_knob));
        return clicked;
    }

    // A "label …… (switch)" row, the switch right-aligned. Returns clicked.
    bool RowToggle(const char* label, bool on)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        const float w = ImGui::GetFrameHeight() * 1.8f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x);
        return ToggleSwitch(label, on);
    }

    // A single icon button that toggles light/dark: a half-filled circle (the
    // classic day/night glyph), custom-drawn so it needs no icon font. Returns clicked.
    bool IconThemeButton(const char* id)
    {
        const float h = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(h, h));
        const bool clicked = ImGui::IsItemClicked();
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("Toggle light / dark");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
        const float r = h * 0.30f;
        const ImU32 col = ImGui::GetColorU32(hov ? g_accent : g_dim);
        constexpr float kPi = 3.14159265f;
        dl->AddCircle(c, r, col, 28, 2.0f);
        dl->PathArcTo(c, r, kPi * 0.5f, kPi * 1.5f, 16);   // left half
        dl->PathFillConvex(col);
        return clicked;
    }

    // A native-looking caption button (kind 0 = minimize, 1 = maximize/restore,
    // 2 = close), drawn with Windows' own Segoe Fluent/MDL2 glyphs at native size.
    // `w` is the button width (native is ~46px); close hovers red. Returns clicked.
    bool CaptionButton(const char* id, int kind, ImFont* iconFont, float w, bool maximized = false)
    {
        const float h  = ImGui::GetFrameHeight();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool clicked = ImGui::IsItemClicked();
        const bool hov     = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov)   // native hover is a flat square fill (red for close)
        {
            const ImU32 bg = (kind == 2) ? IM_COL32(232, 56, 46, 255)
                                         : ImGui::GetColorU32(ImGuiCol_ButtonHovered);
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 0.0f);
        }
        const ImU32 fg = (hov && kind == 2) ? IM_COL32(255, 255, 255, 255)
                                            : ImGui::GetColorU32(g_text);
        const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
        if (iconFont && iconFont->FontSize > 0.0f)
        {
            // Segoe Fluent/MDL2 glyphs: ChromeMinimize E921, ChromeMaximize E922,
            // ChromeRestore E923, ChromeClose E8BB (UTF-8 encoded).
            const char* g = (kind == 0) ? "\xEE\xA4\xA1"
                          : (kind == 1) ? (maximized ? "\xEE\xA4\xA3" : "\xEE\xA4\xA2")
                                        : "\xEE\xA2\xBB";
            const float fs = iconFont->FontSize;
            const ImVec2 gs = iconFont->CalcTextSizeA(fs, FLT_MAX, 0.0f, g);
            dl->AddText(iconFont, fs, ImVec2(c.x - gs.x * 0.5f, c.y - gs.y * 0.5f), fg, g);
        }
        else   // fallback if the system icon font is missing: simple drawn glyphs
        {
            const float r = h * 0.16f, th = 1.2f;
            if (kind == 0) dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, th);
            else if (kind == 1) dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 0, 0, th);
            else { dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, th);
                   dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, th); }
        }
        return clicked;
    }

    // Enumerate top-level windows worth capturing (visible, titled, not tool/owned,
    // not ours) into `out`.
    struct WinEntry { HWND hwnd; char title[160]; };
    BOOL CALLBACK EnumProc(HWND h, LPARAM lp)
    {
        auto* out = reinterpret_cast<std::vector<WinEntry>*>(lp);
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        if (GetWindowLongPtrA(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        // DWM-cloaked: suspended UWP/PWA apps, virtual-desktop-hidden, and
        // some tray-only apps that keep a phantom HWND alive. Filters out
        // Edge, Settings, Mail etc. that the user can't actually see.
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED,
                                            &cloaked, sizeof(cloaked))) && cloaked)
            return TRUE;
        char title[160] = {};
        if (GetWindowTextA(h, title, (int)sizeof(title)) <= 0) return TRUE;
        char cls[64] = {};
        GetClassNameA(h, cls, (int)sizeof(cls));
        if (strcmp(cls, "SRWeaverWindow") == 0 || strcmp(cls, "SRWeaverGuiWindow") == 0) return TRUE;
        WinEntry e{}; e.hwnd = h;
        strncpy_s(e.title, title, _TRUNCATE);
        out->push_back(e);
        return TRUE;
    }

    // Enumerate the monitors (real + virtual/headless) into `out` for the display picker.
    struct MonEntry { HMONITOR mon; char gdi[32]; int w, h; char label[128]; };
    BOOL CALLBACK MonEnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
    {
        auto* out = reinterpret_cast<std::vector<MonEntry>*>(lp);
        MONITORINFOEXA mi{}; mi.cbSize = sizeof(mi);
        GetMonitorInfoA(mon, &mi);
        MonEntry e{}; e.mon = mon;
        strncpy_s(e.gdi, mi.szDevice, _TRUNCATE);     // \\.\DISPLAYn
        e.w = mi.rcMonitor.right - mi.rcMonitor.left;
        e.h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        // Fallback label; ResolveDisplayNames() upgrades it to the friendly name.
        snprintf(e.label, sizeof(e.label), "Display %d  (%dx%d)%s", (int)out->size() + 1, e.w, e.h,
                 (mi.dwFlags & MONITORINFOF_PRIMARY) ? "  primary" : "");
        out->push_back(e);
        return TRUE;
    }

    // Replace each monitor's label with the friendly EDID name Windows knows (e.g.
    // "Odyssey G9 (3840x2160)") when available, via one DisplayConfig query. Headless/
    // virtual displays often have no name → they keep the "Display N" fallback.
    void ResolveDisplayNames(std::vector<MonEntry>& mons)
    {
        UINT32 nPath = 0, nMode = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS) return;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr) != ERROR_SUCCESS)
            return;
        paths.resize(nPath);
        for (auto& p : paths)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            char gdi[32] = {};
            WideCharToMultiByte(CP_UTF8, 0, src.viewGdiDeviceName, -1, gdi, sizeof(gdi), nullptr, nullptr);

            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
            tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = sizeof(tgt);
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
            if (tgt.monitorFriendlyDeviceName[0] == 0) continue;
            char name[64] = {};
            WideCharToMultiByte(CP_UTF8, 0, tgt.monitorFriendlyDeviceName, -1, name, sizeof(name), nullptr, nullptr);

            for (auto& m : mons)
                if (_stricmp(m.gdi, gdi) == 0)
                    snprintf(m.label, sizeof(m.label), "%s  (%dx%d)", name, m.w, m.h);
        }
    }

    // Specific label for a stereo format (for the compact status line) — includes
    // the Full/Half and Row/Column variant so compact shows exactly what's active.
    const char* FormatCatLabel(StereoFormat f)
    {
        switch (f) {
        case StereoFormat::FullSBS:           return "Full Side-by-Side";
        case StereoFormat::HalfSBS:           return "Half Side-by-Side";
        case StereoFormat::FullTAB:           return "Full Top-and-Bottom";
        case StereoFormat::HalfTAB:           return "Half Top-and-Bottom";
        case StereoFormat::RowInterleaved:    return "Row Interleaved";
        case StereoFormat::ColumnInterleaved: return "Column Interleaved";
        case StereoFormat::Checkerboard:      return "Checkerboard";
        case StereoFormat::Anaglyph:          return "Anaglyph";
        case StereoFormat::FrameSequential:   return "Frame Sequential";
        case StereoFormat::Pulfrich:          return "Pulfrich Effect";
        case StereoFormat::FramePacking:      return "Frame Packing";
        default:                              return "Side-by-Side";
        }
    }
}

bool Gui::Init(HWND mainHwnd, ID3D11Device* device, ID3D11DeviceContext* context)
{
    g_gui = this;
    m_mainHwnd = mainHwnd;
    (void)device; (void)context;   // intentionally NOT shared — see below

    // Cache the persisted STARTUP toggles up-front so the panel renders the right
    // state without hitting the registry every frame. Writes go through Settings
    // and update these in place.
    m_runAtStartup = Settings::ReadRunAtStartup();
    m_startInTray  = Settings::ReadStartInTray();

    // The GUI renders on its OWN D3D11 device, never the weaver's. The SR runtime
    // drives the weaver's immediate context (including from its own thread while
    // weaving), so letting ImGui touch that same context from the main loop races
    // it and crashes whenever the panel is open during a weave. A dedicated device
    // fully isolates the panel; it shares no textures with the weaver.
    Log("Gui::Init: D3D11CreateDevice begin");
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                                   nullptr, 0, D3D11_SDK_VERSION, &m_device, &fl, &m_context);
    if (FAILED(hr))
    {
        Log("Gui::Init: D3D11CreateDevice FAILED hr=0x%08X", (unsigned)hr);
        return false;
    }
    Log("Gui::Init: D3D11CreateDevice ok (FL=0x%04X)", (unsigned)fl);

    Log("Gui::Init: register class + create window");
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;   // a drop shadow for the borderless window
    wc.lpfnWndProc = &Gui::WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kGuiClass;
    wc.hIcon   = (HICON)LoadImageA(wc.hInstance, MAKEINTRESOURCEA(IDI_TRAY), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = wc.hIcon;   // taskbar button icon
    RegisterClassExA(&wc);

    // Borderless top-level window: no OS caption, so no app icon and no name. We draw
    // our own Win11-style min/max/close buttons + drag region in the header. WS_POPUP
    // drops the frame; WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU keep those behaviours
    // (and the minimise animation); WS_EX_APPWINDOW gives a normal taskbar button.
    m_hwnd = CreateWindowExA(
        WS_EX_APPWINDOW, kGuiClass, "",
        WS_POPUP | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 720,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!m_hwnd)
    {
        Log("Gui::Init: CreateWindowExA FAILED gle=%lu", GetLastError());
        return false;
    }

    // Round the corners (Win11) to match the modern look.
    DWORD corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, kDwmCornerPref, &corner, sizeof(corner));

    Log("Gui::Init: ImGui CreateContext");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // don't write imgui.ini
    Log("Gui::Init: ImGui_ImplWin32_Init");
    if (!ImGui_ImplWin32_Init(m_hwnd)) { Log("Gui::Init: ImGui_ImplWin32_Init FAILED"); return false; }
    Log("Gui::Init: ImGui_ImplDX11_Init");
    if (!ImGui_ImplDX11_Init(m_device, m_context)) { Log("Gui::Init: ImGui_ImplDX11_Init FAILED"); return false; }
    m_imguiReady = true;
    Log("Gui::Init: imgui ready");

    // Scale everything (font + style + window) to the monitor the window is on, so
    // the panel is the same perceptual size on a 4K/150% display and a 1440p/100%
    // one. WM_DPICHANGED re-applies this when it's dragged between monitors.
    //
    // __try/__except (Windows SEH, not C++ try/catch) — a v2.0 report came
    // back showing 0xC0000005 (access violation) inside ApplyScaling at
    // dpiScale=1.50 on Win10 22H2 + Samsung Odyssey. Access violations are
    // structured exceptions; plain C++ catch doesn't see them under default
    // /EHsc, so the previous try/catch quietly let the process die. SEH
    // catches them. Granular Log calls inside ApplyScaling pin the exact
    // line for next-run diagnosis; the GUI continues with ImGui's defaults
    // if the scaling step dies (un-scaled fonts but functional panel).
    Log("Gui::Init: ImGui_ImplWin32_GetDpiScaleForHwnd");
    m_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(m_hwnd);
    if (m_dpiScale <= 0.0f || m_dpiScale > 8.0f)
    {
        Log("Gui::Init: clamping bogus dpiScale=%.2f to 1.0", m_dpiScale);
        m_dpiScale = 1.0f;
    }
    Log("Gui::Init: dpiScale=%.2f -> ApplyScaling", m_dpiScale);
    __try {
        ApplyScaling();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("Gui::Init: ApplyScaling SEH 0x%08lX -- continuing with defaults",
            (unsigned long)GetExceptionCode());
    }
    Log("Gui::Init: ApplyScaling done -> SystemParametersInfo");
    // Centre on the primary monitor's work area (CW_USEDEFAULT doesn't position a
    // WS_POPUP window); FitHeightToContent later adjusts the height in place.
    const int w0 = (int)(460 * m_dpiScale), h0 = (int)(820 * m_dpiScale);
    RECT wa{}; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    const int x0 = wa.left + ((wa.right - wa.left) - w0) / 2;
    const int y0 = wa.top  + ((wa.bottom - wa.top) - h0) / 2;
    SetWindowPos(m_hwnd, nullptr, x0, y0, w0, h0, SWP_NOZORDER | SWP_NOACTIVATE);
    Log("Gui::Init: SetWindowPos done -> returning true");
    return true;
}

// Rebuild the ImGui style and fonts for the current theme at the current DPI scale.
// Called on init, when the theme is toggled, and on WM_DPICHANGED.
void Gui::ApplyScaling()
{
    Log("Gui::ApplyScaling: enter (dpiScale=%.2f)", m_dpiScale);
    ImGui::GetStyle() = ImGuiStyle();              // reset to defaults FIRST, else ScaleAllSizes
                                                   // compounds un-reset fields each call (e.g.
                                                   // WindowMinSize) → window slowly inflates and
                                                   // pushes the theme button off-window.
    Log("Gui::ApplyScaling: ApplyTheme");
    ApplyTheme(m_lightMode);                       // base (unscaled) sizes + colours
    Log("Gui::ApplyScaling: ScaleAllSizes");
    ImGui::GetStyle().ScaleAllSizes(m_dpiScale);   // scale paddings/rounding to DPI

    ImGuiIO& io = ImGui::GetIO();
    Log("Gui::ApplyScaling: io.Fonts->Clear");
    io.Fonts->Clear();
    const float s = m_dpiScale;
    // Body = Inter (bundled, SIL OFL) — the open-source SF-Pro-like UI font Reeder's
    // look is built on; headings = Inter SemiBold. Falls back to Segoe UI Variable
    // then the built-in font if the bundled files are missing.
    // Bake Inter from the fonts embedded in the exe (no external files). The memory is
    // resource-owned, so FontDataOwnedByAtlas=false keeps ImGui from trying to free it.
    // Body font range: Latin + a tiny slice of the Arrows block so the
    // "Launch OpenTrack ↗" button can show the launch arrow inline.
    // Range includes U+2197 (NORTH EAST ARROW) which Inter ships.
    static const ImWchar bodyRange[] = {
        0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
        0x2190, 0x21FF,   // Arrows (we use U+2197)
        0,
    };
    auto addEmbedded = [&](int resId, float px, const ImWchar* ranges = nullptr) -> ImFont* {
        size_t sz = 0; const void* data = EmbeddedResource(resId, sz);
        if (!data || sz == 0) return nullptr;
        ImFontConfig cfg; cfg.FontDataOwnedByAtlas = false;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(data), (int)sz, px, &cfg, ranges);
    };
    Log("Gui::ApplyScaling: body font (Inter Regular)");
    m_fontRegular = addEmbedded(IDR_FONT_REGULAR, 17.0f * s, bodyRange);
    if (!m_fontRegular) { Log("Gui::ApplyScaling: Inter Regular failed -> SegUIVar.ttf"); m_fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegUIVar.ttf", 17.0f * s, nullptr, bodyRange); }
    if (!m_fontRegular) { Log("Gui::ApplyScaling: SegUIVar.ttf failed -> AddFontDefault"); m_fontRegular = io.Fonts->AddFontDefault(); }
    Log("Gui::ApplyScaling: body font done (ptr=%p)", (void*)m_fontRegular);
    Log("Gui::ApplyScaling: hero font (Inter SemiBold)");
    ImFont* big = addEmbedded(IDR_FONT_SEMIBOLD, 23.0f * s);
    if (!big) { Log("Gui::ApplyScaling: Inter SemiBold failed -> seguisb.ttf"); big = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 23.0f * s); }
    m_fontLarge = big ? big : m_fontRegular;
    Log("Gui::ApplyScaling: hero font done (ptr=%p)", (void*)m_fontLarge);
    // Windows' own caption-button glyphs: Segoe Fluent Icons (Win11) -> Segoe
    // MDL2 Assets (Win10). Baked at the native ~10px so the min/max/close
    // look like the OS's. If BOTH font files fail to load (e.g. corrupted
    // system fonts directory) we leave m_fontIcons = nullptr -- CaptionButton
    // and the About popup both null-check it and fall back to drawn glyphs.
    static const ImWchar iconRange[] = { 0xE700, 0xE9FF, 0 };
    m_fontIcons = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\SegoeIcons.ttf", 10.0f * s, nullptr, iconRange);
    if (m_fontIcons) Log("Gui::ApplyScaling: icon font = SegoeIcons.ttf (Win11)");
    if (!m_fontIcons)
    {
        m_fontIcons = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segmdl2.ttf", 10.0f * s, nullptr, iconRange);
        if (m_fontIcons) Log("Gui::ApplyScaling: icon font = segmdl2.ttf (Win10 fallback)");
    }
    if (!m_fontIcons)
        Log("Gui::ApplyScaling: icon font UNAVAILABLE (both SegoeIcons.ttf and segmdl2.ttf failed) -- using drawn-glyph fallback");
    Log("Gui::ApplyScaling: io.Fonts->Build");
    io.Fonts->Build();
    Log("Gui::ApplyScaling: SetFontDefault");
    io.FontDefault = m_fontRegular;
    Log("Gui::ApplyScaling: ImGui_ImplDX11_InvalidateDeviceObjects");
    ImGui_ImplDX11_InvalidateDeviceObjects();      // recreate the font texture next frame
    Log("Gui::ApplyScaling: font atlas rebuilt");

    // Recolour the native title bar to match the app (keeps the native min/max/close
    // buttons, just tinted). Caption + text + thin border; dark-mode flag controls
    // the button glyph colour. COLORREF is 0x00BBGGRR (the RGB() macro).
    const ImVec4 bgc = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const ImVec4 txc = ImGui::GetStyle().Colors[ImGuiCol_Text];
    COLORREF caption = RGB((int)(bgc.x * 255), (int)(bgc.y * 255), (int)(bgc.z * 255));
    COLORREF txt     = RGB((int)(txc.x * 255), (int)(txc.y * 255), (int)(txc.z * 255));
    BOOL dark = m_lightMode ? FALSE : TRUE;
    DwmSetWindowAttribute(m_hwnd, kDwmImmersiveDark, &dark,    sizeof(dark));
    DwmSetWindowAttribute(m_hwnd, kDwmCaptionColor,  &caption, sizeof(caption));
    DwmSetWindowAttribute(m_hwnd, kDwmTextColor,     &txt,     sizeof(txt));
    DwmSetWindowAttribute(m_hwnd, kDwmBorderColor,   &caption, sizeof(caption));
}

void Gui::Shutdown()
{
    if (m_imguiReady)
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_imguiReady = false;
    }
    ReleaseSwapChain();
    SAFE_RELEASE(m_context);   // our own device/context (not the weaver's)
    SAFE_RELEASE(m_device);
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    g_gui = nullptr;
}

void Gui::Toggle()
{
    m_visible = !m_visible;
    if (m_visible)
    {
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
    }
    else
    {
        ShowWindow(m_hwnd, SW_HIDE);
    }
}

void Gui::ReleaseSwapChain()
{
    if (m_rtv)  { m_rtv->Release();  m_rtv = nullptr; }
    if (m_swap) { m_swap->Release(); m_swap = nullptr; }
    m_w = m_h = 0;
}

bool Gui::EnsureSwapChain(UINT w, UINT h)
{
    if (w == 0 || h == 0) return false;
    if (m_swap && w == m_w && h == m_h) return true;

    if (!m_swap)
    {
        IDXGIDevice* dxgiDev = nullptr;
        if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
        IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter);
        IDXGIFactory2* factory = nullptr; adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w; sd.Height = h;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = factory->CreateSwapChainForHwnd(m_device, m_hwnd, &sd, nullptr, nullptr, &m_swap);
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        if (FAILED(hr)) return false;
    }
    else
    {
        if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
        m_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    }

    ID3D11Texture2D* back = nullptr;
    if (FAILED(m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    m_device->CreateRenderTargetView(back, nullptr, &m_rtv);
    back->Release();
    m_w = w; m_h = h;
    return m_rtv != nullptr;
}

bool Gui::Render(GuiState& state)
{
    if (!m_visible || !m_imguiReady) return false;

    RECT rc{}; GetClientRect(m_hwnd, &rc);
    if (!EnsureSwapChain((UINT)(rc.right - rc.left), (UINT)(rc.bottom - rc.top)))
        return false;

    // Apply a pending theme/DPI rebuild BETWEEN frames. Clearing the font atlas
    // mid-frame (after NewFrame) would crash ImGui::Render — that was the
    // light-mode-toggle crash. Doing it here keeps the whole frame consistent.
    if (m_pendingRescale) { ApplyScaling(); m_pendingRescale = false; }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    auto post = [&](UINT id) { PostMessageA(m_mainHwnd, WM_COMMAND, MAKEWPARAM(id, 0), 0); };
    bool convChanged = false;

    // Full-window panel.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("SR Loom", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Title bar (custom, since the window is borderless): min/max/close are flush to
    // the very top-right corner; the "i" info button + light/dark toggle sit on the
    // row just below them (expanded only). Dragging is handled globally further down
    // (any empty space moves the window), so there's no dedicated drag strip here.
    {
        const ImVec2 win  = ImGui::GetWindowPos();        // (0,0) for the full-window panel
        const float  h    = ImGui::GetFrameHeight();
        const float  bw   = 46.0f * m_dpiScale;           // native caption-button width
        const float  winW = ImGui::GetWindowWidth();
        const float  pad  = ImGui::GetStyle().WindowPadding.x;
        const float  py   = ImGui::GetStyle().WindowPadding.y;

        // Min / max / close — flush to the top-right corner (y = 0).
        ImGui::SetCursorScreenPos(ImVec2(win.x + winW - bw * 3.0f, win.y));
        if (CaptionButton("##min", 0, m_fontIcons, bw)) ShowWindow(m_hwnd, SW_MINIMIZE);
        ImGui::SameLine(0, 0);
        const bool zoomed = IsZoomed(m_hwnd) != 0;
        if (CaptionButton("##max", 1, m_fontIcons, bw, zoomed)) ShowWindow(m_hwnd, zoomed ? SW_RESTORE : SW_MAXIMIZE);
        ImGui::SameLine(0, 0);
        if (CaptionButton("##close", 2, m_fontIcons, bw)) { m_visible = false; ShowWindow(m_hwnd, SW_HIDE); }

        float headerH = h;   // bottom of the title-bar band (absolute window-Y)

        // "i" info (left) + light/dark (right), one row below the caption buttons.
        if (m_expanded)
        {
            const float rowY = h + 2.0f * m_dpiScale;
            ImGui::SetCursorScreenPos(ImVec2(win.x + pad, win.y + rowY));
            {
                const ImVec2 ip = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##info", ImVec2(h, h));
                const bool hov = ImGui::IsItemHovered();
                if (hov) ImGui::SetTooltip("About SR Loom");
                if (ImGui::IsItemClicked()) ImGui::OpenPopup("About");
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 ic(ip.x + h * 0.5f, ip.y + h * 0.5f);
                const ImU32 col = ImGui::GetColorU32(hov ? g_accent : g_dim);
                dl->AddCircle(ic, h * 0.34f, col, 24, 2.0f);
                const ImVec2 ts = ImGui::CalcTextSize("i");
                dl->AddText(ImVec2(ic.x - ts.x * 0.5f, ic.y - ts.y * 0.5f), col, "i");

                // About popup: version, SR runtime version, keyboard
                // shortcuts, GitHub + check-for-updates links.
                if (ImGui::BeginPopup("About"))
                {
                    ImGui::PushFont(m_fontLarge);
                    ImGui::TextUnformatted("SR Loom");
                    ImGui::PopFont();
                    const float nameBottom = ImGui::GetItemRectMax().y;
                    ImGui::SameLine();
                    // Sit "vX.Y" on the title's baseline (align the text bottoms).
                    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
                                                     nameBottom - ImGui::GetTextLineHeight()));
                    ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
                    char ver[32]; snprintf(ver, sizeof(ver), "v%s", kAppVersion);
                    ImGui::TextUnformatted(ver);
                    ImGui::PopStyleColor();

                    // SR Platform runtime version (Leia SR service that we
                    // talk to). Useful for support diagnostics. Skipped if
                    // we couldn't query it -- usually means the service isn't
                    // installed / running.
                    if (state.srPlatformVersion[0])
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
                        ImGui::Text("SR Platform runtime: %s", state.srPlatformVersion);
                        ImGui::PopStyleColor();
                    }

                    Section("KEYBOARD SHORTCUTS");
                    auto sc = [&](const char* keys, const char* what) {
                        ImGui::TextUnformatted(keys);
                        ImGui::SameLine(132 * m_dpiScale);
                        ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
                        ImGui::TextUnformatted(what);
                        ImGui::PopStyleColor();
                    };
                    sc("Ctrl+Alt+W", "Toggle Weaving");
                    sc("Ctrl+Alt+F", "Fullscreen / Windowed");
                    sc("Ctrl+Alt+C", "Make Active Window 3D");

                    ImGui::Dummy(ImVec2(0, 6 * m_dpiScale));
                    // "Check for updates" link. Posts the result directly
                    // to this window so we can show the outcome inline
                    // instead of via a tray balloon (which the user can
                    // miss). Stays open until the user dismisses it.
                    ImGui::PushStyleColor(ImGuiCol_Text, g_accent);
                    const char* checkLabel = (m_updateCheck == UpdateCheck::Checking)
                                             ? "Checking..." : "Check for updates";
                    const bool checkDisabled = (m_updateCheck == UpdateCheck::Checking);
                    if (checkDisabled) ImGui::BeginDisabled();
                    // DontClosePopups keeps the About popup open after the
                    // click so the inline result (Up To Date / failed) is
                    // visible -- by default ImGui::Selectable auto-closes
                    // any open popup on click, which dismissed our feedback.
                    if (ImGui::Selectable(checkLabel, false,
                                          ImGuiSelectableFlags_DontClosePopups,
                                          ImGui::CalcTextSize(checkLabel)))
                    {
                        m_updateCheck = UpdateCheck::Checking;
                        m_updateTag.clear();
                        m_updateUrl.clear();
                        UpdateChecker::StartAsync(m_hwnd, WM_APP_UPDATE_RESULT, true);
                    }
                    if (checkDisabled) ImGui::EndDisabled();
                    ImGui::PopStyleColor();

                    // Inline result of the most recent forced check. Green
                    // tick for up-to-date, accent text for "update available"
                    // (the new release page has already been opened in the
                    // browser by the WndProc handler), red X for failure.
                    // The tick/X glyphs come from the Segoe Fluent / MDL2
                    // icon font we already load for the caption buttons
                    // (E73E = CheckMark, E711 = Cancel/X). The default
                    // font's glyph range doesn't cover U+2713 / U+2715.
                    auto iconLine = [&](const char* iconUtf8, ImU32 col, const char* label) {
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        if (m_fontIcons)
                        {
                            ImGui::PushFont(m_fontIcons);
                            ImGui::TextUnformatted(iconUtf8);
                            ImGui::PopFont();
                            ImGui::SameLine(0, 8.0f * m_dpiScale);
                            ImGui::AlignTextToFramePadding();
                        }
                        ImGui::TextUnformatted(label);
                        ImGui::PopStyleColor();
                    };
                    if (m_updateCheck == UpdateCheck::UpToDate)
                        iconLine("\xEE\x9C\xBE", IM_COL32(94, 178, 110, 255), "Up To Date");
                    else if (m_updateCheck == UpdateCheck::Available)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, g_accent);
                        ImGui::Text("Update available: %s -- opened in browser", m_updateTag.c_str());
                        ImGui::PopStyleColor();
                    }
                    else if (m_updateCheck == UpdateCheck::Failed)
                        iconLine("\xEE\x9C\x91", IM_COL32(210, 105, 74, 255), "Check failed");

                    ImGui::PushStyleColor(ImGuiCol_Text, g_accent);
                    if (ImGui::Selectable("Open Github", false,
                                          ImGuiSelectableFlags_DontClosePopups,
                                          ImGui::CalcTextSize("Open Github")))
                        ShellExecuteA(nullptr, "open", kGithubUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }
                else
                {
                    // Popup closed -- reset state so the next time it opens
                    // we don't flash a stale "Up To Date" message that's
                    // hours old.
                    if (m_updateCheck != UpdateCheck::Checking)
                        m_updateCheck = UpdateCheck::Idle;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(win.x + winW - h - pad, win.y + rowY));
            if (IconThemeButton("##theme")) { m_lightMode = !m_lightMode; m_pendingRescale = true; }
            headerH = rowY + h;
        }

        // Drop the cursor below the title bar for the hero (absolute → window-relative).
        ImGui::SetCursorPos(ImVec2(0.0f, headerH + 4.0f * m_dpiScale - py));
    }

    // Hero weaving toggle — centred: big "Loom Weaver"/"Loom Weaving" label + switch.
    // Top gap is collapsed in compact mode so the three status lines below have
    // real breathing room without the panel overflowing. Expanded mode keeps
    // the original spacing since vertical space isn't as tight there.
    ImGui::Dummy(ImVec2(0, (m_expanded ? 2.0f : 0.0f) * m_dpiScale));
    {
        const float winW = ImGui::GetWindowSize().x;
        const char* title = state.weaving ? "Loom Weaving" : "Loom Weaver";
        ImGui::PushFont(m_fontLarge);
        ImGui::SetCursorPosX((winW - ImGui::CalcTextSize(title).x) * 0.5f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();

        // Title->switch gap: 4px in expanded, 2px in compact (compresses by 2px
        // to free more room for the status lines below).
        ImGui::Dummy(ImVec2(0, (m_expanded ? 4.0f : 2.0f) * m_dpiScale));
        const float sw = ImGui::GetFrameHeight() * 1.7f * 1.8f;   // switch width (heightScale 1.7)
        ImGui::SetCursorPosX((winW - sw) * 0.5f);
        if (ToggleSwitch("##weaving", state.weaving, 1.7f)) post(ID_TRAY_TOGGLE_WEAVE);
    }

    // Compact view: centred status lines -- display mode, stereo input format,
    // and (when active) "OpenTrack". Each line is dim and centred. We tighten
    // ItemSpacing for this block + drop the surrounding Dummys when the third
    // line is shown so the compact panel doesn't visibly grow.
    if (!m_expanded)
    {
        const float winW = ImGui::GetWindowSize().x;
        const char* disp = (state.mode == OutputMode::Fullscreen)   ? "Fullscreen"
                         : (state.mode == OutputMode::LookingGlass) ? "Looking Glass"
                         : (state.sourceName[0] ? state.sourceName : "Window");
        const char* fmt  = FormatCatLabel(state.format);
        // Compact-view label for the tracking row. Toggle conveys
        // on/off, so we never say "On". The label describes WHAT is
        // being tracked based on the output mode:
        //   1 (XYZ+YP) / 4 (6DOF)  -> "Head Tracking" (both pos + rot)
        //   2 (XYZ)               -> "Head Position Tracking"
        //   3 (YP) / 5 (YPR)      -> "Head Rotation Tracking"
        // OFF state stays as plain "Head Tracking" so the toggle still
        // labels itself clearly when greyed.
        const bool tirShown = state.trackIREnabled && state.trackIRAvailable;
        const bool  showOt  = state.openTrackEnabled
                            || state.freeTrackEnabled
                            || tirShown;
        const char* trackLabel = "Head Tracking";
        if (showOt)
        {
            switch (state.openTrackMode)
            {
                case 2: trackLabel = "Head Position Tracking"; break;
                case 3:
                case 5: trackLabel = "Head Rotation Tracking"; break;
                default: trackLabel = "Head Tracking"; break;
            }
        }
        // Truncate an over-long window title to "beginning..." so it fits the panel
        // instead of overflowing off both sides when centred.
        const float avail = winW - 2.0f * ImGui::GetStyle().WindowPadding.x;
        std::string dispS = disp;
        if (ImGui::CalcTextSize(disp).x > avail)
        {
            while (!dispS.empty() && ImGui::CalcTextSize((dispS + "...").c_str()).x > avail)
                dispS.pop_back();
            dispS += "...";
        }
        ImGui::Dummy(ImVec2(0, 2 * m_dpiScale));
        ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
        // Default ItemSpacing.y here -- the hero block above is compressed
        // in compact mode (Dummy(0) before title, Dummy(2) between title +
        // toggle) so the three status lines get to breathe at normal
        // inter-line spacing without the panel overflowing.
        ImGui::SetCursorPosX((winW - ImGui::CalcTextSize(dispS.c_str()).x) * 0.5f);
        ImGui::TextUnformatted(dispS.c_str());
        ImGui::SetCursorPosX((winW - ImGui::CalcTextSize(fmt).x) * 0.5f);
        ImGui::TextUnformatted(fmt);
        // Head-tracking line: small toggle on the left + protocol summary
        // text. Toggle is ALWAYS visible (replacing the previous "show only
        // when active" behaviour) so users can quickly turn tracking off
        // without diving into the dropdown -- important because while
        // tracking is on, the SR camera is engaged. Click = enable all 3
        // protocols (resets to the everything-on default). Click while
        // on = disable all 3, which tears the bridge listener down and
        // releases the camera. Per-protocol fine-tuning still lives in the
        // expanded HEADTRACKING section.
        {
            // OFF: just "Head Tracking" (toggle's off-state colour conveys
            // the state, no need for the "Off" suffix).
            const char* ot = showOt ? trackLabel : "Head Tracking";
            const float th  = ImGui::GetTextLineHeight();
            const float swH = ImGui::GetFrameHeight() * 0.55f;   // toggle height
            const float swW = swH * 1.8f;                        // toggle width
            const float gap = 0.45f * th;
            const float textW = ImGui::CalcTextSize(ot).x;
            const float groupW = swW + gap + textW;
            const float startX = (winW - groupW) * 0.5f;
            const float startY = ImGui::GetCursorPosY();
            // Toggle on the left, vertically centred against the text line.
            ImGui::SetCursorPos(ImVec2(startX, startY + (th - swH) * 0.5f));
            if (ToggleSwitch("##ht_compact", showOt, 0.55f))
            {
                if (showOt)
                {
                    // Was on -> turn everything off.
                    state.openTrackEnabled = false;
                    state.freeTrackEnabled = false;
                    state.trackIREnabled   = false;
                }
                else
                {
                    // Was off -> enable everything (TrackIR included since
                    // the embedded NPClient.dll is always available now).
                    state.openTrackEnabled = true;
                    state.freeTrackEnabled = true;
                    state.trackIREnabled   = true;
                }
                state.openTrackChanged = true;
            }
            // Text sits to the right of the toggle on the same baseline.
            ImGui::SameLine(0, gap);
            ImGui::SetCursorPosY(startY);
            ImGui::TextUnformatted(ot);
        }
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0, (m_expanded ? 4 : 2) * m_dpiScale));

    // Expand/collapse control. Collapsed by default → the panel is just the on/off
    // toggle (defaulting to fullscreen SBS); expand to reveal output + conversion.
    // A chevron points down when collapsed (more below) and up when expanded.
    {
        const char* lbl = m_expanded ? "Hide" : "Options";
        const float fp  = ImGui::GetStyle().FramePadding.x;
        const float s   = 4.0f * m_dpiScale;     // chevron half-size
        const float gap = 7.0f * m_dpiScale;     // space between arrow and word
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        const float bw  = fp * 2 + s * 2 + gap + ts.x;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - bw) * 0.5f);
        const bool clicked = ImGui::Button("##opt", ImVec2(bw, 0));   // empty label, stable id

        const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const float cy = (mn.y + mx.y) * 0.5f;
        const float cx = mn.x + fp + s;          // arrow on the LEFT
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(g_text);
        if (m_expanded)   // up
            dl->AddTriangleFilled(ImVec2(cx - s, cy + s * 0.55f), ImVec2(cx + s, cy + s * 0.55f), ImVec2(cx, cy - s * 0.55f), col);
        else              // down
            dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.55f), ImVec2(cx + s, cy - s * 0.55f), ImVec2(cx, cy + s * 0.55f), col);
        dl->AddText(ImVec2(mn.x + fp + s * 2 + gap, cy - ts.y * 0.5f), col, lbl);

        if (clicked) m_expanded = !m_expanded;
    }

    if (m_expanded)
    {
    // Current anaglyph recovery-mode label (used by the 3D format section).
    const char* curMode = nullptr;
    {
        int n = 0; const AnaglyphModeEntry* m = AnaglyphModeList(n);
        curMode = m[0].label;
        for (int i = 0; i < n; ++i) if (m[i].value == state.anaglyphMode) curMode = m[i].label;
    }

    // --- Scrolling options ---
    const float topY = ImGui::GetCursorPosY();
    float maxChild;
    {
        RECT wr2{}, cr2{}; GetWindowRect(m_hwnd, &wr2); GetClientRect(m_hwnd, &cr2);
        const float ncH = (float)((wr2.bottom - wr2.top) - (cr2.bottom - cr2.top));
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfo(mon, &mi);
        maxChild = (float)(mi.rcWork.bottom - mi.rcWork.top) - ncH - topY - ImGui::GetStyle().WindowPadding.y - 4.0f;
        if (maxChild < 120.0f) maxChild = 120.0f;
    }
    const float want = (m_sectionsH > 1.0f) ? m_sectionsH : (400.0f * m_dpiScale);
    const float childH = (want < maxChild) ? want : maxChild;
    ImGui::BeginChild("opts", ImVec2(0, childH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);   // no box/border around the options
    {
        // What to weave. The source choice implies the output: Monitor = fullscreen,
        // a picked Window = windowed overlay; Looking Glass is the floating loupe.
        Section("DISPLAY");
        {
            const float gap = 10.0f;
            const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;

            // This Display → fullscreen passthrough weave of the SR display itself.
            const bool monActive = (state.source == SourceKind::CaptureMonitor &&
                                    !state.foreignDisplay &&
                                    state.mode == OutputMode::Fullscreen);
            if (monActive) {
                ImGui::PushStyleColor(ImGuiCol_Button,        g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  g_accent);
                ImGui::PushStyleColor(ImGuiCol_Text,          g_accentText);
            }
            if (ImGui::Button("Fullscreen", ImVec2(half, 0))) post(ID_TRAY_SRC_MONITOR);
            if (monActive) ImGui::PopStyleColor(4);

            // Looking Glass → floating loupe of the monitor (drag/resize anywhere).
            ImGui::SameLine(0, gap);
            const bool lgActive = (state.mode == OutputMode::LookingGlass);
            if (lgActive) {
                ImGui::PushStyleColor(ImGuiCol_Button,        g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  g_accent);
                ImGui::PushStyleColor(ImGuiCol_Text,          g_accentText);
            }
            if (ImGui::Button("Looking Glass", ImVec2(half, 0))) post(ID_TRAY_LOOKING_GLASS);
            if (lgActive) ImGui::PopStyleColor(4);

            // Row 2: paired buttons-as-combos for "Other Displays" and "Window".
            // Using Button + popup instead of BeginCombo so we can centre the
            // text via ButtonTextAlign (ImGui combos always left-align preview).
            const bool dispActive = (state.source == SourceKind::CaptureMonitor && state.foreignDisplay);
            const bool winActive  = (state.source == SourceKind::CaptureWindow);

            std::vector<MonEntry> mons;
            EnumDisplayMonitors(nullptr, nullptr, MonEnumProc, reinterpret_cast<LPARAM>(&mons));
            ResolveDisplayNames(mons);   // friendly EDID names where Windows knows them
            const char* dispLabel = "Other Displays";
            if (dispActive)
                for (auto& m : mons) if (m.mon == state.captureMonitor) dispLabel = m.label;
            const char* winLabel = (winActive && state.sourceName[0]) ? state.sourceName : "Window";

            // Use BeginCombo so we get the same built-in dropdown arrow + list
            // styling as the Stereo 3D Input combo above. Active source gets
            // the accent palette so it visually matches the action buttons.
            auto pushAccent = [&]() {
                ImGui::PushStyleColor(ImGuiCol_Button,        g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  g_accent);
                ImGui::PushStyleColor(ImGuiCol_FrameBg,       g_accent);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,g_accent);
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, g_accent);
                ImGui::PushStyleColor(ImGuiCol_Text,          g_accentText);
            };
            auto popAccent  = []() { ImGui::PopStyleColor(7); };

            // -- Other display (left half) -----------------------------------
            if (dispActive) pushAccent();
            ImGui::SetNextItemWidth(half);
            if (ImGui::BeginCombo("##srcdisp", dispLabel, ImGuiComboFlags_HeightLargest))
            {
                int shown = 0;
                for (int i = 0; i < (int)mons.size(); ++i)
                {
                    if (mons[i].mon == state.srMonitor) continue;   // that's "This Display"
                    ImGui::PushID(i);
                    const bool sel = dispActive && mons[i].mon == state.captureMonitor;
                    if (ImGui::Selectable(mons[i].label, sel))
                        PostMessageA(m_mainHwnd, WM_APP_GUI_CAPTURE_DISPLAY, 0, reinterpret_cast<LPARAM>(mons[i].mon));
                    ImGui::PopID();
                    ++shown;
                }
                if (shown == 0) ImGui::TextDisabled("No other displays");
                ImGui::EndCombo();
            }
            if (dispActive) popAccent();

            // -- Window (right half) -----------------------------------------
            ImGui::SameLine(0, gap);
            if (winActive) pushAccent();
            ImGui::SetNextItemWidth(half);
            if (ImGui::BeginCombo("##srcwin", winLabel, ImGuiComboFlags_HeightLargest))
            {
                std::vector<WinEntry> wins;
                EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&wins));
                for (int i = 0; i < (int)wins.size(); ++i)
                {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(wins[i].title, false))
                        PostMessageA(m_mainHwnd, WM_APP_GUI_CAPTURE_WINDOW, 0, reinterpret_cast<LPARAM>(wins[i].hwnd));
                    ImGui::PopID();
                }
                if (wins.empty()) ImGui::TextDisabled("No windows");
                ImGui::EndCombo();
            }
            if (winActive) popAccent();

            // Row 3: paired action buttons — "Load Image..." (left) and
            // "Make Active Window 3D" (right). Image load opens the standard
            // file dialog and routes through the existing tray command.
            if (ImGui::Button("Load Media...",          ImVec2(half, 0))) post(ID_TRAY_SRC_TESTIMAGE);
            ImGui::SameLine(0, gap);
            if (ImGui::Button("Make Active Window 3D", ImVec2(half, 0))) post(ID_TRAY_CAPTURE_FOREGROUND);
        }

        Section("STEREO 3D INPUT");
        {
            // Category combo (whole list, no scroll). SBS / TAB / Interleaved /
            // VR180 / VR360 expose a second "variant" combo for the layout.
            // StereoFormat::Katanga deliberately not exposed (parked from
            // the UI -- see Common.h's StereoFormatList comment). If the
            // current format is Katanga (e.g. carried over from a saved
            // state), catOf below maps it to C_SBS so the dropdown still
            // has something coherent to display.
            enum Cat { C_SBS, C_TAB, C_IL, C_CHECK, C_ANA, C_FSEQ, C_PULF, C_FP, C_QUILT, C_VR180, C_VR360, C_LFP, C_N };
            static const char* const kCat[C_N] = {
                "Side-by-Side", "Top-and-Bottom", "Interleaved", "Checkerboard",
                "Anaglyph", "Frame Sequential", "Pulfrich Effect", "Frame Packing",
                "Quilt", "VR180", "VR360", "Lytro Light Field" };
            auto catOf = [](StereoFormat f) -> int {
                switch (f) {
                case StereoFormat::FullSBS: case StereoFormat::HalfSBS:        return C_SBS;
                case StereoFormat::FullTAB: case StereoFormat::HalfTAB:        return C_TAB;
                case StereoFormat::RowInterleaved: case StereoFormat::ColumnInterleaved: return C_IL;
                case StereoFormat::Checkerboard:    return C_CHECK;
                case StereoFormat::Anaglyph:        return C_ANA;
                case StereoFormat::FrameSequential: return C_FSEQ;
                case StereoFormat::Pulfrich:        return C_PULF;
                case StereoFormat::FramePacking:    return C_FP;
                case StereoFormat::Quilt:           return C_QUILT;
                case StereoFormat::VR180TAB: case StereoFormat::VR180SBS:    return C_VR180;
                case StereoFormat::VR360TAB: case StereoFormat::VR360SBS:    return C_VR360;
                case StereoFormat::LightField:      return C_LFP;
                default:                            return C_SBS;
                }
            };
            auto postFmt = [&](StereoFormat f) { post(ID_TRAY_FMT_BASE + (UINT)StereoFormatIndex(f)); };
            const int curCat = catOf(state.format);

            // VR180/VR360 only make sense for a static SBS/TAB equirect source
            // (loaded image / video); hide them when capturing live windows or
            // monitors. State-only filter — main.cpp also flips the format to
            // a sane non-VR default when the user switches away from the
            // image/video source while a VR format is selected.
            // VR180 / VR360 + Lytro Light Field only make sense for media
            // (image / video) sources -- live capture has no equirectangular
            // or plenoptic angular data.
            const bool vrAvailable = (state.source == SourceKind::TestImage);
            const bool lfpAvailable = (state.source == SourceKind::TestImage);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##format", kCat[curCat], ImGuiComboFlags_HeightLargest))
            {
                for (int c = 0; c < C_N; ++c)
                {
                    if (!vrAvailable && (c == C_VR180 || c == C_VR360)) continue;
                    if (!lfpAvailable && c == C_LFP) continue;
                    if (ImGui::Selectable(kCat[c], c == curCat))
                    {
                        switch (c) {
                        case C_SBS:   postFmt(StereoFormat::HalfSBS); break;          // default Half
                        case C_TAB:   postFmt(StereoFormat::HalfTAB); break;          // default Half
                        case C_IL:    postFmt(StereoFormat::RowInterleaved); break;   // default Row (most common)
                        case C_CHECK: postFmt(StereoFormat::Checkerboard); break;
                        case C_ANA:   post(ID_TRAY_ANA_MODE_BASE + 0); break;
                        case C_FSEQ:  postFmt(StereoFormat::FrameSequential); break;
                        case C_PULF:  postFmt(StereoFormat::Pulfrich); break;
                        case C_FP:    postFmt(StereoFormat::FramePacking); break;
                        case C_QUILT: postFmt(StereoFormat::Quilt); break;
                        case C_VR180: postFmt(StereoFormat::VR180TAB); break;   // default TAB (YouTube convention)
                        case C_VR360: postFmt(StereoFormat::VR360TAB); break;
                        case C_LFP:   postFmt(StereoFormat::LightField); break;
                        }
                    }
                }
                ImGui::EndCombo();
            }

            // Variant sub-combo for the categories that have two layouts.
            auto variant = [&](const char* id, const char* a, const char* b,
                               StereoFormat fa, StereoFormat fb) {
                const int v = (state.format == fa) ? 0 : 1;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo(id, v == 0 ? a : b))
                {
                    if (ImGui::Selectable(a, v == 0)) postFmt(fa);
                    if (ImGui::Selectable(b, v == 1)) postFmt(fb);
                    ImGui::EndCombo();
                }
            };
            if      (curCat == C_SBS) variant("##sbsv", "Full", "Half", StereoFormat::FullSBS, StereoFormat::HalfSBS);
            else if (curCat == C_TAB) variant("##tabv", "Full", "Half", StereoFormat::FullTAB, StereoFormat::HalfTAB);
            else if (curCat == C_IL)  variant("##ilv",  "Row",  "Column", StereoFormat::RowInterleaved, StereoFormat::ColumnInterleaved);
            else if (curCat == C_VR180) variant("##vr180v", "Top-and-Bottom", "Side-by-Side", StereoFormat::VR180TAB, StereoFormat::VR180SBS);
            else if (curCat == C_VR360) variant("##vr360v", "Top-and-Bottom", "Side-by-Side", StereoFormat::VR360TAB, StereoFormat::VR360SBS);
            else if (curCat == C_ANA)
            {
                int modeN = 0; const AnaglyphModeEntry* modes = AnaglyphModeList(modeN);
                int comboN = 0; const char* const* combos = AnaglyphComboList(comboN);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##anacolours", combos[state.anaglyphCombo < comboN ? state.anaglyphCombo : 0]))
                {
                    for (int i = 0; i < comboN; ++i)
                        if (ImGui::Selectable(combos[i], state.anaglyphCombo == i))
                            post(ID_TRAY_ANA_COMBO_BASE + (UINT)i);
                    ImGui::EndCombo();
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##anarecovery", curMode))
                {
                    for (int i = 0; i < modeN; ++i)
                        if (ImGui::Selectable(modes[i].label, modes[i].value == state.anaglyphMode))
                            post(ID_TRAY_ANA_MODE_BASE + (UINT)i);
                    ImGui::EndCombo();
                }
            }
            else if (curCat == C_PULF)
            {
                const char* mlbl[2] = { "Time delay", "ND filter" };
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##pulfmode", mlbl[state.pulfrichMode ? 1 : 0]))
                {
                    if (ImGui::Selectable(mlbl[0], state.pulfrichMode == 0)) post(ID_TRAY_PULF_MODE_BASE + 0);
                    if (ImGui::Selectable(mlbl[1], state.pulfrichMode == 1)) post(ID_TRAY_PULF_MODE_BASE + 1);
                    ImGui::EndCombo();
                }
                if (state.pulfrichMode == 0)   // time delay → frames
                {
                    char cur[32]; snprintf(cur, sizeof(cur), "Delay: %d frame%s", state.pulfrichDelay, state.pulfrichDelay == 1 ? "" : "s");
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##pulfdelay", cur))
                    {
                        for (int d = 1; d <= 4; ++d)
                        {
                            char b[32]; snprintf(b, sizeof(b), "Delay: %d frame%s", d, d == 1 ? "" : "s");
                            if (ImGui::Selectable(b, state.pulfrichDelay == d)) post(ID_TRAY_PULF_DELAY_BASE + (UINT)(d - 1));
                        }
                        ImGui::EndCombo();
                    }
                }
                else   // ND filter → strength
                {
                    int ndN = 0; const NdLevel* nd = PulfrichNdLevels(ndN);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##pulfnd", nd[state.pulfrichNd < ndN ? state.pulfrichNd : 0].label))
                    {
                        for (int i = 0; i < ndN; ++i)
                            if (ImGui::Selectable(nd[i].label, state.pulfrichNd == i)) post(ID_TRAY_PULF_ND_BASE + (UINT)i);
                        ImGui::EndCombo();
                    }
                }
            }
            // Frame Packing has no second dropdown -- the 720p and 1080p HDMI 1.4 modes
            // share the same proportions, so a single preset covers both.

            else if (curCat == C_QUILT)
            {
                // Quilt cols / rows pickers (max 12 / 9 per LG docs) + an
                // image-content auto-detect for files that lack the LG "_qsCxR"
                // filename token. Cols and Rows post WM_APP_QUILT_GRID; auto
                // posts WM_APP_QUILT_AUTODETECT (handled in main's WndProc).
                const float gap  = 8.0f;
                const float colW = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
                // Both cols and rows post WM_APP_QUILT_GRID with (cols in WPARAM,
                // rows in LPARAM); we always send both current values so the main
                // window doesn't have to remember which dim is being changed.
                auto intCombo = [&](const char* id, int curVal, int maxVal,
                                    const char* unit, bool isCols, float width) {
                    char preview[24]; snprintf(preview, sizeof(preview), "%d %s", curVal, unit);
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::BeginCombo(id, preview))
                    {
                        for (int v = 1; v <= maxVal; ++v)
                        {
                            char buf[24]; snprintf(buf, sizeof(buf), "%d %s", v, unit);
                            if (ImGui::Selectable(buf, v == curVal))
                            {
                                const WPARAM wp = (WPARAM)(isCols ? v : state.quiltCols);
                                const LPARAM lp = (LPARAM)(isCols ? state.quiltRows : v);
                                PostMessageA(m_mainHwnd, WM_APP_QUILT_GRID, wp, lp);
                            }
                        }
                        ImGui::EndCombo();
                    }
                };
                intCombo("##quiltcols", state.quiltCols, 12, "cols", true,  colW);
                ImGui::SameLine(0, gap);
                intCombo("##quiltrows", state.quiltRows,  9, "rows", false, colW);
                // Auto-detect lights up only when a test image is loaded (it
                // needs pixel data to run image-content detection).
                ImGui::BeginDisabled(!state.hasTestImage);
                if (ImGui::Button("Auto-detect grid", ImVec2(-FLT_MIN, 0)))
                    PostMessageA(m_mainHwnd, WM_APP_QUILT_AUTODETECT, 0, 0);
                ImGui::EndDisabled();
            }
        }

        // VR viewer extras: only shown when a VR format is the active stereo
        // input. Headlook is a toggle; the two buttons let the user snap the
        // view direction / zoom back to defaults after dragging or scrolling.
        if (IsVRFormat(state.format))
        {
            Section("VR VIEWER");
            if (RowToggle("Head look", state.vrHeadLook))
            {
                state.vrHeadLook        = !state.vrHeadLook;
                state.vrHeadLookChanged = true;
            }
            const float gap2  = ImGui::GetStyle().ItemSpacing.x;
            const float half2 = (ImGui::GetContentRegionAvail().x - gap2) * 0.5f;
            if (ImGui::Button("Reset view", ImVec2(half2, 0))) state.vrResetView = true;
            ImGui::SameLine(0, gap2);
            const bool zoomed = state.vrZoom < 0.99f || state.vrZoom > 1.01f;
            ImGui::BeginDisabled(!zoomed);
            if (ImGui::Button("Reset zoom", ImVec2(half2, 0))) state.vrResetZoom = true;
            ImGui::EndDisabled();
            ImGui::TextDisabled("Drag to look. Scroll to zoom.");
        }

        // Light-field parallax-scale slider: shown when LightField is the
        // active format. The Lytro's aperture is physically tiny (~3.4mm
        // F01, ~7mm Illum) vs ~62mm IPD, so true 1:1 mapping gives almost
        // no usable head-tracked range. The slider lets the user pick how
        // many millimetres of head-lean drive the aperture sample to its
        // edge -- min = physical 1:1 (= aperture radius, tiny), default
        // 30mm, max 100mm for an even bigger compressed range.
        if (state.format == StereoFormat::LightField)
        {
            Section("LIGHT FIELD");
            const float minLean = (std::max)(0.5f, state.lfpApertureMm * 0.5f);
            const float startX = ImGui::GetCursorPosX();
            const float labelCol = ImGui::CalcTextSize("Parallax scale").x + ImGui::GetStyle().ItemSpacing.x * 2;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Head lean");
            ImGui::SameLine();
            ImGui::SetCursorPosX(startX + labelCol);
            ImGui::SetNextItemWidth(-FLT_MIN);
            float lean = state.lfpHeadLeanMm;
            // Logarithmic feel so the low-mm "huge parallax" end has more granularity.
            if (ImGui::SliderFloat("##lfpLean", &lean, minLean, 200.0f, "%.1f mm",
                                    ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
            {
                state.lfpHeadLeanMm      = lean;
                state.lfpHeadLeanChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(280.0f);
                ImGui::TextUnformatted(
                    "Head-lean distance that moves the aperture sample from "
                    "centre to the edge. Smaller = more sensitive (huge "
                    "parallax for tiny head movements). Lytro's physical "
                    "baseline is around the slider's minimum -- 1:1 with "
                    "your real head movement, but microscopic in range.");
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }

        // Swap Eyes + Convergence used to live under their own ADJUST heading;
        // for v2.0 they roll up into the surrounding STEREO 3D INPUT (or
        // VR VIEWER / LIGHT FIELD when those conditional sections render),
        // since they're adjustments to the input rather than a category of
        // their own. Saves a heading + a few pixels in the panel.
        if (RowToggle("Swap Eyes", state.swapEyes)) post(ID_TRAY_SWAP_EYES);
        // Two depth controls: convergence (zero-plane shift) + separation (depth scale).
        auto adjSlider = [&](const char* label, const char* id, float* v) -> bool {
            const float startX  = ImGui::GetCursorPosX();
            const float labelCol = ImGui::CalcTextSize("Convergence").x + ImGui::GetStyle().ItemSpacing.x * 2;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            ImGui::SetCursorPosX(startX + labelCol);   // line the sliders up in one column
            const float resetW = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - resetW - ImGui::GetStyle().ItemSpacing.x);
            bool ch = ImGui::SliderFloat(id, v, -1.0f, 1.0f, "%.2f");
            ImGui::SameLine();
            ImGui::PushID(id);
            if (ImGui::Button("Reset", ImVec2(resetW, 0))) { *v = 0.0f; ch = true; }
            ImGui::PopID();
            return ch;
        };
        if (adjSlider("Convergence", "##conv", &state.convergence)) convChanged = true;

        // HEADTRACKING section -- non-collapsible, sits right after ADJUST.
        // OpenTrack UDP toggle + mode buttons + Calibrate + Launch OpenTrack.
        // Defaults ON whenever an SR session is active; user toggle overrides
        // for the rest of that session (cleared on session end).
        Section("HEADTRACKING");
        {
            // Protocol picker: checkbox-combo over OpenTrack UDP +
            // FreeTrack 2.0 Enhanced. Either, both, or none can be active;
            // the bridge fans the same filtered pose to whichever are on.
            // Preview text reflects the active set so a glance reads what's
            // running without opening the menu.
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap   = ImGui::GetStyle().ItemSpacing.x;
            {
                // Build preview text from the active set. TrackIR included
                // only when both selected AND available; if the user has it
                // ticked but OpenTrack is no longer detected, we treat it
                // as off for preview purposes.
                const bool tirActive = state.trackIREnabled && state.trackIRAvailable;
                std::string previewBuf;
                auto add = [&](const char* s) {
                    if (!previewBuf.empty()) previewBuf += " + ";
                    previewBuf += s;
                };
                if (state.openTrackEnabled) add("OpenTrack UDP");
                if (state.freeTrackEnabled) add("FreeTrack 2.0");
                if (tirActive)              add("TrackIR");
                if (previewBuf.empty())     previewBuf = "Off";

                ImGui::SetNextItemWidth(avail);
                if (ImGui::BeginCombo("##otprotos", previewBuf.c_str()))
                {
                    if (ImGui::Checkbox("OpenTrack UDP", &state.openTrackEnabled))
                        state.openTrackChanged = true;
                    if (ImGui::Checkbox("FreeTrack 2.0", &state.freeTrackEnabled))
                        state.openTrackChanged = true;
                    // TrackIR row. The NPClient DLL is now embedded in
                    // SRLoom.exe and extracted at first launch, so this
                    // checkbox is always live -- no install hint needed.
                    if (ImGui::Checkbox("TrackIR", &state.trackIREnabled))
                        state.openTrackChanged = true;
                    ImGui::EndCombo();
                }
            }
            // Axis mask dropdown. The pipeline always tracks 6DOF; this
            // just gates which axes go out on the wire so games that
            // misbehave on (e.g.) translation can be restricted to rotation.
            // Two labels per mode: a terse "axes" preview (shown when the
            // combo is closed) and a longer explainer (shown only in the
            // open list) carried over from the original SR OpenTrack Bridge.
            // Two-tone labels: terse axis list + dim parenthetical. The
            // parenthetical only shows in the open list; the closed combo
            // preview stays short. Strings split so we can render them in
            // different colours below.
            static const struct { int mode; const char* preview; const char* axes; const char* note; } kModes[] = {
                { 1, "X, Y, Z, Yaw, Pitch",       "X, Y, Z, Yaw, Pitch",       "(Head Position + Head Rotation aka 6DOF)" },
                { 2, "X, Y, Z",                   "X, Y, Z",                   "(Head Position)" },
                { 3, "Yaw, Pitch",                "Yaw, Pitch",                "(Head Rotation aka 3DOF)" },
                { 4, "X, Y, Z, Yaw, Pitch, Roll", "X, Y, Z, Yaw, Pitch, Roll", "(Roll Unnecessary, Not Recommended)" },
                { 5, "Yaw, Pitch, Roll",          "Yaw, Pitch, Roll",          "(Roll Unnecessary, Not Recommended)" },
            };
            // Any protocol on -> section is "live". When all are off, grey
            // out the mode / invert / calibrate controls (they're config for
            // a pipeline that isn't running). TrackIR only counts if the
            // OpenTrack install is detected -- ticked-but-unavailable is
            // effectively off.
            const bool  ena   = state.openTrackEnabled
                              || state.freeTrackEnabled
                              || (state.trackIREnabled && state.trackIRAvailable);
            if (!ena) ImGui::BeginDisabled();
            int curIdx = 0;
            for (int i = 0; i < (int)IM_ARRAYSIZE(kModes); ++i)
                if (kModes[i].mode == state.openTrackMode) { curIdx = i; break; }
            ImGui::SetNextItemWidth(avail);
            if (ImGui::BeginCombo("##otmode", kModes[curIdx].preview))
            {
                const ImU32 colNorm = ImGui::GetColorU32(ImGuiCol_Text);
                const ImU32 colDim  = ImGui::GetColorU32(g_dim);
                const float gapBetween = ImGui::CalcTextSize(" ").x;
                for (int i = 0; i < (int)IM_ARRAYSIZE(kModes); ++i)
                {
                    const bool sel = (i == curIdx);
                    // Selectable owns the click + hover highlight. The
                    // hidden label keeps each item unique; we overlay the
                    // visible two-tone text after via the drawlist so the
                    // parenthetical can be dimmed without dimming the axes.
                    ImGui::PushID(i);
                    const ImVec2 rowTL = ImGui::GetCursorScreenPos();
                    if (ImGui::Selectable("##sel", sel))
                    {
                        state.openTrackMode    = kModes[i].mode;
                        state.openTrackChanged = true;
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float rowH = ImGui::GetItemRectSize().y;
                    const float th   = ImGui::GetTextLineHeight();
                    const float ty   = rowTL.y + (rowH - th) * 0.5f;
                    const float xPad = ImGui::GetStyle().ItemSpacing.x * 0.5f;
                    dl->AddText(ImVec2(rowTL.x + xPad, ty), colNorm, kModes[i].axes);
                    const float axesW = ImGui::CalcTextSize(kModes[i].axes).x;
                    dl->AddText(ImVec2(rowTL.x + xPad + axesW + gapBetween, ty),
                                colDim, kModes[i].note);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            // Invert axes -- checkbox-combo. Preview text lists the axes
            // currently inverted (or "None"), so a glance at the closed
            // combo tells the user what's flipped. Open it to flip
            // individual axes without it closing on click.
            struct InvAxis { const char* label; bool* flag; };
            const InvAxis kAxes[] = {
                { "X",     &state.openTrackInvertX     },
                { "Y",     &state.openTrackInvertY     },
                { "Z",     &state.openTrackInvertZ     },
                { "Yaw",   &state.openTrackInvertYaw   },
                { "Pitch", &state.openTrackInvertPitch },
                { "Roll",  &state.openTrackInvertRoll  },
            };
            std::string preview = "Invert: ";
            bool anyInv = false;
            for (const auto& a : kAxes)
            {
                if (*a.flag)
                {
                    if (anyInv) preview += ", ";
                    preview += a.label;
                    anyInv = true;
                }
            }
            if (!anyInv) preview += "None";
            ImGui::SetNextItemWidth(avail);
            if (ImGui::BeginCombo("##otinvert", preview.c_str()))
            {
                for (const auto& a : kAxes)
                {
                    // Keep the combo open across clicks so the user can
                    // toggle several axes in one session.
                    if (ImGui::Checkbox(a.label, a.flag))
                        state.openTrackChanged = true;
                }
                ImGui::EndCombo();
            }

            // Calibrate + Launch OpenTrack on a third row, half-and-half.
            const float halfBw = (avail - gap) * 0.5f;
            if (ImGui::Button("Calibrate", ImVec2(halfBw, 0)))
                state.openTrackCalibrate = true;
            if (!ena) ImGui::EndDisabled();
            ImGui::SameLine(0, gap);
            const bool hasExe = state.openTrackExePath[0] != '\0';
            // U+2197 NORTH EAST ARROW = the standard "external launch"
            // glyph used across the OS. Custom render: empty Button takes
            // the click; we draw "Launch OpenTrack" + arrow ourselves so
            // there's no inter-glyph gap and the arrow can be nudged up to
            // sit on the cap line (Inter's U+2197 baseline-aligns low). If
            // OpenTrack is installed we launch it; otherwise we open the
            // project page so the user can download it.
            const ImVec2 btnTL = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::Button("##launchot", ImVec2(halfBw, 0));
            if (clicked)
            {
                if (hasExe)
                    ShellExecuteA(nullptr, "open", state.openTrackExePath,
                                  nullptr, nullptr, SW_SHOWNORMAL);
                else
                    ShellExecuteA(nullptr, "open",
                                  "https://github.com/opentrack/opentrack",
                                  nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(hasExe ? "Launch OpenTrack"
                                         : "OpenTrack not installed -- open the project page");
            }
            {
                const char* lbl = "Launch OpenTrack";
                const char* arr = "\xE2\x86\x97";
                const ImVec2 lblSz = ImGui::CalcTextSize(lbl);
                const ImVec2 arrSz = ImGui::CalcTextSize(arr);
                const float totalW = lblSz.x + arrSz.x;
                const float btnH = ImGui::GetItemRectSize().y;
                const float baseY = btnTL.y + (btnH - lblSz.y) * 0.5f;
                const float startX = btnTL.x + (halfBw - totalW) * 0.5f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                dl->AddText(ImVec2(startX, baseY), col, lbl);
                // Nudge the arrow up so its visual centre matches the cap
                // line of the surrounding text. ~10% of line height is the
                // typical ascender offset for arrows in body fonts.
                const float arrYAdj = -0.10f * lblSz.y;
                dl->AddText(ImVec2(startX + lblSz.x, baseY + arrYAdj), col, arr);
            }
        }

        // Acer SpatialLabs: surface Acer's auto-detect registry toggles when an
        // Acer display is present. Those auto-detects tend to fight SR Loom (auto-
        // switching 2D/3D by focus, or auto-engaging on fullscreen apps), so giving
        // the user a one-click off here removes a common class of flicker reports.
        // Writes go to HKLM and need admin; if denied, offer a Restart-as-admin link.
        if (AcerSpatialLabs::Available())
        {
            // Subtle header: dim "ACER SPATIALLABS" label + a small chevron right
            // after the text (no boxed background, no full-width hit-rect).
            ImGui::Dummy(ImVec2(0, 5));
            {
                const char* hdr   = "ACER SPATIALLABS";
                const ImVec2 ts   = ImGui::CalcTextSize(hdr);
                const float  h    = ImGui::GetFrameHeight();
                const float  s    = 4.0f * m_dpiScale;     // chevron half-size
                const float  gap  = 7.0f * m_dpiScale;     // text→chevron gap
                const float  bw   = ts.x + gap + s * 2;    // hit rect width: text + arrow
                const ImVec2 p    = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##acerhdr", ImVec2(bw, h));
                if (ImGui::IsItemClicked()) m_acerSectionOpen = !m_acerSectionOpen;
                const bool hov = ImGui::IsItemHovered();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 col = ImGui::GetColorU32(hov ? g_text : g_dim);
                dl->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), col, hdr);
                const float cx = p.x + ts.x + gap + s;
                const float cy = p.y + h * 0.5f;
                if (m_acerSectionOpen)   // up = collapse
                    dl->AddTriangleFilled(ImVec2(cx - s, cy + s * 0.55f),
                                          ImVec2(cx + s, cy + s * 0.55f),
                                          ImVec2(cx,     cy - s * 0.55f), col);
                else                     // down = expand
                    dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.55f),
                                          ImVec2(cx + s, cy - s * 0.55f),
                                          ImVec2(cx,     cy + s * 0.55f), col);
            }

            // Label + right-aligned toggle inside a given column width. Used for
            // the side-by-side two-toggles-per-row layout in both Acer and Startup.
            auto pairToggle = [&](const char* label, const char* tip,
                                  bool on, float colW) -> bool {
                const float startX = ImGui::GetCursorPosX();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                if (tip && ImGui::IsItemHovered())
                {
                    const float maxW = 280.0f * m_dpiScale;
                    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(maxW, FLT_MAX));
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(maxW - 14.0f * m_dpiScale);
                    ImGui::TextUnformatted(tip);
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
                const float togW = ImGui::GetFrameHeight() * 1.8f;
                ImGui::SetCursorPosX(startX + colW - togW);
                return ToggleSwitch(label, on);
            };

            if (m_acerSectionOpen)
            {
                const auto sls = AcerSpatialLabs::Read();
                auto slWrite = [&](AcerSpatialLabs::Setting s, bool v) {
                    auto r = AcerSpatialLabs::Write(s, v);
                    if (r == AcerSpatialLabs::WriteResult::NeedsAdmin)
                        m_acerNeedsAdmin = true;
                };
                const float halfW = (ImGui::GetContentRegionAvail().x
                                     - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                // "Allow Looking Away": visual ON = looking away is allowed
                // = Acer's Focus_Detection is OFF (detection=0). Inverts
                // from the underlying registry value's natural sense.
                if (pairToggle("Allow Looking Away",
                               "Acer turns weaving off when you look away from the screen. "
                               "Turn this on to keep weaving on when you look away.",
                               sls.focusDetection != 1, halfW))
                    slWrite(AcerSpatialLabs::Setting::Focus, sls.focusDetection != 1);
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
                if (pairToggle("Camera Notification",
                               "Acer have Camera LEDs, and onscreen 'Camera On' "
                               "notifications. This disables the on screen notification.",
                               sls.cameraPopup == 1, halfW))
                    slWrite(AcerSpatialLabs::Setting::CameraPopup, sls.cameraPopup != 1);

                if (m_acerNeedsAdmin)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, g_dim);
                    ImGui::TextWrapped("Changing these writes to HKLM and needs admin.");
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, g_accent);
                    if (ImGui::Selectable("Restart SR Loom as administrator", false, 0,
                                          ImGui::CalcTextSize("Restart SR Loom as administrator")))
                    {
                        char exe[MAX_PATH] = {};
                        GetModuleFileNameA(nullptr, exe, (DWORD)sizeof(exe));
                        SHELLEXECUTEINFOA sei{}; sei.cbSize = sizeof(sei);
                        sei.lpVerb = "runas"; sei.lpFile = exe; sei.nShow = SW_NORMAL;
                        if (ShellExecuteExA(&sei)) PostQuitMessage(0);
                    }
                    ImGui::PopStyleColor();
                }
            }
        }

        // STARTUP section -- collapsible chevron header (same pattern as Acer),
        // with the two preferences in a single two-column row. Skip the top
        // breathing-room dummy when the previous section is just a collapsed
        // header above us, otherwise the two header rows feel too far apart.
        const bool acerCollapsedJustAbove = AcerSpatialLabs::Available() && !m_acerSectionOpen;
        ImGui::Dummy(ImVec2(0, acerCollapsedJustAbove ? 0.0f : 5.0f));
        {
            const char* hdr  = "STARTUP";
            const ImVec2 ts  = ImGui::CalcTextSize(hdr);
            const float  h   = ImGui::GetFrameHeight();
            const float  s   = 4.0f * m_dpiScale;
            const float  gap = 7.0f * m_dpiScale;
            const float  bw  = ts.x + gap + s * 2;
            const ImVec2 p   = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##startuphdr", ImVec2(bw, h));
            if (ImGui::IsItemClicked()) m_startupSectionOpen = !m_startupSectionOpen;
            const bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 col = ImGui::GetColorU32(hov ? g_text : g_dim);
            dl->AddText(ImVec2(p.x, p.y + (h - ts.y) * 0.5f), col, hdr);
            const float cx = p.x + ts.x + gap + s;
            const float cy = p.y + h * 0.5f;
            if (m_startupSectionOpen)
                dl->AddTriangleFilled(ImVec2(cx - s, cy + s * 0.55f),
                                      ImVec2(cx + s, cy + s * 0.55f),
                                      ImVec2(cx,     cy - s * 0.55f), col);
            else
                dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.55f),
                                      ImVec2(cx + s, cy - s * 0.55f),
                                      ImVec2(cx,     cy + s * 0.55f), col);
        }

        if (m_startupSectionOpen)
        {
            // Both are tiny HKCU registry settings; run-at-startup just adds/removes
            // our exe from the Run key (no elevation needed), and start-in-tray
            // defaults to on -- flipping it off opens the control panel on launch.
            auto pairToggle2 = [&](const char* label, bool on, float colW) -> bool {
                const float startX = ImGui::GetCursorPosX();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                const float togW = ImGui::GetFrameHeight() * 1.8f;
                ImGui::SetCursorPosX(startX + colW - togW);
                return ToggleSwitch(label, on);
            };
            const float halfW = (ImGui::GetContentRegionAvail().x
                                 - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (pairToggle2("Run At Startup", m_runAtStartup, halfW))
            {
                m_runAtStartup = !m_runAtStartup;
                Settings::WriteRunAtStartup(m_runAtStartup);
            }
            ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
            if (pairToggle2("Start In Tray", m_startInTray, halfW))
            {
                m_startInTray = !m_startInTray;
                Settings::WriteStartInTray(m_startInTray);
            }
        }
    }

    m_sectionsH = ImGui::GetCursorPosY() + 4.0f;   // measured content height (snug sizing)
    ImGui::EndChild();
    }   // end if (m_expanded)

    // Drag the window from ANY empty space (not over a widget). Done manually with
    // SetWindowPos — NOT the OS move loop / SendMessage, which re-enters Render() and
    // crashes. Absolute cursor tracking (GetCursorPos) avoids feedback as the window
    // moves under the pointer. Children are included so empty options space drags too.
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool overEmpty = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                               !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overEmpty && !IsZoomed(m_hwnd))
        {
            POINT c{}; GetCursorPos(&c);
            RECT wr{};  GetWindowRect(m_hwnd, &wr);
            m_dragCur0 = c; m_dragWin0 = { wr.left, wr.top };
            m_draggingBg = true;
        }
        if (m_draggingBg && io.MouseDown[ImGuiMouseButton_Left])
        {
            POINT c{}; GetCursorPos(&c);
            SetWindowPos(m_hwnd, nullptr,
                         m_dragWin0.x + (c.x - m_dragCur0.x),
                         m_dragWin0.y + (c.y - m_dragCur0.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (!io.MouseDown[ImGuiMouseButton_Left]) m_draggingBg = false;
    }

    // Height the content actually used (for auto-fitting the window below).
    const float contentH = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;

    ImGui::End();

    ImGui::Render();
    const ImVec4 bgc = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const float clear[4] = { bgc.x, bgc.y, bgc.z, 1.0f };
    m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
    m_context->ClearRenderTargetView(m_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swap->Present(0, 0);   // no vsync: don't add a second vsync wait to the weave loop

    m_lastState = state;     // so a window drag can repaint with the latest values
    FitHeightToContent((int)(contentH + 0.5f));
    return convChanged;
}

// Resize the window to fit the content: height to the content, and width to a
// snug collapsed size (just the on/off toggle) or a wider expanded size. Clamped to
// the monitor; position is left alone.
void Gui::FitHeightToContent(int clientContentH)
{
    if (clientContentH <= 0) return;
    if (IsIconic(m_hwnd) || IsZoomed(m_hwnd)) return;   // don't fight minimise/maximise
    RECT wr{}, cr{};
    GetWindowRect(m_hwnd, &wr);
    GetClientRect(m_hwnd, &cr);
    const int nonClientH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const int nonClientW = (wr.right  - wr.left) - (cr.right  - cr.left);

    int desiredH = clientContentH + nonClientH;
    int desiredW = (int)((m_expanded ? 460.0f : 210.0f) * m_dpiScale) + nonClientW;

    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfo(mon, &mi))
    {
        const int maxH = mi.rcWork.bottom - mi.rcWork.top;
        if (desiredH > maxH) desiredH = maxH;
    }
    if (abs(desiredH - (wr.bottom - wr.top)) > 2 || abs(desiredW - (wr.right - wr.left)) > 2)
        SetWindowPos(m_hwnd, nullptr, 0, 0, desiredW, desiredH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK Gui::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:                      // hide instead of destroy (tray keeps the app alive)
        if (g_gui) { g_gui->m_visible = false; ShowWindow(hwnd, SW_HIDE); }
        return 0;
    case WM_GETMINMAXINFO:              // borderless: clamp maximise to the work area
    {                                   // (else WS_POPUP covers the taskbar)
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfo(mon, &mi))
        {
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mm->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mm->ptMaxSize.x = mi.rcWork.right  - mi.rcWork.left;
            mm->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }
    // The title-bar drag runs a modal move loop that blocks the main render loop, so
    // the panel would otherwise freeze (only redrawing where you release). Repaint it
    // from a timer during the move so it follows the mouse.
    case WM_ENTERSIZEMOVE:
        SetTimer(hwnd, 1, 8, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, 1);
        return 0;
    case WM_TIMER:
        if (g_gui && wParam == 1) g_gui->Render(g_gui->m_lastState);
        return 0;
    case WM_DPICHANGED:
        // Dragged to a monitor with a different DPI: resize to the OS-suggested
        // rect and rebuild fonts/style at the new scale so the panel stays the
        // same perceptual size everywhere.
        if (g_gui)
        {
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            g_gui->m_dpiScale = HIWORD(wParam) / 96.0f;
            g_gui->m_pendingRescale = true;   // rebuild fonts/style between frames
        }
        return 0;
    case WM_APP_UPDATE_RESULT:
        // User-forced check from the About popup landed here directly (we
        // passed our own HWND to UpdateChecker::StartAsync). Stash the
        // outcome so the popup can render an inline tick / "Up To Date"
        // / "Update available" / "Check failed" line on the next frame.
        if (g_gui && wParam)
        {
            auto* info = reinterpret_cast<ReleaseInfo*>(wParam);
            switch (info->status)
            {
            case ReleaseInfo::Available:
                g_gui->m_updateCheck = Gui::UpdateCheck::Available;
                g_gui->m_updateTag   = info->tag;
                g_gui->m_updateUrl   = info->url;
                // Open the release page in the user's default browser too,
                // since that's the actionable thing they want.
                ShellExecuteA(nullptr, "open", info->url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case ReleaseInfo::UpToDate:
                g_gui->m_updateCheck = Gui::UpdateCheck::UpToDate;
                g_gui->m_updateTag   = info->tag;
                g_gui->m_updateUrl.clear();
                break;
            case ReleaseInfo::Failed:
            default:
                g_gui->m_updateCheck = Gui::UpdateCheck::Failed;
                g_gui->m_updateTag.clear();
                g_gui->m_updateUrl.clear();
                break;
            }
            delete info;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}
