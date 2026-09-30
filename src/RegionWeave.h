// RegionWeave.h -- Auto Stereo's region compositor.
//
// Weaves several independent screen regions, each with its own stereo
// format, through ONE SR weaver. Each region is cropped out of the full
// monitor capture, run through its own Converter (so an anaglyph image and
// an SBS video can be on screen at the same time), and its left/right views
// are drawn into the matching rectangle of a full-display side-by-side
// composite. Everything outside the regions is left BLACK -- 2D desktop
// content is never drawn into the woven image; the weave window is clipped
// to the region rectangles instead, so the real desktop shows around them.
#pragma once

#include "Common.h"
#include "Converter.h"
#include <d3d11.h>
#include <memory>
#include <vector>

namespace srw
{
    struct AnaTint;   // (ScreenAnalysis.h)
    struct WeaveRegion
    {
        int          id       = 0;
        RECT         screen{};           // desktop coordinates (for the window clip)
        RECT         frame{};            // capture-frame pixels: the WHOLE image. May run off
                                         // the frame / under a toolbar -- SBS/TAB halves are
                                         // split from this, so it must stay whole.
        RECT         vis{};              // frame pixels actually visible (empty = all of frame);
                                         // source outside it is blacked out before conversion
        StereoFormat format   = StereoFormat::HalfSBS;
        bool         swapEyes = false;
        int          anaglyphCombo = 0;
        int          anaglyphMode  = 0;
        std::shared_ptr<const AnaTint> anaTint;  // anaglyphMode 5: the one-colour picture's tint tables (AnalyseAnaPicture)
        bool         anaAuto  = false;        // its anaglyph colour pair / decode were detected (not the panel's)
        bool         autoSwap = false;        // detected with its eyes the other way round (flips the panel's Swap Eyes)
        HWND         trackWindow = nullptr;   // follow this window's client area, or null
        bool         trackContent = false;    // follow the image content itself (scrolling)
        HWND         host = nullptr;          // top-level window the image is in (content regions)
        HWND         viewWindow = nullptr;    // its scrolling viewport child (e.g. a browser page), or host
        bool         autoDetected = false;    // found by the scanner (vs picked by hand)
        POINT        scrollLead{};            // (lead parts: page-scroll prediction ...
        POINT        winLead{};               //  ... and window-move compensation)
        POINT        lead{};                  // draw this far (frame px) ahead of where it was captured:
                                              // where a scrolling picture will be once on screen
        RECT         clip{};                  // never draw outside this (frame px; its viewport), or empty
        int          gpuSlot = -1;            // GpuTracker slot (its offset moves this picture on the GPU), or -1
        bool         trueAspect = false;      // Full SBS: show each eye at its real shape (half the
                                              // width, centred) instead of stretched over the region
    };

    class RegionWeaver
    {
    public:
        RegionWeaver() = default;
        ~RegionWeaver();

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Region list. Ids are stable for the life of a region.
        int  Add(const WeaveRegion& r);            // returns the new id
        void Remove(int id);
        void Clear();
        std::vector<WeaveRegion>&       Regions()       { return m_regions; }
        const std::vector<WeaveRegion>& Regions() const { return m_regions; }
        bool Empty() const { return m_regions.empty(); }

        // Build the composite from the current full-frame capture texture.
        // captureTex: the capture's texture (typeless BGRA), captureSrvFormat
        // its view format. Returns true on success; compositeResized tells
        // the caller to re-register the weaver's input view.
        // GpuTracker results (int4 per slot), read on the GPU by regions with
        // a gpuSlot. Null = no GPU tracking.
        // scale: capture px per GpuTracker row (the analysis image scale).
        void SetGpuResults(ID3D11ShaderResourceView* srv, int scale) { m_gpuRes = srv; m_gpuScale = scale; }

        bool Build(ID3D11Texture2D* captureTex, ID3D11ShaderResourceView* captureSRV, DXGI_FORMAT captureSrvFormat,
                   int frameW, int frameH, bool& compositeResized);

        ID3D11ShaderResourceView* CompositeSRV()    const { return m_compSRV; }
        int                       PerEyeWidth()     const { return m_compW / 2; }
        int                       Height()          const { return m_compH; }
        DXGI_FORMAT               CompositeFormat() const { return kCompFormat; }

        // Looking Glass: the part of the composite under the glass (`r`, capture
        // frame px) as its own side-by-side texture -- the weave input there.
        // Returns false on failure; `resized` when the texture was remade.
        bool CropComposite(const RECT& r, bool& resized);
        ID3D11ShaderResourceView* CroppedSRV()      const { return m_lgSRV; }
        int                       CroppedPerEyeWidth() const { return m_lgW; }
        int                       CroppedHeight()   const { return m_lgH; }

    private:
        struct Slot   // per-region GPU resources, keyed by region id
        {
            int                         id = 0;
            std::unique_ptr<Converter>  conv;
            ID3D11Texture2D*            crop    = nullptr;
            ID3D11ShaderResourceView*   cropSRV = nullptr;
            ID3D11RenderTargetView*     cropRTV = nullptr;   // for blacking out hidden source
            int                         cropW = 0, cropH = 0;
            DXGI_FORMAT                 cropFmt = DXGI_FORMAT_UNKNOWN;
        };

        static constexpr DXGI_FORMAT kCompFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

        Slot* SlotFor(int id, int w, int h);
        bool  EnsureCrop(Slot& s, int w, int h, DXGI_FORMAT texFmt, DXGI_FORMAT srvFmt);
        void  SetScissor(const RECT& r);
        bool  EnsureComposite(int w, int h);
        void  ReleaseSlot(Slot& s);
        void  DrawInto(ID3D11ShaderResourceView* src, float u0, float v0, float du, float dv,
                       int dx, int dy, int dw, int dh);

        ID3D11Device*             m_device  = nullptr;
        ID3D11DeviceContext*      m_context = nullptr;
        ID3D11VertexShader*       m_vs      = nullptr;
        ID3D11PixelShader*        m_ps      = nullptr;
        ID3D11PixelShader*        m_psBlur  = nullptr;   // edge-colour fill beside true-shape Full SBS
        ID3D11PixelShader*        m_psCrop  = nullptr;   // crop at the GPU-tracked position
        ID3D11VertexShader*       m_vsShift = nullptr;   // placement shifted by the GPU-tracked offset
        ID3D11Buffer*             m_regionCB = nullptr;  // Region cbuffer (b1)
        ID3D11ShaderResourceView* m_gpuRes  = nullptr;   // GpuTracker results (not owned)
        int                       m_gpuScale = 2;
        ID3D11SamplerState*       m_sampler = nullptr;
        ID3D11Buffer*             m_cb      = nullptr;
        ID3D11RasterizerState*    m_rs      = nullptr;   // scissor on: each eye stays in its half

        ID3D11Texture2D*          m_comp    = nullptr;
        ID3D11RenderTargetView*   m_compRTV = nullptr;
        ID3D11ShaderResourceView* m_compSRV = nullptr;
        ID3D11Texture2D*          m_lgTex = nullptr;     // CropComposite output (2 x m_lgW wide)
        ID3D11ShaderResourceView* m_lgSRV = nullptr;
        int                       m_lgW = 0, m_lgH = 0;
        int                       m_compW = 0, m_compH = 0;   // full SBS size

        std::vector<WeaveRegion>  m_regions;
        std::vector<Slot>         m_slots;
        // Recycled slots -- converter and crop, with their textures: a picture
        // of the same size as one that went reuses them as they are (making
        // textures stalls the frame a new picture turns up in).
        std::vector<Slot>         m_slotPool;
        int                       m_nextId = 1;
    };
}
