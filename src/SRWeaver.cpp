#include "SRWeaver.h"

// Allows SR::TryGetDisplayManagerInstance() (lazy-bound display manager).
#define SRDISPLAY_LAZYBINDING

#include "sr/management/srcontext.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"
#include "sr/world/display/window2.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/sense/headtracker/headposetracker.h"
#include "sr/sense/headtracker/headposelistener.h"
#include "sr/sense/headtracker/headposestream.h"
#include "sr/sense/headtracker/head.h"
#include "sr/sense/core/inputstream.h"
#include "sr/utility/exception.h"
#include "sr/utility/logging.h"
#include "sr/version_c.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cmath>
#include <thread>
#include <chrono>
#include <exception>
#include <mutex>

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

SRWeaver::~SRWeaver()
{
    Shutdown();
}

bool SRWeaver::CreateContext(double maxSeconds)
{
    const auto start = std::chrono::steady_clock::now();
    while (m_context == nullptr)
    {
        try
        {
            m_context = SR::SRContext::create();
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
    if (!m_lensHint) return;
    try { m_lensHint->enable(); }
    catch (...) { Log("SwitchableLensHint::enable() threw"); }
}

void SRWeaver::LensDisable()
{
    if (!m_lensHint) return;
    try { m_lensHint->disable(); }
    catch (...) { Log("SwitchableLensHint::disable() threw"); }
}

bool SRWeaver::GetPredictedEyePositions(float lEye[3], float rEye[3])
{
    if (!m_weaver) return false;
    try
    {
        m_weaver->getPredictedEyePositions(lEye, rEye);
        return true;
    }
    catch (...) { return false; }
}


bool SRWeaver::IsLensEnabled() const
{
    if (!m_lensHint) return false;
    try { return m_lensHint->isEnabled(); }
    catch (...) { return false; }
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

void SRWeaver::InitSRLog(const char* logDir, const char* filePrefix)
{
    try
    {
        SR::Log::initializeToFile(SR::MediumVerbosity,
                                  logDir ? logDir : "",
                                  filePrefix ? filePrefix : "sr-");
    }
    catch (...) { /* Already initialized or runtime too old -- ignore */ }
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

bool SRWeaver::CreateWeaver(ID3D11DeviceContext* immediateContext, HWND window)
{
    if (!m_context)
        return false;

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
    try { m_weaver->enableLateLatching(true); }
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

    // ACT (Anti-Crosstalk): mode setter only lives on the deprecated
    // PredictingDX11Weaver class; not yet ported to IDX11Weaver1. ACT is a
    // LENS-HARDWARE property (one-mode-per-panel), not per-weaver state, so
    // we briefly spin up a deprecated weaver instance JUST to set the mode
    // to Dynamic (best optical quality per Leia user guidance), then
    // destroy it. Our modern IDX11Weaver1 keeps doing the real weaving;
    // the lens hardware retains the ACT setting.
    try
    {
        ID3D11Device* device = nullptr;
        immediateContext->GetDevice(&device);
        if (device)
        {
            #pragma warning(push)
            #pragma warning(disable: 4996)   // [[deprecated]] on PredictingDX11Weaver
            SR::PredictingDX11Weaver actHelper(
                *m_context, device, immediateContext, 1, 1, window);
            actHelper.setACTMode(::WeaverACTMode::Dynamic);
            const ::WeaverACTMode got = actHelper.getACTMode();
            Log("ACT: requested Dynamic, runtime now reports mode=%d",
                (int)got);
            #pragma warning(pop)
            device->Release();
        }
        else
            Log("ACT: couldn't fetch device from immediate context -- skipped");
    }
    catch (std::exception& e) { Log("ACT: setACTMode threw: %s", e.what()); }
    catch (...)               { Log("ACT: setACTMode threw (unknown)"); }

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
    try { m_lensHint = SR::SwitchableLensHint::create(*m_context); }
    catch (...) { m_lensHint = nullptr; }
    Log("CreateWeaver OK (weaver=%p, lensHint=%p); SR context initialized",
        (void*)m_weaver, (void*)m_lensHint);
    return true;
}

void SRWeaver::StartHeadTracker()
{
    if (m_headListener || !m_context) return;
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
    if (m_weaver && srv)
        m_weaver->setInputViewTexture(srv, perEyeWidth, height, format);
}

bool SRWeaver::StartSR(ID3D11DeviceContext* immediateContext, HWND window)
{
    if (!m_context && !CreateContext(10.0))
        return false;
    if (!m_weaver)
        return CreateWeaver(immediateContext, window);
    return true;
}

void SRWeaver::StopSR()
{
    // Releasing the weaver and context lets the SR platform power down the
    // lenticular lens and eye-tracking camera. The image texture is preserved.
    StopHeadTracker();   // before the context goes away
    // Window2 holds an SRContext reference -- release it before the context
    // is torn down so the next IsWindowPartVisible() lazily rebuilds it
    // against the fresh context.
    m_window2.reset();
    m_window2Hwnd = nullptr;
    // SwitchableLensHint is owned by the SRContext; just drop our pointer.
    m_lensHint = nullptr;
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
    if (!m_weaver)
        return;
    // The SR weaver can throw (e.g. lost SR service / tracking). Log every
    // distinct exception (one per frame max -- we re-log after 1s of silence
    // so we capture diagnostic info without flooding the log at 165Hz on a
    // persistently-broken state).
    try
    {
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
    ReleaseViewTexture();
    StopHeadTracker();
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
