// LFPRenderer.h -- per-eye head-tracked plenoptic renderer. Takes a
// demosaiced Lytro sensor RGB image + microlens calibration + per-eye
// aperture positions, and produces a side-by-side stereo image suitable
// to feed straight into the SR weaver. Replaces the load-time
// "pre-extract two sub-aperture views" path with a per-frame shader pass
// so the stereo aperture position can update with the user's actual
// eye positions every frame.
//
// Pipeline per render call:
//   sensorRGB (full sensor, RGBA8)        -> bound as t0
//   lensCentres (cols x rows, R32G32F)    -> bound as t1
//   constants (aperture L/R, lens radius) -> b0
//   full-screen quad triangle             -> PS
//   shader computes which microlens each output pixel corresponds to,
//   bilinear-blends across the 4 nearest microlenses' aperture samples,
//   writes the per-eye output to its half of the SBS RT.
//
// Memory note: the sensor RGB texture is the biggest cost (~160 MB on
// Illum, ~40 MB on F01). The lens-centres lookup is tiny (~1.8 MB max).
#pragma once

#include "Common.h"
#include "LFPReader.h"
#include <d3d11.h>
#include <vector>

namespace srw
{
    class LFPRenderer
    {
    public:
        LFPRenderer() = default;
        ~LFPRenderer();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Hand off a freshly-demosaiced sensor image + its calibration,
        // plus the target output aspect ratio (typically the SR display's
        // per-eye aspect). The output RT is sized to match the target
        // aspect so the LFP can be letter/pillar-boxed to fit instead
        // of cropped -- the captured plenoptic image isn't 16:9.
        // outputAspect = (perEyeWidth / height); pass 0 to fall back to
        // the MLA grid's own aspect.
        bool LoadFromMemory(const std::vector<uint8_t>& sensorRgb,
                            const LFPCalibration& cal,
                            float outputAspect = 0.0f);

        // Drop all GPU resources tied to a loaded LFP. After this,
        // HasData() returns false again and the render loop falls back
        // to its normal source path.
        void Unload();

        // Re-target the output RT to a new per-eye aspect. Pass 0 to fit
        // the content tightly (no bars -- right for Windowed mode where
        // the window itself adapts to content aspect). Pass an aspect to
        // match (e.g. SR-display aspect for Fullscreen) so the renderer
        // pillarboxes / letterboxes inside the RT. Re-allocates the RT
        // only when the dimensions actually change.
        bool SetTargetAspect(float perEyeAspect);

        // True if a sensor texture is currently bound.
        bool HasData() const { return m_sensorSRV != nullptr; }

        // Render a single SBS frame using the given per-eye aperture
        // positions. Aperture inputs are in NORMALISED units: [-1, +1]
        // where +/-1 means the edge of the main lens aperture. Caller
        // is expected to map physical eye-mm to these via the aperture
        // diameter (and clamp to the aperture circle).
        void Run(float eyeLU, float eyeLV, float eyeRU, float eyeRV);

        // After Run() the SBS view is available here.
        ID3D11ShaderResourceView* OutputSRV() const { return m_outSRV; }
        int          OutputPerEyeWidth()  const { return m_perEyeWidth; }
        int          OutputHeight()       const { return m_outHeight; }
        DXGI_FORMAT  OutputFormat()       const { return m_outFormat; }

        // Aperture diameter of the loaded camera in metres, for the
        // caller's eye-mm -> normalised-aperture conversion. Returns
        // 0 if nothing's loaded.
        double ApertureDiameterMetres() const { return m_apertureDiameterM; }

    private:
        bool   CompileShaders();
        bool   EnsureOutput(int perEyeWidth, int height);
        void   ReleaseTextures();

        ID3D11Device*            m_device  = nullptr;
        ID3D11DeviceContext*     m_context = nullptr;
        ID3D11VertexShader*      m_vs      = nullptr;
        ID3D11PixelShader*       m_ps      = nullptr;
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Buffer*            m_cbuffer = nullptr;

        // Sensor RGB (full sensor resolution, demosaiced + colour-graded on CPU).
        ID3D11Texture2D*          m_sensorTex = nullptr;
        ID3D11ShaderResourceView* m_sensorSRV = nullptr;
        int                       m_sensorW = 0;
        int                       m_sensorH = 0;

        // Lens-centres lookup. (col, row) -> sensor (x, y) in pixels.
        ID3D11Texture2D*          m_lensTex = nullptr;
        ID3D11ShaderResourceView* m_lensSRV = nullptr;
        int                       m_lensCols = 0;
        int                       m_lensRows = 0;

        // SBS output (renderable + sampleable). Sized to the MLA grid
        // (one output pixel per microlens, per eye -- the actual scene
        // resolution the camera captured).
        ID3D11Texture2D*          m_outTex = nullptr;
        ID3D11RenderTargetView*   m_outRTV = nullptr;
        ID3D11ShaderResourceView* m_outSRV = nullptr;
        int                       m_perEyeWidth = 0;
        int                       m_outHeight   = 0;
        DXGI_FORMAT               m_outFormat   = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        // Aperture sample radius inside each microlens, in sensor pixels.
        // Set at LoadFromMemory time from the calibration.
        float                     m_lensRadiusPx     = 0.0f;
        double                    m_apertureDiameterM = 0.0;
    };
}
