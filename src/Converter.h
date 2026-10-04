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
        // The Custom anaglyph pair (anaCombo 6): Common.h AnaCustomFromColours.
        void SetAnaCustom(const AnaCustom& k)
        {
            for (int c = 0; c < 3; ++c)
            {
                m_anaMaskL[c] = k.ml[c]; m_anaMaskR[c] = k.mr[c]; m_anaWL[c] = k.wl[c]; m_anaWR[c] = k.wr[c];
                m_anaTL[c] = k.tl[c]; m_anaTR[c] = k.tr[c];
                m_anaChanL = k.cl; m_anaChanR = k.cr;
            }
        }
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
        // Recovered Colour: each eye at most half the source's width (off by default:
        // PSAnaPair made full width affordable). (Other modes always keep full width.)
        void SetHalfWidthEyes(bool on) { m_halfWidthEyes = on; }
        // Recovered Colour at full width: the refine once per pixel pair (default).
        // Off: per pixel, as before (tools/anatest comparisons).
        void SetPairRefine(bool on) { m_pairRefineOn = on; }
        // Redraw only what changed since the last frame (default). Off: every
        // frame in full (tools/anatest compares the two).
        void SetChangeSkip(bool on) { m_changeSkipOn = on; }
        // Recovered Colour: a block that only scrolled since the last frame takes
        // last frame's output, moved (PSChangeScroll). Off: every moved block redrawn.
        void SetScrollReuse(bool on) { m_scrollReuseOn = on; }
        // Recovered Colour is what's set: the one conversion that can take several ms.
        bool IsRecoveredColour() const { return m_fmt == StereoFormat::Anaglyph && m_anaMode == 4; }
        // Recovered Colour: the 1/4-size disparity passes only where a redrawn block
        // reads them (PSChangeGrow2). Off: all of them, whenever anything changed.
        void SetPyramidSkip(bool on) { m_pyramidSkipOn = on; }
        // Recovered Colour: a change redraws the blocks whose own borrow reaches it
        // (PSReach). Off: every block within the whole search range of it.
        void SetReach(bool on) { m_reachOn = on; }
        // Recovered Colour from an encoded source (the capture's own frame): checked
        // for change first, copied and converted only if it did. Off: read as it is.
        void SetPreCheck(bool on) { m_preCheckOn = on; }
        // Recovered Colour: plain grey areas found at 1/4 size and skipped by the
        // pair and smoothing passes. Off: each pixel pair tested on its own.
        void SetFlatSkip(bool on) { m_flatOn = on; }
        // Quilt resampled in two passes (rows, then columns): the same picture,
        // a third of the reads. Off: in one.
        void SetQuiltTwoPass(bool on) { m_quiltTwoPass = on; }
        // Light field (an experiment): a Quilt's views interlaced across the lens by
        // SR Loom itself, every view at once -- no eye tracking: move your head to
        // look around. pitchPx: the lens' pitch in pixels (0 = off); slant: its
        // slant; panelW / H: the display's size (the output is made pixel for
        // pixel). The phase offset rides in the Quilt's left blend (SetQuilt).
        void SetLightField(float pitchPx, float slant, int panelW, int panelH) { m_lfPitch = pitchPx; m_lfSlant = slant; m_lfW = panelW; m_lfH = panelH; }
        // Both eyes from one compute thread where they share source pixels (the
        // anaglyph modes but Recovered, checkerboard, interleaved). Off: pixel shaders.
        void SetComputeBothEyes(bool on) { m_csOn = on; }
        // The output texture made shareable with another Direct3D device (the
        // DX12 presenter weaves from it without a copy). Remade on the next
        // Convert when this changes, which reports it as resized.
        void SetShareableOutput(bool on) { m_outShare = on; }
        // Conversion apart from the weave: each pass sent to the GPU on its own and
        // the long ones in this many bands (0: all in one go, as ever). See Convert.
        void SetGpuYield(int bands) { m_yieldBands = bands; }
        // HDR: the output 16-bit float (linear, brighter-than-white values kept)
        // instead of 8-bit sRGB. Remade on the next Convert, reported as resized.
        void SetHdr(bool on) { m_hdr = on; m_format = on ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        // Whether Recovered Colour is best given the capture's own frame (above).
        bool RecoveredWantsDirect() const { return m_preCheckOn && m_changeSkipOn; }

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
        // beat copying it (4K, measured with anatest ANATEST_RAW, full-width eyes):
        // the layouts and the simple anaglyph modes (+0-0.2 ms against the 0.37 ms
        // copy). Frame packing (+0.6), DeAnaglyph (+2 ms) and Recovered read it
        // many times; Quilt, VR and the temporal formats aren't measured.
        bool CheapEncodedSource() const { return CheapEncodedSource(m_fmt, m_anaMode); }
        static bool CheapEncodedSource(StereoFormat fmt, int anaMode)
        {
            switch (fmt)
            {
            case StereoFormat::FullSBS: case StereoFormat::HalfSBS:
            case StereoFormat::FullTAB: case StereoFormat::HalfTAB:
            case StereoFormat::RowInterleaved: case StereoFormat::ColumnInterleaved:
            case StereoFormat::Checkerboard:
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
        ID3D11Texture2D*          OutputTexture()    const { return m_outTex; }
        // For a presenter that weaves from a copy of the output (the DX12
        // presenter, conversion apart from the weave): see Converter.cpp.
        void CopyOutputTo(ID3D11Texture2D* dst, bool force);
        int  OutputChanged();
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
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        };

        bool EnsureOutput(int width, int height);
        void ReleaseOutput();
        bool EnsureHistory(int width, int height);
        void ReleaseHistory();
        bool EnsureDispTarget(DispTarget& t, int width, int height, DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT);
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
        ID3D11PixelShader*       m_psPair    = nullptr;  // Recovered Colour at full width: the refine per pixel pair (PSAnaPair)
        ID3D11PixelShader*       m_psAnaCompose = nullptr;  // ... and its compose alone (PSAnaCompose), after PSAnaPair
        ID3D11PixelShader*       m_psPairPlain = nullptr, *m_psAnaComposePlain = nullptr;  // ... both for a source read as it is (SRC_PLAIN)
        // The common formats' own shaders (PSFmt*: Half SBS / Katanga, Full SBS, TAB,
        // row, column, checkerboard, frame packing, anaglyph without Recovered).
        ID3D11PixelShader*       m_psFmt[9] = {};
        // Quilt's first pass (rows) and its result (SetQuiltTwoPass).
        ID3D11PixelShader*       m_psQuiltH = nullptr;
        DispTarget               m_quiltH;
        bool                     m_quiltTwoPass = true;
        float                    m_lfPitch = 0.0f, m_lfSlant = 0.0f;   // (SetLightField)
        int                      m_lfW = 0, m_lfH = 0;
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Buffer*            m_cbuffer = nullptr;

        // Multi-scale L<->R disparity pyramid for anaglyph recovery (all RGBA16F).
        DispTarget m_disp0;   // coarsest (1/16) full search
        DispTarget m_disp1;   // refined   (1/4)
        DispTarget m_disp2;   // occlusion-filled (1/4)
        DispTarget m_dispF;   // edge-aware smoothed (1/4); the compose pass reads this
        DispTarget m_pair;    // ... both eyes' dRef / conf per pixel pair (PSAnaPair; full-width eyes)
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
        uint64_t                  m_wholeTintHash = 0, m_lastTintHash = 0;   // (rows 0-1 alone; m_tintHash at the last conversion)
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
        DispTarget         m_change, m_changeGrow, m_changeGrow2, m_boxMapPrev;   // (m_changeGrow2: what the 1/4 passes work out, PSChangeGrow2)
        ID3D11PixelShader* m_psChange = nullptr;
        ID3D11PixelShader* m_psChangeGrow = nullptr;
        ID3D11PixelShader* m_psChangeGrow2 = nullptr;
        // How far sideways each block borrows (PSReach), kept from frame to frame:
        // a change redraws only the blocks that reach it (PSChangeGrow).
        DispTarget         m_reach, m_reachNext;
        ID3D11PixelShader* m_psReach = nullptr;
        bool               m_reachValid = false, m_reachOn = true;
        ID3D11Predicate*   m_changePred = nullptr;
        // The capture's own frame checked for change before it's copied (Convert):
        // the copy the passes read (m_work), the frame averaged down undecoded this
        // frame and last, and the predicate that skips the copy and the conversion.
        ID3D11Texture2D*          m_workTex = nullptr;
        ID3D11ShaderResourceView* m_workSRV = nullptr;
        int                       m_workW = 0, m_workH = 0;
        DXGI_FORMAT               m_workFmt = DXGI_FORMAT_UNKNOWN;
        DispTarget                m_enc4, m_enc4Prev;
        bool                      m_encPrevValid = false, m_preCheckOn = true;
        ID3D11Predicate*          m_prePred = nullptr;
        ID3D11PixelShader*        m_psDownBox = nullptr;
        // The 1/4 level's plain grey texels (PSDownFlat), shrunk (PSFlatShrink): the
        // pair and smoothing passes skip them (SetFlatSkip).
        ID3D11PixelShader*        m_psDownFlat = nullptr;
        ID3D11PixelShader*        m_psFlatShrink = nullptr;
        DispTarget                m_flat4, m_flatS;
        bool                      m_flatOn = true;
        // Scroll reuse: last frame's source (full size) and output, how far the
        // picture scrolled (PSScrollCost / PSScrollPick, 1x1), and a predicate that
        // skips the copy of last frame's output when it didn't.
        static constexpr int kScroll = 192;   // (Converter.hlsl)
        ID3D11Texture2D*          m_srcPrevTex = nullptr;
        ID3D11ShaderResourceView* m_srcPrevSRV = nullptr;
        DXGI_FORMAT               m_srcPrevFmt = DXGI_FORMAT_UNKNOWN, m_srcPrevViewFmt = DXGI_FORMAT_UNKNOWN;
        int                       m_srcPrevW = 0, m_srcPrevH = 0;
        bool                      m_srcPrevValid = false;
        ID3D11Texture2D*          m_outPrevTex = nullptr;
        ID3D11ShaderResourceView* m_outPrevSRV = nullptr;
        DispTarget         m_scrollRows, m_scrollCost, m_scroll, m_changeQ;   // (PSScrollCost per grid row, summed, picked; PSChangeScrollQ)
        ID3D11PixelShader* m_psScrollCost = nullptr;
        ID3D11PixelShader* m_psScrollPick = nullptr;
        ID3D11PixelShader* m_psScrollSum = nullptr;
        ID3D11PixelShader* m_psChangeScrollQ = nullptr;
        ID3D11PixelShader* m_psChangeScroll = nullptr;
        ID3D11Predicate*   m_scrollPred = nullptr;
        void ReleaseScroll();
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
        // ... and the same for the coarse search's candidates (PSAnaDescCoarse):
        // worked out once per position instead of once per pixel that tries it.
        struct DescSet { ID3D11Texture2D* tex[4] = {}; ID3D11RenderTargetView* rtv[4] = {}; ID3D11ShaderResourceView* srv[4] = {}; int w = 0, h = 0; };
        bool EnsureDescSet(DescSet& s, int width, int height);
        void ReleaseDescSet(DescSet& s);
        DescSet                   m_descCoarse;
        ID3D11PixelShader*        m_psDescCoarse = nullptr;

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
        // descriptors, refine, occlusion fill, smoothing, the full-res pair
        // refine, the compose; the last is unused); false if none ran.
        bool TakeRecoveryTimes(double ms[kTimeMarks - 1], int& count);
        // For the perf log: what the recovery did with the frames since the last
        // call. Counted only while on (SetChangeStats): two tiny passes and a
        // 1 x 1 read-back per frame, never waited for.
        struct ChangeStats
        {
            int    frames = 0;        // conversions run
            int    full = 0;          // ... drawn whole (first frame, new settings, a new size)
            int    changed = 0;       // ... where something had changed
            int    scrolled = 0;      // ... of those, a scroll found
            double redrawn = 0;       // changed frames: the part redrawn, 0-1 (average)
            double moved = 0;         // ... the part taken from last frame's output, moved
            double rows = 0;          // scrolled frames: rows per frame (average, either way)
            bool   reuse = false;     // scroll reuse could run (else: off, or the source isn't a plain texture of the picture's size)
        };
        void SetChangeStats(bool on) { m_statsOn = on; }
        bool TakeChangeStats(ChangeStats& out);
    private:
        bool               m_statsOn = false;
        DispTarget         m_statRows, m_stat;
        ID3D11PixelShader* m_psStatRows = nullptr;
        ID3D11PixelShader* m_psStat = nullptr;
        static constexpr int kStatRing = 4;
        ID3D11Texture2D*   m_statStaging[kStatRing] = {};
        bool               m_statPending[kStatRing] = {};
        float              m_statBlocks[kStatRing] = {};   // (blocks in that frame's picture)
        int                m_statNext = 0;
        ChangeStats        m_statAcc;
        void CollectStats();
    public:
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
        ID3D11UnorderedAccessView* m_outUAV = nullptr;   // (UNORM view: the both-eyes compute shaders)
        ID3D11ComputeShader*      m_cs[4] = {};           // (both eyes per thread: anaglyph, checkerboard, column, row)
        bool                      m_csOn = true;
        bool                      m_outShare = false, m_outShared = false;   // (SetShareableOutput: wanted / as made)
        ID3D11Predicate*          m_outPred = nullptr;   // the predicate the last Convert drew under (not owned; null: none)
        int                       m_yieldBands = 0;      // (SetGpuYield)
        ID3D11RasterizerState*    m_bandRS = nullptr;
        bool                      m_hdr = false, m_outHdr = false;   // (SetHdr: wanted / as the output was made)
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
        float        m_anaMaskL[3] = { 1, 0, 0 }, m_anaMaskR[3] = { 0, 1, 1 };   // (SetAnaCustom)
        float        m_anaWL[3] = { 1, 0, 0 }, m_anaWR[3] = { 0, 0.5f, 0.5f };   // (... each eye's brightness from a pixel)
        float        m_anaTL[3] = { 1, 0, 0 }, m_anaTR[3] = { 0, 1, 1 };         // (... and its colour)
        int          m_anaChanL = 0, m_anaChanR = 1;                               // (... and the channel it's matched on)
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
        bool         m_halfWidthEyes = false;   // (Recovered Colour: full width since PSAnaPair made it affordable)
        bool         m_pairRefineOn  = true;
        bool         m_scrollReuseOn = false;
        bool         m_pyramidSkipOn = true;
        bool         m_changeSkipOn = true;
        float        m_vrYaw   = 0.0f;                   // VR viewer: yaw (radians)
        float        m_vrPitch = 0.0f;                   // VR viewer: pitch (radians)
        float        m_vrZoom  = 1.0f;                   // VR viewer: zoom (1=~90° HFOV)
        DXGI_FORMAT  m_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
}
