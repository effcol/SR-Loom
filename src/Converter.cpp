#include "Converter.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>

// The shaders, compiled at build time (shaders/Converter.hlsl; see CMakeLists.txt).
#include "Converter_VSMain.h"
#include "Converter_PSMain.h"
#include "Converter_PSAnaDisp.h"
#include "Converter_PSAnaDesc.h"
#include "Converter_PSDown.h"
#include "Converter_PSAnaRefine.h"
#include "Converter_PSAnaFill.h"
#include "Converter_PSAnaSmooth.h"

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
        case StereoFormat::HalfSBS:           ew = w / 2; eh = h;     break;
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
               float lvlToSrcY; float _pad_e, _pad_f, _pad_g; };

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
        { g_Converter_PSDown,         sizeof(g_Converter_PSDown),         &m_psDown      },
        { g_Converter_PSAnaRefine,    sizeof(g_Converter_PSAnaRefine),    &m_psRefine    },
        { g_Converter_PSAnaFill,      sizeof(g_Converter_PSAnaFill),      &m_psFill      },
        { g_Converter_PSAnaSmooth,    sizeof(g_Converter_PSAnaSmooth),    &m_psSmooth    },
    };
    for (const auto& p : ps)
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(p.code, p.size, nullptr, p.out);
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
    td.Format           = m_format;   // sRGB; both RTV and SRV use it directly
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_outTex))) return false;
    if (FAILED(m_device->CreateRenderTargetView(m_outTex, nullptr, &m_outRTV))) { ReleaseOutput(); return false; }
    if (FAILED(m_device->CreateShaderResourceView(m_outTex, nullptr, &m_outSRV))) { ReleaseOutput(); return false; }

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
    const uint64_t key = SettingsKey(source, srcWidth, srcHeight);
    if (!temporal && m_srcVersion != 0 && m_srcVersion == m_lastVersion && key == m_lastKey && m_outSRV)
        return true;

    int ew = 0, eh = 0;
    PerEyeSize(m_fmt, srcWidth, srcHeight, ew, eh);
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
        // (A remade previous-frame target holds nothing yet.)
        if (EnsureDispTarget(m_dispPrev, w4, h4) | EnsureDispTarget(m_src4Prev, w4, h4)) m_dispPrevValid = false;
    }
    else ReleaseDisparity();

    const float dispMaxUV = 0.06f;
    auto uploadCB = [&](float coarseW, float coarseH, float prop = 0.0f)
    {
        CB cb{ FormatCode(m_fmt), m_swap ? 1 : 0, (float)srcWidth, (float)srcHeight,
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
               anaRecover ? 16.0f * h16 / srcHeight : 1.0f, 0, 0, 0 };
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
        m_psDesc && EnsureDescTargets(w4, h4))
    {
        // The source averaged down to 1/4 and 1/16: what the passes below read.
        runDispPass(m_psDown, m_src4,  nullptr, source);
        runDispPass(m_psDown, m_src16, nullptr, m_src4.srv);
        uploadCB((float)w16, (float)h16); runDispPass(m_psCoarse, m_disp0, nullptr, m_src16.srv);   // full search 1/16
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
            runDispPass(m_psSmooth, m_dispF, m_disp2.srv, m_src4.srv);
            m_context->PSSetShaderResources(7, 2, nuls);
            // Keep this frame's for the next one.
            if (m_dispPrev.tex && m_src4Prev.tex)
            {
                m_context->CopyResource(m_dispPrev.tex, m_dispF.tex);
                m_context->CopyResource(m_src4Prev.tex, m_src4.tex);
                m_dispPrevValid = true;
            }
        }
    }

    if (tSlot >= 0) TimeMark(tSlot, 5);
    // Compose: convert source (delayed frame for Pulfrich, filled disparity map for
    // anaglyph recovery) into the SBS output.
    uploadCB((float)w4, (float)h4);
    {
        D3D11_VIEWPORT vp{};
        vp.Width = (FLOAT)m_outWidth; vp.Height = (FLOAT)m_outHeight; vp.MaxDepth = 1.0f;
        m_context->PSSetShader(m_ps, nullptr, 0);
        m_context->OMSetRenderTargets(1, &m_outRTV, nullptr);
        m_context->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* srvs[3] = { source, delayedSRV, anaRecover ? m_dispF.srv : nullptr };
        m_context->PSSetShaderResources(0, 3, srvs);
        ID3D11ShaderResourceView* src4 = anaRecover ? m_src4.srv : nullptr;   // (the recovery's edge-aware upsampling guide)
        m_context->PSSetShaderResources(9, 1, &src4);
        m_context->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[3] = { nullptr, nullptr, nullptr };
        m_context->PSSetShaderResources(0, 3, nulls);
        m_context->PSSetShaderResources(9, 1, nulls);
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
    m_lastVersion = m_srcVersion;   // (0: unknown -- the next call converts regardless)
    m_lastKey     = key;
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
    SAFE_RELEASE(m_outRTV);
    SAFE_RELEASE(m_outTex);
    m_outWidth = m_outHeight = 0;
}

bool Converter::EnsureDispTarget(DispTarget& t, int width, int height)
{
    if (t.tex && width == t.w && height == t.h)
        return false;

    ReleaseDispTarget(t);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width; td.Height = (UINT)height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;   // dLR, dRL (UV), confidence
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &t.tex))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateRenderTargetView(t.tex, nullptr, &t.rtv))) { ReleaseDispTarget(t); return false; }
    if (FAILED(m_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) { ReleaseDispTarget(t); return false; }
    t.w = width; t.h = height;
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
    mix(&source, sizeof(source)); mix(&srcWidth, sizeof(srcWidth)); mix(&srcHeight, sizeof(srcHeight));
    mix(&m_fmt, sizeof(m_fmt)); mix(&m_swap, sizeof(m_swap)); mix(&m_anaCombo, sizeof(m_anaCombo));
    mix(&m_anaMode, sizeof(m_anaMode)); mix(&m_fpEyeFrac, sizeof(m_fpEyeFrac)); mix(&m_fpGapFrac, sizeof(m_fpGapFrac));
    mix(&m_fpEyeAlign, sizeof(m_fpEyeAlign)); mix(&m_convergence, sizeof(m_convergence));
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

void Converter::ReleaseDisparity()
{
    ReleaseDispTarget(m_disp0);
    ReleaseDispTarget(m_disp1);
    ReleaseDispTarget(m_disp2);
    ReleaseDispTarget(m_dispF);
    ReleaseDescTargets();
    ReleaseDispTarget(m_src4);
    ReleaseDispTarget(m_src16);
    ReleaseDispTarget(m_dispPrev);
    ReleaseDispTarget(m_src4Prev);
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
    SAFE_RELEASE(m_psDown);
    SAFE_RELEASE(m_psSmooth);
    SAFE_RELEASE(m_psFill);
    SAFE_RELEASE(m_psRefine);
    SAFE_RELEASE(m_psCoarse);
    SAFE_RELEASE(m_ps);
    SAFE_RELEASE(m_vs);
}
