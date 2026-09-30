// Converter.h — turns a source frame in any stereo layout into the full
// side-by-side (left | right) texture the SR weaver consumes, via an HLSL pass.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <cstdint>
#include <vector>

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
        // Anaglyph mode 5 (a one-colour picture under it, AnalyseAnaPicture):
        // its tint tables, AnaTint::single (256 x RGBA) and ::missing (256).
        // Null: none. Only re-uploaded when the tables change.
        void SetAnaTint(const uint8_t (*single)[4], const uint8_t* missing);
        // Recovered Colour on a page of several anaglyphs: the pictures that were
        // black-and-white (kind 1: Mono) or one colour (kind 2: its tint tables)
        // under the anaglyph, each decoded that way inside its box (source uv).
        // Everything else is still recovered. n = 0: none.
        struct AnaBox { float u0 = 0, v0 = 0, u1 = 0, v1 = 0; int kind = 0;
                        const uint8_t (*single)[4] = nullptr; const uint8_t* missing = nullptr;
                        const uint8_t* pairA = nullptr; const uint8_t* pairB = nullptr;   // (AnaTint)
                        float checkInsetU = 0, checkInsetV = 0; };   // (the per-frame check looks this far inside: past the padding)
        void SetAnaBoxes(const AnaBox* boxes, int n);
        // Diagnostics: this frame's PSAnaBoxCheck result per box (1 = still so);
        // stalls on the GPU -- only for the log, now and then. False if none.
        bool ReadAnaBoxValid(float out[32]);
        // The boxes follow the page as it scrolls: a 1/16 snapshot of the frame
        // the check judged (requested when that frame is handed to the check,
        // taken at the next Convert), made the boxes' reference once their
        // verdict is in (Commit). Each frame PSAnaBoxShift finds how far each
        // box's picture has moved since.
        void RequestAnaSnapshot() { m_snapRequested = true; }
        void CommitAnaSnapshot();

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
        // Each eye at most half the source's width (default; the display shows no
        // more). Off: full-width eyes (tools/anatest's quality measurements).
        void SetHalfWidthEyes(bool on) { m_halfWidthEyes = on; }
        // Redraw only what changed since the last frame (default). Off: every
        // frame in full (tools/anatest compares the two).
        void SetChangeSkip(bool on) { m_changeSkipOn = on; }

        // VR180 / VR360 viewer parameters. yaw / pitch in RADIANS. zoom: 1.0
        // is ~90° horizontal FOV; higher = zoomed in. ipdScale shifts the
        // stereo views laterally to approximate IPD parallax (small value,
        // e.g. 0.02 of the sphere width).
        void SetVRView(float yaw, float pitch, float zoom)
        {
            m_vrYaw = yaw; m_vrPitch = pitch; m_vrZoom = zoom;
        }

        // Convert the source view into the internal SBS texture. Sets
        // outputResized=true when the SBS texture was (re)created (the caller
        // must then re-register OutputSRV() with the weaver).
        // Which picture the next Convert's source holds (e.g. the capture's
        // content version): with the same version and settings as last time
        // the previous output is still right and Convert skips the work.
        // 0 = unknown (always convert).
        void SetSourceVersion(uint64_t v) { m_srcVersion = v; }
        // The next Convert's source view reads sRGB-encoded values as they are
        // (a plain UNORM view: the capture's own frame, not a copy -- WGC's
        // textures can't take an _SRGB view). The shaders then decode them,
        // and filter after decoding, as the hardware would. identity: what the
        // source is, for the settings key, when its view changes every frame
        // (the capture's buffers take turns); nullptr = the view itself.
        void SetSourceEncoded(bool encoded, const void* identity = nullptr) { m_srcEncoded = encoded; m_srcIdentity = identity; }
        // Whether the current settings read an encoded source cheaply enough to
        // beat copying it (4K, measured with anatest ANATEST_RAW): the layouts
        // and the simple anaglyph modes, a few reads a pixel (+0-0.24 ms vs the
        // 0.37 ms copy). DeAnaglyph (+2.7 ms) and Recovered (+9.7 ms) read it
        // many times; Quilt, VR and the temporal formats aren't measured.
        bool CheapEncodedSource() const { return CheapEncodedSource(m_fmt, m_anaMode); }
        static bool CheapEncodedSource(StereoFormat fmt, int anaMode)
        {
            switch (fmt)
            {
            case StereoFormat::FullSBS: case StereoFormat::HalfSBS:
            case StereoFormat::FullTAB: case StereoFormat::HalfTAB:
            case StereoFormat::RowInterleaved: case StereoFormat::ColumnInterleaved:
            case StereoFormat::Checkerboard: case StereoFormat::FramePacking:
                return true;
            case StereoFormat::Anaglyph:
                return anaMode >= 1 && anaMode <= 3;   // (Filtered, Half Colour, Mono)
            default:
                return false;
            }
        }

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
        bool EnsureDescTargets(int width, int height);
        void ReleaseDescTargets();
        uint64_t SettingsKey(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight) const;

        static const int kHistory = 6;  // frame-history ring depth (delay 1..5)

        ID3D11Device*            m_device  = nullptr;
        ID3D11DeviceContext*     m_context = nullptr;
        ID3D11VertexShader*      m_vs      = nullptr;
        ID3D11PixelShader*       m_ps      = nullptr;
        ID3D11PixelShader*       m_psCoarse = nullptr;  // full-search disparity (coarsest level)
        ID3D11PixelShader*       m_psRefine = nullptr;  // pyramid refine from a coarser level
        ID3D11PixelShader*       m_psFill   = nullptr;  // occlusion fill + confidence
        ID3D11PixelShader*       m_psSmooth  = nullptr;  // edge-aware disparity smoothing
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Buffer*            m_cbuffer = nullptr;

        // Multi-scale L<->R disparity pyramid for anaglyph recovery (all RGBA16F).
        DispTarget m_disp0;   // coarsest (1/16) full search
        DispTarget m_disp1;   // refined   (1/4)
        DispTarget m_disp2;   // occlusion-filled (1/4)
        DispTarget m_dispF;   // edge-aware smoothed (1/4); the compose pass reads this
        DispTarget m_src4;    // the source averaged down 4x (4x4 blocks) -- what the 1/4 passes read
        DispTarget m_src16;   // ... and 16x (the coarse search)
        DispTarget m_dispPrev;   // last frame's smoothed disparity (video: steadies the next)
        DispTarget m_src4Prev;   // last frame's 1/4 source (to see what changed)
        // What the pictures under an anaglyph were (SetAnaTint / SetAnaBoxes):
        // tint tables (256 x kTintRows, RGBA8, sRGB-encoded) at t10 -- rows 0-1
        // the whole picture's, 2+2i / 3+2i box i's -- and the boxes (RGBA32F,
        // 1 + 2 x kMaxBoxes: [0].x the count, then per box its rect and kind) at t11.
        static constexpr int kMaxBoxes = 32, kTintRows = 2 + 2 * kMaxBoxes;
        std::vector<uint8_t>      m_tintPx = std::vector<uint8_t>(256 * kTintRows * 4, 0);
        float                     m_boxPx[(1 + 2 * kMaxBoxes) * 4] = {};
        bool                      m_anaTablesDirty = false;
        ID3D11Texture2D*          m_tintTex = nullptr;
        ID3D11ShaderResourceView* m_tintSRV = nullptr;
        ID3D11Texture2D*          m_boxTex = nullptr;
        ID3D11ShaderResourceView* m_boxSRV = nullptr;
        uint64_t                  m_tintHash = 0;   // (of the tables; 0 = none in use)
        void UploadAnaTables();
        // Every frame, whether each box still holds what it was judged to be
        // (PSAnaBoxCheck on the 1/16 block averages): kMaxBoxes x 1, .r 1 = yes.
        // A page scrolled, a colour picture moved in -- off at once.
        DispTarget         m_boxValid, m_boxRows, m_boxMap;   // (PSAnaBoxRows: per box and row; PSAnaBoxMap: boxes per 16x16 block)
        ID3D11PixelShader* m_psBoxRows = nullptr;
        ID3D11PixelShader* m_psBoxMap = nullptr;
        // What changed since the last frame (PSChange / PSChangeGrow): only those
        // blocks are recovered again; the occlusion predicate skips the whole
        // recovery when nothing did. m_changeValid: the output holds a whole
        // frame for the current settings (else the next one is drawn in full).
        DispTarget         m_change, m_changeGrow, m_boxMapPrev;
        ID3D11PixelShader* m_psChange = nullptr;
        ID3D11PixelShader* m_psChangeGrow = nullptr;
        ID3D11Predicate*   m_changePred = nullptr;
        bool               m_changeValid = false;
        DispTarget         m_snapPending, m_snapActive, m_boxShiftCost, m_boxShift;   // (RequestAnaSnapshot, PSAnaBoxShift)
        ID3D11PixelShader* m_psBoxShiftCost = nullptr;
        ID3D11PixelShader* m_psBoxShift = nullptr;
        bool               m_snapRequested = false, m_snapPendingValid = false, m_snapActiveValid = false;
        ID3D11PixelShader* m_psBoxCheck = nullptr;
        bool       m_dispPrevValid = false;
        ID3D11PixelShader* m_psDown = nullptr;   // 4x box downsample
        // Packed gradient descriptors at the refine level (PSAnaDesc): 4 x uint4.
        ID3D11PixelShader*        m_psDesc = nullptr;
        ID3D11Texture2D*          m_descTex[4] = {};
        ID3D11RenderTargetView*   m_descRTV[4] = {};
        ID3D11ShaderResourceView* m_descSRV[4] = {};
        int                       m_descW = 0, m_descH = 0;

        // GPU time of the anaglyph recovery's stages, for the perf log: a small
        // ring of timestamp sets, read back a few frames later (never stalls).
        static constexpr int kTimeRing = 4, kTimeMarks = 9;
        struct TimeSet { ID3D11Query* disjoint = nullptr; ID3D11Query* ts[kTimeMarks] = {}; bool pending = false; };
        TimeSet m_times[kTimeRing];
        int     m_timeNext = 0;
        double  m_timeSum[kTimeMarks - 1] = {};
        int     m_timeCount = 0;
        void    TimeMark(int slot, int mark);
        void    CollectTimes();
    public:
        // Average ms per recovery stage since the last call (coarse search,
        // descriptors, refine, occlusion fill, smoothing, full-res decode,
        // colour pyramid, colour fill); false if none ran.
        bool TakeRecoveryTimes(double ms[kTimeMarks - 1], int& count);
        // Diagnostics (tools/anatest): the recovery's disparity map after a stage
        // -- 0 coarse search, 1 refine, 2 occlusion fill, 3 smoothing (what the
        // compose reads) -- or null.
        ID3D11ShaderResourceView* DebugStageSRV(int stage) const
        {
            const DispTarget* t[4] = { &m_disp0, &m_disp1, &m_disp2, &m_dispF };
            return (stage >= 0 && stage < 4) ? t[stage]->srv : nullptr;
        }
    private:

        // Unchanged-source skip (SetSourceVersion).
        uint64_t                  m_srcVersion  = 0;
        bool                      m_srcEncoded  = false;     // (SetSourceEncoded)
        const void*               m_srcIdentity = nullptr;
        uint64_t                  m_lastVersion = 0;
        uint64_t                  m_lastKey     = 0;

        ID3D11Texture2D*          m_outTex = nullptr;  // full SBS (2*perEye wide)
        ID3D11RenderTargetView*   m_outRTV = nullptr;
        ID3D11ShaderResourceView* m_outSRV = nullptr;
        int                       m_outWidth  = 0;     // full SBS width
        int                       m_outHeight = 0;

        // Per-eye colour pyramids (mip-chained, RGBA16F, premultiplied) for the
        // push-pull colorization of anaglyph recovery. GenerateMips on premultiplied
        // colour gives a confidence-weighted average at each level.

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
        bool         m_halfWidthEyes = true;
        bool         m_changeSkipOn = true;
        float        m_vrYaw   = 0.0f;                   // VR viewer: yaw (radians)
        float        m_vrPitch = 0.0f;                   // VR viewer: pitch (radians)
        float        m_vrZoom  = 1.0f;                   // VR viewer: zoom (1=~90° HFOV)
        DXGI_FORMAT  m_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
