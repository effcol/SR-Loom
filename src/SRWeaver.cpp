#include "SRWeaver.h"

// Allows SR::TryGetDisplayManagerInstance() (lazy-bound display manager).
#define SRDISPLAY_LAZYBINDING

#include "sr/management/srcontext.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"
#include "sr/utility/exception.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <thread>
#include <chrono>

using namespace srw;

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
        ShowError("Failed to create the DirectX 11 weaver.");
        return false;
    }

    // Finalize the SR context now that the weaver is registered.
    m_context->initialize();
    return true;
}

bool SRWeaver::SetStereoImageFromFile(ID3D11Device* device,
                                      const char* path,
                                      StereoFormat fmt,
                                      DXGI_FORMAT texFormat)
{
    if (!device || !m_weaver)
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

    // The weaver samples the left half and right half of the SBS texture, so
    // the per-eye width is half of the image width. (Half-SBS uses the same
    // sampling; correct-aspect rescaling will come with the conversion stage.)
    (void)fmt;
    SetInputView(m_viewSRV, w / 2, h, texFormat);
    return true;
}

void SRWeaver::SetInputView(ID3D11ShaderResourceView* srv, int perEyeWidth,
                            int height, DXGI_FORMAT format)
{
    if (m_weaver && srv)
        m_weaver->setInputViewTexture(srv, perEyeWidth, height, format);
}

void SRWeaver::Weave()
{
    if (m_weaver)
        m_weaver->weave();
}

void SRWeaver::ReleaseViewTexture()
{
    SAFE_RELEASE(m_viewSRV);
    SAFE_RELEASE(m_viewTex);
}

void SRWeaver::Shutdown()
{
    ReleaseViewTexture();

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
