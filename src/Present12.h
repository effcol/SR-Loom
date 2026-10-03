// Present12.h -- the Direct3D 12 back end of the DirectComposition presenter
// (the "DX12" weaver choice). Capture and conversion stay Direct3D 11; the
// picture to weave crosses over as a shared texture (no copy when it is the
// converter's output), a shared fence puts Direct3D 11's drawing before
// Direct3D 12's weave, and the weave, the see-through mask and the present
// run on a Direct3D 12 queue of their own.
#pragma once

#include "Common.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>

namespace srw
{
    class Present12
    {
    public:
        Present12() = default;
        ~Present12() { Shutdown(); }

        // On the adapter of d11 (the renderer's device). maskHLSL: the
        // renderer's mask shader source (VSMain / PSMain), shared with the
        // Direct3D 11 path.
        // Before Initialize: a 16-bit float swap chain in scRGB (HDR output)
        // instead of the 8-bit sRGB one.
        void SetHdr(bool on) { m_hdr = on; }
        bool IsHdr() const { return m_hdr; }
        // The format the weaver draws in (the back buffer's view).
        DXGI_FORMAT OutputFormat() const { return m_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        bool Initialize(ID3D11Device* d11, ID3D11DeviceContext* c11, IDXGIFactory2* factory, UINT width, UINT height,
                        const char* maskHLSL);
        void Shutdown();
        bool Resize(UINT width, UINT height);
        // The swap chain made anew, see-through (false) or opaque (see Present12.cpp).
        bool RecreateSwapChain(IDXGIFactory2* factory, bool opaque, bool tearing);
        UINT SwapFlags() const { return m_scFlags; }

        IDXGISwapChain1*           SwapChain() const { return m_swapChain; }
        ID3D12Device*              Device() const    { return m_device; }
        ID3D12GraphicsCommandList* List() const      { return m_list; }
        ID3D12CommandQueue*        Queue() const     { return m_queue; }
        ID3D12Resource*            BackBuffer() const { return m_back[m_index]; }   // (this frame's)
        // An allocator of its own for a weaver that sends commands while being set up.
        ID3D12CommandAllocator*    SetupAllocator()  { if (!m_setupAlloc) m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_setupAlloc)); return m_setupAlloc; }
        bool                       InFrame() const   { return m_inFrame; }
        D3D12_VIEWPORT             Viewport() const  { return D3D12_VIEWPORT{ 0, 0, (FLOAT)m_width, (FLOAT)m_height, 0, 1 }; }
        D3D12_RECT                 Scissor() const   { return D3D12_RECT{ 0, 0, (LONG)m_width, (LONG)m_height }; }
        // The part of the back buffer the weave is wanted in (the renderer's
        // visible areas' bounds; the whole of it by default). Empty: no weave.
        void                       SetWeaveScissor(const D3D12_RECT& r) { m_weaveScissor = r; m_weaveScissorSet = true; }
        void                       ClearWeaveScissor() { m_weaveScissorSet = false; }
        D3D12_RECT                 WeaveScissor() const { return m_weaveScissorSet ? m_weaveScissor : Scissor(); }

        // The Direct3D 11 texture the weaver is to read, as a Direct3D 12
        // resource: the same memory when the texture was made shareable (the
        // converter's output); otherwise a shareable copy, refreshed every
        // frame (EndFrame). Null on failure.
        ID3D12Resource* Share(ID3D11Texture2D* tex);

        // A frame: back buffer cleared (opaque black) and bound; the weave and
        // the mask are recorded in between; EndFrame sends it, to run once
        // Direct3D 11's drawing so far is done. The caller then presents.
        void BeginFrame();
        void EndFrame();
        // The see-through mask over the bound back buffer: cb / tiles are the
        // mask shader's constants and tile lists; one draw per scissor rect.
        void DrawMask(const void* cb, size_t cbBytes, const void* tiles, size_t tilesBytes,
                      const D3D12_RECT* scissors, int count);
        bool SetGpuResults(ID3D11Texture2D* tex);   // (see Present12.cpp)
        bool CopyRows(UINT y0, UINT rows);            // (reading a band of the frame back: see Present12.cpp)
        bool FetchRows(std::vector<uint8_t>& out);
        // Does the weave leave the picture opaque (alpha 1)? 0 not known yet
        // (asked: call again in later frames, after the weave), 2 yes, 3 no.
        int  AlphaState();
        // GPU timings since the last call (averages over frames): the weave and
        // the mask on the GPU, and from sending a frame to the GPU starting /
        // finishing it. False when nothing was measured.
        struct Times { int frames = 0; double weaveMs = 0, maskMs = 0, startMs = 0, doneMs = 0, worstDoneMs = 0; };
        bool TakeTimes(Times& out);
        void Mark(int i);   // (a GPU timestamp: see Present12.cpp)
        // Conversion apart from the weave (see Present12.cpp). SetAsync: this
        // frame weaves from AsyncFront() and does not wait for Direct3D 11.
        void SetAsync(bool on);
        bool IsAsync() const { return m_async; }
        // Is the conversion sent last still running? (Then none is started.)
        bool ConversionPending();
        void NoteSkipped() { ++m_asyncSkipped; }
        double LastConversionMs() const { return m_lastConvMs; }   // (asked-to-finished, the last that changed the picture)
        // Around a conversion: AsyncBeforeConvert ahead of its drawing, then
        // AsyncSubmitted with the converter's (shareable) output -- false: could
        // not be set up.
        void AsyncBeforeConvert();
        bool AsyncSubmitted(ID3D11Texture2D* out);
        // Before the weave: has the conversion under way finished (waiting up to
        // waitMs)? Then AsyncResolve with whether it changed the picture (1 / 0;
        // -1 not known yet): a changed one is copied into the picture the weave keeps.
        bool AsyncReady(double waitMs);
        void AsyncResolve(int changed);
        ID3D12Resource* AsyncFront() const { return m_keptValid ? m_kept : nullptr; }
        // Since the last call: pictures shown, those of them a refresh or more
        // after they were asked for, conversions that changed nothing, loops that
        // started no conversion (the last still running), and ask-to-shown ms.
        struct AsyncStats { int shown = 0, late = 0, same = 0, skipped = 0; double avgMs = 0, worstMs = 0; };
        bool TakeAsyncStats(AsyncStats& out);
        // Blocks until everything sent has run (before releasing what it used).
        void WaitIdle();

    private:
        bool CreateBuffers();
        void ReleaseBuffers();
        void ReleaseShared();
        void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

        static constexpr UINT kBuffers = 2;
        static constexpr UINT kCbBytes = 4096, kTilesAt = 4096, kTilesBytes = 20480, kUploadBytes = 4096 + 20480 + 256;

        ID3D11Device*              m_d11 = nullptr;     // not owned
        ID3D11DeviceContext*       m_c11 = nullptr;     // not owned
        ID3D11DeviceContext4*      m_c11_4 = nullptr;
        ID3D11Fence*               m_fence11 = nullptr;      // the shared fence, Direct3D 11's end
        ID3D12Device*              m_device = nullptr;
        ID3D12CommandQueue*        m_queue = nullptr;
        ID3D12CommandAllocator*    m_alloc[kBuffers] = {};
        ID3D12GraphicsCommandList* m_list = nullptr;
        ID3D12CommandAllocator*    m_setupAlloc = nullptr;
        ID3D12Fence*               m_fenceShared = nullptr;  // signalled by Direct3D 11 (its drawing is done)
        ID3D12Fence*               m_fenceFrame = nullptr;   // signalled by our queue (a frame has run)
        HANDLE                     m_event = nullptr;
        UINT64                     m_sharedValue = 0, m_frameValue = 0, m_slotValue[kBuffers] = {};
        IDXGISwapChain1*           m_swapChain = nullptr;
        IDXGISwapChain3*           m_swapChain3 = nullptr;
        ID3D12Resource*            m_back[kBuffers] = {};
        ID3D12DescriptorHeap*      m_rtvHeap = nullptr;
        UINT                       m_rtvStep = 0, m_width = 0, m_height = 0, m_index = 0;
        bool                       m_inFrame = false;
        UINT                       m_scFlags = 0;            // the swap chain's creation flags
        bool                       m_hdr = false;
        D3D12_RECT                 m_weaveScissor{};
        bool                       m_weaveScissorSet = false;

        // The mask pass.
        ID3D12RootSignature*       m_maskRoot = nullptr;
        ID3D12PipelineState*       m_maskPSO = nullptr;
        ID3D12DescriptorHeap*      m_gpuHeap = nullptr;      // the mask's t0: the GPU tracker's results
        ID3D12Resource*            m_rowsBuf = nullptr;      // (CopyRows / FetchRows)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_rowsFp{};
        UINT                       m_rowsCount = 0;
        ID3D12Resource*            m_gpu12 = nullptr;
        ID3D11Texture2D*           m_gpuSrc = nullptr;       // (not owned)
        ID3D12Resource*            m_upload[kBuffers] = {};   // constants + tile lists, one per back buffer
        uint8_t*                   m_uploadPtr[kBuffers] = {};

        // The weave's alpha, read back once (AlphaState).
        ID3D12Resource*            m_probeBuf = nullptr;
        int                        m_probe = 0;               // 0 not asked, 1 pending, 2 opaque, 3 not
        int                        m_probeWait = 0;
        UINT64                     m_probeValue = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_probeFp{};

        // GPU timestamps (TakeTimes).
        static constexpr UINT      kMarks = 3;
        void ReadTimes(UINT slot);
        ID3D12QueryHeap*           m_timeHeap = nullptr;
        ID3D12Resource*            m_timeBuf = nullptr;
        UINT64                     m_gpuHz = 0, m_calibGpu = 0, m_calibCpu = 0;
        LONGLONG                   m_qpcHz = 1, m_calibQpcAt = 0, m_submitQpc[kBuffers] = {};
        bool                       m_timed[kBuffers] = {};
        double                     m_sumWeave = 0, m_sumMask = 0, m_sumStart = 0, m_sumDone = 0, m_maxDone = 0;
        int                        m_timeFrames = 0;

        // Conversion apart from the weave.
        void ReleaseKept();
        void TakeIfAsked();
        ID3D12Fence*               m_fenceTake = nullptr;      // signalled by our queue: a copy out of the converter's output is done
        ID3D11Fence*               m_fenceTake11 = nullptr;    // ... Direct3D 11's end of it
        UINT64                     m_takeIssued = 0;           // copies asked for so far
        ID3D12Resource*            m_kept = nullptr;           // the picture the weave reads
        bool                       m_keptValid = false, m_takeNow = false, m_tookNow = false;
        ID3D11Fence*               m_fenceFrame11 = nullptr;   // m_fenceFrame, Direct3D 11's end
        bool                       m_async = false, m_pending = false;
        UINT64                     m_pendingValue = 0, m_waitValue = 0;
        LONGLONG                   m_pendingQpc = 0;
        int                        m_asyncFlips = 0, m_asyncLate = 0, m_asyncSame = 0, m_asyncSkipped = 0, m_asyncLoopsPending = 0;
        double                     m_asyncSumMs = 0, m_asyncMaxMs = 0, m_lastConvMs = 0;

        // The picture handed over (Share).
        ID3D11Texture2D*           m_srcTex = nullptr;        // what was asked for (held)
        ID3D11Texture2D*           m_copyTex = nullptr;       // our shareable copy of it, when it isn't shareable itself
        ID3D12Resource*            m_shared12 = nullptr;
    };
}
