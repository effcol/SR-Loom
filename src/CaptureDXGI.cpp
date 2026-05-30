// CaptureDXGI.cpp — DXGI Output Duplication implementation. AcquireNextFrame
// with a zero timeout (we poll, never block); on ACCESS_LOST we rebuild the
// duplication on the same monitor (happens on display mode changes, output
// switches, fullscreen transitions). The captured texture is a full-monitor
// surface; we CopySubresourceRegion the requested region into our persistent
// target so the rest of the app stays oblivious to which capture backend ran.
#include "CaptureDXGI.h"

#include <wrl/client.h>

using namespace srw;
using Microsoft::WRL::ComPtr;

namespace
{
    template<typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
}

CaptureDXGI::~CaptureDXGI() { Shutdown(); }

bool CaptureDXGI::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device  = device;
    m_context = context;
    return m_device && m_context;
}

void CaptureDXGI::Shutdown()
{
    Stop();
    m_device  = nullptr;
    m_context = nullptr;
}

void CaptureDXGI::Stop()
{
    SafeRelease(m_dup);
    ReleaseTarget();
    m_monitor = nullptr;
    m_frameW = m_frameH = 0;
    m_regX = m_regY = m_regW = m_regH = 0;
}

bool CaptureDXGI::StartMonitor(HMONITOR monitor)
{
    Stop();
    if (!m_device || !monitor) return false;

    // Walk our device's adapter outputs to find the one whose HMONITOR matches.
    // DXGI Output Duplication is per-output; we need to bind to the exact output.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    ComPtr<IDXGIOutput>  output;
    ComPtr<IDXGIOutput1> output1;
    for (UINT idx = 0; ; ++idx)
    {
        output.Reset();
        if (adapter->EnumOutputs(idx, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) continue;
        if (desc.Monitor != monitor) continue;
        if (FAILED(output.As(&output1))) continue;
        m_frameW = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        m_frameH = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        break;
    }
    if (!output1) return false;

    if (FAILED(output1->DuplicateOutput(m_device, &m_dup)))
        return false;

    m_monitor = monitor;
    // Initial target sized to the full frame; Update() will resize if a region
    // is set or the output dimensions change after a mode switch.
    if (!EnsureTarget(m_frameW, m_frameH)) { Stop(); return false; }
    m_width = m_frameW; m_height = m_frameH;
    return true;
}

bool CaptureDXGI::Update(bool& sizeChanged)
{
    sizeChanged = false;
    if (!m_dup) return false;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource>   resource;
    HRESULT hr = m_dup->AcquireNextFrame(0, &info, resource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;   // no new frame, nothing to do
    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        // The duplication was invalidated (display mode change, GPU reset, output
        // moved). Rebuild on the same monitor; the next Update() will pick up frames.
        HMONITOR mon = m_monitor;
        Stop();
        StartMonitor(mon);
        sizeChanged = true;
        return false;
    }
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> srcTex;
    if (FAILED(resource.As(&srcTex))) { m_dup->ReleaseFrame(); return false; }

    // Sync our cached frame dimensions to whatever the duplication actually delivered.
    D3D11_TEXTURE2D_DESC sdesc{};
    srcTex->GetDesc(&sdesc);
    if ((int)sdesc.Width != m_frameW || (int)sdesc.Height != m_frameH)
    {
        m_frameW = (int)sdesc.Width;
        m_frameH = (int)sdesc.Height;
    }

    // Resolve the effective sub-rect (clamped to frame).
    int rx = m_regX, ry = m_regY, rw = m_regW, rh = m_regH;
    if (rw <= 0 || rh <= 0) { rx = 0; ry = 0; rw = m_frameW; rh = m_frameH; }
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > m_frameW) rw = m_frameW - rx;
    if (ry + rh > m_frameH) rh = m_frameH - ry;
    if (rw < 1 || rh < 1) { m_dup->ReleaseFrame(); return false; }

    if (EnsureTarget(rw, rh)) sizeChanged = true;

    D3D11_BOX box{ (UINT)rx, (UINT)ry, 0, (UINT)(rx + rw), (UINT)(ry + rh), 1 };
    m_context->CopySubresourceRegion(m_tex, 0, 0, 0, 0, srcTex.Get(), 0, &box);

    m_dup->ReleaseFrame();
    return true;
}

void CaptureDXGI::SetSourceRegion(int x, int y, int w, int h)
{
    m_regX = x; m_regY = y; m_regW = w; m_regH = h;
}

bool CaptureDXGI::EnsureTarget(int width, int height)
{
    if (m_tex && width == m_width && height == m_height)
        return false;

    ReleaseTarget();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = m_texFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_tex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = m_srvFormat;   // sRGB view over the BGRA buffer
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(m_tex, &sd, &m_srv)))
    {
        ReleaseTarget();
        return false;
    }
    m_width = width; m_height = height;
    return true;
}

void CaptureDXGI::ReleaseTarget()
{
    SafeRelease(m_srv);
    SafeRelease(m_tex);
    m_width = m_height = 0;
}
