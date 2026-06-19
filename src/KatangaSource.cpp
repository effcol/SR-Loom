// KatangaSource.cpp — see header.
#include "KatangaSource.h"

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
    Log("KatangaSource: armed -- waiting for %s", kMapName);
    return true;
}

void KatangaSource::End()
{
    ReleaseTexture();
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
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format              = td.Format;
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = m_device->CreateShaderResourceView(tex, &sd, &srv);
    if (FAILED(hr) || !srv) { tex->Release(); return false; }

    m_tex    = tex;
    m_srv    = srv;
    m_width  = (int)td.Width;
    m_height = (int)td.Height;
    m_format = td.Format;
    return true;
}

void KatangaSource::ReleaseTexture()
{
    if (m_srv) { m_srv->Release(); m_srv = nullptr; }
    if (m_tex) { m_tex->Release(); m_tex = nullptr; }
    m_width  = 0;
    m_height = 0;
    m_format = DXGI_FORMAT_UNKNOWN;
}
