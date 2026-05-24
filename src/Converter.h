// Converter.h — turns a source frame in any stereo layout into the full
// side-by-side (left | right) texture the SR weaver consumes, via an HLSL pass.
#pragma once

#include "Common.h"
#include <d3d11.h>

namespace srw
{
    class Converter
    {
    public:
        Converter() = default;
        ~Converter();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // anaCombo: 0..5 (see AnaglyphComboList); anaMode: 0..3 (AnaglyphModeList).
        void SetFormat(StereoFormat fmt, bool swapEyes, int anaCombo, int anaMode);

        // Pulfrich settings (used only when format == Pulfrich).
        void SetPulfrich(PulfrichMode mode, int affectedEye, float ndTransmission, int delayFrames);

        // Convert the source view into the internal SBS texture. Sets
        // outputResized=true when the SBS texture was (re)created (the caller
        // must then re-register OutputSRV() with the weaver).
        bool Convert(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight,
                     bool& outputResized);

        ID3D11ShaderResourceView* OutputSRV()        const { return m_outSRV; }
        int          OutputPerEyeWidth()  const { return m_outWidth / 2; }
        int          OutputHeight()       const { return m_outHeight; }
        DXGI_FORMAT  OutputFormat()       const { return m_format; }

    private:
        bool EnsureOutput(int width, int height);
        void ReleaseOutput();
        bool EnsureHistory(int width, int height);
        void ReleaseHistory();

        static const int kHistory = 6;  // frame-history ring depth (delay 1..5)

        ID3D11Device*            m_device  = nullptr;
        ID3D11DeviceContext*     m_context = nullptr;
        ID3D11VertexShader*      m_vs      = nullptr;
        ID3D11PixelShader*       m_ps      = nullptr;
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Buffer*            m_cbuffer = nullptr;

        ID3D11Texture2D*          m_outTex = nullptr;  // full SBS (2*perEye wide)
        ID3D11RenderTargetView*   m_outRTV = nullptr;
        ID3D11ShaderResourceView* m_outSRV = nullptr;
        int                       m_outWidth  = 0;     // full SBS width
        int                       m_outHeight = 0;

        // Frame-history ring (for Pulfrich time delay), source-sized, sRGB.
        ID3D11Texture2D*          m_hist[kHistory]    = {};
        ID3D11RenderTargetView*   m_histRTV[kHistory] = {};
        ID3D11ShaderResourceView* m_histSRV[kHistory] = {};
        int  m_histW = 0, m_histH = 0;   // history texture size
        int  m_histWrite = 0;            // next slot to write

        StereoFormat m_fmt  = StereoFormat::FullSBS;
        bool         m_swap = false;
        int          m_anaCombo = 0;
        int          m_anaMode  = 0;
        PulfrichMode m_pulfMode = PulfrichMode::TimeDelay;
        int          m_pulfEye  = 1;       // affected eye (0 left, 1 right)
        float        m_ndTrans  = 0.30f;   // ND transmission
        int          m_pulfDelay = 1;      // delay frames
        DXGI_FORMAT  m_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
