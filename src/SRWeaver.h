// SRWeaver.h — wraps the Simulated Reality context and the DirectX 11 weaver.
// Owns the SBS "view" texture that is handed to the weaver each frame.
#pragma once

#include "Common.h"
#include <d3d11.h>

// Forward declarations of SR SDK types (kept out of the header to avoid
// leaking <windows.h>/SDK includes into the rest of the app).
namespace SR
{
    class SRContext;
    class IDX11Weaver1;
    class HeadPoseTracker;
}

namespace srw
{
    class SRWeaver
    {
    public:
        SRWeaver() = default;
        ~SRWeaver();

        // Connect to the SR service. Waits up to maxSeconds for it to come up.
        bool CreateContext(double maxSeconds);

        // Query the SR display's screen rectangle (virtual-desktop coords).
        // Returns false until the display is connected/ready.
        bool GetSRDisplayRect(RECT& out);

        // Create the weaver bound to the output window + device context, then
        // finalize the SR context. Call after the D3D device exists.
        bool CreateWeaver(ID3D11DeviceContext* immediateContext, HWND window);

        // Start/stop the SR session (context + weaver). Stopping releases the SR
        // display's lens and eye-tracking; the loaded image texture is kept.
        bool StartSR(ID3D11DeviceContext* immediateContext, HWND window);
        void StopSR();

        // Register the texture the weaver should sample as its SBS input. The
        // weaver re-samples this view live each weave(), so callers that update
        // the underlying texture in place only need to call this on size change.
        // perEyeWidth is half the full SBS width.
        void SetInputView(ID3D11ShaderResourceView* srv, int perEyeWidth,
                          int height, DXGI_FORMAT format);

        // Load a stereo image from disk into an owned SBS texture and register
        // it as the input. fmt currently distinguishes SBS variants only.
        bool SetStereoImageFromFile(ID3D11Device* device,
                                    const char* path,
                                    StereoFormat fmt,
                                    DXGI_FORMAT texFormat);

        // Perform weaving into the currently-bound render target.
        void Weave();

        void Shutdown();

        // The loaded test image as a source for the converter.
        ID3D11ShaderResourceView* SourceSRV() const { return m_viewSRV; }
        int SourceWidth()  const { return m_imgW; }
        int SourceHeight() const { return m_imgH; }

        bool HasContext() const { return m_context != nullptr; }
        bool HasWeaver()  const { return m_weaver != nullptr; }

        // Latest tracked head pose (position in mm relative to display centre,
        // orientation in radians as (pitch, yaw, roll)). Returns false if the
        // head-pose stream has never delivered a sample. Thread-safe.
        bool GetHeadPose(double pos[3], double orient[3]) const;

    private:
        class HeadListenerImpl;          // opaque to keep SDK headers out of this file
        void ReleaseViewTexture();
        void StartHeadTracker();
        void StopHeadTracker();

        SR::SRContext*            m_context = nullptr;
        SR::IDX11Weaver1*         m_weaver  = nullptr;
        SR::HeadPoseTracker*      m_headTracker  = nullptr;
        HeadListenerImpl*         m_headListener = nullptr;
        ID3D11Texture2D*          m_viewTex = nullptr;
        ID3D11ShaderResourceView* m_viewSRV = nullptr;
        int                       m_imgW    = 0;   // loaded image full size
        int                       m_imgH    = 0;
    };
}
