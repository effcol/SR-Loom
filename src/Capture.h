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

        bool IsActive() const { return m_active; }

        // Poll for the newest frame and copy it into the source texture.
        // Returns true if a frame was consumed; sets sizeChanged when the
        // capture dimensions changed (the caller must then re-register SRV()).
        bool Update(bool& sizeChanged);

        ID3D11ShaderResourceView* SRV() const { return m_srv; }
        int         Width()     const { return m_width; }   // full SBS width
        int         Height()    const { return m_height; }
        DXGI_FORMAT SRVFormat() const { return m_srvFormat; }

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
        int                       m_width   = 0;
        int                       m_height  = 0;
        bool                      m_active  = false;
        // TYPELESS buffer so we can copy the BGRA frame into it and still create
        // an sRGB shader view (sRGB casting isn't allowed on a fully-typed res).
        DXGI_FORMAT               m_texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        DXGI_FORMAT               m_srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
}
