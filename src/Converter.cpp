#include "Converter.h"
#include <string>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstdio>

// The shaders, compiled at build time (shaders/Converter.hlsl; see CMakeLists.txt).
#include "Converter_VSMain.h"
#include "Converter_PSMain.h"
#include "Converter_PSAnaDisp.h"
#include "Converter_PSAnaDesc.h"
#include "Converter_PSAnaDescCoarse.h"
#include "Converter_PSDown.h"
#include "Converter_PSAnaRefine.h"
#include "Converter_PSAnaFill.h"
#include "Converter_PSAnaSmooth.h"
#include "Converter_PSAnaPair.h"
#include "Converter_PSAnaCompose.h"
#include "Converter_PSFmtAnaglyph.h"
#include "Converter_PSFmtQuilt.h"
#include "Converter_CSFmtAnaglyph.h"
#include "Converter_CSFmtChecker.h"
#include "Converter_CSFmtColumn.h"
#include "Converter_CSFmtRow.h"
#include "Converter_PSQuiltH.h"
#include "Converter_PSFmtFramePack.h"
#include "Converter_PSFmtChecker.h"
#include "Converter_PSFmtColumn.h"
#include "Converter_PSFmtRow.h"
#include "Converter_PSFmtTAB.h"
#include "Converter_PSFmtFullSBS.h"
#include "Converter_PSFmtHalfSBS.h"
#include "Converter_PSAnaBoxCheck.h"
#include "Converter_PSAnaBoxRows.h"
#include "Converter_PSAnaBoxMap.h"
#include "Converter_PSChange.h"
#include "Converter_PSChangeGrow.h"
#include "Converter_PSChangeGrow2.h"
#include "Converter_PSDownBox.h"
#include "Converter_PSDownFlat.h"
#include "Converter_PSFlatShrink.h"
#include "Converter_PSReach.h"
#include "Converter_PSChangeStatRows.h"
#include "Converter_PSChangeStat.h"
#include "Converter_PSScrollCost.h"
#include "Converter_PSScrollPick.h"
#include "Converter_PSScrollSum.h"
#include "Converter_PSChangeScrollQ.h"
#include "Converter_PSChangeScroll.h"
#include "Converter_PSAnaBoxShiftCost.h"
#include "Converter_PSAnaBoxShift.h"

using namespace srw;

namespace
{

    int FormatCode(StereoFormat f)
    {
        switch (f)
        {
        case StereoFormat::FullTAB:
        case StereoFormat::HalfTAB:           return 1;
        case StereoFormat::Anaglyph:          return 2;
        case StereoFormat::RowInterleaved:    return 3;
        case StereoFormat::ColumnInterleaved: return 4;
        case StereoFormat::Checkerboard:      return 5;
        case StereoFormat::Pulfrich:          return 6;
        case StereoFormat::FramePacking:      return 7;
        case StereoFormat::FrameSequential:   return 8;
        case StereoFormat::Quilt:             return 9;
        case StereoFormat::VR180TAB:
        case StereoFormat::VR180SBS:
        case StereoFormat::VR360TAB:
        case StereoFormat::VR360SBS:          return 10;  // VR equirect (sub-mode via cbuffer)
        case StereoFormat::FullSBS:           return 11;  // SBS + crop source to centre vertical 50%
        default:                              return 0;   // HalfSBS / unimplemented (sample source as-is)
        }
    }

    // Per-eye dimensions produced from a source of (w,h) in the given layout.
    // Quilt is intentionally NOT handled here — the converter overrides it at the
    // call site using its known cols/rows.
    void PerEyeSize(StereoFormat f, int w, int h, int& ew, int& eh)
    {
        switch (f)
        {
        case StereoFormat::FullSBS:
            // FullSBS now means "32:9 letterboxed content in the source":
            // the shader crops to the centre 50% vertical strip, and the
            // output texture is sized to that strip (h/2). Each eye ends
            // up at the source's natural per-eye aspect (16:9 from a
            // 16:9 source) instead of being stretched into Half-SBS
            // shape -- so downstream weaver / LG window samples it 1:1
            // and the content doesn't get anisotropic distortion.
            ew = w / 2; eh = h / 2; break;
        case StereoFormat::HalfSBS:
        case StereoFormat::Katanga:           ew = w / 2; eh = h;     break;   // (Katanga: side by side, full size per eye)
        case StereoFormat::FullTAB:
        case StereoFormat::HalfTAB:
        case StereoFormat::RowInterleaved:    ew = w;     eh = h / 2; break;
        case StereoFormat::ColumnInterleaved: ew = w / 2; eh = h;     break;
        case StereoFormat::VR180TAB:
        case StereoFormat::VR180SBS:
        case StereoFormat::VR360TAB:
        case StereoFormat::VR360SBS:
            // VR produces a synthesized perspective view per eye; size is
            // governed by the caller-supplied target pane (panel native dims),
            // not the equirect input. Fall through to "full size per eye"
            // here -- the Convert() path overrides via SetTargetPaneSize.
            ew = w; eh = h; break;
        default:                              ew = w;     eh = h;     break; // anaglyph/checker/pulfrich/quilt
        }
        if (ew < 1) ew = 1;
        if (eh < 1) eh = 1;
    }

    // 36 x 4 bytes = 144 (9 rows of 16); cbuffer ByteWidth must be a multiple of 16.
    struct CB { int format; int swap; float srcW; float srcH;
               int anaCombo; int anaMode; int pulfMode; int pulfEye;
               float ndTrans; float fpEyeFrac; float fpGapFrac; float convergence;
               float dispMaxUV; float coarseW; float coarseH; float propStride;
               float fpEyeAlign; int quiltCols; int quiltRows; int quiltLeftIdx;
               int quiltRightIdx; float paneW; float paneH; float quiltLBlend;
               float quiltRBlend; float vrYaw; float vrPitch; float vrZoom;
               int vrIs360; int vrIsSBS; float temporal; float lvlToSrcX;
               float lvlToSrcY; float changeSkip; float srcDecode; float pairRefine;
               float scrollOn; float boxesNew; float pad1; float pad2;
               float anaMaskL[4]; float anaMaskR[4]; float anaWL[4]; float anaWR[4]; float anaTL[4]; float anaTR[4]; };

}

Converter::~Converter()
{
    Shutdown();
}

bool Converter::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    m_device  = device;
    m_context = context;

    // Every shader from its build-time bytecode (nothing compiled here).
    HRESULT hr = device->CreateVertexShader(g_Converter_VSMain, sizeof(g_Converter_VSMain), nullptr, &m_vs);
    const struct { const BYTE* code; size_t size; ID3D11PixelShader** out; } ps[] = {
        { g_Converter_PSMain,         sizeof(g_Converter_PSMain),         &m_ps          },
        // Disparity passes (used only by the multi-scale anaglyph recovery mode).
        { g_Converter_PSAnaDisp,      sizeof(g_Converter_PSAnaDisp),      &m_psCoarse    },
        { g_Converter_PSAnaDesc,      sizeof(g_Converter_PSAnaDesc),      &m_psDesc      },
        { g_Converter_PSAnaDescCoarse, sizeof(g_Converter_PSAnaDescCoarse), &m_psDescCoarse },
        { g_Converter_PSDown,         sizeof(g_Converter_PSDown),         &m_psDown      },
        { g_Converter_PSAnaRefine,    sizeof(g_Converter_PSAnaRefine),    &m_psRefine    },
        { g_Converter_PSAnaFill,      sizeof(g_Converter_PSAnaFill),      &m_psFill      },
        { g_Converter_PSAnaSmooth,    sizeof(g_Converter_PSAnaSmooth),    &m_psSmooth    },
        { g_Converter_PSAnaPair,      sizeof(g_Converter_PSAnaPair),      &m_psPair      },
        { g_Converter_PSAnaCompose,   sizeof(g_Converter_PSAnaCompose),   &m_psAnaCompose },
        { g_Converter_PSFmtAnaglyph, sizeof(g_Converter_PSFmtAnaglyph), &m_psFmt[7] },
        { g_Converter_PSFmtQuilt, sizeof(g_Converter_PSFmtQuilt), &m_psFmt[8] },
        { g_Converter_PSQuiltH, sizeof(g_Converter_PSQuiltH), &m_psQuiltH },
        { g_Converter_PSFmtFramePack, sizeof(g_Converter_PSFmtFramePack), &m_psFmt[6] },
        { g_Converter_PSFmtChecker, sizeof(g_Converter_PSFmtChecker), &m_psFmt[5] },
        { g_Converter_PSFmtColumn, sizeof(g_Converter_PSFmtColumn), &m_psFmt[4] },
        { g_Converter_PSFmtRow, sizeof(g_Converter_PSFmtRow), &m_psFmt[3] },
        { g_Converter_PSFmtTAB, sizeof(g_Converter_PSFmtTAB), &m_psFmt[2] },
        { g_Converter_PSFmtFullSBS, sizeof(g_Converter_PSFmtFullSBS), &m_psFmt[1] },
        { g_Converter_PSFmtHalfSBS, sizeof(g_Converter_PSFmtHalfSBS), &m_psFmt[0] },
        { g_Converter_PSAnaBoxCheck,  sizeof(g_Converter_PSAnaBoxCheck),  &m_psBoxCheck  },
        { g_Converter_PSAnaBoxRows,   sizeof(g_Converter_PSAnaBoxRows),   &m_psBoxRows   },
        { g_Converter_PSAnaBoxMap,    sizeof(g_Converter_PSAnaBoxMap),    &m_psBoxMap    },
        { g_Converter_PSChange,       sizeof(g_Converter_PSChange),       &m_psChange    },
        { g_Converter_PSChangeGrow,   sizeof(g_Converter_PSChangeGrow),   &m_psChangeGrow },
        { g_Converter_PSChangeGrow2,  sizeof(g_Converter_PSChangeGrow2),  &m_psChangeGrow2 },
        { g_Converter_PSDownBox,      sizeof(g_Converter_PSDownBox),      &m_psDownBox },
        { g_Converter_PSDownFlat,     sizeof(g_Converter_PSDownFlat),     &m_psDownFlat },
        { g_Converter_PSFlatShrink,   sizeof(g_Converter_PSFlatShrink),   &m_psFlatShrink },
        { g_Converter_PSReach,        sizeof(g_Converter_PSReach),        &m_psReach },
        { g_Converter_PSChangeStatRows, sizeof(g_Converter_PSChangeStatRows), &m_psStatRows },
        { g_Converter_PSChangeStat,   sizeof(g_Converter_PSChangeStat),   &m_psStat },
        { g_Converter_PSScrollCost,   sizeof(g_Converter_PSScrollCost),   &m_psScrollCost },
        { g_Converter_PSScrollPick,   sizeof(g_Converter_PSScrollPick),   &m_psScrollPick },
        { g_Converter_PSScrollSum,    sizeof(g_Converter_PSScrollSum),    &m_psScrollSum },
        { g_Converter_PSChangeScrollQ, sizeof(g_Converter_PSChangeScrollQ), &m_psChangeScrollQ },
        { g_Converter_PSChangeScroll, sizeof(g_Converter_PSChangeScroll), &m_psChangeScroll },
        { g_Converter_PSAnaBoxShiftCost, sizeof(g_Converter_PSAnaBoxShiftCost), &m_psBoxShiftCost },
        { g_Converter_PSAnaBoxShift,  sizeof(g_Converter_PSAnaBoxShift),  &m_psBoxShift  },
    };
    for (const auto& p : ps)
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(p.code, p.size, nullptr, p.out);
    // (The both-eyes compute shaders: optional -- without them, the pixel shaders.)
    {
        struct { const void* code; size_t size; ID3D11ComputeShader** out; } cs[] = {
            { g_Converter_CSFmtAnaglyph, sizeof(g_Converter_CSFmtAnaglyph), &m_cs[0] },
            { g_Converter_CSFmtChecker,  sizeof(g_Converter_CSFmtChecker),  &m_cs[1] },
            { g_Converter_CSFmtColumn,   sizeof(g_Converter_CSFmtColumn),   &m_cs[2] },
            { g_Converter_CSFmtRow,      sizeof(g_Converter_CSFmtRow),      &m_cs[3] },
        };
        if (SUCCEEDED(hr)) for (const auto& c : cs) if (FAILED(device->CreateComputeShader(c.code, c.size, nullptr, c.out))) *c.out = nullptr;
    }
    if (FAILED(hr)) { ShowError("Converter shader creation failed."); return false; }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sd, &m_sampler);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(CB);   // MUST be a multiple of 16 (D3D11 cbuffer rule)
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    HRESULT bhr = device->CreateBuffer(&bd, nullptr, &m_cbuffer);
    if (FAILED(bhr) || !m_cbuffer)
    {
        char msg[160];
        _snprintf_s(msg, _TRUNCATE,
                    "Converter cbuffer allocation failed (hr=0x%08X, size=%d).",
                    (unsigned)bhr, (int)sizeof(CB));
        Log("%s", msg);
        ShowError(msg);
        return false;
    }

    return m_vs && m_ps && m_sampler && m_cbuffer;
}

void Converter::SetFormat(StereoFormat fmt, bool swapEyes, int anaCombo, int anaMode)
{
    m_fmt      = fmt;
    m_swap     = swapEyes;
    m_anaCombo = anaCombo;
    m_anaMode  = anaMode;
}

namespace
{
    // One tint table pair into rows `row` and `row + 1` of the 256-wide table
    // (null: row `row` cleared -- its alpha 0 means "none").
    void PutTint(std::vector<uint8_t>& px, int row, const uint8_t (*single)[4], const uint8_t* missing,
                 const uint8_t* pairA = nullptr, const uint8_t* pairB = nullptr)
    {
        uint8_t* a = &px[(size_t)row * 256 * 4];
        uint8_t* b = a + 256 * 4;
        if (!single || !missing) { memset(a, 0, 256 * 4 * 2); return; }
        memcpy(a, single, 256 * 4);
        for (int i = 0; i < 256; ++i)   // (.r the third channel; .g .b the pair along the curve)
        {
            b[i * 4] = missing[i];
            b[i * 4 + 1] = pairA ? pairA[i] : missing[i]; b[i * 4 + 2] = pairB ? pairB[i] : missing[i];
            b[i * 4 + 3] = 255;
        }
    }
}

void Converter::SetAnaTint(const uint8_t (*single)[4], const uint8_t* missing)
{
    std::vector<uint8_t> before(m_tintPx.begin(), m_tintPx.begin() + 256 * 4 * 2);
    PutTint(m_tintPx, 0, single, missing);
    if (!std::equal(before.begin(), before.end(), m_tintPx.begin())) m_anaTablesDirty = true;
}

void Converter::SetAnaBoxes(const AnaBox* boxes, int n)
{
    n = (std::max)(0, (std::min)(n, kMaxBoxes));
    float bx[(1 + 2 * kMaxBoxes) * 4] = {};
    bx[0] = (float)n;
    std::vector<uint8_t> tints(m_tintPx.begin() + 256 * 4 * 2, m_tintPx.end());
    for (int i = 0; i < n; ++i)
    {
        const AnaBox& b = boxes[i];
        const bool tinted = b.kind == 2 && b.single && b.missing;
        float* r = &bx[(1 + 2 * i) * 4];
        r[0] = b.u0; r[1] = b.v0; r[2] = b.u1; r[3] = b.v1;
        r[4] = tinted ? 2.0f : (b.kind == 1 ? 1.0f : 0.0f);
        r[5] = (float)(2 + 2 * i);   // (its tint's first row)
        r[6] = b.checkInsetU; r[7] = b.checkInsetV;
        PutTint(m_tintPx, 2 + 2 * i, tinted ? b.single : nullptr, tinted ? b.missing : nullptr, b.pairA, b.pairB);
    }
    for (int i = n; i < kMaxBoxes; ++i) PutTint(m_tintPx, 2 + 2 * i, nullptr, nullptr);
    if (memcmp(bx, m_boxPx, sizeof(bx)) != 0 ||
        !std::equal(tints.begin(), tints.end(), m_tintPx.begin() + 256 * 4 * 2))
    {
        memcpy(m_boxPx, bx, sizeof(bx));
        m_anaTablesDirty = true;
    }
}

// The tint tables and boxes to the GPU when they changed (Convert, before the
// compose). m_tintHash: 0 when nothing is in use, otherwise a hash of both
// (part of the settings key: a change re-converts an unchanged source).
void Converter::UploadAnaTables()
{
    if (!m_anaTablesDirty) return;
    m_anaTablesDirty = false;
    const bool any = m_boxPx[0] > 0.0f || m_tintPx[3] != 0;
    if (!any) { m_tintHash = 0; return; }
    if (!m_tintTex)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 256; td.Height = kTintRows; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;   // (sRGB-encoded values, read as they are)
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC bd = td;
        bd.Width = 1 + 2 * kMaxBoxes; bd.Height = 1; bd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_tintTex)) ||
            FAILED(m_device->CreateShaderResourceView(m_tintTex, nullptr, &m_tintSRV)) ||
            FAILED(m_device->CreateTexture2D(&bd, nullptr, &m_boxTex)) ||
            FAILED(m_device->CreateShaderResourceView(m_boxTex, nullptr, &m_boxSRV)))
        {
            SAFE_RELEASE(m_tintSRV); SAFE_RELEASE(m_tintTex); SAFE_RELEASE(m_boxSRV); SAFE_RELEASE(m_boxTex);
            m_tintHash = 0; return;
        }
    }
    m_context->UpdateSubresource(m_tintTex, 0, nullptr, m_tintPx.data(), 256 * 4, 0);
    m_context->UpdateSubresource(m_boxTex, 0, nullptr, m_boxPx, sizeof(m_boxPx), 0);
    uint64_t h = 1469598103934665603ull;   // (FNV-1a)
    for (uint8_t v : m_tintPx) h = (h ^ v) * 1099511628211ull;
    for (size_t i = 0; i < sizeof(m_boxPx); ++i) h = (h ^ ((const uint8_t*)m_boxPx)[i]) * 1099511628211ull;
    m_tintHash = h ? h : 1;
    // (The whole picture's tables alone -- SettingsKey: new boxes redraw only
    // the blocks they touch, PSChange's g_boxesNew, not the whole frame.)
    uint64_t hw = 1469598103934665603ull;
    for (size_t i = 0; i < 2 * 256 * 4; ++i) hw = (hw ^ m_tintPx[i]) * 1099511628211ull;
    m_wholeTintHash = hw;
}

void Converter::SetPulfrich(PulfrichMode mode, int affectedEye, float ndTransmission, int delayFrames)
{
    m_pulfMode  = mode;
    m_pulfEye   = affectedEye;
    m_ndTrans   = ndTransmission;
    m_pulfDelay = delayFrames < 1 ? 1 : (delayFrames > kHistory - 1 ? kHistory - 1 : delayFrames);
}

void Converter::SetFramePacking(float eyeFrac, float gapFrac, float eyeAlign)
{
    m_fpEyeFrac = eyeFrac;
    m_fpGapFrac = gapFrac;
    m_fpEyeAlign = eyeAlign;
}

void Converter::SetQuilt(int cols, int rows, int leftIdx, int rightIdx,
                         float leftBlend, float rightBlend)
{
    m_quiltCols     = cols  > 0 ? cols  : 1;
    m_quiltRows     = rows  > 0 ? rows  : 1;
    const int total = m_quiltCols * m_quiltRows;
    auto clampIx    = [total](int v) { return v < 0 ? 0 : (v >= total ? total - 1 : v); };
    m_quiltLeftIdx    = clampIx(leftIdx);
    m_quiltRightIdx   = clampIx(rightIdx);
    auto clampBlend   = [](float b) { return b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b); };
    m_quiltLeftBlend  = clampBlend(leftBlend);
    m_quiltRightBlend = clampBlend(rightBlend);
}

bool Converter::EnsureOutput(int width, int height)
{
    if (m_outTex && width == m_outWidth && height == m_outHeight)
        return false;

    ReleaseOutput();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)width;
    td.Height           = (UINT)height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    // (Typeless: the render target and shader views are the sRGB format; the
    // both-eyes compute shaders write through a plain UNORM view, encoding
    // themselves -- a UAV can't be sRGB.)
    td.Format           = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_outTex))) return false;
    D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = m_format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = m_format; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(m_device->CreateRenderTargetView(m_outTex, &rd, &m_outRTV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_outTex, &vd, &m_outSRV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateUnorderedAccessView(m_outTex, &ud, &m_outUAV))) m_outUAV = nullptr;   // (then the pixel shaders only)

    m_outWidth  = width;
    m_outHeight = height;
    return true;
}

bool Converter::Convert(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight,
                        bool& outputResized)
{
    outputResized = false;
    if (!source || !m_ps || srcWidth <= 0 || srcHeight <= 0)
        return false;

    // Temporal formats need the frame-history ring: Pulfrich (delayed/ND eye) and
    // Frame-sequential (the previous frame IS the other eye).
    const bool pulfrich = (m_fmt == StereoFormat::Pulfrich);
    const bool temporal = pulfrich || (m_fmt == StereoFormat::FrameSequential);
    if (!temporal) ReleaseHistory();   // free the ring when not needed

    // Same picture, same settings as the last conversion: its output still
    // stands. (Recovering a 4K anaglyph costs milliseconds of GPU per frame;
    // a still picture now costs it once.)
    UploadAnaTables();   // (before the key: it holds their hash)
    const uint64_t key = SettingsKey(source, srcWidth, srcHeight);
    const bool sameSettings = key == m_lastKey;   // (else the recovery draws a whole frame: see PSChange)
    // (The boxes of a page of several pictures judged afresh -- about every
    // second while it scrolls: the blocks they touch are redrawn, see PSChange.)
    const bool boxesNew = m_tintHash != m_lastTintHash;
    // (A snapshot asked for on an unchanged picture: the last 1/16 source is it.)
    if (m_snapRequested && m_srcVersion != 0 && m_srcVersion == m_lastVersion && m_src16.tex && m_snapPending.tex)
    {
        m_context->CopyResource(m_snapPending.tex, m_src16.tex);
        m_snapRequested = false; m_snapPendingValid = true;
    }
    if (!temporal && m_srcVersion != 0 && m_srcVersion == m_lastVersion && key == m_lastKey && !boxesNew && m_outSRV)
        return true;

    int ew = 0, eh = 0;
    PerEyeSize(m_fmt, srcWidth, srcHeight, ew, eh);
    // Full top-and-bottom: each eye is a whole frame, one above the other -- a
    // picture twice as tall as it is wide-screen. From a file that's the
    // source's own shape. On a captured screen it's shown pillarboxed in the
    // middle half of the width (as Full SBS is letterboxed): that half is the
    // picture. (It used to be taken as Half top-and-bottom: stretched tall.)
    const bool fullTabPillar = (m_fmt == StereoFormat::FullTAB && srcWidth >= srcHeight);
    if (fullTabPillar) ew = srcWidth / 2;
    if (m_fmt == StereoFormat::FramePacking)   // each eye is eyeFrac of the source height
        eh = (int)(srcHeight * m_fpEyeFrac + 0.5f);
    if (m_fmt == StereoFormat::Quilt)
    {
        // For Quilt, sizing the SBS pane to the SR PANEL'S per-eye dims means
        // the weaver samples 1:1 with no stretch -- the shader pillar/letterboxes
        // the view inside each pane. Falling back to the view's native cell size
        // when we don't know the panel dims lets the weaver do its own resample
        // (anamorphic but at least not crashing on a degenerate size).
        if (m_targetPaneW > 0 && m_targetPaneH > 0)
        {
            ew = m_targetPaneW;
            eh = m_targetPaneH;
        }
        else
        {
            const int qc = m_quiltCols > 0 ? m_quiltCols : 1;
            const int qr = m_quiltRows > 0 ? m_quiltRows : 1;
            ew = srcWidth  / qc;
            eh = srcHeight / qr;
        }
    }
    if (IsVRFormat(m_fmt))
    {
        // VR synthesises a perspective view per eye -- size the output to the
        // SR panel's per-eye pane (passed in via SetTargetPaneSize) so the
        // weaver samples 1:1. If we don't have those dims, fall back to a
        // sane 16:9 output sized off the source so we at least render.
        if (m_targetPaneW > 0 && m_targetPaneH > 0)
        {
            ew = m_targetPaneW;
            eh = m_targetPaneH;
        }
        else
        {
            ew = 1920;
            eh = 1080;
        }
    }
    // Recovered Colour only: each eye at most half the source's width. Its
    // disparity search and decode scale with the eye's size (full-width eyes
    // were the single biggest cost of a frame). Everything else keeps every
    // column it has: the SR display's slanted lens gives each eye more than
    // half the panel's columns, so a full-width TAB / row / anaglyph eye shows
    // more detail than a halved one -- and converting it costs little.
    const bool recoveredColour = (m_fmt == StereoFormat::Anaglyph && m_anaMode == 4);
    if (m_halfWidthEyes && recoveredColour && ew > (srcWidth + 1) / 2)
        ew = (srcWidth + 1) / 2;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    outputResized = EnsureOutput(ew * 2, eh);
    if (!m_outRTV)
        return false;

    ID3D11ShaderResourceView* delayedSRV = nullptr;
    if (temporal)
    {
        EnsureHistory(srcWidth, srcHeight);
        // Frame-sequential pairs the current frame with the immediately previous one.
        const int delay = (m_fmt == StereoFormat::FrameSequential) ? 1 : m_pulfDelay;
        const int delayed = (m_histWrite - delay + kHistory) % kHistory;
        delayedSRV = m_histSRV[delayed];
    }

    // Multi-scale anaglyph recovery: build a coarse->fine disparity pyramid first.
    const bool anaRecover = (m_fmt == StereoFormat::Anaglyph && m_anaMode == 4);   // (any colour pair)
    int w16 = 0, h16 = 0, w4 = 0, h4 = 0;
    if (anaRecover)
    {
        // Whole 16-px blocks, rounded up (a little past the picture's edge),
        // and the 1/4 level exactly 4x the 1/16: the levels' grids sit
        // exactly on the picture's pixels whatever its size. (Rounded down,
        // each level separately, they drifted against it by up to a block
        // -- differently for every size, so the result changed with it.)
        w16 = (srcWidth  + 15) / 16; if (w16 < 1) w16 = 1;
        h16 = (srcHeight + 15) / 16; if (h16 < 1) h16 = 1;
        w4  = w16 * 4;
        h4  = h16 * 4;
        EnsureDispTarget(m_disp0, w16, h16);
        EnsureDispTarget(m_disp1, w4,  h4);
        EnsureDispTarget(m_disp2, w4,  h4);
        EnsureDispTarget(m_dispF, w4,  h4);
        EnsureDispTarget(m_src4,  w4,  h4);
        EnsureDispTarget(m_src16, w16, h16);
        // (What changed since the last frame -- PSChange / PSChangeGrow.)
        if (EnsureDispTarget(m_change, w16, h16) | EnsureDispTarget(m_changeGrow, w16, h16) | EnsureDispTarget(m_changeGrow2, w16, h16)) m_changeValid = false;
        if (EnsureDispTarget(m_reach, w16, h16) | EnsureDispTarget(m_reachNext, w16, h16)) m_reachValid = false;
        // The per-picture boxes (SetAnaBoxes; manual Anaglyph on a page of several)
        // and their scroll-following snapshots -- only when used: an Auto Stereo
        // region's converter never is, and every texture made is a driver
        // allocation in the frame a new picture turns up (20+ ms stalls).
        if (m_boxPx[0] > 0.0f || m_snapRequested || m_snapPendingValid)
        {
            EnsureDispTarget(m_boxValid, kMaxBoxes, 1);   // (PSAnaBoxCheck)
            EnsureDispTarget(m_boxRows, kMaxBoxes, h16);  // (PSAnaBoxRows)
            EnsureDispTarget(m_boxMap, w16, h16);         // (PSAnaBoxMap)
            if (EnsureDispTarget(m_boxMapPrev, w16, h16)) m_changeValid = false;
            if (EnsureDispTarget(m_snapPending, w16, h16)) m_snapPendingValid = false;
            if (EnsureDispTarget(m_snapActive, w16, h16)) m_snapActiveValid = false;
            EnsureDispTarget(m_boxShiftCost, kMaxBoxes, 97);   // (2 x kBoxShift + 1, Converter.hlsl)
            EnsureDispTarget(m_boxShift, kMaxBoxes, 1);
        }
        // (A remade previous-frame target holds nothing yet.)
        if (EnsureDispTarget(m_dispPrev, w4, h4) | EnsureDispTarget(m_src4Prev, w4, h4)) m_dispPrevValid = false;
    }
    else ReleaseDisparity();

    // Recovered Colour given the capture's own frame (zero-copy: an encoded
    // UNORM view, SetSourceEncoded). Windows hands over a new frame every
    // refresh whether or not anything on screen changed (and calls the whole
    // screen changed: SR Loom's own window is on it), so most frames are the
    // one before again. Copying each one to read it and averaging it down just
    // to find that out was the cost of every frame. Instead: the frame
    // averaged down as it is (no decode: one cheap pass), compared with last
    // frame's -- and only if something changed is it copied to a texture the
    // passes can read decoded (m_work: reading the frame itself there costs
    // three times the whole conversion), and converted. `source` is that copy
    // from here on; directSrc the frame.
    ID3D11ShaderResourceView* directSrc = nullptr;
    ID3D11Texture2D* directTex = nullptr;
    if (anaRecover && m_srcEncoded && m_preCheckOn && source)
    {
        ID3D11Resource* res = nullptr; source->GetResource(&res);
        if (res) { res->QueryInterface(&directTex); res->Release(); }
        D3D11_TEXTURE2D_DESC sd{}; if (directTex) directTex->GetDesc(&sd);
        DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, srgb = DXGI_FORMAT_UNKNOWN;
        if (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM || sd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        { typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS; srgb = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
        else if (sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        { typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; srgb = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        if (directTex && typeless != DXGI_FORMAT_UNKNOWN && (int)sd.Width == srcWidth && (int)sd.Height == srcHeight &&
            sd.MipLevels == 1 && sd.ArraySize == 1 && sd.SampleDesc.Count == 1)
        {
            if (!m_workTex || m_workW != srcWidth || m_workH != srcHeight || m_workFmt != typeless)
            {
                SAFE_RELEASE(m_workSRV); SAFE_RELEASE(m_workTex);
                D3D11_TEXTURE2D_DESC wd{};
                wd.Width = (UINT)srcWidth; wd.Height = (UINT)srcHeight; wd.MipLevels = 1; wd.ArraySize = 1;
                wd.Format = typeless; wd.SampleDesc.Count = 1; wd.Usage = D3D11_USAGE_DEFAULT; wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SHADER_RESOURCE_VIEW_DESC wv{};
                wv.Format = srgb; wv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; wv.Texture2D.MipLevels = 1;
                if (SUCCEEDED(m_device->CreateTexture2D(&wd, nullptr, &m_workTex)) &&
                    FAILED(m_device->CreateShaderResourceView(m_workTex, &wv, &m_workSRV)))
                    SAFE_RELEASE(m_workTex);
                m_workW = srcWidth; m_workH = srcHeight; m_workFmt = typeless;
                m_encPrevValid = false; m_changeValid = false;   // (the copy holds nothing yet: a whole frame)
            }
            if (EnsureDispTarget(m_enc4, w4, h4, DXGI_FORMAT_R16G16B16A16_UNORM) | EnsureDispTarget(m_enc4Prev, w4, h4, DXGI_FORMAT_R16G16B16A16_UNORM)) m_encPrevValid = false;
            if (m_workSRV && m_enc4.rtv && m_enc4Prev.rtv && m_psDownBox) { directSrc = source; source = m_workSRV; }
        }
        if (!directSrc) SAFE_RELEASE(directTex);
    }
    if (!directSrc && m_workTex)
    {
        SAFE_RELEASE(m_workSRV); SAFE_RELEASE(m_workTex);
        ReleaseDispTarget(m_enc4); ReleaseDispTarget(m_enc4Prev);
        m_encPrevValid = false;
    }

    // Scroll reuse (see PSChangeScroll): last frame's source is kept, full
    // size, to compare this one with. Only where an output row is a source row
    // and the source is a plain texture of the picture's size.
    bool scrollWanted = false;
    ID3D11Texture2D* srcTex2D = nullptr; UINT srcMip = 0;
    if (anaRecover && m_scrollReuseOn && m_changeSkipOn && eh == srcHeight && source)
    {
        ID3D11Resource* res = nullptr; source->GetResource(&res);
        if (res) { res->QueryInterface(&srcTex2D); res->Release(); }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; source->GetDesc(&vd);
        D3D11_TEXTURE2D_DESC sd{}; if (srcTex2D) srcTex2D->GetDesc(&sd);
        if (srcTex2D && vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sd.SampleDesc.Count == 1 &&
            (int)(sd.Width >> vd.Texture2D.MostDetailedMip) == srcWidth && (int)(sd.Height >> vd.Texture2D.MostDetailedMip) == srcHeight)
        {
            srcMip = vd.Texture2D.MostDetailedMip;
            if (!m_srcPrevTex || m_srcPrevW != srcWidth || m_srcPrevH != srcHeight || m_srcPrevFmt != sd.Format || m_srcPrevViewFmt != vd.Format)
            {
                SAFE_RELEASE(m_srcPrevSRV); SAFE_RELEASE(m_srcPrevTex);
                m_srcPrevValid = false;
                D3D11_TEXTURE2D_DESC pd{};
                pd.Width = (UINT)srcWidth; pd.Height = (UINT)srcHeight; pd.MipLevels = 1; pd.ArraySize = 1;
                pd.Format = sd.Format; pd.SampleDesc.Count = 1; pd.Usage = D3D11_USAGE_DEFAULT; pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SHADER_RESOURCE_VIEW_DESC pv{};
                pv.Format = vd.Format; pv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; pv.Texture2D.MipLevels = 1;
                if (SUCCEEDED(m_device->CreateTexture2D(&pd, nullptr, &m_srcPrevTex)) &&
                    FAILED(m_device->CreateShaderResourceView(m_srcPrevTex, &pv, &m_srcPrevSRV)))
                    SAFE_RELEASE(m_srcPrevTex);
                m_srcPrevW = srcWidth; m_srcPrevH = srcHeight; m_srcPrevFmt = sd.Format; m_srcPrevViewFmt = vd.Format;
            }
            scrollWanted = m_srcPrevSRV != nullptr;
        }
    }
    if (!scrollWanted && (m_srcPrevTex || m_outPrevTex)) ReleaseScroll();

    const float dispMaxUV = 0.06f;
    float changeSkip = 0.0f;   // (1: the compose redraws only changed blocks -- set below)
    // 1 while a pass reads the source itself at t0 and it's sRGB-encoded
    // UNORM (SetSourceEncoded). The recovery's own levels are linear already.
    const float srcDecode = (m_srcEncoded && !directSrc) ? 1.0f : 0.0f;   // (directSrc: `source` is the decoded-view copy)
    float decode = 0.0f;
    float pairRefine = 0.0f;   // (1 while the compose reads the per-pair refine: m_pair, PSAnaPair)
    float scrollOn = 0.0f;     // (1 while blocks that only scrolled take last frame's output: PSChangeScroll)
    bool statsNow = false;     // (this frame's change statistics are being taken: TakeChangeStats)
    bool flatNow = false;      // (the 1/4 level's plain grey map was made this frame: PSDownFlat)
    ID3D11ShaderResourceView* pyramidMask = nullptr;   // (the 1/4 passes' part to work out: PSChangeGrow2)
    auto uploadCB = [&](float coarseW, float coarseH, float prop = 0.0f)
    {
        CB cb{ fullTabPillar ? 12 : FormatCode(m_fmt), m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
               m_anaCombo, m_anaMode, (int)m_pulfMode, m_pulfEye,
               m_ndTrans, m_fpEyeFrac, m_fpGapFrac, m_convergence,
               dispMaxUV, coarseW, coarseH, prop,
               m_fpEyeAlign, m_quiltCols, m_quiltRows, m_quiltLeftIdx,
               m_quiltRightIdx, (float)ew, (float)eh, m_quiltLeftBlend,
               m_quiltRightBlend,
               m_vrYaw, m_vrPitch, m_vrZoom,
               IsVR360(m_fmt) ? 1 : 0, IsVRSBS(m_fmt) ? 1 : 0,
               m_dispPrevValid ? 1.0f : 0.0f,
               anaRecover ? 16.0f * w16 / srcWidth : 1.0f,
               anaRecover ? 16.0f * h16 / srcHeight : 1.0f, changeSkip, decode, pairRefine, scrollOn, boxesNew ? 1.0f : 0.0f };
        for (int c = 0; c < 3; ++c)   // (the Custom pair: SetAnaCustom)
        {
            cb.anaMaskL[c] = m_anaMaskL[c]; cb.anaMaskR[c] = m_anaMaskR[c]; cb.anaWL[c] = m_anaWL[c]; cb.anaWR[c] = m_anaWR[c];
            cb.anaTL[c] = m_anaTL[c]; cb.anaTR[c] = m_anaTR[c];
            cb.anaWL[3] = (float)m_anaChanL; cb.anaWR[3] = (float)m_anaChanR;
        }
        m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &cb, 0, 0);
    };

    // Shared pipeline state.
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vs, nullptr, 0);
    m_context->PSSetSamplers(0, 1, &m_sampler);
    m_context->PSSetConstantBuffers(0, 1, &m_cbuffer);

    // Render one disparity-pyramid level: source at t0, an optional coarser-level
    // disparity at t2, output to a DispTarget.
    auto runDispPass = [&](ID3D11PixelShader* ps, const DispTarget& rt,
                           ID3D11ShaderResourceView* prior, ID3D11ShaderResourceView* src)
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)rt.w; vp.Height = (FLOAT)rt.h; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(ps, nullptr, 0);
        m_context->OMSetRenderTargets(1, &rt.rtv, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { src, nullptr, prior };
        m_context->PSSetShaderResources(0, 3, srvs);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    };

    // (Recovery: GPU timestamps between its stages, for the perf log.)
    int tSlot = -1;
    if (anaRecover)
    {
        CollectTimes();
        TimeSet& t = m_times[m_timeNext];
        if (!t.disjoint) { D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; m_device->CreateQuery(&qd, &t.disjoint); }
        if (t.disjoint && !t.pending) { tSlot = m_timeNext; m_context->Begin(t.disjoint); TimeMark(tSlot, 0); }
    }
    if (anaRecover && m_disp0.rtv && m_disp1.rtv && m_disp2.rtv && m_dispF.rtv && m_src4.rtv && m_src16.rtv && m_psDown &&
        m_psDesc && EnsureDescTargets(w4, h4) && m_psDescCoarse && EnsureDescSet(m_descCoarse, 2 * w16 + 1, h16))
    {
        const float zero4[4] = {};
        // (For the perf log, TakeChangeStats: cleared outside any predicate -- a
        // frame where nothing changed reads all 0.)
        statsNow = m_statsOn && m_psStatRows && m_psStat &&
                   (EnsureDispTarget(m_statRows, 1, h16, DXGI_FORMAT_R32G32B32A32_FLOAT), m_statRows.rtv) &&
                   (EnsureDispTarget(m_stat, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT), m_stat.rtv);
        if (statsNow) m_context->ClearRenderTargetView(m_stat.rtv, zero4);
        if (directSrc)
        {
            // The frame itself averaged down, undecoded, against last frame's:
            // nothing changed -- everything below is skipped, the copy too.
            decode = 0.0f; uploadCB((float)w4, (float)h4);
            runDispPass(m_psDownBox, m_enc4, nullptr, directSrc);
            if (!m_prePred)
            {
                D3D11_QUERY_DESC pd{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
                m_device->CreatePredicate(&pd, &m_prePred);
            }
            // (A whole frame is due, the boxes were judged afresh, a snapshot is
            // wanted: converted regardless.)
            if (m_changeSkipOn && m_changeValid && sameSettings && !boxesNew && !outputResized && m_dispPrevValid &&
                m_encPrevValid && !m_snapRequested && m_prePred && m_psChange && m_change.rtv)
            {
                m_context->ClearRenderTargetView(m_change.rtv, zero4);
                m_context->PSSetShaderResources(8, 1, &m_enc4Prev.srv);
                m_context->Begin(m_prePred);
                runDispPass(m_psChange, m_change, nullptr, m_enc4.srv);
                m_context->End(m_prePred);
                ID3D11ShaderResourceView* nul = nullptr;
                m_context->PSSetShaderResources(8, 1, &nul);
                m_context->SetPredication(m_prePred, FALSE);
            }
            m_context->CopyResource(m_enc4Prev.tex, m_enc4.tex);
            m_context->CopyResource(m_workTex, directTex);
            m_encPrevValid = true;
        }
        // The source averaged down to 1/4 and 1/16: what the passes below read.
        decode = srcDecode; uploadCB((float)w4, (float)h4);
        // (... and the 1/4 level's plain grey texels with it, shrunk: PSDownFlat /
        // PSFlatShrink -- the pair and smoothing passes skip what's plain.)
        flatNow = m_flatOn && m_psDownFlat && m_psFlatShrink &&
                  (EnsureDispTarget(m_flat4, w4, h4, DXGI_FORMAT_R8_UNORM), m_flat4.rtv) &&
                  (EnsureDispTarget(m_flatS, w4, h4, DXGI_FORMAT_R8G8_UNORM), m_flatS.rtv);
        if (flatNow)
        {
            D3D11_VIEWPORT vp{}; vp.Width = (FLOAT)w4; vp.Height = (FLOAT)h4; vp.MaxDepth = 1.0f;
            ID3D11RenderTargetView* rt[2] = { m_src4.rtv, m_flat4.rtv };
            m_context->PSSetShader(m_psDownFlat, nullptr, 0);
            m_context->OMSetRenderTargets(2, rt, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShaderResources(0, 1, &source);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nul = nullptr;
            m_context->PSSetShaderResources(0, 1, &nul);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            runDispPass(m_psFlatShrink, m_flatS, nullptr, m_flat4.srv);
        }
        else
        runDispPass(m_psDown, m_src4,  nullptr, source);
        decode = 0.0f; uploadCB((float)w4, (float)h4);
        runDispPass(m_psDown, m_src16, nullptr, m_src4.srv);
        // (The frame the check is judging: the boxes' reference, once they come.)
        if (m_snapRequested && m_snapPending.tex)
        {
            m_context->CopyResource(m_snapPending.tex, m_src16.tex);
            m_snapRequested = false; m_snapPendingValid = true;
        }
        // Whether each black-and-white / one-colour box still holds that this
        // frame (PSAnaBoxCheck, on the 1/16 block averages; the compose reads it).
        if (m_tintHash && m_boxPx[0] > 0.0f && m_psBoxCheck && m_psBoxRows && m_psBoxMap &&
            m_boxValid.rtv && m_boxRows.rtv && m_boxMap.rtv)
        {
            uploadCB((float)w16, (float)h16);
            ID3D11ShaderResourceView* ana[2] = { m_tintSRV, m_boxSRV };
            m_context->PSSetShaderResources(10, 2, ana);
            // How far each box's picture has scrolled since the check (PSAnaBoxShift,
            // at t17 for the passes below and the compose). No snapshot yet: not moved.
            ID3D11ShaderResourceView* shift = nullptr;
            if (m_snapActiveValid && m_psBoxShiftCost && m_psBoxShift && m_boxShiftCost.rtv && m_boxShift.rtv)
            {
                m_context->PSSetShaderResources(16, 1, &m_snapActive.srv);
                runDispPass(m_psBoxShiftCost, m_boxShiftCost, nullptr, m_src16.srv);
                runDispPass(m_psBoxShift, m_boxShift, m_boxShiftCost.srv, nullptr);
                ID3D11ShaderResourceView* nul = nullptr;
                m_context->PSSetShaderResources(16, 1, &nul);
                shift = m_boxShift.srv;
            }
            m_context->PSSetShaderResources(17, 1, &shift);
            runDispPass(m_psBoxRows, m_boxRows, nullptr, m_src16.srv);        // each box, row by row
            runDispPass(m_psBoxCheck, m_boxValid, m_boxRows.srv, nullptr);    // ... summed: still so?
            m_context->PSSetShaderResources(12, 1, &m_boxValid.srv);
            runDispPass(m_psBoxMap, m_boxMap, nullptr, nullptr);              // which touch each 16x16 block
            ID3D11ShaderResourceView* nul3[3] = {};
            m_context->PSSetShaderResources(10, 3, nul3);
        }
        // What changed since the last frame (PSChange): only those blocks (and
        // what's within the search's reach of them, PSChangeGrow) are drawn
        // again -- and when nothing did, the occlusion predicate skips all of
        // the recovery below, the compose too: last frame's output stands. (A
        // page that isn't moving still arrives as new frames.) A whole frame
        // after anything that makes the old output wrong: new settings, a new
        // size, the first frame.
        const bool boxesNow = m_tintHash && m_boxPx[0] > 0.0f && m_boxMap.srv;
        if (!m_changePred)
        {
            D3D11_QUERY_DESC pd{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
            m_device->CreatePredicate(&pd, &m_changePred);
        }
        if (m_changeSkipOn && m_changeValid && sameSettings && !(boxesNew && !boxesNow) && !outputResized && m_dispPrevValid && m_changePred &&
            m_psChange && m_psChangeGrow && m_change.rtv && m_changeGrow.rtv)
        {
            const float zero[4] = {};
            m_context->ClearRenderTargetView(m_change.rtv, zero);
            ID3D11ShaderResourceView* prevQ = m_src4Prev.srv;
            ID3D11ShaderResourceView* maps[2] = { boxesNow ? m_boxMap.srv : nullptr, boxesNow ? m_boxMapPrev.srv : nullptr };
            m_context->PSSetShaderResources(8, 1, &prevQ);
            m_context->PSSetShaderResources(13, 2, maps);
            m_context->Begin(m_changePred);
            runDispPass(m_psChange, m_change, nullptr, m_src4.srv);
            m_context->End(m_changePred);
            ID3D11ShaderResourceView* nul2[2] = {};
            m_context->PSSetShaderResources(8, 1, nul2);
            m_context->PSSetShaderResources(13, 2, nul2);
            m_context->SetPredication(m_changePred, FALSE);   // (nothing changed: all below is skipped)
            // A page being scrolled: every block changed, but most of them are
            // last frame's, moved. How far (PSScrollCost / PSScrollPick), then
            // which blocks are exactly that (PSChangeScroll, in place of
            // PSChange's answer): they take last frame's output from where it
            // was, and only the rest is recovered. Last frame's output is
            // copied for that only when the picture did scroll (m_scrollPred).
            if (scrollWanted && m_srcPrevValid && m_psScrollCost && m_psScrollPick && m_psChangeScroll)
            {
                EnsureDispTarget(m_scrollRows, 2 * kScroll + 1, 54);
                EnsureDispTarget(m_scrollCost, 2 * kScroll + 1, 1);
                EnsureDispTarget(m_changeQ, w4, h4, DXGI_FORMAT_R8G8_UNORM);
                EnsureDispTarget(m_scroll, 1, 1);
                if (!m_outPrevTex && m_outTex)
                {
                    D3D11_TEXTURE2D_DESC od{}; m_outTex->GetDesc(&od);
                    od.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SHADER_RESOURCE_VIEW_DESC prevView{}; prevView.Format = m_format; prevView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; prevView.Texture2D.MipLevels = 1;
                    if (SUCCEEDED(m_device->CreateTexture2D(&od, nullptr, &m_outPrevTex)) &&
                        FAILED(m_device->CreateShaderResourceView(m_outPrevTex, &prevView, &m_outPrevSRV)))
                        SAFE_RELEASE(m_outPrevTex);
                }
                if (!m_scrollPred)
                {
                    D3D11_QUERY_DESC pd{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
                    m_device->CreatePredicate(&pd, &m_scrollPred);
                }
                if (m_scrollRows.rtv && m_scrollCost.rtv && m_scroll.rtv && m_changeQ.rtv && m_outPrevSRV && m_scrollPred && m_psScrollSum && m_psChangeScrollQ)
                {
                    m_context->PSSetShaderResources(19, 1, &m_srcPrevSRV);
                    runDispPass(m_psScrollCost, m_scrollRows, nullptr, source);
                    runDispPass(m_psScrollSum, m_scrollCost, nullptr, m_scrollRows.srv);
                    m_context->ClearRenderTargetView(m_scroll.rtv, zero);
                    m_context->Begin(m_scrollPred);
                    runDispPass(m_psScrollPick, m_scroll, nullptr, m_scrollCost.srv);
                    m_context->End(m_scrollPred);
                    m_context->PSSetShaderResources(20, 1, &m_scroll.srv);
                    runDispPass(m_psChangeScrollQ, m_changeQ, nullptr, source);
                    {
                        // (Not scrolling, it judges as PSChange: the 1/4 sources.)
                        ID3D11ShaderResourceView* q[2] = { m_src4Prev.srv, m_src4.srv };
                        m_context->PSSetShaderResources(8, 2, q);
                        m_context->PSSetShaderResources(13, 2, maps);   // (the boxes' blocks: redrawn, but for those well inside one)
                        ID3D11ShaderResourceView* boxTex = boxesNow ? m_boxSRV : nullptr;
                        m_context->PSSetShaderResources(11, 1, &boxTex);
                        runDispPass(m_psChangeScroll, m_change, nullptr, m_changeQ.srv);
                        m_context->PSSetShaderResources(13, 2, nul2);
                        m_context->PSSetShaderResources(11, 1, nul2);
                        ID3D11ShaderResourceView* nulq[2] = {};
                        m_context->PSSetShaderResources(8, 2, nulq);
                    }
                    m_context->SetPredication(m_scrollPred, FALSE);
                    m_context->CopyResource(m_outPrevTex, m_outTex);
                    m_context->SetPredication(m_changePred, FALSE);
                    m_context->PSSetShaderResources(21, 1, &m_outPrevSRV);
                    scrollOn = 1.0f;
                }
            }
            uploadCB((float)w16, (float)h16);
            {
                // (Each block's reach, from the frames before: PSReach below.)
                ID3D11ShaderResourceView* reach = (m_reachOn && m_reachValid) ? m_reach.srv : nullptr;
                m_context->PSSetShaderResources(22, 1, &reach);
                if (scrollOn > 0.5f) m_context->PSSetShaderResources(20, 1, &m_scroll.srv);
            }
            runDispPass(m_psChangeGrow, m_changeGrow, nullptr, m_change.srv);
            // (... and what the 1/4 passes have to work out for those: PSChangeGrow2.)
            if (m_pyramidSkipOn && m_psChangeGrow2 && m_changeGrow2.rtv)
            {
                runDispPass(m_psChangeGrow2, m_changeGrow2, nullptr, m_changeGrow.srv);
                pyramidMask = m_changeGrow2.srv;
            }
            changeSkip = 1.0f;
            if (statsNow)
            {
                runDispPass(m_psStatRows, m_statRows, nullptr, m_changeGrow.srv);
                runDispPass(m_psStat, m_stat, nullptr, m_statRows.srv);
            }
        }
        uploadCB((float)w16, (float)h16);
        {
            // The coarse search's candidates' descriptors, once each: every 8
            // source px along a row (PSAnaDescCoarse), read by PSAnaDisp at t3-t6.
            D3D11_VIEWPORT vp{};
            vp.Width = (FLOAT)m_descCoarse.w; vp.Height = (FLOAT)m_descCoarse.h; vp.MaxDepth = 1.0f;
            m_context->PSSetShader(m_psDescCoarse, nullptr, 0);
            m_context->OMSetRenderTargets(4, m_descCoarse.rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShaderResources(0, 1, &m_src16.srv);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nul = nullptr;
            m_context->PSSetShaderResources(0, 1, &nul);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_context->PSSetShaderResources(3, 4, m_descCoarse.srv);
        }
        runDispPass(m_psCoarse, m_disp0, nullptr, m_src16.srv);   // full search 1/16
        {
            ID3D11ShaderResourceView* nuls[4] = {};
            m_context->PSSetShaderResources(3, 4, nuls);   // (the refine binds its own level's below)
        }
        if (tSlot >= 0) TimeMark(tSlot, 1);
        uploadCB((float)w4,  (float)h4);
        {
            // Gradient descriptors of every 1/4 pixel, once (read by the refine).
            D3D11_VIEWPORT vp{};
            vp.Width = (FLOAT)w4; vp.Height = (FLOAT)h4; vp.MaxDepth = 1.0f;
            m_context->PSSetShader(m_psDesc, nullptr, 0);
            m_context->OMSetRenderTargets(4, m_descRTV, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShaderResources(0, 1, &m_src4.srv);   // (the 1/4 copy: its pixels are the descriptor taps)
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nul = nullptr;
            m_context->PSSetShaderResources(0, 1, &nul);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        if (tSlot >= 0) TimeMark(tSlot, 2);
        m_context->PSSetShaderResources(3, 4, m_descSRV);
        m_context->PSSetShaderResources(15, 1, &pyramidMask);   // (null: all of it)
        runDispPass(m_psRefine, m_disp1, m_disp0.srv, m_src4.srv);                                  // refine 1/4
        {
            ID3D11ShaderResourceView* nuls[4] = {};
            m_context->PSSetShaderResources(3, 4, nuls);
        }
        if (tSlot >= 0) TimeMark(tSlot, 3);
                                          runDispPass(m_psFill,   m_disp2, m_disp1.srv, m_src4.srv);  // occlusion fill 1/4
        if (tSlot >= 0) TimeMark(tSlot, 4);
        {
            // Edge-aware smooth 1/4, steadied by last frame's result (video).
            ID3D11ShaderResourceView* prev[2] = { m_dispPrev.srv, m_src4Prev.srv };
            ID3D11ShaderResourceView* nuls[2] = {};
            m_context->PSSetShaderResources(7, 2, prev);
            ID3D11ShaderResourceView* fl = flatNow ? m_flatS.srv : nullptr;
            m_context->PSSetShaderResources(23, 1, &fl);
            runDispPass(m_psSmooth, m_dispF, m_disp2.srv, m_src4.srv);
            m_context->PSSetShaderResources(23, 1, nuls);
            m_context->PSSetShaderResources(7, 2, nuls);
            m_context->PSSetShaderResources(15, 1, nuls);
            // Keep this frame's for the next one.
            if (m_dispPrev.tex && m_src4Prev.tex)
            {
                m_context->CopyResource(m_dispPrev.tex, m_dispF.tex);
                m_context->CopyResource(m_src4Prev.tex, m_src4.tex);
                m_dispPrevValid = true;
            }
            // Each block's reach after this frame (PSReach), for the next one's
            // PSChangeGrow. (Written apart and copied over: skipped as one with
            // the rest when nothing changed.)
            if (m_reachOn && m_psReach && m_reach.rtv && m_reachNext.rtv)
            {
                uploadCB((float)w4, (float)h4);
                ID3D11ShaderResourceView* in[2] = { changeSkip > 0.5f ? m_changeGrow.srv : nullptr, nullptr };
                ID3D11ShaderResourceView* reach = m_reachValid ? m_reach.srv : nullptr;
                m_context->PSSetShaderResources(15, 1, in);
                m_context->PSSetShaderResources(22, 1, &reach);
                runDispPass(m_psReach, m_reachNext, m_dispF.srv, nullptr);
                m_context->PSSetShaderResources(15, 1, in + 1);
                m_context->PSSetShaderResources(22, 1, in + 1);
                m_context->CopyResource(m_reach.tex, m_reachNext.tex);
                m_reachValid = true;
            }
        }
    }

    if (tSlot >= 0) TimeMark(tSlot, 5);
    // Compose: convert source (delayed frame for Pulfrich, filled disparity map for
    // anaglyph recovery) into the SBS output.
    // Recovered Colour at full-width eyes: first the refine (where to borrow
    // from, how far to trust it) once per horizontal pixel pair -- it was the
    // costliest part of the compose, and it barely changes from one pixel to
    // the next. The compose then reads it (g_pairRefine); every colour it
    // takes is still read at full resolution.
    decode = srcDecode;   // (t0 is the source again)
    // Quilt: the views resampled along their rows first (PSQuiltH), the compose
    // then only down the columns -- see Converter.hlsl.
    bool quiltRows = false;
    if (m_fmt == StereoFormat::Quilt && m_psQuiltH && m_quiltTwoPass)
    {
        const int vh = (std::max)(1, srcHeight / (m_quiltRows > 0 ? m_quiltRows : 1));
        EnsureDispTarget(m_quiltH, m_outWidth, 2 * vh);
        if (m_quiltH.rtv)
        {
            uploadCB(0.0f, 0.0f);
            D3D11_VIEWPORT vp{};
            vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)(2 * vh); vp.MaxDepth = 1.0f;
            m_context->PSSetShader(m_psQuiltH, nullptr, 0);
            m_context->OMSetRenderTargets(1, &m_quiltH.rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            m_context->PSSetShaderResources(0, 1, &source);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nul = nullptr;
            m_context->PSSetShaderResources(0, 1, &nul);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            quiltRows = true;
        }
    }
    else ReleaseDispTarget(m_quiltH);
    if (anaRecover && m_psPair && m_pairRefineOn && ew > (srcWidth + 1) / 2)
    {
        const int pairs = (ew + 1) / 2;
        EnsureDispTarget(m_pair, pairs, eh, DXGI_FORMAT_R16G16B16A16_FLOAT);   // (per pair: both eyes' dRef / conf)
        if (m_pair.rtv)
        {
            uploadCB((float)w4, (float)h4);
            D3D11_VIEWPORT vp{};
            vp.Width = (FLOAT)pairs; vp.Height = (FLOAT)eh; vp.MaxDepth = 1.0f;
            m_context->PSSetShader(m_psPair, nullptr, 0);
            m_context->OMSetRenderTargets(1, &m_pair.rtv, nullptr);
            m_context->RSSetViewports(1, &vp);
            ID3D11ShaderResourceView* srvs[3] = { source, nullptr, m_dispF.srv };
            ID3D11ShaderResourceView* fl = flatNow ? m_flatS.srv : nullptr;
            m_context->PSSetShaderResources(23, 1, &fl);
            m_context->PSSetShaderResources(0, 3, srvs);
            ID3D11ShaderResourceView* src4 = m_src4.srv;
            m_context->PSSetShaderResources(9, 1, &src4);
            ID3D11ShaderResourceView* grow = changeSkip > 0.5f ? m_changeGrow.srv : nullptr;
            m_context->PSSetShaderResources(15, 1, &grow);
            m_context->Draw(3, 0);
            ID3D11ShaderResourceView* nulls[3] = {};
            m_context->PSSetShaderResources(0, 3, nulls);
            m_context->PSSetShaderResources(9, 1, nulls);
            m_context->PSSetShaderResources(15, 1, nulls);
            m_context->PSSetShaderResources(23, 1, nulls);
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            pairRefine = 1.0f;
        }
    }
    else ReleaseDispTarget(m_pair);
    uploadCB((float)w4, (float)h4);
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)m_outHeight; vp.MaxDepth = 1.0f;
        // The smallest shader for the job: after PSAnaPair the Recovered-only
        // compose, else the format's own (PSFmt*), else PSMain (all formats).
        int fi = -1;
        switch (m_fmt)
        {
        case StereoFormat::HalfSBS: case StereoFormat::Katanga: fi = 0; break;
        case StereoFormat::FullSBS:                             fi = 1; break;
        case StereoFormat::FullTAB: case StereoFormat::HalfTAB: fi = 2; break;
        case StereoFormat::RowInterleaved:                      fi = 3; break;
        case StereoFormat::ColumnInterleaved:                   fi = 4; break;
        case StereoFormat::Checkerboard:                        fi = 5; break;
        case StereoFormat::FramePacking:                        fi = 6; break;
        case StereoFormat::Quilt:                               fi = 8; break;
        case StereoFormat::Anaglyph:                            fi = anaRecover ? -1 : 7; break;
        default: break;
        }
        // Both eyes from one thread (compute) where they share their source pixels:
        // the anaglyph modes but Recovered Colour, checkerboard, the interleaved.
        int ci = -1;
        if (m_csOn && m_outUAV)
            // (Checkerboard and row interleaved measured no faster -- slower, the
            // first -- so they stay pixel shaders.)
            ci = (m_fmt == StereoFormat::Anaglyph && !anaRecover) ? 0 :
                 m_fmt == StereoFormat::ColumnInterleaved ? 2 : -1;
        if (ci >= 0 && m_cs[ci])
        {
            m_context->CSSetShader(m_cs[ci], nullptr, 0);
            m_context->CSSetConstantBuffers(0, 1, &m_cbuffer);
            m_context->CSSetSamplers(0, 1, &m_sampler);
            ID3D11ShaderResourceView* cs0[1] = { source };
            m_context->CSSetShaderResources(0, 1, cs0);
            ID3D11ShaderResourceView* csAna[2] = { m_tintHash ? m_tintSRV : nullptr, m_tintHash ? m_boxSRV : nullptr };
            m_context->CSSetShaderResources(10, 2, csAna);
            m_context->CSSetUnorderedAccessViews(0, 1, &m_outUAV, nullptr);
            m_context->Dispatch((UINT)(m_outWidth / 2 + 15) / 16, (UINT)(m_outHeight + 7) / 8, 1);
            ID3D11UnorderedAccessView* nu = nullptr;
            m_context->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
            ID3D11ShaderResourceView* ns[2] = {};
            m_context->CSSetShaderResources(0, 1, ns);
            m_context->CSSetShaderResources(10, 2, ns);
            m_context->CSSetShader(nullptr, nullptr, 0);
        }
        else {
        ID3D11PixelShader* ps = (pairRefine > 0.5f && m_psAnaCompose) ? m_psAnaCompose
                              : (fi >= 0 && m_psFmt[fi]) ? m_psFmt[fi] : m_ps;
        m_context->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* pairSrv = pairRefine > 0.5f ? m_pair.srv : nullptr;   // (PSAnaPair's result)
        m_context->PSSetShaderResources(18, 1, &pairSrv);
        m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { source, delayedSRV, anaRecover ? m_dispF.srv : quiltRows ? m_quiltH.srv : nullptr };
        m_context->PSSetShaderResources(0, 3, srvs);
        ID3D11ShaderResourceView* src4 = anaRecover ? m_src4.srv : nullptr;   // (the recovery's edge-aware upsampling guide)
        m_context->PSSetShaderResources(9, 1, &src4);
        const bool boxes = m_tintHash && anaRecover && m_boxPx[0] > 0.0f;
        ID3D11ShaderResourceView* ana[4] = { m_tintHash ? m_tintSRV : nullptr, m_tintHash ? m_boxSRV : nullptr,   // (what the pictures under an anaglyph were)
                                             boxes ? m_boxValid.srv : nullptr, boxes ? m_boxMap.srv : nullptr };
        m_context->PSSetShaderResources(10, 4, ana);
        ID3D11ShaderResourceView* grow = changeSkip > 0.5f ? m_changeGrow.srv : nullptr;   // (the blocks to redraw: PSChange)
        m_context->PSSetShaderResources(15, 1, &grow);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
        ID3D11ShaderResourceView* nulls7[7] = {};
        m_context->PSSetShaderResources(9, 7, nulls7);
        m_context->PSSetShaderResources(16, 3, nulls7);   // (the boxes' snapshot and shift, the pair refine)
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        }
    }
    if (anaRecover)
    {
        // (Last frame's box map, for the next PSChange -- still under the predicate:
        // if nothing changed it's the same.) Then the predicate off, and the
        // output holds a whole frame for these settings.
        if (m_tintHash && m_boxPx[0] > 0.0f && m_boxMap.tex && m_boxMapPrev.tex) m_context->CopyResource(m_boxMapPrev.tex, m_boxMap.tex);
        // (Scroll reuse: this frame's source, for the next to be compared with.
        // Under the predicate too, once it holds one: nothing changed, the same.)
        if (scrollWanted && m_srcPrevValid) m_context->CopySubresourceRegion(m_srcPrevTex, 0, 0, 0, 0, srcTex2D, srcMip, nullptr);
        m_context->SetPredication(nullptr, FALSE);
        if (scrollWanted && !m_srcPrevValid)
        {
            m_context->CopySubresourceRegion(m_srcPrevTex, 0, 0, 0, 0, srcTex2D, srcMip, nullptr);
            m_srcPrevValid = true;
        }
        ID3D11ShaderResourceView* nul3[3] = {};
        m_context->PSSetShaderResources(19, 3, nul3);
        if (m_statsOn)
        {
            CollectStats();
            ++m_statAcc.frames;
            m_statAcc.reuse = scrollWanted;
            if (changeSkip < 0.5f) ++m_statAcc.full;
            else if (statsNow && !m_statPending[m_statNext])
            {
                ID3D11Texture2D*& st = m_statStaging[m_statNext];
                if (!st)
                {
                    D3D11_TEXTURE2D_DESC sd{};
                    sd.Width = sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    m_device->CreateTexture2D(&sd, nullptr, &st);
                }
                if (st)
                {
                    m_context->CopyResource(st, m_stat.tex);
                    m_statPending[m_statNext] = true;
                    m_statBlocks[m_statNext] = (float)w16 * (float)h16;
                    m_statNext = (m_statNext + 1) % kStatRing;
                }
            }
        }
        m_changeValid = true;
    }
    // (No colour-pyramid fill after it any more: see the note in
    // Converter.hlsl -- it never changed the picture. The perf log's
    // "colour pyramid" and "colour fill" now read 0.)
    if (tSlot >= 0) { TimeMark(tSlot, 6); TimeMark(tSlot, 7); }
    if (tSlot >= 0)
    {
        TimeMark(tSlot, 8);
        m_context->End(m_times[tSlot].disjoint);
        m_times[tSlot].pending = true;
        m_timeNext = (tSlot + 1) % kTimeRing;
    }

    // Pass 2 (temporal): copy the current source into the history ring for later.
    if (temporal && m_histRTV[m_histWrite])
    {
        CB copyCb{ 99, m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
                   m_anaCombo, m_anaMode, (int)m_pulfMode, m_pulfEye,
                   m_ndTrans, m_fpEyeFrac, m_fpGapFrac, 0,
                   dispMaxUV, 0, 0, 0,
                   0, 0, 0, 0, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, 0, 0, 0 };
        copyCb.srcDecode = srcDecode;   // (the history stores decoded pixels either way)
        m_context->UpdateSubresource(m_cbuffer, 0, nullptr, &copyCb, 0, 0);
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_histW; vp.Height = (FLOAT)m_histH; vp.MaxDepth = 1.0f;
        m_context->OMSetRenderTargets(1, &m_histRTV[m_histWrite], nullptr);
        m_context->RSSetViewports(1, &vp);
        m_context->PSSetShaderResources(0, 1, &source);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSRV);
        m_histWrite = (m_histWrite + 1) % kHistory;
    }
    SAFE_RELEASE(srcTex2D);
    SAFE_RELEASE(directTex);
    m_lastVersion = m_srcVersion;   // (0: unknown -- the next call converts regardless)
    m_lastKey     = key;
    m_lastTintHash = m_tintHash;
    return true;
}

bool Converter::EnsureHistory(int width, int height)
{
    if (m_hist[0] && width == m_histW && height == m_histH)
        return false;

    ReleaseHistory();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1; td.Format = m_format;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (int i = 0; i < kHistory; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_hist[i]))) { ReleaseHistory(); return false; }
        if (FAILED(m_device->CreateRenderTargetView(m_hist[i], nullptr, &m_histRTV[i]))) { ReleaseHistory(); return false; }
        if (FAILED(m_device->CreateShaderResourceView(m_hist[i], nullptr, &m_histSRV[i]))) { ReleaseHistory(); return false; }
    }
    m_histW = width; m_histH = height; m_histWrite = 0;
    return true;
}

void Converter::ReleaseHistory()
{
    for (int i = 0; i < kHistory; ++i)
    {
        SAFE_RELEASE(m_histSRV[i]);
        SAFE_RELEASE(m_histRTV[i]);
        SAFE_RELEASE(m_hist[i]);
    }
    m_histW = m_histH = 0;
    m_histWrite = 0;
}

void Converter::ReleaseOutput()
{
    SAFE_RELEASE(m_outSRV);
    SAFE_RELEASE(m_outPrevSRV);
    SAFE_RELEASE(m_outPrevTex);
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outUAV);
    SAFE_RELEASE(m_outTex);
    m_outWidth = m_outHeight = 0;
}

bool Converter::EnsureDispTarget(DispTarget& t, int width, int height, DXGI_FORMAT format)
{
    if (t.tex && width == t.w && height == t.h && format == t.fmt)
        return false;

    ReleaseDispTarget(t);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = format;   // (default RGBA16F: dLR, dRL (UV), confidence)
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &t.tex))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) { ReleaseDispTarget(t); return false; }
    t.w = width; t.h = height; t.fmt = format;
    return true;
}

void Converter::TimeMark(int slot, int mark)
{
    TimeSet& t = m_times[slot];
    if (!t.ts[mark])
    {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP, 0 };
        m_device->CreateQuery(&qd, &t.ts[mark]);
    }
    if (t.ts[mark]) m_context->End(t.ts[mark]);
}

void Converter::CollectTimes()
{
    for (int i = 0; i < kTimeRing; ++i)
    {
        TimeSet& t = m_times[i];
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (m_context->GetData(t.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 v[kTimeMarks] = {};
        bool ok = !dj.Disjoint && dj.Frequency > 0;
        for (int k = 0; k < kTimeMarks && ok; ++k)
            ok = t.ts[k] && m_context->GetData(t.ts[k], &v[k], sizeof(v[k]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok && !dj.Disjoint) continue;   // (not all in yet)
        t.pending = false;
        if (!ok) continue;
        for (int k = 0; k + 1 < kTimeMarks; ++k)
            m_timeSum[k] += (double)(v[k + 1] - v[k]) * 1000.0 / (double)dj.Frequency;
        ++m_timeCount;
    }
}

void Converter::CollectStats()
{
    for (int i = 0; i < kStatRing; ++i)
    {
        if (!m_statPending[i] || !m_statStaging[i]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (m_context->Map(m_statStaging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        const float* v = (const float*)m.pData;
        const float r = v[0], g = v[1], dy = v[2], any = v[3];
        m_context->Unmap(m_statStaging[i], 0);
        m_statPending[i] = false;
        if (any < 0.5f) continue;   // (nothing had changed: the passes were skipped)
        const float blocks = (std::max)(1.0f, m_statBlocks[i]);
        ++m_statAcc.changed;
        m_statAcc.redrawn += r / blocks; m_statAcc.moved += g / blocks;
        if (dy != 0.0f) { ++m_statAcc.scrolled; m_statAcc.rows += std::abs(dy); }
    }
}

bool Converter::TakeChangeStats(ChangeStats& out)
{
    CollectStats();
    out = m_statAcc;
    if (out.changed > 0) { out.redrawn /= out.changed; out.moved /= out.changed; }
    if (out.scrolled > 0) out.rows /= out.scrolled;
    m_statAcc = ChangeStats{};
    return out.frames > 0;
}

bool Converter::TakeRecoveryTimes(double ms[kTimeMarks - 1], int& count)
{
    CollectTimes();
    count = m_timeCount;
    if (m_timeCount == 0) return false;
    for (int k = 0; k + 1 < kTimeMarks; ++k) { ms[k] = m_timeSum[k] / m_timeCount; m_timeSum[k] = 0; }
    m_timeCount = 0;
    return true;
}

bool Converter::EnsureDescTargets(int width, int height)
{
    if (m_descTex[0] && width == m_descW && height == m_descH) return true;
    ReleaseDescTargets();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_UINT;   // 8 packed halves each
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 4; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_descTex[i])) ||
            FAILED(m_device->CreateRenderTargetView(m_descTex[i], nullptr, &m_descRTV[i])) ||
            FAILED(m_device->CreateShaderResourceView(m_descTex[i], nullptr, &m_descSRV[i])))
        { ReleaseDescTargets(); return false; }
    }
    m_descW = width; m_descH = height;
    return true;
}

void Converter::ReleaseDescTargets()
{
    for (int i = 0; i < 4; ++i)
    {
        SAFE_RELEASE(m_descSRV[i]);
        SAFE_RELEASE(m_descRTV[i]);
        SAFE_RELEASE(m_descTex[i]);
    }
    m_descW = m_descH = 0;
    ReleaseDescSet(m_descCoarse);
}

// Everything the output depends on besides the source's pixels: a changed
// setting (or a different source texture / size) means converting again.
uint64_t Converter::SettingsKey(ID3D11ShaderResourceView* source, int srcWidth, int srcHeight) const
{
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    const void* id = m_srcIdentity ? m_srcIdentity : (const void*)source;
    mix(&id, sizeof(id)); mix(&m_srcEncoded, sizeof(m_srcEncoded)); mix(&srcWidth, sizeof(srcWidth)); mix(&srcHeight, sizeof(srcHeight));
    mix(&m_fmt, sizeof(m_fmt)); mix(&m_swap, sizeof(m_swap)); mix(&m_anaCombo, sizeof(m_anaCombo));
    const bool tintsInUse = m_tintHash != 0;
    mix(&m_anaMode, sizeof(m_anaMode)); mix(&tintsInUse, sizeof(tintsInUse)); if (tintsInUse) mix(&m_wholeTintHash, sizeof(m_wholeTintHash)); mix(&m_fpEyeFrac, sizeof(m_fpEyeFrac)); mix(&m_fpGapFrac, sizeof(m_fpGapFrac));
    mix(&m_fpEyeAlign, sizeof(m_fpEyeAlign)); mix(&m_convergence, sizeof(m_convergence));
    if (m_anaCombo == 6) { mix(m_anaMaskL, sizeof(m_anaMaskL)); mix(m_anaMaskR, sizeof(m_anaMaskR)); mix(m_anaWL, sizeof(m_anaWL)); mix(m_anaWR, sizeof(m_anaWR)); mix(m_anaTL, sizeof(m_anaTL)); mix(m_anaTR, sizeof(m_anaTR)); }
    mix(&m_quiltCols, sizeof(m_quiltCols)); mix(&m_quiltRows, sizeof(m_quiltRows));
    mix(&m_quiltLeftIdx, sizeof(m_quiltLeftIdx)); mix(&m_quiltRightIdx, sizeof(m_quiltRightIdx));
    mix(&m_quiltLeftBlend, sizeof(m_quiltLeftBlend)); mix(&m_quiltRightBlend, sizeof(m_quiltRightBlend));
    mix(&m_targetPaneW, sizeof(m_targetPaneW)); mix(&m_targetPaneH, sizeof(m_targetPaneH));
    mix(&m_vrYaw, sizeof(m_vrYaw)); mix(&m_vrPitch, sizeof(m_vrPitch)); mix(&m_vrZoom, sizeof(m_vrZoom));
    return h;
}

void Converter::ReleaseDispTarget(DispTarget& t)
{
    SAFE_RELEASE(t.srv);
    SAFE_RELEASE(t.rtv);
    SAFE_RELEASE(t.tex);
    t.w = t.h = 0;
}

void Converter::ReleaseScroll()
{
    SAFE_RELEASE(m_srcPrevSRV);
    SAFE_RELEASE(m_srcPrevTex);
    SAFE_RELEASE(m_outPrevSRV);
    SAFE_RELEASE(m_outPrevTex);
    ReleaseDispTarget(m_scrollCost);
    ReleaseDispTarget(m_scrollRows);
    ReleaseDispTarget(m_changeQ);
    ReleaseDispTarget(m_scroll);
    m_srcPrevValid = false;
    m_srcPrevW = m_srcPrevH = 0;
}

void Converter::ReleaseDisparity()
{
    ReleaseDispTarget(m_disp0);
    ReleaseDispTarget(m_disp1);
    ReleaseDispTarget(m_disp2);
    ReleaseDispTarget(m_dispF);
    ReleaseDispTarget(m_pair);
    ReleaseDescTargets();
    ReleaseDispTarget(m_src4);
    ReleaseDispTarget(m_src16);
    ReleaseDispTarget(m_dispPrev);
    ReleaseDispTarget(m_src4Prev);
    ReleaseDispTarget(m_change);
    ReleaseDispTarget(m_changeGrow);
    ReleaseDispTarget(m_changeGrow2);
    ReleaseDispTarget(m_flat4); ReleaseDispTarget(m_flatS);
    SAFE_RELEASE(m_workSRV); SAFE_RELEASE(m_workTex);
    ReleaseDispTarget(m_enc4); ReleaseDispTarget(m_enc4Prev);
    m_encPrevValid = false;
    ReleaseDispTarget(m_reach);
    ReleaseDispTarget(m_reachNext);
    m_reachValid = false;
    ReleaseDispTarget(m_statRows);
    ReleaseDispTarget(m_stat);
    ReleaseScroll();
    ReleaseDispTarget(m_boxMapPrev);
    ReleaseDispTarget(m_snapPending);
    ReleaseDispTarget(m_snapActive);
    ReleaseDispTarget(m_boxShiftCost);
    ReleaseDispTarget(m_boxShift);
    m_snapPendingValid = m_snapActiveValid = false;
    m_changeValid = false;
    m_dispPrevValid = false;
}

void Converter::Shutdown()
{
    ReleaseOutput();
    ReleaseHistory();
    ReleaseDisparity();
    SAFE_RELEASE(m_cbuffer);
    SAFE_RELEASE(m_sampler);
    for (auto& t : m_times) { SAFE_RELEASE(t.disjoint); for (auto& q : t.ts) SAFE_RELEASE(q); t.pending = false; }
    SAFE_RELEASE(m_psDesc);
    SAFE_RELEASE(m_psDescCoarse);
    SAFE_RELEASE(m_psDown);
    SAFE_RELEASE(m_psSmooth);
    SAFE_RELEASE(m_psPair);
    SAFE_RELEASE(m_psAnaCompose);
    for (auto*& p : m_psFmt) SAFE_RELEASE(p);
    SAFE_RELEASE(m_tintSRV);
    SAFE_RELEASE(m_tintTex);
    SAFE_RELEASE(m_boxSRV);
    SAFE_RELEASE(m_boxTex);
    SAFE_RELEASE(m_psBoxCheck);
    SAFE_RELEASE(m_psBoxRows);
    SAFE_RELEASE(m_psBoxMap);
    SAFE_RELEASE(m_psChange);
    SAFE_RELEASE(m_psChangeGrow);
    SAFE_RELEASE(m_psChangeGrow2);
    SAFE_RELEASE(m_psDownBox);
    SAFE_RELEASE(m_psDownFlat); SAFE_RELEASE(m_psFlatShrink);
    SAFE_RELEASE(m_psQuiltH);
    for (auto*& c : m_cs) SAFE_RELEASE(c);
    ReleaseDispTarget(m_quiltH);
    SAFE_RELEASE(m_psReach);
    SAFE_RELEASE(m_psStatRows);
    SAFE_RELEASE(m_psStat);
    for (auto*& t : m_statStaging) SAFE_RELEASE(t);
    SAFE_RELEASE(m_changePred);
    SAFE_RELEASE(m_prePred);
    SAFE_RELEASE(m_psScrollCost);
    SAFE_RELEASE(m_psScrollPick);
    SAFE_RELEASE(m_psScrollSum);
    SAFE_RELEASE(m_psChangeScrollQ);
    SAFE_RELEASE(m_psChangeScroll);
    SAFE_RELEASE(m_scrollPred);
    SAFE_RELEASE(m_psBoxShiftCost);
    SAFE_RELEASE(m_psBoxShift);
    ReleaseDispTarget(m_boxRows);
    ReleaseDispTarget(m_boxMap);
    ReleaseDispTarget(m_boxValid);
    m_anaTablesDirty = true;   // (re-uploaded after a re-initialise)
    m_tintHash = 0;
    SAFE_RELEASE(m_psFill);
    SAFE_RELEASE(m_psRefine);
    SAFE_RELEASE(m_psCoarse);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}

bool Converter::ReadAnaBoxValid(float out[32])
{
    if (!m_boxValid.tex || m_boxPx[0] <= 0.0f) return false;
    D3D11_TEXTURE2D_DESC d{}; m_boxValid.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(m_device->CreateTexture2D(&d, nullptr, &st))) return false;
    m_context->CopyResource(st, m_boxValid.tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(m_context->Map(st, 0, D3D11_MAP_READ, 0, &m)))
    {
        // (RGBA16F: .r of each texel)
        const uint16_t* p = (const uint16_t*)m.pData;
        for (int i = 0; i < 32 && i < (int)d.Width; ++i)
        {
            const uint16_t hbits = p[i * 4];
            out[i] = (hbits & 0x7fff) == 0 ? 0.0f : ((hbits & 0x7c00) >= 0x3c00 ? 1.0f : 0.5f);   // (0 or 1 written)
        }
        m_context->Unmap(st, 0);
        ok = true;
    }
    st->Release();
    return ok;
}

void Converter::CommitAnaSnapshot()
{
    if (!m_snapPendingValid || !m_snapPending.tex || !m_snapActive.tex) { m_snapActiveValid = false; return; }
    m_context->CopyResource(m_snapActive.tex, m_snapPending.tex);
    m_snapActiveValid = true;
}

bool Converter::EnsureDescSet(DescSet& s, int width, int height)
{
    if (s.tex[0] && width == s.w && height == s.h) return true;
    ReleaseDescSet(s);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_UINT;   // 8 packed halves each
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (int i = 0; i < 4; ++i)
    {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &s.tex[i])) ||
            FAILED(m_device->CreateRenderTargetView(s.tex[i], nullptr, &s.rtv[i])) ||
            FAILED(m_device->CreateShaderResourceView(s.tex[i], nullptr, &s.srv[i])))
        { ReleaseDescSet(s); return false; }
    }
    s.w = width; s.h = height;
    return true;
}

void Converter::ReleaseDescSet(DescSet& s)
{
    for (int i = 0; i < 4; ++i)
    {
        SAFE_RELEASE(s.srv[i]);
        SAFE_RELEASE(s.rtv[i]);
        SAFE_RELEASE(s.tex[i]);
    }
    s.w = s.h = 0;
}
