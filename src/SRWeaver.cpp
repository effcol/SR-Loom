#include "SRWeaver.h"
#include "Settings.h"

// Allows SR::TryGetDisplayManagerInstance() (lazy-bound display manager).
#define SRDISPLAY_LAZYBINDING

#include "sr/management/srcontext.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/weaver/dx12weaver.h"
#include "sr/weaver/Weaver.h"        // Dimenco::Weaver: the lens' own figures
#include "Present12.h"
#include "sr/world/display/display.h"
#include "sr/world/display/window2.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/sense/headtracker/headposetracker.h"
#include "sr/sense/headtracker/headposelistener.h"
#include "sr/sense/headtracker/headposestream.h"
#include "sr/sense/headtracker/head.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/system/systemsense.h"
#include "sr/sense/system/systemeventlistener.h"
#include "sr/sense/system/systemeventstream.h"
#include "sr/utility/exception.h"
#include "sr/utility/logging.h"
#include "sr/version_c.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <exception>
#include <mutex>

// The weaver settings interface of the SR runtime (contrast, anti-crosstalk
// mode and strengths, on the modern weavers). Declared in the 1.34.10 SDK's
// sr/weaver/IWeaverSettings.h, which the 1.36.2 SDK does not ship: as there,
// so the runtime can be asked for it (IQueryInterface::query).
namespace SR
{
    class IWeaverSettings1 : virtual IQueryInterface
    {
    protected:
        IWeaverSettings1() = default;
        virtual ~IWeaverSettings1() = default;
    public:
        virtual void setContrast(float contrast) = 0;
        virtual float getContrast() const = 0;
        virtual void setACTMode(WeaverACTMode mode) = 0;
        virtual WeaverACTMode getACTMode() const = 0;
        virtual void setCrosstalkStaticFactor(float factor) = 0;
        virtual float getCrosstalkStaticFactor() const = 0;
        virtual void setCrosstalkDynamicFactor(float factor) = 0;
        virtual float getCrosstalkDynamicFactor() const = 0;
    };
}

using namespace srw;

// Head-pose listener -- LeiaSR pushes head poses on a background thread; we
// snapshot the latest into a mutex-guarded cache that the render loop reads
// once per frame. Same pattern as the LeiaSR DX11 example + the leia-track-app
// OpenTrack bridge.
class SRWeaver::HeadListenerImpl : public SR::HeadPoseListener
{
public:
    SR::InputStream<SR::HeadPoseStream> stream;

    void accept(const SR_headPose& f) override
    {
        std::lock_guard<std::mutex> lk(m);
        pos[0] = f.position.x;     pos[1] = f.position.y;     pos[2] = f.position.z;
        orient[0] = f.orientation.x; orient[1] = f.orientation.y; orient[2] = f.orientation.z;
        has = true;
    }

    bool get(double p[3], double o[3]) const
    {
        std::lock_guard<std::mutex> lk(m);
        if (!has) return false;
        p[0] = pos[0]; p[1] = pos[1]; p[2] = pos[2];
        o[0] = orient[0]; o[1] = orient[1]; o[2] = orient[2];
        return true;
    }

private:
    mutable std::mutex m;
    double pos[3]    = { 0.0, 0.0, 600.0 };   // sensible default: 60cm in front
    double orient[3] = { 0.0, 0.0, 0.0 };
    bool   has       = false;
};

// The runtime's system events (SR service lost / back, the display duplicated or
// at the wrong resolution, the lens on / off, a viewer found / lost): logged, so
// a report shows what the display was doing. Arrive on an SR thread.
class SRWeaver::SystemListenerImpl : public SR::SystemEventListener
{
public:
    SR::InputStream<SR::SystemEventStream> stream;
    void accept(const SR::SystemEvent& e) override
    {
        static const char* kNames[] = { "Info", "ContextInvalid", "SRUnavailable", "SRRestored", "USBNotConnected", "USBNotConnectedResolved",
            "DisplayNotConnected", "DisplayNotConnectedResolved", "Duplicated", "DuplicatedResolved", "NonNativeResolution",
            "NonNativeResolutionResolved", "DeviceConnectedAndReady", "DeviceDisconnected", "LensOn", "LensOff", "UserFound", "UserLost" };
        const uint64_t t = (uint64_t)e.eventType;
        Log("SR event: %s (%llu)%s%s", t < sizeof(kNames) / sizeof(kNames[0]) ? kNames[t] : "?", (unsigned long long)t,
            e.message.empty() ? "" : " -- ", e.message.c_str());
    }
};

SRWeaver::~SRWeaver()
{
    Shutdown();
}

bool SRWeaver::CreateContext(double maxSeconds, bool lensPreference)
{
    const auto start = std::chrono::steady_clock::now();
    while (m_context == nullptr)
    {
        try
        {
            m_context = SR::SRContext::create(lensPreference);
            break;
        }
        catch (SR::ServerNotAvailableException&)
        {
            // SR service may still be starting; wait and retry.
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > maxSeconds)
            break;
    }
    return m_context != nullptr;
}

void SRWeaver::LensEnable()
{
    // Only when it changes: each call waits on the SR runtime (~17 ms+), and
    // every source / mode pick used to repeat it -- a render-thread hitch.
    if (!m_lensHint || m_lensReq == 1) return;
    m_lensReq = 1;
    try { m_lensHint->enable(); }
    catch (...) { Log("SwitchableLensHint::enable() threw"); m_lensReq = -1; }
}

void SRWeaver::LensDisable()
{
    if (!m_lensHint || m_lensReq == 0) return;
    m_lensReq = 0;
    try { m_lensHint->disable(); }
    catch (...) { Log("SwitchableLensHint::disable() threw"); m_lensReq = -1; }
}

bool SRWeaver::GetPredictedEyePositions(float lEye[3], float rEye[3])
{
    if (m_weaver12)
    {
        try { m_weaver12->getPredictedEyePositions(lEye, rEye); return true; }
        catch (...) { return false; }
    }
    if (!m_weaver) return false;
    try
    {
        m_weaver->getPredictedEyePositions(lEye, rEye);
        return true;
    }
    catch (...) { return false; }
}


// The latency the weaver predicts the eyes ahead by: from weave() to the
// picture being seen, in microseconds (0: not known / no modern weaver).
// (Asked of the weaver once, when it is made, and remembered: not every time.)
uint64_t SRWeaver::GetLatencyUs() const { return m_latencyUs; }
void SRWeaver::ReadLatency()
{
    m_latencyUs = 0;
    try
    {
        if (m_weaver12) m_latencyUs = m_weaver12->getLatency();
        else if (m_weaver) m_latencyUs = m_weaver->getLatency();
    }
    catch (...) {}
}

// ... set outright (us), or 0: the SDK's own (a number of frames at the
// display's refresh rate).
void SRWeaver::SetLatencyUs(uint64_t us)
{
    try
    {
        if (m_weaver12) { if (us) { m_weaver12->setLatency(us); m_latencyUs = us; } }
        else if (m_weaver) { if (us) { m_weaver->setLatency(us); m_latencyUs = us; } }
    }
    catch (...) { Log("SRWeaver: setLatency threw"); }
}

// Anti-crosstalk: mode 0 the display's default, 1 off, 2 static, 3 dynamic;
// strength a percentage of the display's own amounts. Through the runtime's
// weaver settings on the standard weavers; on the legacy ones the mode is the
// weaver choice itself and only the strength applies.
void SRWeaver::ApplyAct(int mode, int strengthPct)
{
    if (!HasWeaver()) return;
    #pragma warning(push)
    #pragma warning(disable: 4996)
    try
    {
        if (!m_actDefKnown)
        {
            if (m_ws)            { m_actDefMode = (int)m_ws->getACTMode(); m_actDefStatic = m_ws->getCrosstalkStaticFactor(); m_actDefDynamic = m_ws->getCrosstalkDynamicFactor(); }
            else if (m_legacy)   { m_actDefStatic = m_legacy->getCrosstalkStaticFactor();   m_actDefDynamic = m_legacy->getCrosstalkDynamicFactor(); }
            else if (m_legacy12) { m_actDefStatic = m_legacy12->getCrosstalkStaticFactor(); m_actDefDynamic = m_legacy12->getCrosstalkDynamicFactor(); }
            else return;
            m_actDefKnown = true;
        }
        const float k = (float)strengthPct / 100.0f;
        if (m_ws)
        {
            const ::WeaverACTMode m = mode == 1 ? ::WeaverACTMode::Off : mode == 2 ? ::WeaverACTMode::Static : mode == 3 ? ::WeaverACTMode::Dynamic
                                                                                                         : (::WeaverACTMode)m_actDefMode;
            m_ws->setACTMode(m);
            m_ws->setCrosstalkStaticFactor(m_actDefStatic * k);
            m_ws->setCrosstalkDynamicFactor(m_actDefDynamic * k);
            Log("Anti-crosstalk: mode %d, static %.4f, dynamic %.4f (the display's own: mode %d, %.4f, %.4f)", (int)m_ws->getACTMode(),
                m_ws->getCrosstalkStaticFactor(), m_ws->getCrosstalkDynamicFactor(), m_actDefMode, m_actDefStatic, m_actDefDynamic);
        }
        else if (m_legacy)   { m_legacy->setCrosstalkStaticFactor(m_actDefStatic * k);   m_legacy->setCrosstalkDynamicFactor(m_actDefDynamic * k); }
        else if (m_legacy12) { m_legacy12->setCrosstalkStaticFactor(m_actDefStatic * k); m_legacy12->setCrosstalkDynamicFactor(m_actDefDynamic * k); }
    }
    catch (...) { Log("Anti-crosstalk: the weaver refused the setting"); }
    #pragma warning(pop)
}

bool SRWeaver::IsLensEnabled() const
{
    if (!m_lensHint) return false;
    try { return m_lensHint->isEnabled(); }
    catch (...) { return false; }
}

bool SRWeaver::SetLateLatching(bool on)
{
    if (m_legacy)
    {
        try { m_legacy->enableLateLatching(on); } catch (...) {}
        const bool now = m_legacy->isLateLatchingEnabled();
        Log("LateLatching enabled=%d (requested %d, legacy weaver)", (int)now, (int)on);
        return now;
    }
    if (m_weaver12)
    {
        try { m_weaver12->enableLateLatching(on); } catch (...) {}
        const bool now = m_weaver12->isLateLatchingEnabled();
        Log("LateLatching enabled=%d (requested %d, Direct3D 12 weaver)", (int)now, (int)on);
        return now;
    }
    if (!m_weaver) return false;
    try { m_weaver->enableLateLatching(on); }
    catch (...) { Log("enableLateLatching(%d) threw", (int)on); }
    const bool now = m_weaver->isLateLatchingEnabled();
    Log("LateLatching enabled=%d (requested %d)", (int)now, (int)on);
    return now;
}

bool SRWeaver::IsWindowPartVisible(HWND hwnd, int width, int height)
{
    if (!m_context || !hwnd || width <= 0 || height <= 0)
        return true;   // unknown -> safe default

    if (m_window2Hwnd != hwnd)
    {
        // HWND changed (or first call): rebuild Window2 instance.
        m_window2.reset(SR::Window2::create(*m_context, hwnd));
        m_window2Hwnd = m_window2 ? hwnd : nullptr;
    }
    if (!m_window2) return true;

    try
    {
        return m_window2->isWindowPartVisible(0, 0,
                                              (unsigned int)width,
                                              (unsigned int)height);
    }
    catch (...)
    {
        return true;   // SDK threw -> err on the side of rendering
    }
}

const char* SRWeaver::GetSRPlatformVersion()
{
    try
    {
        const char* v = ::getSRPlatformVersion();
        return v ? v : "";
    }
    catch (...) { return ""; }
}

bool SRWeaver::GetSRDisplayRect(RECT& out)
{
    if (!m_context)
        return false;

    auto fillFromLocation = [&out](SR_recti loc) -> bool
    {
        const int64_t w = loc.right - loc.left;
        const int64_t h = loc.bottom - loc.top;
        if (w <= 0 || h <= 0)
            return false;
        out.left   = (LONG)loc.left;
        out.top    = (LONG)loc.top;
        out.right  = (LONG)loc.right;
        out.bottom = (LONG)loc.bottom;
        return true;
    };

    // Preferred: the lazy-bound display manager.
    SR::IDisplayManager* dm = SR::TryGetDisplayManagerInstance(*m_context);
    if (dm != nullptr)
    {
        SR::IDisplay* display = dm->getPrimaryActiveSRDisplay();
        if (display && display->isValid())
            return fillFromLocation(display->getLocation());
        return false;
    }

    // Fallback: the deprecated Display class.
    SR::Display* display = SR::Display::create(*m_context);
    if (display != nullptr)
        return fillFromLocation(display->getLocation());

    return false;
}

bool SRWeaver::GetRecommendedViewsSize(int& w, int& h)
{
    w = h = 0;
    if (!m_context) return false;
    try
    {
        if (SR::IDisplayManager* dm = SR::TryGetDisplayManagerInstance(*m_context))
        {
            SR::IDisplay* d = dm->getPrimaryActiveSRDisplay();
            if (d && d->isValid()) { w = d->getRecommendedViewsTextureWidth(); h = d->getRecommendedViewsTextureHeight(); }
        }
        else if (SR::Display* d = SR::Display::create(*m_context))
        {
            w = d->getRecommendedViewsTextureWidth(); h = d->getRecommendedViewsTextureHeight();
        }
    }
    catch (...) { w = h = 0; }
    return w > 0 && h > 0;
}

bool SRWeaver::CreateWeaver(ID3D11DeviceContext* immediateContext, HWND window)
{
    if (!m_context)
        return false;

    m_immediate = immediateContext;
    m_choice = Settings::ReadWeaverChoice();
    if (m_choice >= 4)
    {
        if (m_p12 && (m_choice == 4 ? CreateWeaver12(window) : CreateLegacy12(window))) return FinishWeaver();
        m_choice = 0;   // (no Direct3D 12 presenter, or its weaver failed: the modern Direct3D 11 one)
    }
    if (m_choice > 0 && CreateLegacyWeaver(immediateContext, window))
        return FinishWeaver();
    m_choice = 0;   // (modern, chosen or as the fallback)

    WeaverErrorCode result = SR::CreateDX11Weaver(m_context, immediateContext, window, &m_weaver);
    if (result != WeaverErrorCode::WeaverSuccess || m_weaver == nullptr)
    {
        Log("CreateWeaver FAILED: WeaverErrorCode=%d weaver=%p", (int)result, (void*)m_weaver);
        ShowError("Failed to create the DirectX 11 weaver.");
        return false;
    }

    // Late latching: the weaver re-pulls head/eye positions for frames
    // already in flight, cutting effective tracking latency. Free for
    // tracked content (per LeiaSR docs in IWeaverBase.h:48). Wrapped in
    // try/catch in case an older runtime / global INI forces it off.
    // (A/B-tested off in an earlier build to rule out as the cause of
    // window-drag flicker -- flicker persists with it off, so it's not
    // the culprit; long-standing converter behaviour we'll chase later.)
    try { m_weaver->enableLateLatching(Settings::ReadLateLatching()); }
    catch (...) { Log("enableLateLatching threw -- continuing without it"); }
    Log("LateLatching enabled=%d", m_weaver->isLateLatchingEnabled() ? 1 : 0);

    // sRGB conversion: SR Loom uses sRGB-typed SRVs (capture is
    // BGRA8_UNORM_SRGB) and an sRGB-typed RTV on the backbuffer, so the
    // hardware handles BOTH the sample-time sRGB->linear conversion AND
    // the write-time linear->sRGB conversion. Tell the weaver shader NOT
    // to apply its own conversion -- otherwise we double-convert and the
    // weave comes out subtly wrong (typically too dark / desaturated).
    // Per IWeaverBase.h:59: "When input is already linear or set for
    // hardware conversion, set read to false."
    try { m_weaver->setShaderSRGBConversion(false, false); }
    catch (...) { Log("setShaderSRGBConversion threw -- using defaults"); }
    Log("Weaver: IDX11Weaver1");
    try
    {
        SR::IWeaverSettings1* ws = m_weaver->query<SR::IWeaverSettings1>();
        m_ws = ws;
        if (ws) Settings::WriteWeaverActDefault((int)ws->getACTMode());   // (for the panel: the display's own mode)
        if (ws) Log("Weaver settings interface: yes (anti-crosstalk mode %d, static %.3f, dynamic %.3f, contrast %.3f)", (int)ws->getACTMode(),
                    ws->getCrosstalkStaticFactor(), ws->getCrosstalkDynamicFactor(), ws->getContrast());
        else    Log("Weaver settings interface: not offered by this runtime (DX11)");
    }
    catch (...) { Log("Weaver settings interface: asking threw (DX11)"); }
    return FinishWeaver();
}

// The modern weaver's Direct3D 12 form, drawing on the presenter's command
// list into its back buffer (an sRGB view of it, as with Direct3D 11).
bool SRWeaver::CreateWeaver12(HWND window)
{
    try
    {
        const WeaverErrorCode result = SR::CreateDX12Weaver(m_context, m_p12->Device(), window, &m_weaver12);
        if (result != WeaverErrorCode::WeaverSuccess || !m_weaver12)
        {
            Log("Weaver: Direct3D 12 weaver failed (WeaverErrorCode=%d) -- using the Direct3D 11 one", (int)result);
            m_weaver12 = nullptr;
            return false;
        }
        m_weaver12->setOutputFormat(m_p12->OutputFormat());
        try { m_weaver12->enableLateLatching(Settings::ReadLateLatching()); } catch (...) {}
        try { m_weaver12->setShaderSRGBConversion(false, false); } catch (...) {}   // (_SRGB views both ends, as Direct3D 11)
        Log("Weaver: IDX12Weaver1, late latching %d", m_weaver12->isLateLatchingEnabled() ? 1 : 0);
        try
        {
            SR::IWeaverSettings1* ws = m_weaver12->query<SR::IWeaverSettings1>();
            m_ws = ws;
            if (ws) Settings::WriteWeaverActDefault((int)ws->getACTMode());
            if (ws) Log("Weaver settings interface: yes (anti-crosstalk mode %d, static %.3f, dynamic %.3f, contrast %.3f)", (int)ws->getACTMode(),
                        ws->getCrosstalkStaticFactor(), ws->getCrosstalkDynamicFactor(), ws->getContrast());
            else    Log("Weaver settings interface: not offered by this runtime (DX12)");
        }
        catch (...) { Log("Weaver settings interface: asking threw (DX12)"); }
        return true;
    }
    catch (std::exception& e) { Log("Weaver: Direct3D 12 weaver threw (%s) -- using the Direct3D 11 one", e.what()); }
    catch (...)               { Log("Weaver: Direct3D 12 weaver threw -- using the Direct3D 11 one"); }
    if (m_weaver12) { try { m_weaver12->destroy(); } catch (...) {} m_weaver12 = nullptr; }
    return false;
}

// The SDK's older Direct3D 12 weaver (PredictingDX12Weaver, deprecated): the one
// with the anti-crosstalk modes on Direct3D 12. Choice 5 static, 6 dynamic. It
// draws into the back buffer it is given each frame (a plain view of it: the
// shader encodes sRGB itself, unless the output is 16-bit float).
bool SRWeaver::CreateLegacy12(HWND window)
{
    try
    {
        #pragma warning(push)
        #pragma warning(disable: 4996)   // [[deprecated]]
        m_legacy12 = new SR::PredictingDX12Weaver(*m_context, m_p12->Device(), m_p12->SetupAllocator(), m_p12->Queue(), nullptr, nullptr, window);
        m_legacy12->setACTMode(m_choice == 5 ? ::WeaverACTMode::Static : ::WeaverACTMode::Dynamic);
        // (No conversion in its shader: the input is an sRGB view, and what it writes
        // comes out encoded -- a woven grey of 32 measured 32, as the modern weaver's;
        // with its "write" conversion on it measured 107: washed out.)
        m_legacy12->setShaderSRGBConversion(false, false);
        Log("Weaver: PredictingDX12Weaver, ACT %s (runtime reports mode %d, static %.2f, dynamic %.2f)", m_choice == 5 ? "Static" : "Dynamic",
            (int)m_legacy12->getACTMode(), m_legacy12->getCrosstalkStaticFactor(), m_legacy12->getCrosstalkDynamicFactor());
        #pragma warning(pop)
    }
    catch (std::exception& e) { Log("Weaver: the older Direct3D 12 weaver failed (%s) -- using Direct3D 11", e.what()); delete m_legacy12; m_legacy12 = nullptr; }
    catch (...)               { Log("Weaver: the older Direct3D 12 weaver failed -- using Direct3D 11"); delete m_legacy12; m_legacy12 = nullptr; }
    return m_legacy12 != nullptr;
}

// The Direct3D 12 weaver's input, whichever of the two it is.
void SRWeaver::SetInput12(ID3D12Resource* res, int perEyeWidth, int height, DXGI_FORMAT format)
{
    if (m_weaver12) m_weaver12->setInputViewTexture(res, perEyeWidth, height, format);
    else if (m_legacy12) m_legacy12->setInputFrameBuffer(res, format);
}

// The legacy weaver (PredictingDX11Weaver): deprecated in the SDK, but the only
// one with the anti-crosstalk modes. (It used to be spun up for a moment just
// to set ACT Dynamic for the modern weaver -- that is per-weaver software
// filtering, not a lens setting, so it did nothing.)
bool SRWeaver::CreateLegacyWeaver(ID3D11DeviceContext* immediateContext, HWND window)
{
    ID3D11Device* device = nullptr;
    immediateContext->GetDevice(&device);
    if (!device) return false;
    // (The side-by-side size it's made for; the input set later replaces its
    // own buffer.)
    int vw = 0, vh = 0;
    if (!GetRecommendedViewsSize(vw, vh)) { vw = 1920; vh = 1080; }
    const ::WeaverACTMode modes[4] = { ::WeaverACTMode::Off, ::WeaverACTMode::Off,
                                       ::WeaverACTMode::Static, ::WeaverACTMode::Dynamic };
    try
    {
        #pragma warning(push)
        #pragma warning(disable: 4996)   // [[deprecated]] on PredictingDX11Weaver
        m_legacy = new SR::PredictingDX11Weaver(*m_context, device, immediateContext,
                                                (unsigned)(vw * 2), (unsigned)vh, window);
        m_legacy->setACTMode(modes[m_choice]);
        try { m_legacy->enableLateLatching(Settings::ReadLateLatching()); } catch (...) {}
        m_legacy->setShaderSRGBConversion(false, false);   // (as the modern one: _SRGB views both ends)
        Log("Weaver: PredictingDX11Weaver, ACT %s (runtime reports mode %d, static %.2f, dynamic %.2f), late latching %d",
            m_choice == 1 ? "Off" : m_choice == 2 ? "Static" : "Dynamic", (int)m_legacy->getACTMode(),
            m_legacy->getCrosstalkStaticFactor(), m_legacy->getCrosstalkDynamicFactor(),
            m_legacy->isLateLatchingEnabled() ? 1 : 0);
        #pragma warning(pop)
    }
    catch (std::exception& e) { Log("Weaver: legacy failed (%s) -- using the modern one", e.what()); delete m_legacy; m_legacy = nullptr; }
    catch (...)               { Log("Weaver: legacy failed -- using the modern one"); delete m_legacy; m_legacy = nullptr; }
    device->Release();
    return m_legacy != nullptr;
}

// The lens as the runtime knows it (for the log, and for SR Loom's own
// interlacing: the light-field experiment). Guarded: the figures are read
// from the runtime's weaving library, which may not be set up.
static bool ReadLensFacts(float out[6], bool flags[2], int& notTracking)
{
    __try
    {
        out[0] = Dimenco::Weaver::GetSlant(); out[1] = Dimenco::Weaver::GetPx();
        out[2] = Dimenco::Weaver::GetN();     out[3] = Dimenco::Weaver::GetDoN();
        out[4] = Dimenco::Weaver::GetXTalkFactor(); out[5] = Dimenco::Weaver::GetPattern();
        flags[0] = Dimenco::Weaver::GetLateLatchingForceOn(); flags[1] = Dimenco::Weaver::GetLateLatchingForceOff();
        notTracking = (int)Dimenco::Weaver::GetBehaviorWhenNotTracking();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The optical distance from lens to pixels (D/N, mm) and the size of a pixel
// (mm): with the lens pitch they give how wide one lens' fan of views is at
// the viewer's distance. False: not known.
bool SRWeaver::GetLensGeometry(float& doNmm, float& dotPitchMm) const
{
    doNmm = m_lensDoNmm; dotPitchMm = m_dotPitchMm;
    return m_lensDoNmm > 0.0f && m_dotPitchMm > 0.0f;
}

// The SDK's weaving library told which device and where the SR Platform is
// installed, so that it loads that device's figures (needed when no weaver has
// been made in this process: the lens-only session).
static void ConfigureStaticWeaver(const char* serial, const char* installPath)
{
    try
    {
        if (installPath && *installPath) Dimenco::Weaver::SetInstallPath(std::string(installPath));
        if (serial && *serial) Dimenco::Weaver::SetDeviceSerialNumbers(std::vector<std::string>{ std::string(serial) });
        Dimenco::Weaver::ReconfigureWeaver();
    }
    catch (...) { Log("Lens: configuring the weaving library threw"); }
}

// Light field: where under its lens each pixel is seen from a viewing position,
// straight from the SDK's weaving library -- the same figures its own weave
// uses, for whatever SR display this is. FillAttributes gives, at a corner of
// the picture, the lens phase of the pixel there (in lens periods: red, green,
// blue) and the shift of that phase for a viewer at userPos (vars.x); the two
// added are the phase seen from there, and it runs evenly across the picture.
// From three corners: its step a pixel across and a row down, and its value at
// the middle. userPos is in CENTIMETRES (100 moves the centre by 6435 pixels of
// 0.01554 cm).
static bool LightFieldPhases(float w, float h, float xCm, float yCm, float zCm, double out[3])
{
    __try
    {
        Dimenco::Weaver::SetGlobalParameters(FLOAT3(xCm, yCm, zCm));
        double phi[3] = {};
        for (int k = 0; k < 3; ++k)
        {
            FLOAT4 ph, dxy; FLOAT2 sp, wv;
            Dimenco::Weaver::FillAttributes(FLOAT2(w, h), k == 0 ? FLOAT4(-1.0f, 1.0f, 0.0f, 1.0f) : k == 1 ? FLOAT4(1.0f, 1.0f, 0.0f, 1.0f)
                                                                                                    : FLOAT4(1.0f, -1.0f, 0.0f, 1.0f), ph, dxy, sp, wv);
            phi[k] = (double)ph.y + (double)wv.x;   // (the green sub-pixel's; top-left, top-right, bottom-right)
        }
        out[0] = (phi[1] - phi[0]) / w;                       // a pixel across
        out[1] = (phi[2] - phi[1]) / h;                       // a row down
        out[2] = phi[0] + out[0] * w * 0.5 + out[1] * h * 0.5; // at the middle
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ... as the light-field shader takes it: the lens pitch in pixels and its
// slant as seen from there, and the phase at the picture's middle (0..1).
bool SRWeaver::GetLightFieldGeometry(float w, float h, float xCm, float yCm, float zCm, float& pitchPx, float& slant, float& centrePhase)
{
    double o[3] = {};
    if (!HasWeaver() || zCm < 5.0f) return false;
    auto good = [&] { return LightFieldPhases(w, h, xCm, yCm, zCm, o) && o[0] > 1e-4 && std::isfinite(o[0]) && std::isfinite(o[1]) && std::isfinite(o[2]); };
    if (!good())
    {
        // (With no weaver made -- the lens-only session -- the library has not
        // been told which display to load: told here, once. The device is the
        // folder the SR service keeps for it; the platform, where its DLLs are.)
        static bool s_tried = false;
        if (s_tried) return false;
        s_tried = true;
        std::string serial, install;
        {
            char pd[MAX_PATH] = {};
            if (GetEnvironmentVariableA("ProgramData", pd, MAX_PATH))
            {
                WIN32_FIND_DATAA fd{};
                const HANDLE h = FindFirstFileA((std::string(pd) + "\Simulated Reality\Devices\*").c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE)
                {
                    do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') { serial = fd.cFileName; break; } } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
            }
            char mod[MAX_PATH] = {};
            if (HMODULE core = GetModuleHandleA("SimulatedRealityCore.dll"))
                if (GetModuleFileNameA(core, mod, MAX_PATH))
                {
                    install = mod;
                    for (int up = 0; up < 2; ++up) { const size_t s = install.find_last_of("\/"); if (s == std::string::npos) break; install.erase(s); }
                }
        }
        Log("Light field: telling the weaving library its display (%s, platform at %s)", serial.empty() ? "?" : "found", install.empty() ? "?" : install.c_str());
        ConfigureStaticWeaver(serial.c_str(), install.c_str());
        if (!good()) return false;
    }
    pitchPx = (float)(1.0 / o[0]);
    slant = (float)(o[1] / o[0]);
    centrePhase = (float)(o[2] - std::floor(o[2]));
    return true;
}

bool SRWeaver::GetLens(float& slant, float& pitchPx) const
{
    slant = m_lensSlant; pitchPx = m_lensPitchPx;
    return m_lensPitchPx > 0.0f;
}

bool SRWeaver::FinishWeaver()
{
    // Subscribe to head-pose updates BEFORE initialize() -- the LeiaSR docs +
    // example apps create their trackers between context creation and
    // initialize(). Failure to start the tracker is non-fatal (the weaver still
    // works, we just won't have head data for quilt view selection).
    StartHeadTracker();

    // Finalize the SR context now that the weaver + tracker are registered.
    m_context->initialize();
    // SwitchableLensHint: cooperative app-level control over the lens
    // power state, separate from SR-session lifecycle. Lets us keep SR
    // session up while temporarily backing off to plain 2D (Katanga arm).
    // Owned by the SRContext per SDK docs.
    m_lensReq = -1;
    try { m_lensHint = SR::SwitchableLensHint::create(*m_context); }
    catch (...) { m_lensHint = nullptr; }
    {
        float f[6] = {}; bool fl[2] = {}; int nt = 0;
        if (ReadLensFacts(f, fl, nt))
        {
            m_lensSlant = f[0]; m_lensPitchPx = f[1]; m_lensDoNmm = f[3];
            Log("Lens: slant %.5f, pitch %.4f px, refractive index %.3f, D/N %.3f mm | crosstalk factor %.3f, pattern %.0f | late latching forced on %d, off %d | when not tracking: %d",
                f[0], f[1], f[2], f[3], f[4], f[5], (int)fl[0], (int)fl[1], nt);
        }
        else Log("Lens: the runtime's figures could not be read");
    }
    try
    {
        if (SR::IDisplayManager* dm = SR::TryGetDisplayManagerInstance(*m_context))
        {
            SR::IDisplay* d = dm->getPrimaryActiveSRDisplay();
            if (d && d->isValid()) m_dotPitchMm = (float)d->getDotPitch() * 10.0f;   // (reported in cm)
            if (d && d->isValid())
                Log("Display: %d x %d px (physical %d x %d), %.1f x %.1f cm, dot pitch %.5f", (int)d->getResolutionWidth(), (int)d->getResolutionHeight(),
                    (int)d->getPhysicalResolutionWidth(), (int)d->getPhysicalResolutionHeight(), (double)d->getPhysicalSizeWidth(), (double)d->getPhysicalSizeHeight(), (double)d->getDotPitch());
        }
    }
    catch (...) { Log("Display: figures could not be read"); }
    Log("CreateWeaver OK (weaver=%p, lensHint=%p); SR context initialized",
        m_legacy ? (void*)m_legacy : m_weaver12 ? (void*)m_weaver12 : (void*)m_weaver, (void*)m_lensHint);
    return true;
}

void SRWeaver::StartHeadTracker()
{
    if (m_headListener || !m_context) return;
    if (!m_sysListener)
    {
        try
        {
            SR::SystemSense* sense = SR::SystemSense::create(*m_context);
            m_sysListener = new SystemListenerImpl();
            m_sysListener->stream.set(sense->openSystemEventStream(m_sysListener));
        }
        catch (...) { Log("SR events: could not be listened to"); delete m_sysListener; m_sysListener = nullptr; }
    }
    if (m_lensOnly) return;   // (no head tracker: that is the camera)
    try
    {
        m_headTracker  = SR::HeadPoseTracker::create(*m_context);
        m_headListener = new HeadListenerImpl();
        m_headListener->stream.set(m_headTracker->openHeadPoseStream(m_headListener));
    }
    catch (std::exception& e)
    {
        Log("HeadPoseTracker create FAILED: %s (quilt head-tracking will be off)", e.what());
        delete m_headListener; m_headListener = nullptr;
        m_headTracker = nullptr;
    }
}

void SRWeaver::StopHeadTracker()
{
    delete m_sysListener; m_sysListener = nullptr;
    // The InputStream destructor calls stopListening; the tracker itself is a
    // Sense registered with the SRContext and will be freed when the context
    // is. Just drop our pointers/listener.
    delete m_headListener; m_headListener = nullptr;
    m_headTracker = nullptr;
}

bool SRWeaver::GetHeadPose(double pos[3], double orient[3]) const
{
    if (!m_headListener) return false;
    return m_headListener->get(pos, orient);
}

bool SRWeaver::SetStereoImageFromPixels(ID3D11Device* device,
                                         const uint8_t* pixels,
                                         int w, int h,
                                         DXGI_FORMAT texFormat)
{
    if (!device || !pixels || w <= 0 || h <= 0) return false;

    ReleaseViewTexture();

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = pixels;
    init.SysMemPitch = (UINT)w * 4;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)w;
    td.Height           = (UINT)h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = texFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&td, &init, &m_viewTex);
    if (FAILED(hr)) { ShowError("Failed to create stereo texture."); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format               = texFormat;
    sd.ViewDimension        = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels  = 1;
    hr = device->CreateShaderResourceView(m_viewTex, &sd, &m_viewSRV);
    if (FAILED(hr)) { ShowError("Failed to create stereo shader resource view."); return false; }

    m_imgW = w;
    m_imgH = h;
    return true;
}

bool SRWeaver::SetStereoImageFromFile(ID3D11Device* device,
                                      const char* path,
                                      StereoFormat fmt,
                                      DXGI_FORMAT texFormat)
{
    if (!device) return false;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4); // force RGBA
    if (!pixels) { ShowError("Failed to load stereo test image."); return false; }

    const bool ok = SetStereoImageFromPixels(device, pixels, w, h, texFormat);
    stbi_image_free(pixels);
    (void)fmt;
    return ok;
}

void SRWeaver::SetInputView(ID3D11ShaderResourceView* srv, int perEyeWidth,
                            int height, DXGI_FORMAT format)
{
    if (!srv) return;
    // (Named by a caller while the presenter's own picture was the input: that
    // is over -- TakeOverrideDropped tells the loop.)
    if (IsDX12() && m_override12) { m_override12 = false; m_input12 = nullptr; m_overrideDropped = true; }
    if (IsDX12())
    {
        // The view's texture, handed to Direct3D 12 (shared, or copied across
        // when it isn't shareable: Present12::Share).
        ID3D11Resource* res = nullptr; ID3D11Texture2D* tex = nullptr;
        srv->GetResource(&res);
        if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        ID3D12Resource* r12 = (tex && m_p12) ? m_p12->Share(tex) : nullptr;
        if (r12 && (r12 != m_input12 || perEyeWidth != m_input12W || height != m_input12H || format != m_input12Fmt))
        {
            try { SetInput12(r12, perEyeWidth, height, format); }
            catch (...) { Log("SRWeaver: setInputViewTexture (Direct3D 12) threw"); }
            m_input12 = r12; m_input12W = perEyeWidth; m_input12H = height; m_input12Fmt = format;
        }
        SAFE_RELEASE(tex); SAFE_RELEASE(res);
        return;
    }
    if (m_legacy)
        m_legacy->setInputFrameBuffer(srv);   // (it samples the side-by-side view as it is)
    else if (m_weaver)
        m_weaver->setInputViewTexture(srv, perEyeWidth, height, format);
}

// The Direct3D 12 weaver's input given directly (the presenter's finished copy
// of the converter's picture, which changes from frame to frame), in place of
// whatever SetInputView names; null: SetInputView's again.
void SRWeaver::SetInputOverride12(ID3D12Resource* res, int perEyeWidth, int height, DXGI_FORMAT format)
{
    if (!IsDX12()) { m_override12 = false; return; }
    if (!res)
    {
        if (m_override12) { m_override12 = false; m_input12 = nullptr; }
        return;
    }
    m_override12 = true;
    if (res == m_input12 && perEyeWidth == m_input12W && height == m_input12H && format == m_input12Fmt) return;
    try { SetInput12(res, perEyeWidth, height, format); }
    catch (...) { Log("SRWeaver: setInputViewTexture (Direct3D 12) threw"); }
    m_input12 = res; m_input12W = perEyeWidth; m_input12H = height; m_input12Fmt = format;
}

bool SRWeaver::StartSR(ID3D11DeviceContext* immediateContext, HWND window)
{
    if (!m_context && !CreateContext(10.0))
        return false;
    // Lens only (the untracked light field): the SR session with the lens held
    // on and nothing that tracks -- no weaver, no head tracker -- so the
    // eye-tracking camera stays off. SR Loom draws the picture itself.
    if (m_lensOnly)
    {
        if (m_lensOnlyUp) return true;
        m_immediate = immediateContext;
        m_choice = 0;
        const bool ok = FinishWeaver();
        m_lensOnlyUp = ok;
        Log("SR session: lens only (no weaver, no tracking: the camera stays off)");
        return ok;
    }
    if (!m_weaver && !m_weaver12 && !m_legacy && !m_legacy12)
        return CreateWeaver(immediateContext, window);
    return true;
}

void SRWeaver::StopSR()
{
    // Releasing the weaver and context lets the SR platform power down the
    // lenticular lens and eye-tracking camera. The image texture is preserved.
    m_ws = nullptr; m_actDefKnown = false;
    m_lensOnlyUp = false;
    StopHeadTracker();   // before the context goes away
    // Window2 holds an SRContext reference -- release it before the context
    // is torn down so the next IsWindowPartVisible() lazily rebuilds it
    // against the fresh context.
    m_window2.reset();
    m_window2Hwnd = nullptr;
    // SwitchableLensHint is owned by the SRContext; just drop our pointer.
    m_lensHint = nullptr;
    m_lensReq = -1;
    if (m_legacy)
    {
        #pragma warning(suppress: 4996)
        delete m_legacy;
        m_legacy = nullptr;
    }
    if (m_legacy12)
    {
        if (m_p12) m_p12->WaitIdle();   // (its last weave may still be running)
        #pragma warning(suppress: 4996)
        delete m_legacy12;
        m_legacy12 = nullptr;
        m_input12 = nullptr;
        m_override12 = false;
    }
    if (m_weaver12)
    {
        if (m_p12) m_p12->WaitIdle();   // (its last weave may still be running)
        m_weaver12->destroy();
        m_weaver12 = nullptr;
        m_input12 = nullptr;
        m_override12 = false;
    }
    if (m_weaver)
    {
        m_weaver->destroy();
        m_weaver = nullptr;
    }
    if (m_context)
    {
        SR::SRContext::deleteSRContext(m_context);
        m_context = nullptr;
    }
}

void SRWeaver::Weave()
{
    if (!m_weaver && !m_legacy && !m_weaver12 && !m_legacy12)
        return;
    // The SR weaver can throw (e.g. lost SR service / tracking). Log every
    // distinct exception (one per frame max -- we re-log after 1s of silence
    // so we capture diagnostic info without flooding the log at 165Hz on a
    // persistently-broken state).
    try
    {
        if (m_legacy12)
        {
            if (m_p12 && m_p12->InFrame() && m_input12)
            {
                const D3D12_VIEWPORT vp = m_p12->Viewport();
                #pragma warning(suppress: 4996)
                m_legacy12->setOutputFrameBuffer(m_p12->BackBuffer());
                #pragma warning(suppress: 4996)
                m_legacy12->setCommandList(m_p12->List());
                #pragma warning(suppress: 4996)
                m_legacy12->weave((unsigned)vp.Width, (unsigned)vp.Height);
            }
        }
        else if (m_weaver12)
        {
            // (Onto the presenter's command list, over its whole back buffer;
            // run when the frame is sent: Present12::EndFrame.)
            if (m_p12 && m_p12->InFrame() && m_input12)
            {
                m_weaver12->setCommandList(m_p12->List());
                m_weaver12->setViewport(m_p12->Viewport());
                // (Only where the weave shows -- a window, the looking glass,
                // Auto Stereo's pictures: the rest is see-through anyway.)
                const D3D12_RECT ws = m_p12->WeaveScissor();
                if (ws.right <= ws.left || ws.bottom <= ws.top) return;
                m_weaver12->setScissorRect(ws);
                m_weaver12->weave();
            }
        }
        else if (m_legacy)
        {
            // (Into the bound back buffer, over the viewport the renderer set.)
            D3D11_VIEWPORT vp{}; UINT n = 1;
            m_immediate->RSGetViewports(&n, &vp);
            if (n && vp.Width > 0 && vp.Height > 0)
                m_legacy->weave((unsigned)vp.Width, (unsigned)vp.Height);
        }
        else
            m_weaver->weave();
    }
    catch (std::exception& e)
    {
        static DWORD lastTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastTick > 1000) { Log("SRWeaver::Weave std::exception: %s", e.what()); lastTick = now; }
    }
    catch (...)
    {
        static DWORD lastTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastTick > 1000) { Log("SRWeaver::Weave unknown exception"); lastTick = now; }
    }
}

void SRWeaver::ReleaseViewTexture()
{
    SAFE_RELEASE(m_viewSRV);
    SAFE_RELEASE(m_viewTex);
}

void SRWeaver::Shutdown()
{
    m_ws = nullptr; m_actDefKnown = false;
    m_lensOnlyUp = false;
    ReleaseViewTexture();
    StopHeadTracker();
    if (m_legacy)
    {
        #pragma warning(suppress: 4996)
        delete m_legacy;
        m_legacy = nullptr;
    }
    if (m_legacy12)
    {
        if (m_p12) m_p12->WaitIdle();   // (its last weave may still be running)
        #pragma warning(suppress: 4996)
        delete m_legacy12;
        m_legacy12 = nullptr;
        m_input12 = nullptr;
        m_override12 = false;
    }
    if (m_weaver12)
    {
        if (m_p12) m_p12->WaitIdle();   // (its last weave may still be running)
        m_weaver12->destroy();
        m_weaver12 = nullptr;
        m_input12 = nullptr;
        m_override12 = false;
    }
    if (m_weaver)
    {
        m_weaver->destroy();
        m_weaver = nullptr;
    }
    if (m_context)
    {
        SR::SRContext::deleteSRContext(m_context);
        m_context = nullptr;
    }
}
