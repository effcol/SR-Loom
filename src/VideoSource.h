// VideoSource.h -- Windows Media Foundation video decoder. Opens an MP4/MOV/
// MKV/WEBM/AVI file, decodes one frame per Update() into a CPU-mapped dynamic
// D3D11 texture, and loops on end-of-stream. Designed to slot into the same
// source-SRV pathway as the static stbi-loaded test image so the converter +
// weaver pipeline can render quilt VIDEOS the same way it renders quilt PNGs.
#pragma once

#include "Common.h"
#include <d3d11.h>

struct IMFSourceReader;

namespace srw
{
    class VideoSource
    {
    public:
        VideoSource() = default;
        ~VideoSource();

        // Filename extension check -- mp4 / mov / mkv / webm / avi / m4v / wmv.
        static bool IsVideoFile(const char* path);

        // Process-wide MF lifecycle. Call Startup once at app start, Shutdown
        // at exit. Safe to call Open before Startup -- Open will lazy-init.
        static void Startup();
        static void Shutdown();

        // Open a video file. Returns false on any I/O / decoder failure. On
        // success, Width/Height/SRV/Format are populated. The first frame is
        // NOT yet uploaded -- the first Update() does that.
        bool Open(ID3D11Device* device, const wchar_t* utf16Path);
        void Close();

        // Decode + upload the next frame if playback time has reached it. No
        // work / no upload if the previously-decoded frame is still current.
        // Loops automatically at EOF. Returns true when a fresh frame was
        // uploaded this call.
        bool Update(ID3D11DeviceContext* ctx);

        bool        IsOpen() const { return m_reader != nullptr; }
        int         Width()  const { return m_width;  }
        int         Height() const { return m_height; }
        DXGI_FORMAT Format() const { return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
        ID3D11ShaderResourceView* SRV() const { return m_srv; }

        // Playback transport. Positions are in 100ns units (MF native).
        long long DurationHns() const { return m_durationHns; }
        long long PositionHns() const;       // current playback time
        bool      IsPaused()    const { return m_paused; }
        void      TogglePause();
        bool      Seek(long long hns);

    private:
        bool SetupOutputFormat();
        void Restart();
        void Release();

        ID3D11Device*             m_device   = nullptr;
        IMFSourceReader*          m_reader   = nullptr;
        ID3D11Texture2D*          m_tex      = nullptr;
        ID3D11ShaderResourceView* m_srv      = nullptr;
        int                       m_width    = 0;
        int                       m_height   = 0;
        int                       m_stride   = 0;     // MF RGB32 row bytes (may differ from w*4)
        ULONGLONG                 m_startTick = 0;    // GetTickCount64() at playback start
        LONGLONG                  m_lastDecodedHns = -1;
        LONGLONG                  m_durationHns    = 0;   // total length (0 = unknown)
        bool                      m_paused         = false;
        ULONGLONG                 m_pauseElapsedMs = 0;   // elapsed time captured at pause
    };
}
