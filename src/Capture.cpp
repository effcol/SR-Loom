#include "Capture.h"

#include <winrt/Windows.Foundation.h>
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

struct Capture::Impl
{
    IDirect3DDevice              device{ nullptr };
    GraphicsCaptureItem          item{ nullptr };
    Direct3D11CaptureFramePool   framePool{ nullptr };
    GraphicsCaptureSession       session{ nullptr };
    winrt::Windows::Graphics::SizeInt32 lastSize{ 0, 0 };
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
        m_impl->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    m_impl->session = m_impl->framePool.CreateCaptureSession(m_impl->item);
    m_impl->session.StartCapture();
    m_active = true;
    return true;
}

void Capture::Stop()
{
    if (!m_impl) return;
    if (m_impl->session)   { m_impl->session.Close();   m_impl->session = nullptr; }
    if (m_impl->framePool) { m_impl->framePool.Close();  m_impl->framePool = nullptr; }
    m_impl->item = nullptr;
    m_active = false;
    m_regX = m_regY = m_regW = m_regH = 0;   // reset crop to full frame
}

bool Capture::Update(bool& sizeChanged)
{
    sizeChanged = false;
    if (!m_active || !m_impl || !m_impl->framePool)
        return false;

    try
    {
        auto frame = m_impl->framePool.TryGetNextFrame();
        if (!frame)
            return false;

        // Drain any queued frames and weave only the newest — minimizes latency.
        for (;;)
        {
            auto next = m_impl->framePool.TryGetNextFrame();
            if (!next) break;
            frame.Close();
            frame = next;
        }

        auto contentSize = frame.ContentSize();
        auto frameTex = GetDXGIInterface<ID3D11Texture2D>(frame.Surface());

        D3D11_TEXTURE2D_DESC desc{};
        frameTex->GetDesc(&desc);
        m_frameW = (int)desc.Width;
        m_frameH = (int)desc.Height;

        // Resolve the crop region (default = whole frame), clamped to the frame.
        int rx = m_regX, ry = m_regY, rw = m_regW, rh = m_regH;
        if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = m_frameW; rh = m_frameH; }
        if (rx < 0) rx = 0;
        if (ry < 0) ry = 0;
        if (rx + rw > m_frameW) rw = m_frameW - rx;
        if (ry + rh > m_frameH) rh = m_frameH - ry;
        if (rw <= 0 || rh <= 0) { frame.Close(); return false; }  // fully off-screen

        sizeChanged = EnsureTarget(rw, rh);
        D3D11_BOX box{ (UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1 };
        m_context->CopySubresourceRegion(m_tex, 0, 0, 0, 0, frameTex.get(), 0, &box);

        frame.Close();

        // Track window resizes by recreating the pool at the new content size.
        if (contentSize.Width != m_impl->lastSize.Width ||
            contentSize.Height != m_impl->lastSize.Height)
        {
            m_impl->lastSize = contentSize;
            m_impl->framePool.Recreate(
                m_impl->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
        }
        return true;
    }
    catch (hresult_error const&)
    {
        return false;
    }
}

void Capture::SetSourceRegion(int x, int y, int w, int h)
{
    m_regX = x; m_regY = y; m_regW = w; m_regH = h;
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
    if (m_impl) { m_impl->device = nullptr; m_impl.reset(); }
}
