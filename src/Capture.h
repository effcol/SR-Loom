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

        // Restrict what gets fed to the weaver to a sub-rectangle of the captured
        // frame, in capture-frame (physical) pixels. Pass w<=0 or h<=0 for the
        // whole frame. Used by the looking-glass / passthrough to weave only the
        // region beneath the viewer.
        void SetSourceRegion(int x, int y, int w, int h);

        ID3D11ShaderResourceView* SRV() const { return m_srv; }
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
        bool                      m_active  = false;
        bool                      m_captureCursor = false;   // composite the OS cursor into the frame
        // TYPELESS buffer so we can copy the BGRA frame into it and still create
        // an sRGB shader view (sRGB casting isn't allowed on a fully-typed res).
        DXGI_FORMAT               m_texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        DXGI_FORMAT               m_srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
}
