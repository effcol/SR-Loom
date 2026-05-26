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

        // Frame-packing layout (fractions of source height), used when FramePacking.
        void SetFramePacking(float eyeFrac, float gapFrac);

        // Convergence: per-eye horizontal shift in UV (typically ±0.03).
        void SetConvergence(float shift) { m_convergence = shift; }

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
        // A render-to-texture disparity level (RGBA16F: dLR, dRL, confidence).
        struct DispTarget
        {
            ID3D11Texture2D*          tex = nullptr;
            ID3D11RenderTargetView*   rtv = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            int w = 0, h = 0;
        };

        bool EnsureOutput(int width, int height);
        void ReleaseOutput();
        bool EnsureHistory(int width, int height);
        void ReleaseHistory();
        bool EnsureDispTarget(DispTarget& t, int width, int height);
        void ReleaseDispTarget(DispTarget& t);
        void ReleaseDisparity();   // releases all disparity levels

        static const int kHistory = 6;  // frame-history ring depth (delay 1..5)

        ID3D11Device*            m_device  = nullptr;
        ID3D11DeviceContext*     m_context = nullptr;
        ID3D11VertexShader*      m_vs      = nullptr;
        ID3D11PixelShader*       m_ps      = nullptr;
        ID3D11PixelShader*       m_psCoarse = nullptr;  // full-search disparity (coarsest level)
        ID3D11PixelShader*       m_psRefine = nullptr;  // pyramid refine from a coarser level
        ID3D11PixelShader*       m_psFill   = nullptr;  // occlusion fill + confidence
        ID3D11PixelShader*       m_psSmooth  = nullptr;  // edge-aware disparity smoothing
        ID3D11PixelShader*       m_psSplit   = nullptr;  // SBS half -> premultiplied per-eye pyramid
        ID3D11PixelShader*       m_psCompose = nullptr;  // push-pull colorize -> final SBS
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Buffer*            m_cbuffer = nullptr;

        // Multi-scale L<->R disparity pyramid for anaglyph recovery (all RGBA16F).
        DispTarget m_disp0;   // coarsest (1/16) full search
        DispTarget m_disp1;   // refined   (1/4)
        DispTarget m_disp2;   // occlusion-filled (1/4)
        DispTarget m_dispF;   // edge-aware smoothed (1/4); the compose pass reads this

        ID3D11Texture2D*          m_outTex = nullptr;  // full SBS (2*perEye wide)
        ID3D11RenderTargetView*   m_outRTV = nullptr;
        ID3D11ShaderResourceView* m_outSRV = nullptr;
        int                       m_outWidth  = 0;     // full SBS width
        int                       m_outHeight = 0;

        // Per-eye colour pyramids (mip-chained, RGBA16F, premultiplied) for the
        // push-pull colorization of anaglyph recovery. GenerateMips on premultiplied
        // colour gives a confidence-weighted average at each level.
        ID3D11Texture2D*          m_ppLeftTex  = nullptr;
        ID3D11RenderTargetView*   m_ppLeftRTV  = nullptr;   // mip 0
        ID3D11ShaderResourceView* m_ppLeftSRV  = nullptr;   // full chain
        ID3D11Texture2D*          m_ppRightTex = nullptr;
        ID3D11RenderTargetView*   m_ppRightRTV = nullptr;
        ID3D11ShaderResourceView* m_ppRightSRV = nullptr;
        int                       m_ppMips = 0;

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
        float        m_fpEyeFrac = 1080.0f / 2205.0f;  // frame-packing eye height fraction
        float        m_fpGapFrac = 45.0f / 2205.0f;    // frame-packing gap fraction
        float        m_convergence = 0.0f;             // per-eye horizontal shift (UV)
        DXGI_FORMAT  m_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
