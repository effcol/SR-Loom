// VideoSource.cpp -- Media Foundation video decoder, see header for design.
#include "VideoSource.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

#include <string>
#include <cctype>

using namespace srw;

namespace
{
    bool g_mfStarted = false;
}

void VideoSource::Startup()
{
    if (g_mfStarted) return;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) g_mfStarted = true;
}

void VideoSource::Shutdown()
{
    if (!g_mfStarted) return;
    MFShutdown();
    g_mfStarted = false;
}

bool VideoSource::IsVideoFile(const char* path)
{
    if (!path) return false;
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    std::string ext(dot + 1);
    for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
    return ext == "mp4"  || ext == "mov"  || ext == "mkv" || ext == "webm" ||
           ext == "avi"  || ext == "m4v"  || ext == "wmv";
}

VideoSource::~VideoSource()
{
    Release();
}

bool VideoSource::Open(ID3D11Device* device, const wchar_t* path)
{
    if (!g_mfStarted) Startup();   // lazy init so callers can skip explicit Startup
    Release();
    if (!device || !path) return false;
    m_device = device;

    // Enable the Video Processor MFT inside the source reader. H264/HEVC/VP9
    // decoders output NV12 / I420; without the Video Processor MFT bridging
    // them to RGB32 the source-reader media-type negotiation fails with
    // MF_E_INVALIDMEDIATYPE (0xC00D36B4). The ADVANCED flag activates the
    // more featureful Video Processor MFT (handles HD/UHD-range YUV).
    // IMPORTANT: ENABLE_ADVANCED_VIDEO_PROCESSING and ENABLE_VIDEO_PROCESSING
    // are MUTUALLY EXCLUSIVE -- setting both makes MFCreateSourceReaderFromURL
    // return E_INVALIDARG (0x80070057). Set only the advanced one.
    IMFAttributes* readerAttrs = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&readerAttrs, 1)) && readerAttrs)
        readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(path, readerAttrs, &m_reader);
    if (readerAttrs) readerAttrs->Release();
    if (FAILED(hr) || !m_reader)
    {
        Log("VideoSource: MFCreateSourceReaderFromURL hr=0x%08X", (unsigned)hr);
        return false;
    }

    // Disable all streams, then enable just the first video stream -- avoids
    // the reader pulling audio frames we don't consume.
    m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    if (!SetupOutputFormat()) { Release(); return false; }

    // Query the negotiated output frame size and stride.
    IMFMediaType* mt = nullptr;
    if (FAILED(m_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt)) || !mt)
        { Release(); return false; }
    UINT32 w = 0, h = 0, stride = 0;
    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
    UINT32 strideU = 0;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideU)))
        stride = strideU;
    mt->Release();
    if (w == 0 || h == 0) { Release(); return false; }

    m_width  = (int)w;
    m_height = (int)h;
    // MF stride can be negative for bottom-up; we always sample as if top-down
    // and rely on row-by-row memcpy. If it's missing or zero, assume tightly
    // packed (width*4 bytes per row).
    m_stride = (stride != 0) ? (int)abs((int)stride) : m_width * 4;

    // Dynamic texture in SRGB-typed BGRA format (matches MF RGB32 byte order).
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)m_width;
    td.Height           = (UINT)m_height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &m_tex)))
        { Release(); return false; }
    if (FAILED(device->CreateShaderResourceView(m_tex, nullptr, &m_srv)))
        { Release(); return false; }

    // Total duration (in 100ns units) for the seek bar / time text. The
    // attribute is on the underlying media source, not the reader.
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(
            (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)))
    {
        m_durationHns = (LONGLONG)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
    }

    m_startTick      = GetTickCount64();
    m_lastDecodedHns = -1;
    m_paused         = false;
    m_pauseElapsedMs = 0;
    Log("VideoSource: opened %dx%d (stride %d, duration %.1fs)",
        m_width, m_height, m_stride, (double)m_durationHns / 10000000.0);
    return true;
}

bool VideoSource::SetupOutputFormat()
{
    // Use a MINIMAL output type -- just major + subtype. CopyAllItems from the
    // native type drags codec-specific attributes along that can clash with
    // RGB32 (MF_MT_SUBTYPE conflicts with leftover MF_MT_VIDEO_PROFILE etc.)
    // and yield MF_E_INVALIDMEDIATYPE. With the Video Processor MFT enabled
    // (see Open()), MF auto-negotiates frame size / rate from the source.
    auto tryRequest = [&](const GUID& subtype) -> HRESULT {
        IMFMediaType* out = nullptr;
        if (FAILED(MFCreateMediaType(&out)) || !out) return E_FAIL;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE,    subtype);
        HRESULT h = m_reader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
        out->Release();
        return h;
    };

    HRESULT hr = tryRequest(MFVideoFormat_RGB32);
    if (FAILED(hr))
    {
        Log("VideoSource: SetCurrentMediaType(RGB32)  hr=0x%08X, trying ARGB32", (unsigned)hr);
        hr = tryRequest(MFVideoFormat_ARGB32);
    }
    if (FAILED(hr))
    {
        // Log every native type the source is willing to produce -- helps
        // diagnose codec/Media Feature Pack issues.
        Log("VideoSource: SetCurrentMediaType(ARGB32) hr=0x%08X. Native types follow:", (unsigned)hr);
        for (DWORD i = 0; i < 16; ++i)
        {
            IMFMediaType* nt = nullptr;
            HRESULT gh = m_reader->GetNativeMediaType(
                (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nt);
            if (FAILED(gh) || !nt) break;
            GUID sub = {};
            nt->GetGUID(MF_MT_SUBTYPE, &sub);
            Log("VideoSource:   native[%lu] subtype=%08lX-%04X-%04X-%02X%02X...", i,
                sub.Data1, sub.Data2, sub.Data3, sub.Data4[0], sub.Data4[1]);
            nt->Release();
        }
        Log("VideoSource: codec or Color Converter DMO probably missing. "
            "On Windows N / KN editions, install the Media Feature Pack: "
            "https://support.microsoft.com/help/3145500");
        return false;
    }
    return true;
}

bool VideoSource::Update(ID3D11DeviceContext* ctx)
{
    if (!m_reader || !ctx) return false;
    if (m_paused) return false;   // hold on the last-decoded frame while paused

    const ULONGLONG nowMs  = GetTickCount64() - m_startTick;
    const LONGLONG  nowHns = (LONGLONG)nowMs * 10000LL;   // 100ns ticks

    // If the previously-decoded frame's presentation time is still in the
    // future relative to playback, just keep showing it -- no decode work.
    if (m_lastDecodedHns >= 0 && m_lastDecodedHns > nowHns) return false;

    IMFSample* sample      = nullptr;
    DWORD      streamFlags = 0;
    LONGLONG   ts          = 0;
    HRESULT hr = m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                      nullptr, &streamFlags, &ts, &sample);
    if (FAILED(hr)) return false;

    if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
    {
        if (sample) sample->Release();
        Restart();                  // loop the video
        return false;
    }
    if (!sample) return false;

    IMFMediaBuffer* buf = nullptr;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf)
        { sample->Release(); return false; }

    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED(buf->Lock(&data, &maxLen, &curLen)))
        { buf->Release(); sample->Release(); return false; }

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(ctx->Map(m_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
    {
        const int  rowBytes  = m_width * 4;
        const int  dstStride = (int)map.RowPitch;
        const BYTE* src      = data;
        BYTE*       dst      = (BYTE*)map.pData;
        for (int y = 0; y < m_height; ++y)
            memcpy(dst + (size_t)y * dstStride, src + (size_t)y * m_stride, rowBytes);
        ctx->Unmap(m_tex, 0);
    }

    buf->Unlock();
    buf->Release();
    sample->Release();
    m_lastDecodedHns = ts;
    return true;
}

void VideoSource::Restart()
{
    if (!m_reader) return;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt           = VT_I8;
    pv.hVal.QuadPart = 0;
    m_reader->SetCurrentPosition(GUID_NULL, pv);
    PropVariantClear(&pv);
    m_startTick      = GetTickCount64();
    m_lastDecodedHns = -1;
    m_pauseElapsedMs = 0;
}

long long VideoSource::PositionHns() const
{
    if (!m_reader) return 0;
    if (m_paused)  return (LONGLONG)m_pauseElapsedMs * 10000LL;
    return (LONGLONG)(GetTickCount64() - m_startTick) * 10000LL;
}

void VideoSource::TogglePause()
{
    if (!m_reader) return;
    if (m_paused)
    {
        // Resume: shift the playback origin so the clock continues where we
        // paused. Without this, resumed playback would jump forward by the
        // length of the pause.
        m_startTick = GetTickCount64() - m_pauseElapsedMs;
        m_paused    = false;
    }
    else
    {
        m_pauseElapsedMs = GetTickCount64() - m_startTick;
        m_paused         = true;
    }
}

bool VideoSource::Seek(long long hns)
{
    if (!m_reader) return false;
    if (hns < 0) hns = 0;
    if (m_durationHns > 0 && hns > m_durationHns) hns = m_durationHns;

    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt           = VT_I8;
    pv.hVal.QuadPart = hns;
    HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, pv);
    PropVariantClear(&pv);
    if (FAILED(hr)) return false;

    const ULONGLONG ms = (ULONGLONG)(hns / 10000LL);
    if (m_paused) m_pauseElapsedMs = ms;
    else          m_startTick      = GetTickCount64() - ms;
    m_lastDecodedHns = -1;        // force a decode of the new position next Update
    return true;
}

void VideoSource::Close()
{
    Release();
}

void VideoSource::Release()
{
    if (m_srv)    { m_srv->Release();    m_srv    = nullptr; }
    if (m_tex)    { m_tex->Release();    m_tex    = nullptr; }
    if (m_reader) { m_reader->Release(); m_reader = nullptr; }
    m_device         = nullptr;
    m_width          = m_height = m_stride = 0;
    m_startTick      = 0;
    m_lastDecodedHns = -1;
}
