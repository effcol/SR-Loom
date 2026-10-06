// KatangaSource.cpp — see header.
#include "KatangaSource.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace srw;

namespace
{
    // Katanga's IPC mapping name. The Local\ prefix scopes it to the current
    // session, which is fine: Katanga + the receiver run as the same user.
    constexpr char  kMapName[]    = "Local\\KatangaMappedFile";
    // How often to ask the OS whether the mapping exists / has a new handle.
    // Cheap call but no point checking every frame at 165 Hz.
    constexpr DWORD kPollMs       = 500;
}

KatangaSource::~KatangaSource()
{
    End();
}

bool KatangaSource::Begin(ID3D11Device* device)
{
    if (!device) return false;
    End();
    m_device       = device;
    m_lastPollTick = 0;     // force an immediate poll on first Update
    device->GetImmediateContext(&m_ctx);
    Log("KatangaSource: armed -- waiting for %s", kMapName);
    return true;
}

void KatangaSource::End()
{
    ReleaseTexture();
    if (m_ctx) { m_ctx->Release(); m_ctx = nullptr; }
    m_device      = nullptr;
    m_lastHandle  = 0;
    m_lastPollTick = 0;
}

bool KatangaSource::Update()
{
    if (!m_device) return false;

    const DWORD now = GetTickCount();
    if (now - m_lastPollTick < kPollMs && m_srv != nullptr)
        return true;   // texture is still bound and not time to re-poll yet
    m_lastPollTick = now;

    // Probe the named mapping ourselves each poll: open it, read the handle,
    // immediately close. We deliberately do NOT keep our own persistent
    // handle to the mapping -- the mapping is a kernel object, and as long
    // as ANYONE holds it open it stays alive. The Katanga DLL inside the
    // publishing game holds it open while the game runs. When the game
    // exits, the DLL unloads and the publisher's handle closes; if we don't
    // hold our own ref, the kernel destroys the mapping and our next
    // OpenFileMapping fails -- that's our definitive "publisher gone"
    // signal. (Bo3b's Katanga doesn't zero out the handle value on exit,
    // and our SRV holds the shared GPU resource alive frozen at the last
    // frame, so the only reliable way to detect the publisher dying is to
    // let the kernel decide.)
    uintptr_t latest = 0;
    {
        HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, kMapName);
        if (!map)
        {
            if (m_srv) { ReleaseTexture(); m_lastHandle = 0; }
            return false;
        }
        void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(uintptr_t));
        if (view)
        {
            latest = *reinterpret_cast<volatile uintptr_t*>(view);
            UnmapViewOfFile(view);
        }
        CloseHandle(map);
    }

    if (latest == 0)
    {
        // Katanga published a zero -> not currently rendering. Drop our texture
        // (the underlying shared resource may already be gone).
        if (m_srv) { ReleaseTexture(); m_lastHandle = 0; }
        return false;
    }

    if (latest != m_lastHandle || !m_srv)
    {
        // New handle, or first time seeing one. Re-open the shared texture.
        ReleaseTexture();
        if (!TryOpenTexture(reinterpret_cast<HANDLE>(latest)))
        {
            // Could be a transient state where Katanga published the handle but
            // the texture isn't yet visible to other processes. Don't update
            // lastHandle so the next poll retries.
            return false;
        }
        m_lastHandle = latest;
        Log("KatangaSource: receiving %dx%d (format=%d)",
            m_width, m_height, (int)m_format);
    }

    return m_srv != nullptr;
}

bool KatangaSource::TryOpenTexture(HANDLE sharedHandle)
{
    if (!m_device || !sharedHandle) return false;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&tex));
    if (FAILED(hr) || !tex)
    {
        // Will be re-attempted on the next poll if Katanga still publishes this
        // handle; logged once per failure burst to keep the log readable.
        static DWORD lastWarnTick = 0;
        if (GetTickCount() - lastWarnTick > 2000)
        {
            Log("KatangaSource: OpenSharedResource hr=0x%08X (handle=%p)",
                (unsigned)hr, sharedHandle);
            lastWarnTick = GetTickCount();
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // Match the SRV format to whatever Katanga sent. For sRGB-typed BGRA the
    // existing weaver pipeline accepts it directly via setInputViewTexture's
    // DXGI_FORMAT parameter.
    // Colour: a game's 8-bit picture is sRGB-encoded. An sRGB-typed texture is
    // decoded by the hardware as it is read; a TYPELESS one can be viewed as
    // sRGB; a plain UNORM one (the original Katanga strips the sRGB type)
    // can't be -- it's read as it is and decoded in the converter, or it came
    // out washed out. (Float formats are linear already.)
    DXGI_FORMAT viewFormat = td.Format;
    bool encoded = false;
    switch (td.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: viewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: viewFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: viewFormat = DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: encoded = true; break;
    default: break;
    }
    // The original Katanga's layout is right eye | left eye, and it's the one
    // that sends plain 8-bit UNORM (the 3D Slicer bridge: sRGB-typed, left first).
    const bool rightFirst = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM);

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format              = viewFormat;
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = m_device->CreateShaderResourceView(tex, &sd, &srv);
    if (FAILED(hr) || !srv) { tex->Release(); return false; }

    m_tex    = tex;
    m_srv    = srv;
    m_width  = (int)td.Width;
    m_height = (int)td.Height;
    m_format = viewFormat;
    m_texFormat = td.Format;
    m_encoded = encoded;
    m_rightFirst = rightFirst;
    ++m_generation;
    Log("KatangaSource: %ux%u, format %d -- %s, %s eye first", td.Width, td.Height, (int)td.Format,
        encoded ? "sRGB decoded in the converter" : "read as it is", rightFirst ? "right" : "left");
    return true;
}

bool KatangaSource::PublisherPresent()
{
    HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, kMapName);
    if (!map) return false;
    uintptr_t value = 0;
    if (void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(uintptr_t)))
    {
        value = *reinterpret_cast<volatile uintptr_t*>(view);
        UnmapViewOfFile(view);
    }
    CloseHandle(map);
    return value != 0;
}

void KatangaSource::ReleaseTexture()
{
    if (m_srv) { m_srv->Release(); m_srv = nullptr; }
    if (m_tex) { m_tex->Release(); m_tex = nullptr; }
    if (m_probeRows) { m_probeRows->Release(); m_probeRows = nullptr; }
    if (m_probeCols) { m_probeCols->Release(); m_probeCols = nullptr; }
    if (m_cropSrv) { m_cropSrv->Release(); m_cropSrv = nullptr; }
    if (m_cropTex) { m_cropTex->Release(); m_cropTex = nullptr; }
    m_cropW = m_cropH = 0; m_probePending = false; m_probeTick = 0;
    m_fill = 0.0; m_fillSame = m_fillLow = 0;
    m_width  = 0;
    m_height = 0;
    m_format = DXGI_FORMAT_UNKNOWN;
    m_encoded = m_rightFirst = false;
}

// ---- The filled part of the texture ------------------------------------------
// geo-11 in katanga_vr mode was reported (v3.1, a game set below the display's
// resolution) to show its picture in the top-left corner only, black around it:
// the texture is larger than what the game draws into it. The receiver cannot
// know the game's size, but it can see the texture: where nothing but black lies
// to the right of, and below, a part that keeps the texture's own proportions,
// that part is the picture.
void KatangaSource::PrepareFrame()
{
    if (!m_tex || !m_srv || !m_ctx || m_width <= 0 || m_height <= 0) return;
    // (8 bits a channel, and 10: a pixel is four bytes. Anything else: as it is.)
    bool ten = false;
    switch (m_texFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: ten = true; break;
    default: return;
    }
    const DWORD now = GetTickCount();
    if (!m_probePending && now - m_probeTick >= 1000)
    {
        m_probeTick = now;
        if (!m_probeRows || !m_probeCols)
        {
            D3D11_TEXTURE2D_DESC sd{};
            sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = m_texFormat; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.Width = (UINT)m_width; sd.Height = kProbe;
            if (!m_probeRows) m_device->CreateTexture2D(&sd, nullptr, &m_probeRows);
            sd.Width = kProbe; sd.Height = (UINT)m_height;
            if (!m_probeCols) m_device->CreateTexture2D(&sd, nullptr, &m_probeCols);
        }
        if (m_probeRows && m_probeCols)
        {
            for (int k = 0; k < kProbe; ++k)
            {
                // (Spread evenly over the whole texture.)
                const UINT y = (UINT)((2 * k + 1) * m_height / (2 * kProbe)), x = (UINT)((2 * k + 1) * m_width / (2 * kProbe));
                const D3D11_BOX row{ 0, y, 0, (UINT)m_width, y + 1, 1 }, col{ x, 0, 0, x + 1, (UINT)m_height, 1 };
                m_ctx->CopySubresourceRegion(m_probeRows, 0, 0, (UINT)k, 0, m_tex, 0, &row);
                m_ctx->CopySubresourceRegion(m_probeCols, 0, (UINT)k, 0, 0, m_tex, 0, &col);
            }
            m_probePending = true;
        }
    }
    else if (m_probePending)
    {
        // (Read when the GPU has it: never waited for.)
        D3D11_MAPPED_SUBRESOURCE mr{}, mc{};
        if (m_ctx->Map(m_probeRows, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr) == S_OK)
        {
            if (m_ctx->Map(m_probeCols, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mc) == S_OK)
            {
                auto lit = [ten](uint32_t p) {
                    if (ten) return (p & 0x3FF) > 8 || ((p >> 10) & 0x3FF) > 8 || ((p >> 20) & 0x3FF) > 8;
                    return (p & 0xFF) > 2 || ((p >> 8) & 0xFF) > 2 || ((p >> 16) & 0xFF) > 2;
                };
                int fw = 0, fh = 0;
                for (int k = 0; k < kProbe; ++k)
                {
                    const uint32_t* r = (const uint32_t*)((const uint8_t*)mr.pData + (size_t)k * mr.RowPitch);
                    for (int x = m_width - 1; x >= fw; --x) if (lit(r[x])) { fw = x + 1; break; }
                    for (int y = m_height - 1; y >= fh; --y)
                        if (lit(((const uint32_t*)((const uint8_t*)mc.pData + (size_t)y * mc.RowPitch))[k])) { fh = y + 1; break; }
                }
                m_ctx->Unmap(m_probeCols, 0);
                m_ctx->Unmap(m_probeRows, 0);
                m_probePending = false;
                TakeProbe(fw, fh);
            }
            else m_ctx->Unmap(m_probeRows, 0);
        }
    }
    if (m_cropTex && m_cropSrv)
    {
        const D3D11_BOX b{ 0, 0, 0, (UINT)m_cropW, (UINT)m_cropH, 1 };
        m_ctx->CopySubresourceRegion(m_cropTex, 0, 0, 0, 0, m_tex, 0, &b);
    }
}

// One probe's answer: the texture is lit as far as filledW across and filledH
// down (0: all black -- a loading screen says nothing).
void KatangaSource::TakeProbe(int filledW, int filledH)
{
    if (filledW <= 0 || filledH <= 0) return;
    // (A dark edge can only make the filled part look smaller than it is: the
    // larger of the two shares is the nearer the truth, and so is the largest
    // seen so far.)
    const double share = (std::max)((double)filledW / m_width, (double)filledH / m_height);
    const bool proportional = std::abs((double)filledW / m_width - (double)filledH / m_height) < 0.03;
    if (share > m_fill + 0.004) { m_fill = share; m_fillSame = 0; m_fillLow = 0; }
    else if (share > m_fill - 0.004 && proportional) { ++m_fillSame; m_fillLow = 0; }
    else if (share < m_fill * 0.9 && ++m_fillLow >= 10) { m_fill = share; m_fillSame = 0; m_fillLow = 0; }   // (the game drew smaller since)
    // Something lit outside the part kept: the whole texture again, at once.
    if (m_cropSrv && (filledW > m_cropW + 2 || filledH > m_cropH + 2)) SetCrop(0, 0);
    if (m_fill > 0.97) { if (m_cropSrv) SetCrop(0, 0); return; }
    // (Three probes agreeing, both ways in proportion, at least a fifth of the
    // texture: the picture. Even sizes -- the two eyes are its halves.)
    if (m_fillSame >= 3 && m_fill >= 0.2)
    {
        const int w = (std::min)(((int)(m_fill * m_width + 0.5) + 1) & ~1, m_width);
        const int h = (std::min)(((int)(m_fill * m_height + 0.5) + 1) & ~1, m_height);
        if (!m_cropSrv || std::abs(w - m_cropW) > 2 || std::abs(h - m_cropH) > 2) SetCrop(w, h);
    }
}

void KatangaSource::SetCrop(int w, int h)
{
    if (m_cropSrv) { m_cropSrv->Release(); m_cropSrv = nullptr; }
    if (m_cropTex) { m_cropTex->Release(); m_cropTex = nullptr; }
    m_cropW = m_cropH = 0;
    ++m_generation;   // (the picture handed on is another texture: bound again)
    if (w <= 0 || h <= 0) { Log("KatangaSource: the picture fills the whole %dx%d texture again", m_width, m_height); return; }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1; td.Format = m_texFormat;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = m_format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_cropTex)) || FAILED(m_device->CreateShaderResourceView(m_cropTex, &sd, &m_cropSrv)))
    {
        if (m_cropTex) { m_cropTex->Release(); m_cropTex = nullptr; }
        m_cropSrv = nullptr;
        return;
    }
    m_cropW = w; m_cropH = h;
    Log("KatangaSource: only the top-left %dx%d of the %dx%d texture is filled (black round it) -- showing that part", w, h, m_width, m_height);
}
