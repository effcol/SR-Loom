// Capture.h — captures a window or monitor with Windows.Graphics.Capture and
// exposes the latest frame as a D3D11 shader resource view for weaving.
//
// WinRT details are hidden in the .cpp via a PIMPL so this header stays a plain
// Win32/D3D11 interface.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <memory>
#include <vector>

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
        // What Windows reported as changed since the last call: updates taken,
        // those with no dirty-region information, those with nothing changed,
        // and the average share of the frame the rest said had changed (0-1).
        void TakeDirtyStats(uint64_t& updates, uint64_t& unknown, uint64_t& none, double& area)
        {
            updates = m_statUpdates; unknown = m_statDirtyUnknown; none = m_statDirtyNone;
            const uint64_t some = m_statUpdates - m_statDirtyUnknown - m_statDirtyNone;
            area = some > 0 ? m_statDirtyArea / some : 0.0;
            m_statUpdates = m_statDirtyUnknown = m_statDirtyNone = 0; m_statDirtyArea = 0;
        }
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

        // Zero-copy (call before Start*): a whole-frame capture isn't copied
        // at all. The newest WGC frame is held (one of the pool's buffers)
        // and DirectSRV() views it; the copy is made only if SRV()/Texture()
        // are asked for (Auto Stereo, analysis, crops).
        void SetZeroCopy(bool on) { m_zeroCopy = on; }
        // Frames as 16-bit float (HDR) instead of 8-bit: see Capture.cpp.
        void SetHdr(bool on);
        bool IsHdr() const { return m_hdr; }

        // The newest picture, the cheapest way: the held capture frame itself
        // when zero-copy has it (encoded = true: a UNORM view of sRGB values,
        // which the reader decodes; a different view as the pool's buffers
        // take turns), else SRV() (encoded = false).
        ID3D11ShaderResourceView* DirectSRV(bool& encoded);

        // The copy (an _SRGB view), made now if zero-copy skipped it.
        ID3D11ShaderResourceView* SRV()     { EnsureCopy(); return m_srv; }
        ID3D11Texture2D*          Texture() { EnsureCopy(); return m_tex; }   // the (cropped) copy target
        bool                      HasPicture() const { return m_tex != nullptr; }
        ID3D11ShaderResourceView* CopyView() const { return m_srv; }   // (the copy's view, as it is: for comparing)
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
        void EnsureCopy();            // zero-copy skipped the copy: make it from the held frame
        void ReleaseDirectViews();
        void DropRetiredViews(bool now);

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
        bool                      m_zeroCopy   = false;     // (SetZeroCopy)
        bool                      m_direct     = false;     // the held frame is the newest whole picture
        bool                      m_copyStale  = false;     // ... and m_tex hasn't been given it (EnsureCopy)
        std::vector<RECT>         m_pendingDirty;           // ... what changed since it was (frame px)
        bool                      m_pendingAll = false;     // ... or too much / unknown: all of it
        // UNORM views of the pool's buffers (they take turns; each keeps its
        // texture alive so a pointer can't be reused by another).
        struct DirectView { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; };
        static constexpr int      kDirectViews = 8;
        DirectView                m_views[kDirectViews];
        DirectView                m_retired[kDirectViews];   // (ReleaseDirectViews)
        int                       m_viewNext = 0;
        int                       m_retireIn = 0;
        uint64_t                  m_statFrames = 0;
        // (For the perf log, TakeDirtyStats: what Windows said had changed.)
        uint64_t                  m_statUpdates = 0, m_statDirtyUnknown = 0, m_statDirtyNone = 0;
        double                    m_statDirtyArea = 0;   // (sum over updates of the changed share of the frame, 0-1)
        int64_t                   m_statFirstT = 0, m_statLastT = 0;
        bool                      m_captureCursor = false;   // composite the OS cursor into the frame
        // TYPELESS buffer so we can copy the BGRA frame into it and still create
        // an sRGB shader view (sRGB casting isn't allowed on a fully-typed res).
        DXGI_FORMAT               m_texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        DXGI_FORMAT               m_srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        bool                      m_hdr = false;
    };
}
