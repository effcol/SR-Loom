// CaptureDXGI.h — DXGI Output Duplication monitor capture, used as the
// fallback when WGC stops delivering frames because the foreground app went
// exclusive-fullscreen (where WGC returns blank). DXGI captures the post-
// flip surface and works for those cases. Limitation vs Capture (WGC):
// monitor capture ONLY (no per-window), and no window-exclusion (capturing
// the SR display itself would feed our own overlay back into the capture —
// we never use DXGI duplication for passthrough).
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <dxgi1_2.h>

namespace srw
{
    class CaptureDXGI
    {
    public:
        CaptureDXGI() = default;
        ~CaptureDXGI();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Begin DXGI Output Duplication on the output that contains `monitor`.
        bool StartMonitor(HMONITOR monitor);
        void Stop();

        bool IsActive() const { return m_dup != nullptr; }

        // Poll for the newest frame; copy (cropped to source region) into m_tex.
        // Returns true if a frame was consumed; sets sizeChanged when the
        // weave-input dimensions changed (caller must re-register SRV()).
        bool Update(bool& sizeChanged);

        // Restrict what we feed forward to a sub-rectangle of the captured frame,
        // in capture-frame (physical) pixels. Pass w<=0 or h<=0 for the whole frame.
        void SetSourceRegion(int x, int y, int w, int h);

        ID3D11ShaderResourceView* SRV() const { return m_srv; }
        int         Width()       const { return m_width; }
        int         Height()      const { return m_height; }
        int         FrameWidth()  const { return m_frameW; }
        int         FrameHeight() const { return m_frameH; }
        DXGI_FORMAT SRVFormat()   const { return m_srvFormat; }

    private:
        bool EnsureTarget(int width, int height);
        void ReleaseTarget();

        ID3D11Device*             m_device  = nullptr;
        ID3D11DeviceContext*      m_context = nullptr;
        IDXGIOutputDuplication*   m_dup     = nullptr;
        ID3D11Texture2D*          m_tex     = nullptr;
        ID3D11ShaderResourceView* m_srv     = nullptr;
        int                       m_width   = 0;   // region (weave-input) size
        int                       m_height  = 0;
        int                       m_frameW  = 0;   // full captured-frame size
        int                       m_frameH  = 0;
        int                       m_regX = 0, m_regY = 0, m_regW = 0, m_regH = 0;
        HMONITOR                  m_monitor = nullptr;   // remembered for ACCESS_LOST recovery
        DXGI_FORMAT               m_texFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        DXGI_FORMAT               m_srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };
}
