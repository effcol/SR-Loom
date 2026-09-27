#include "Capture.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>   // (frame dirty regions)
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dxgi.h>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dxgi.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

using namespace srw;

// Classic-COM bridge interface for getting the DXGI resource behind a WinRT
// IDirect3DSurface/Device. Declared inline (per Microsoft's WGC samples) since
// the interop header doesn't expose it in a usable namespace.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) __declspec(novtable)
IDirect3DDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const& id, void** object) = 0;
};

namespace
{
    // Pull the underlying DXGI/D3D11 interface out of a WinRT surface/device.
    template <typename T>
    com_ptr<T> GetDXGIInterface(winrt::Windows::Foundation::IInspectable const& obj)
    {
        auto access = obj.as<IDirect3DDxgiInterfaceAccess>();
        com_ptr<T> result;
        check_hresult(access->GetInterface(guid_of<T>(), result.put_void()));
        return result;
    }

    IDirect3DDevice CreateWinRTDevice(ID3D11Device* d3dDevice)
    {
        com_ptr<IDXGIDevice> dxgiDevice;
        check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
        com_ptr<::IInspectable> inspectable;
        check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        return inspectable.as<IDirect3DDevice>();
    }
}

// Frame-pool depth: 4 (as Magpie) so the compositor never waits for a buffer while we
// hold one (Magpie uses 4; 2 can stall capture delivery).
static constexpr int kPoolBuffers = 4;

struct Capture::Impl
{
    IDirect3DDevice              device{ nullptr };
    GraphicsCaptureItem          item{ nullptr };
    Direct3D11CaptureFramePool   framePool{ nullptr };
    GraphicsCaptureSession       session{ nullptr };
    winrt::Windows::Graphics::SizeInt32 lastSize{ 0, 0 };
    winrt::event_token           frameArrivedToken{};
    HANDLE                       frameEvent = nullptr;   // auto-reset, set on FrameArrived
    // Looking Glass: the latest frame, held open as the source for re-crops
    // until the next one arrives (see Update).
    Direct3D11CaptureFrame       held{ nullptr };
    com_ptr<ID3D11Texture2D>     heldTex;
    // Windows 11 24H2+: each frame says which parts of the screen changed.
    bool                         dirtyOk = false;
};

Capture::Capture() = default;

Capture::~Capture()
{
    Shutdown();
}

bool Capture::IsSupported()
{
    try { return GraphicsCaptureSession::IsSupported(); }
    catch (...) { return false; }
}

bool Capture::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    // Initialize the WinRT apartment once on this (the main) thread. If COM was
    // already initialized in a different mode, that's fine — WGC still works.
    static int once = []
    {
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
        catch (...) {}
        return 0;
    }();
    (void)once;

    const bool supported = IsSupported();
    Log("Capture::Initialize IsSupported=%d", supported ? 1 : 0);
    if (!supported)
        return false;

    m_device  = device;
    m_context = context;
    m_impl    = std::make_unique<Impl>();

    try
    {
        m_impl->device = CreateWinRTDevice(device);
    }
    catch (hresult_error const& e)
    {
        Log("Capture::Initialize CreateWinRTDevice failed: 0x%08X %ls",
            (unsigned)e.code(), e.message().c_str());
        m_impl.reset();
        return false;
    }
    Log("Capture::Initialize OK");
    return true;
}

bool Capture::StartWindow(HWND window)
{
    if (!m_impl) return false;
    Stop();
    try
    {
        auto interop = get_activation_factory<GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForWindow(
            window, guid_of<GraphicsCaptureItem>(), put_abi(m_impl->item)));
        bool ok = StartCaptureInternalActive();
        Log("Capture::StartWindow ok=%d size=%dx%d", ok ? 1 : 0,
            m_impl->lastSize.Width, m_impl->lastSize.Height);
        return ok;
    }
    catch (hresult_error const& e)
    {
        Log("Capture::StartWindow failed: 0x%08X %ls", (unsigned)e.code(), e.message().c_str());
        Stop();
        return false;
    }
}

bool Capture::StartMonitor(HMONITOR monitor)
{
    if (!m_impl) return false;
    Stop();
    try
    {
        auto interop = get_activation_factory<GraphicsCaptureItem>().as<IGraphicsCaptureItemInterop>();
        check_hresult(interop->CreateForMonitor(
            monitor, guid_of<GraphicsCaptureItem>(), put_abi(m_impl->item)));
        bool ok = StartCaptureInternalActive();
        Log("Capture::StartMonitor ok=%d size=%dx%d", ok ? 1 : 0,
            m_impl->lastSize.Width, m_impl->lastSize.Height);
        return ok;
    }
    catch (hresult_error const& e)
    {
        Log("Capture::StartMonitor failed: 0x%08X %ls", (unsigned)e.code(), e.message().c_str());
        Stop();
        return false;
    }
}

// Helper shared by StartWindow/StartMonitor: build the frame pool + session for
// the item already stored in m_impl and begin capturing.
bool Capture::StartCaptureInternalActive()
{
    auto size = m_impl->item.Size();
    m_impl->lastSize = size;
    m_impl->framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_impl->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, size);
    m_impl->session = m_impl->framePool.CreateCaptureSession(m_impl->item);
    // FrameArrived fires on a pool thread (free-threaded pool): just signal.
    if (!m_impl->frameEvent) m_impl->frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        HANDLE ev = m_impl->frameEvent;
        m_impl->frameArrivedToken = m_impl->framePool.FrameArrived([ev](auto&&, auto&&) { SetEvent(ev); });
    }
    // Composite the OS cursor only when asked (the display picker wants it so you can
    // see your pointer on the captured display; passthrough/overlay leave it off to
    // avoid a second cursor on the screen the real one is already on). Property added
    // in Windows 10 2004; guarded so older builds still work.
    try { m_impl->session.IsCursorCaptureEnabled(m_captureCursor); } catch (...) {}
    // Remove the yellow "capture in progress" rectangle WGC draws around the
    // captured monitor/window. Needs borderless access (granted per-app on Win11);
    // all guarded so it degrades gracefully on older builds.
    try { GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get(); } catch (...) {}
    try { m_impl->session.IsBorderRequired(false); } catch (...) {}
    // Lift the WGC ~60fps cap: without an explicit MinUpdateInterval, the API
    // delivers frames at roughly display-composition rate (60Hz) regardless of
    // how fast the source updates. Same fix used by Sunshine (PR #4424) and
    // Apollo (#676). 1ms is small enough to track 120/144/160/240Hz content;
    // wrapped in try/catch in case the API isn't present on older Win10 builds.
    try { m_impl->session.MinUpdateInterval(std::chrono::milliseconds(1)); } catch (...) {}
    // Which parts of the screen changed, with each frame (Windows 11 24H2+):
    // a change outside what we weave then isn't a new picture for us (see Update).
    m_impl->dirtyOk = false;
    m_contentValid = false;   // (a new session: the first frame is copied whole)
    try
    {
        m_impl->session.DirtyRegionMode(GraphicsCaptureDirtyRegionMode::ReportAndRender);
        m_impl->dirtyOk = true;
    }
    catch (...) {}
    static bool s_logged = false;
    if (!s_logged) { s_logged = true; Log("Capture: dirty regions %s", m_impl->dirtyOk ? "supported" : "not available (older Windows)"); }
    m_impl->session.StartCapture();
    m_active = true;
    return true;
}

void Capture::Stop()
{
    if (!m_impl) return;
    if (m_impl->held) { try { m_impl->held.Close(); } catch (...) {} m_impl->held = nullptr; }
    m_impl->heldTex = nullptr;
    if (m_impl->session)   { m_impl->session.Close();   m_impl->session = nullptr; }
    if (m_impl->framePool)
    {
        try { m_impl->framePool.FrameArrived(m_impl->frameArrivedToken); } catch (...) {}
        m_impl->framePool.Close();
        m_impl->framePool = nullptr;
    }
    m_impl->item = nullptr;
    m_active = false;
    m_regX = m_regY = m_regW = m_regH = 0;   // reset crop to full frame
    m_appX = m_appY = m_appW = m_appH = 0;
    SAFE_RELEASE(m_full);                    // belongs to the old session's frames
}

bool Capture::Update(bool& sizeChanged)
{
    sizeChanged = false;
    if (!m_active || !m_impl || !m_impl->framePool)
        return false;

    try
    {
        // Frames about to be taken no longer count as "new" for
        // WaitForNewFrame (reset BEFORE draining: one landing after this
        // re-signals).
        if (m_impl->frameEvent) ResetEvent(m_impl->frameEvent);
        auto frame = m_impl->framePool.TryGetNextFrame();
        if (!frame)
            return RecropIfRegionChanged(sizeChanged);

        // Delivery statistics: every frame Windows hands us (even ones skipped
        // below), timed by its own capture stamp -- the SR display's real
        // capture rate, independent of how fast our loop runs.
        // What changed on screen (dirty regions, Windows 11 24H2+), over every
        // frame taken this time. Unknown -> treat all of it as changed.
        std::vector<RECT> dirty;
        bool dirtyKnown = m_impl->dirtyOk;
        auto countFrame = [this, &dirty, &dirtyKnown](const auto& fr) {
            int64_t t = 0;
            try { t = fr.SystemRelativeTime().count(); } catch (...) {}
            if (t)
            {
                if (m_statFrames == 0) m_statFirstT = t;
                m_statLastT = t;
                ++m_statFrames;
            }
            if (!dirtyKnown) return;
            try
            {
                for (const auto& d : fr.DirtyRegions())
                    dirty.push_back({ d.X, d.Y, d.X + d.Width, d.Y + d.Height });
            }
            catch (...) { dirtyKnown = false; }
        };
        countFrame(frame);
        // Drain any queued frames and weave only the newest — minimizes latency.
        for (;;)
        {
            auto next = m_impl->framePool.TryGetNextFrame();
            if (!next) break;
            countFrame(next);
            frame.Close();
            frame = next;
        }

        // When this frame was captured (QPC-based, 100 ns units).
        try { m_lastFrameTime = frame.SystemRelativeTime().count(); } catch (...) {}
        auto contentSize = frame.ContentSize();
        auto frameTex = GetDXGIInterface<ID3D11Texture2D>(frame.Surface());

        D3D11_TEXTURE2D_DESC desc{};
        frameTex->GetDesc(&desc);
        m_frameW = (int)desc.Width;
        m_frameH = (int)desc.Height;

        // Resolve the crop region (default = whole frame), clamped to the frame.
        int rx, ry, rw, rh;
        if (!ResolveRegion(rx, ry, rw, rh)) { frame.Close(); return false; }  // fully off-screen

        // m_tex holds the crop (the whole frame, or the Looking Glass's part):
        // straight from the captured frame. (The Looking Glass also HOLDS the
        // frame -- one of the pool's buffers -- until the next one arrives, to
        // re-crop from if the glass moves meanwhile; see below.)
        const bool fullFrame = (rx == 0 && ry == 0 && rw == m_frameW && rh == m_frameH);
        SAFE_RELEASE(m_full);
        sizeChanged = EnsureTarget(rw, rh);
        // With the dirty regions: only what changed inside the crop is copied,
        // and a frame that changed nothing inside it isn't new content at all
        // (a clock ticking elsewhere on screen no longer re-converts the weave).
        const bool sameCrop = m_contentValid && !sizeChanged &&
                              rx == m_appX && ry == m_appY && rw == m_appW && rh == m_appH;
        bool touched = true;
        std::vector<RECT> parts;
        if (dirtyKnown && sameCrop)
        {
            touched = false;
            const RECT crop{ rx, ry, rx + rw, ry + rh };
            for (const RECT& d : dirty)
            {
                RECT i{};
                if (IntersectRect(&i, &d, &crop)) { touched = true; parts.push_back(i); }
            }
        }
        if (m_tex && touched)
        {
            if (!parts.empty() && parts.size() <= 16)
                for (const RECT& p : parts)
                {
                    D3D11_BOX b{ (UINT)p.left, (UINT)p.top, 0, (UINT)p.right, (UINT)p.bottom, 1 };
                    m_context->CopySubresourceRegion(m_tex, 0, (UINT)(p.left - rx), (UINT)(p.top - ry), 0,
                                                     frameTex.get(), 0, &b);
                }
            else
            {
                D3D11_BOX box{ (UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1 };
                m_context->CopySubresourceRegion(m_tex, 0, 0, 0, 0, frameTex.get(), 0, &box);
            }
            m_contentValid = true;
        }
        m_appX = rx; m_appY = ry; m_appW = rw; m_appH = rh;

        if (m_impl->held) { m_impl->held.Close(); m_impl->held = nullptr; }
        m_impl->heldTex = nullptr;
        if (!fullFrame)
        {
            m_impl->held    = frame;
            m_impl->heldTex = frameTex;
        }
        else
            frame.Close();

        // Track window resizes by recreating the pool at the new content size.
        if (contentSize.Width != m_impl->lastSize.Width ||
            contentSize.Height != m_impl->lastSize.Height)
        {
            if (m_impl->held) { m_impl->held.Close(); m_impl->held = nullptr; m_impl->heldTex = nullptr; }
            m_impl->lastSize = contentSize;
            m_impl->framePool.Recreate(
                m_impl->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, kPoolBuffers, contentSize);
        }
        if (!touched) return false;   // (nothing inside what we weave changed)
        ++m_version;   // (new pixels in m_tex)
        return true;
    }
    catch (hresult_error const&)
    {
        return false;
    }
}

bool Capture::ResolveRegion(int& rx, int& ry, int& rw, int& rh) const
{
    rx = m_regX; ry = m_regY; rw = m_regW; rh = m_regH;
    if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = m_frameW; rh = m_frameH; }
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + rw > m_frameW) rw = m_frameW - rx;
    if (ry + rh > m_frameH) rh = m_frameH - ry;
    return rw > 0 && rh > 0;
}

// No new WGC frame this tick, but the requested crop may have changed (mode
// switch, loupe moved/resized). Re-crop from the last full frame right away.
// Returns true (like a new frame) when m_tex was refreshed.
bool Capture::RecropIfRegionChanged(bool& sizeChanged)
{
    if (!m_tex || m_frameW <= 0 || m_frameH <= 0) return false;
    int rx, ry, rw, rh;
    if (!ResolveRegion(rx, ry, rw, rh)) return false;
    if (rx == m_appX && ry == m_appY && rw == m_appW && rh == m_appH) return false;

    // The held capture frame (Looking Glass) is the whole frame: crop from it.
    if (m_impl && m_impl->heldTex)
    {
        sizeChanged = EnsureTarget(rw, rh);
        if (!m_tex) return false;
        D3D11_BOX hb{ (UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1 };
        m_context->CopySubresourceRegion(m_tex, 0, 0, 0, 0, m_impl->heldTex.get(), 0, &hb);
        m_appX = rx; m_appY = ry; m_appW = rw; m_appH = rh;
        ++m_version;
        m_contentValid = true;
        return true;
    }

    if (!m_full)
    {
        // Without a retained copy, m_tex is only usable as the source if
        // it currently holds the whole frame (last crop was full-frame).
        // Adopt it as the full-frame source; EnsureTarget makes a new m_tex.
        const bool texIsFull = (m_appX == 0 && m_appY == 0 &&
                                m_appW == m_frameW && m_appH == m_frameH);
        if (!texIsFull) return false;
        m_full = m_tex;
        m_tex  = nullptr;
        SAFE_RELEASE(m_srv);
        m_width = m_height = 0;
    }

    sizeChanged = EnsureTarget(rw, rh);
    if (!m_tex) return false;
    D3D11_BOX box{ (UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1 };
    m_context->CopySubresourceRegion(m_tex, 0, 0, 0, 0, m_full, 0, &box);
    m_appX = rx; m_appY = ry; m_appW = rw; m_appH = rh;
    // Back to a full-frame crop: m_tex holds everything again.
    if (rx == 0 && ry == 0 && rw == m_frameW && rh == m_frameH)
        SAFE_RELEASE(m_full);
    ++m_version;
    m_contentValid = true;
    return true;
}

bool Capture::EnsureFull(int width, int height)
{
    if (m_full)
    {
        D3D11_TEXTURE2D_DESC d{};
        m_full->GetDesc(&d);
        if ((int)d.Width == width && (int)d.Height == height) return true;
        SAFE_RELEASE(m_full);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_texFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = 0;   // copy source only
    return SUCCEEDED(m_device->CreateTexture2D(&td, nullptr, &m_full));
}

void Capture::SetSourceRegion(int x, int y, int w, int h)
{
    m_regX = x; m_regY = y; m_regW = w; m_regH = h;
}

void Capture::SetCaptureCursor(bool enabled)
{
    m_captureCursor = enabled;
    if (m_impl && m_impl->session)
        try { m_impl->session.IsCursorCaptureEnabled(enabled); } catch (...) {}
}

bool Capture::EnsureTarget(int width, int height)
{
    if (m_tex && width == m_width && height == m_height)
        return false;

    ReleaseTarget();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = m_texFormat;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_tex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format              = m_srvFormat;   // sRGB view over the BGRA buffer
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_tex, &sd, &m_srv)))
    {
        ReleaseTarget();
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

void Capture::ReleaseTarget()
{
    SAFE_RELEASE(m_srv);
    SAFE_RELEASE(m_tex);
    m_width = m_height = 0;
}

void Capture::Shutdown()
{
    Stop();
    ReleaseTarget();
    if (m_impl)
    {
        if (m_impl->frameEvent) { CloseHandle(m_impl->frameEvent); m_impl->frameEvent = nullptr; }
        m_impl->device = nullptr;
        m_impl.reset();
    }
}

bool Capture::WaitForNewFrame(DWORD timeoutMs)
{
    if (!m_active || !m_impl || !m_impl->frameEvent) return false;
    return WaitForSingleObject(m_impl->frameEvent, timeoutMs) == WAIT_OBJECT_0;
}
