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
        // eyeAlign shifts the bottom eye sampling start by this many source rows
        // to correct residual rounding misalignment from the capture pipeline.
        void SetFramePacking(float eyeFrac, float gapFrac, float eyeAlign);

        // Convergence: per-eye horizontal shift in UV (typically ±0.03).
        void SetConvergence(float shift) { m_convergence = shift; }

        // Quilt layout (used only when format == Quilt). cols x rows grid of
        // views indexed left-to-right, BOTTOM-to-top (Looking Glass convention).
        // leftIdx / rightIdx pick the integer-floor view for each pane; the
        // optional leftBlend / rightBlend (0..1) cross-fades to the next view
        // -- mimicking the smooth between-view transition Looking Glass shows
        // as the user moves their head between physical lenticular columns.
        void SetQuilt(int cols, int rows, int leftIdx, int rightIdx,
                      float leftBlend = 0.0f, float rightBlend = 0.0f);

        // Per-eye pane the weaver will actually display on (panel native dims).
        // When set, Quilt sizes its output panes to this and pillar/letterboxes
        // each view inside; the weaver then samples 1:1 with no stretching.
        // (0,0) disables -- panes default to the view's native pixel size and
        // the weaver scales to the panel.
        void SetTargetPaneSize(int w, int h) { m_targetPaneW = w; m_targetPaneH = h; }

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
        float        m_fpEyeAlign = 0.0f;              // bottom-eye vertical alignment (source rows)
        float        m_convergence = 0.0f;             // per-eye horizontal shift (UV)
        int          m_quiltCols = 8;                   // quilt grid: columns of views
        int          m_quiltRows = 6;                   // quilt grid: rows of views
        int          m_quiltLeftIdx  = 23;              // L pane view index (8x6: centre-1)
        int          m_quiltRightIdx = 24;              // R pane view index (8x6: centre)
        float        m_quiltLeftBlend  = 0.0f;          // 0..1 fade from leftIdx to leftIdx+1
        float        m_quiltRightBlend = 0.0f;          // 0..1 fade from rightIdx to rightIdx+1
        int          m_targetPaneW = 0;                 // SR panel per-eye dims (0 = unset)
        int          m_targetPaneH = 0;
        DXGI_FORMAT  m_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
