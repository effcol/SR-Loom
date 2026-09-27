// Capture.h — captures a window or monitor with Windows.Graphics.Capture and
// exposes the latest frame as a D3D11 shader resource view for weaving.
//
// WinRT details are hidden in the .cpp via a PIMPL so this header stays a plain
// Win32/D3D11 interface.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <memory>

namespace srw
{
    class Capture
    {
    public:
        Capture();
        ~Capture();

        // One-time setup with the app's D3D11 device/context. Returns false if
        // WGC is unavailable on this OS.
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Begin capturing a window or a monitor. Stops any current capture.
        bool StartWindow(HWND window);
        bool StartMonitor(HMONITOR monitor);
        void Stop();

        // Whether the OS cursor is composited into the captured frame. Off by default
        // (avoids a double cursor when weaving the same screen the cursor is on); turned
        // ON for the display picker so you can see your pointer on a captured display.
        // Call before Start*; also applied live if a session is running.
        void SetCaptureCursor(bool enabled);

        bool IsActive() const { return m_active; }

        // Poll for the newest frame and copy it into the source texture.
        // Returns true if a frame was consumed; sets sizeChanged when the
        // capture dimensions changed (the caller must then re-register SRV()).
        bool Update(bool& sizeChanged);

        // Block (up to timeoutMs) until the capture has a frame not yet taken
        // by Update(). Lets the loop start work the moment a new frame lands
        // instead of polling a moment too early and running a frame behind.
        bool WaitForNewFrame(DWORD timeoutMs);

        // When the newest frame taken by Update() was captured: QPC time in
        // 100 ns units (0 = unknown). What the woven picture shows is the
        // screen as it was at this moment.
        int64_t LastFrameTime100ns() const { return m_lastFrameTime; }
        // Bumped whenever the texture behind SRV() gets new pixels (a new frame
        // or a re-crop), so users can tell an unchanged picture from a new one.
        uint64_t ContentVersion() const { return m_version; }

        // Frames Windows delivered since the last call and their rate (by
        // capture timestamps; includes frames Update() skipped). Resets.
        double TakeDeliveryRate(uint64_t& frames)
        {
            frames = m_statFrames;
            const double span = (m_statLastT - m_statFirstT) / 1.0e7;
            const double fps = (m_statFrames > 1 && span > 0) ? (m_statFrames - 1) / span : 0.0;
            m_statFrames = 0;
            return fps;
        }

        // Restrict what gets fed to the weaver to a sub-rectangle of the captured
        // frame, in capture-frame (physical) pixels. Pass w<=0 or h<=0 for the
        // whole frame. Used by the looking-glass / passthrough to weave only the
        // region beneath the viewer.
        void SetSourceRegion(int x, int y, int w, int h);

        ID3D11ShaderResourceView* SRV() const { return m_srv; }
        ID3D11Texture2D*          Texture() const { return m_tex; }   // the (cropped) copy target
        int         Width()      const { return m_width; }   // region (weave-input) width
        int         Height()     const { return m_height; }
        int         FrameWidth()  const { return m_frameW; } // full captured-frame size
        int         FrameHeight() const { return m_frameH; }
        DXGI_FORMAT SRVFormat()  const { return m_srvFormat; }

        static bool IsSupported();

    private:
        bool StartCaptureInternalActive();   // build pool+session for m_impl->item
        void ReleaseTarget();
        bool EnsureTarget(int width, int height);
        bool EnsureFull(int width, int height);   // (re)create m_full at frame size
        bool ResolveRegion(int& rx, int& ry, int& rw, int& rh) const;   // clamp m_reg* to the frame
        bool RecropIfRegionChanged(bool& sizeChanged);   // re-crop without a new WGC frame

        struct Impl;                       // holds the WinRT objects
        std::unique_ptr<Impl>     m_impl;

        ID3D11Device*             m_device  = nullptr;
        ID3D11DeviceContext*      m_context = nullptr;
        ID3D11Texture2D*          m_tex     = nullptr;  // persistent copy target
        ID3D11ShaderResourceView* m_srv     = nullptr;
        int                       m_width   = 0;   // region (weave-input) size
        int                       m_height  = 0;
        int                       m_frameW  = 0;   // full captured-frame size
        int                       m_frameH  = 0;
        int                       m_regX = 0, m_regY = 0, m_regW = 0, m_regH = 0; // crop, frame px
        // Region actually baked into m_tex (resolved + clamped). When the
        // requested region stops matching it (Looking Glass <-> Fullscreen,
        // moving/resizing the loupe) we re-crop immediately from the last
        // full frame instead of waiting for WGC's next frame -- our own
        // window is excluded from capture, so on a still desktop that next
        // frame may not come for a long time, leaving a stale crop
        // stretched over the new window.
        int                       m_appX = 0, m_appY = 0, m_appW = 0, m_appH = 0;
        // Full-frame copy of the latest capture, kept only while cropping to
        // a sub-region. When the crop IS the full frame, m_tex already holds
        // it (no extra copy), and gets adopted as the source on a re-crop.
        ID3D11Texture2D*          m_full    = nullptr;
        bool                      m_active  = false;
        int64_t                   m_lastFrameTime = 0;
        uint64_t                  m_version = 1;
        bool                      m_contentValid = false;   // m_tex holds a whole crop (partial copies build on it)
        uint64_t                  m_statFrames = 0;
        int64_t                   m_statFirstT = 0, m_statLastT = 0;
        bool                      m_captureCursor = false;   // composite the OS cursor into the frame
        // TYPELESS buffer so we can copy the BGRA frame into it and still create
        // an sRGB shader view (sRGB casting isn't allowed on a fully-typed res).
        DXGI_FORMAT               m_texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        DXGI_FORMAT               m_srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
}
