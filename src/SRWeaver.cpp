#include "SRWeaver.h"

// Allows SR::TryGetDisplayManagerInstance() (lazy-bound display manager).
#define SRDISPLAY_LAZYBINDING

#include "sr/management/srcontext.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"
#include "sr/sense/headtracker/headposetracker.h"
#include "sr/sense/headtracker/headposelistener.h"
#include "sr/sense/headtracker/headposestream.h"
#include "sr/sense/headtracker/head.h"
#include "sr/sense/core/inputstream.h"
#include "sr/utility/exception.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

    // Subscribe to head-pose updates BEFORE initialize() -- the LeiaSR docs +
    // example apps create their trackers between context creation and
    // initialize(). Failure to start the tracker is non-fatal (the weaver still
    // works, we just won't have head data for quilt view selection).
    StartHeadTracker();

    // Finalize the SR context now that the weaver + tracker are registered.
    m_context->initialize();
    Log("CreateWeaver OK (weaver=%p); SR context initialized", (void*)m_weaver);
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

bool SRWeaver::SetStereoImageFromFile(ID3D11Device* device,
                                      const char* path,
                                      StereoFormat fmt,
                                      DXGI_FORMAT texFormat)
{
    // Loads the image into a texture; the weaver is not required here.
    if (!device)
        return false;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4); // force RGBA
    if (!pixels)
    {
        ShowError("Failed to load stereo test image.");
        return false;
    }

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
    stbi_image_free(pixels);
    if (FAILED(hr))
    {
        ShowError("Failed to create stereo texture.");
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format                    = texFormat;
    sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels       = 1;
    hr = device->CreateShaderResourceView(m_viewTex, &sd, &m_viewSRV);
    if (FAILED(hr))
    {
        ShowError("Failed to create stereo shader resource view.");
        return false;
    }

    // Store the full image size; the converter consumes this as its source.
    (void)fmt;
    m_imgW = w;
    m_imgH = h;
    return true;
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
