#include "Present12.h"
#include <d3dcompiler.h>
#include <cstring>
#pragma comment(lib, "d3d12.lib")

using namespace srw;

namespace
{
    // The typeless family of a format: a copy in it can be viewed as the plain
    // or the sRGB format on either side.
    DXGI_FORMAT TypelessOf(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        default: return f;
        }
    }
}

bool Present12::Initialize(ID3D11Device* d11, ID3D11DeviceContext* c11, IDXGIFactory2* factory, UINT width, UINT height,
                           const char* maskHLSL)
{
    if (!d11 || !c11 || !factory || !maskHLSL) return false;
    m_d11 = d11; m_c11 = c11; m_width = width; m_height = height;

    // Direct3D 12 on the same adapter as the renderer (the SR display's).
    IDXGIDevice* dd = nullptr; IDXGIAdapter* adapter = nullptr;
    HRESULT hr = d11->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd);
    if (SUCCEEDED(hr)) hr = dd->GetAdapter(&adapter);
    SAFE_RELEASE(dd);
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    SAFE_RELEASE(adapter);
    if (FAILED(hr)) { Log("Present12: no Direct3D 12 device (0x%08X)", (unsigned)hr); return false; }

    // Its queue a class up (as the Direct3D 11 device's GPU priority): the
    // weave is what must make the refresh.
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) { qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL; hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)); }
    if (FAILED(hr)) { Log("Present12: no command queue (0x%08X)", (unsigned)hr); return false; }
    for (UINT i = 0; i < kBuffers && SUCCEEDED(hr); ++i)
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i]));
    if (SUCCEEDED(hr)) hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0], nullptr, IID_PPV_ARGS(&m_list));
    if (SUCCEEDED(hr)) hr = m_list->Close();
    if (SUCCEEDED(hr)) hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceFrame));
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The fence both sides see: Direct3D 11 signals it after its drawing, our
    // queue waits on it before the weave reads the picture.
    HANDLE hFence = nullptr;
    ID3D11Device5* d11_5 = nullptr;
    if (SUCCEEDED(hr)) hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceShared));
    if (SUCCEEDED(hr)) hr = m_device->CreateSharedHandle(m_fenceShared, nullptr, GENERIC_ALL, nullptr, &hFence);
    if (SUCCEEDED(hr)) hr = d11->QueryInterface(__uuidof(ID3D11Device5), (void**)&d11_5);
    if (SUCCEEDED(hr)) hr = d11_5->OpenSharedFence(hFence, IID_PPV_ARGS(&m_fence11));
    // (... and one the other way: Direct3D 11 waits for a copy out of the
    // converter's output -- conversion apart from the weave, below.)
    {
        HANDLE hTake = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fenceTake))) &&
            SUCCEEDED(m_device->CreateSharedHandle(m_fenceTake, nullptr, GENERIC_ALL, nullptr, &hTake)))
        {
            d11_5->OpenSharedFence(hTake, IID_PPV_ARGS(&m_fenceTake11));
            CloseHandle(hTake);
        }
    }
    // (... and the frame fence, the other way: Direct3D 11 can wait for a weave.)
    {
        HANDLE hFrame = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(m_device->CreateSharedHandle(m_fenceFrame, nullptr, GENERIC_ALL, nullptr, &hFrame)))
        {
            SAFE_RELEASE(m_fenceFrame11);
            d11_5->OpenSharedFence(hFrame, IID_PPV_ARGS(&m_fenceFrame11));
            CloseHandle(hFrame);
        }
    }
    if (SUCCEEDED(hr)) hr = c11->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&m_c11_4);
    if (hFence) CloseHandle(hFence);
    SAFE_RELEASE(d11_5);
    if (FAILED(hr) || !m_event) { Log("Present12: command list / fences failed (0x%08X)", (unsigned)hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kBuffers;
    if (FAILED(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    m_rtvStep = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!RecreateSwapChain(factory, false, false)) return false;

    // The mask pass: the renderer's shader as it is. b0 its constants, t0 the
    // GPU tracker's results (not carried over yet: an empty buffer), t1 the
    // tile lists -- all three straight in the root signature.
    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr; ID3DBlob* rsb = nullptr;
    const size_t len = strlen(maskHLSL);
    const D3D_SHADER_MACRO gpuTex[2] = { { "GPU_TEX", "1" }, { nullptr, nullptr } };   // (the tracker's offsets as a texture)
    if (FAILED(D3DCompile(maskHLSL, len, "Mask", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(maskHLSL, len, "Mask", gpuTex, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err)))
    {
        Log("Present12: mask shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SAFE_RELEASE(err); SAFE_RELEASE(vsb); SAFE_RELEASE(psb);
        return false;
    }
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;
    const D3D12_DESCRIPTOR_RANGE gpuRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &gpuRange;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[2].Descriptor.ShaderRegister = 1;
    for (auto& p : rp) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = rp;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
    if (SUCCEEDED(hr)) hr = m_device->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&m_maskRoot));
    if (SUCCEEDED(hr))
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = m_maskRoot;
        pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
        pd.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
        // result = picture * m (colour), alpha = m.
        auto& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_ZERO; b.DestBlend = D3D12_BLEND_SRC_COLOR; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = 0xFFFFFFFF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = OutputFormat();
        pd.SampleDesc.Count = 1;
        hr = m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_maskPSO));
    }
    SAFE_RELEASE(err); SAFE_RELEASE(rsb); SAFE_RELEASE(vsb); SAFE_RELEASE(psb);
    if (FAILED(hr)) { Log("Present12: mask pipeline failed (0x%08X)", (unsigned)hr); return false; }
    // (t0: a one-descriptor heap -- empty until SetGpuResults.)
    {
        D3D12_DESCRIPTOR_HEAP_DESC gh{}; gh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; gh.NumDescriptors = 1; gh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_device->CreateDescriptorHeap(&gh, IID_PPV_ARGS(&m_gpuHeap)))) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R32G32B32A32_SINT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullptr, &sv, m_gpuHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Its constants and tile lists: one upload buffer per back buffer (the
    // frame before may still be reading the other), kept mapped.
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = kUploadBytes; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (UINT i = 0; i < kBuffers; ++i)
    {
        if (FAILED(m_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&m_upload[i]))) ||
            FAILED(m_upload[i]->Map(0, nullptr, (void**)&m_uploadPtr[i])))
            return false;
        memset(m_uploadPtr[i], 0, kUploadBytes);
    }
    // One pixel of the weave, read back (AlphaState).
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    bd.Width = 256;
    if (FAILED(m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&m_probeBuf))))
        return false;
    m_probeFp.Footprint.Format = m_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM; m_probeFp.Footprint.Width = 1; m_probeFp.Footprint.Height = 1;
    m_probeFp.Footprint.Depth = 1; m_probeFp.Footprint.RowPitch = 256;

    // GPU timestamps (the perf log's Direct3D 12 line): three marks a frame,
    // per back buffer.
    {
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = kBuffers * kMarks;
        D3D12_RESOURCE_DESC tb = bd; tb.Width = kBuffers * kMarks * 8;
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); m_qpcHz = f.QuadPart;
        if (FAILED(m_device->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_timeHeap))) ||
            FAILED(m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &tb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_timeBuf))) ||
            FAILED(m_queue->GetTimestampFrequency(&m_gpuHz)))
        { SAFE_RELEASE(m_timeHeap); SAFE_RELEASE(m_timeBuf); m_gpuHz = 0; }
    }
    Log("Present12: Direct3D 12 presenter (%ux%u, %s-priority queue)", m_width, m_height,
        qd.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high" : "normal");
    return true;
}

bool Present12::CreateBuffers()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    // (A plain UNORM buffer with an sRGB view, as the Direct3D 11 presenter:
    // the weaver writes linear values, the hardware encodes them.)
    D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = OutputFormat(); rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (UINT i = 0; i < kBuffers; ++i, h.ptr += m_rtvStep)
    {
        if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_back[i])))) return false;
        m_device->CreateRenderTargetView(m_back[i], &rv, h);
    }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) { sc2->SetMaximumFrameLatency(1); sc2->Release(); }
    return true;
}

void Present12::ReleaseBuffers()
{
    for (auto& b : m_back) SAFE_RELEASE(b);
}

bool Present12::Resize(UINT width, UINT height)
{
    if (!m_swapChain || width == 0 || height == 0) return false;
    if (width == m_width && height == m_height) return true;
    WaitIdle();
    ReleaseBuffers();
    if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, m_scFlags)))
    {
        Log("Present12: resizing the swap chain failed");
        return false;
    }
    m_width = width; m_height = height;
    m_probe = 0;
    return CreateBuffers();
}

void Present12::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    m_list->ResourceBarrier(1, &b);
}

void Present12::WaitIdle()
{
    if (!m_queue || !m_fenceFrame || !m_event) return;
    // (A signal of its own: it follows whatever was sent, even with nothing
    // of ours in between.)
    m_queue->Signal(m_fenceFrame, ++m_frameValue);
    if (m_fenceFrame->GetCompletedValue() < m_frameValue &&
        SUCCEEDED(m_fenceFrame->SetEventOnCompletion(m_frameValue, m_event)))
        WaitForSingleObject(m_event, 2000);
}

void Present12::ReleaseShared()
{
    SAFE_RELEASE(m_shared12);
    SAFE_RELEASE(m_copyTex);
    SAFE_RELEASE(m_srcTex);
}

ID3D12Resource* Present12::Share(ID3D11Texture2D* tex)
{
    if (!tex || !m_device) return nullptr;
    if (tex == m_srcTex && m_shared12) return m_shared12;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (m_copyTex && m_shared12 && !(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
    {
        // (Captured frames are a different texture every time: the copy made
        // for the last one serves while the size and format stay.)
        D3D11_TEXTURE2D_DESC cd{};
        m_copyTex->GetDesc(&cd);
        if (cd.Width == td.Width && cd.Height == td.Height && TypelessOf(cd.Format) == TypelessOf(td.Format) &&
            td.MipLevels == 1 && td.SampleDesc.Count == 1)
        {
            SAFE_RELEASE(m_srcTex);
            m_srcTex = tex; m_srcTex->AddRef();
            return m_shared12;
        }
    }
    WaitIdle();   // (a frame still in flight may be reading the old one)
    ReleaseShared();

    ID3D11Texture2D* shareable = tex;
    if (!(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
    {
        // Not shareable (a captured frame as it is, a picture file, the
        // region composite): a shareable copy, refreshed in EndFrame.
        D3D11_TEXTURE2D_DESC cd = td;
        cd.Format = TypelessOf(td.Format);
        cd.MipLevels = 1; cd.ArraySize = 1; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
        cd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        cd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hc = m_d11->CreateTexture2D(&cd, nullptr, &m_copyTex);
        if (FAILED(hc)) { cd.Format = td.Format; hc = m_d11->CreateTexture2D(&cd, nullptr, &m_copyTex); }
        if (FAILED(hc) || td.MipLevels != 1 || td.SampleDesc.Count != 1)
        {
            Log("Present12: no shareable copy of a %ux%u format-%d texture (0x%08X)", td.Width, td.Height, (int)td.Format, (unsigned)hc);
            SAFE_RELEASE(m_copyTex);
            return nullptr;
        }
        shareable = m_copyTex;
    }
    IDXGIResource1* r1 = nullptr; HANDLE h = nullptr;
    HRESULT hr = shareable->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
    if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (SUCCEEDED(hr)) hr = m_device->OpenSharedHandle(h, IID_PPV_ARGS(&m_shared12));
    if (h) CloseHandle(h);
    SAFE_RELEASE(r1);
    if (FAILED(hr))
    {
        Log("Present12: opening the picture in Direct3D 12 failed (0x%08X)", (unsigned)hr);
        ReleaseShared();
        return nullptr;
    }
    m_srcTex = tex; m_srcTex->AddRef();
    Log("Present12: the weaver's picture %ux%u is %s", td.Width, td.Height,
        m_copyTex ? "copied across each frame (not a shareable texture)" : "shared with Direct3D 11 (no copy)");
    return m_shared12;
}

void Present12::BeginFrame()
{
    if (!m_swapChain3 || m_inFrame) return;
    m_index = m_swapChain3->GetCurrentBackBufferIndex() % kBuffers;
    // (This buffer's allocator and constants: free once the frame that last
    // used them has run.)
    if (m_fenceFrame->GetCompletedValue() < m_slotValue[m_index] &&
        SUCCEEDED(m_fenceFrame->SetEventOnCompletion(m_slotValue[m_index], m_event)))
        WaitForSingleObject(m_event, 2000);
    ReadTimes(m_index);   // (what this buffer's last frame measured: it has run)
    m_alloc[m_index]->Reset();
    m_list->Reset(m_alloc[m_index], nullptr);
    Mark(0);
    TakeIfAsked();
    Transition(m_back[m_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)m_index * m_rtvStep;
    const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_list->ClearRenderTargetView(rtv, black, 0, nullptr);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = Viewport(); const D3D12_RECT sc = Scissor();
    m_list->RSSetViewports(1, &vp);
    m_list->RSSetScissorRects(1, &sc);
    m_inFrame = true;
}

int Present12::AlphaState()
{
    if (m_probe == 1 && m_fenceFrame->GetCompletedValue() >= m_probeValue && m_probeValue != 0)
    {
        uint8_t* p = nullptr; const D3D12_RANGE rr{ 0, 8 }; const D3D12_RANGE none{ 0, 0 };
        if (SUCCEEDED(m_probeBuf->Map(0, &rr, (void**)&p)) && p)
        {
            m_probe = (m_hdr ? (p[6] == 0x00 && p[7] == 0x3C) : p[3] == 255) ? 2 : 3;   // (alpha 1: 255, or the half float 0x3C00)
            m_probeBuf->Unmap(0, &none);
            Log("Present12: the SR weave %s -- full-screen mask pass %s", m_probe == 2 ? "keeps the picture opaque" : "writes its own alpha",
                m_probe == 2 ? "skipped when nothing is hidden" : "kept");
        }
    }
    else if (m_probe == 0 && m_inFrame && ++m_probeWait > 240)   // (a real picture by then: the first frames can be black)
    {
        // One pixel from the middle of the weave (before any mask), read when
        // that frame has run.
        Transition(m_back[m_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = m_back[m_index]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = m_probeBuf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = m_probeFp;
        const D3D12_BOX box{ m_width / 4, m_height / 3, 0, m_width / 4 + 1, m_height / 3 + 1, 1 };   // (a pixel well inside the picture)
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        Transition(m_back[m_index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_probe = 1; m_probeValue = 0;   // (its fence value is set in EndFrame)
    }
    return m_probe >= 2 ? m_probe : 0;
}

void Present12::DrawMask(const void* cb, size_t cbBytes, const void* tiles, size_t tilesBytes,
                         const D3D12_RECT* scissors, int count)
{
    if (!m_inFrame || !m_maskPSO || cbBytes > kCbBytes || tilesBytes > kTilesBytes) return;
    memcpy(m_uploadPtr[m_index], cb, cbBytes);
    memcpy(m_uploadPtr[m_index] + kTilesAt, tiles, tilesBytes);
    const D3D12_GPU_VIRTUAL_ADDRESS base = m_upload[m_index]->GetGPUVirtualAddress();

    // (Everything set again: the weaver left its own state on the list.)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)m_index * m_rtvStep;
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = Viewport();
    m_list->RSSetViewports(1, &vp);
    m_list->SetPipelineState(m_maskPSO);
    m_list->SetGraphicsRootSignature(m_maskRoot);
    m_list->SetGraphicsRootConstantBufferView(0, base);
    m_list->SetDescriptorHeaps(1, &m_gpuHeap);
    m_list->SetGraphicsRootDescriptorTable(1, m_gpuHeap->GetGPUDescriptorHandleForHeapStart());   // (the GPU tracker's results: SetGpuResults)
    m_list->SetGraphicsRootShaderResourceView(2, base + kTilesAt);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int i = 0; i < count; ++i)
    {
        m_list->RSSetScissorRects(1, &scissors[i]);
        m_list->DrawInstanced(3, 1, 0, 0);
    }
}

void Present12::EndFrame()
{
    if (!m_inFrame) return;
    m_inFrame = false;
    if (!m_async && m_copyTex && m_srcTex) m_c11->CopyResource(m_copyTex, m_srcTex);
    Mark(2);
    if (m_timeHeap)
        m_list->ResolveQueryData(m_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, m_index * kMarks, kMarks, m_timeBuf, (UINT64)m_index * kMarks * 8);
    Transition(m_back[m_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_list->Close();
    // Direct3D 11's drawing so far (the conversion into the shared picture)
    // first: it signals the shared fence when done, and our queue waits for
    // that on the GPU -- nothing blocks here.
    if (m_async)
    {
        // (Weaving from a finished copy: nothing to wait for -- but for the
        // very first picture, AsyncSubmitted.)
        if (m_waitValue) { m_queue->Wait(m_fenceShared, m_waitValue); m_waitValue = 0; }
        if (m_pending) ++m_asyncLoopsPending;
    }
    else
    {
        m_c11_4->Signal(m_fence11, ++m_sharedValue);
        m_c11->Flush();
        m_queue->Wait(m_fenceShared, m_sharedValue);
    }
    ID3D12CommandList* lists[1] = { m_list };
    { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); m_submitQpc[m_index] = q.QuadPart; m_timed[m_index] = true; }
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fenceFrame, ++m_frameValue);
    m_slotValue[m_index] = m_frameValue;
    if (m_tookNow) { m_queue->Signal(m_fenceTake, m_takeIssued); m_tookNow = false; }   // (the output may be written again)
    if (m_probe == 1 && m_probeValue == 0) m_probeValue = m_frameValue;
}

// GPU timestamps on the frame being recorded: 0 its start, 1 after the weave,
// 2 its end (after the mask).
void Present12::Mark(int i)
{
    if (m_timeHeap && i >= 0 && i < (int)kMarks && (i != 1 || m_inFrame))
        m_list->EndQuery(m_timeHeap, D3D12_QUERY_TYPE_TIMESTAMP, m_index * kMarks + i);
}

// Adds the last frame recorded with this buffer to the running totals: the
// GPU time of its weave and mask, and how long after it was sent the GPU
// began / finished it (the wait for Direct3D 11's drawing and for the GPU).
void Present12::ReadTimes(UINT slot)
{
    if (!m_timeBuf || !m_timed[slot] || m_gpuHz == 0) return;
    m_timed[slot] = false;
    // (The GPU's clock against the CPU's, renewed about once a second.)
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    if (m_calibQpcAt == 0 || now.QuadPart - m_calibQpcAt > m_qpcHz)
    {
        UINT64 g = 0, c = 0;
        if (SUCCEEDED(m_queue->GetClockCalibration(&g, &c))) { m_calibGpu = g; m_calibCpu = c; m_calibQpcAt = now.QuadPart; }
    }
    UINT64* t = nullptr; const D3D12_RANGE rr{ (SIZE_T)slot * kMarks * 8, (SIZE_T)(slot + 1) * kMarks * 8 }; const D3D12_RANGE none{ 0, 0 };
    if (FAILED(m_timeBuf->Map(0, &rr, (void**)&t)) || !t) return;
    const UINT64 t0 = t[slot * kMarks], t1 = t[slot * kMarks + 1], t2 = t[slot * kMarks + 2];
    m_timeBuf->Unmap(0, &none);
    if (t2 < t1 || t1 < t0 || m_calibQpcAt == 0) return;
    const double toMs = 1000.0 / (double)m_gpuHz;
    // (The submit time on the GPU's clock.)
    const double submitGpu = (double)m_calibGpu + ((double)m_submitQpc[slot] - (double)m_calibCpu) * ((double)m_gpuHz / (double)m_qpcHz);
    const double start = ((double)t0 - submitGpu) * toMs, done = ((double)t2 - submitGpu) * toMs;
    m_sumWeave += (double)(t1 - t0) * toMs; m_sumMask += (double)(t2 - t1) * toMs;
    m_sumStart += start; m_sumDone += done;
    if (done > m_maxDone) m_maxDone = done;
    ++m_timeFrames;
}

bool Present12::TakeTimes(Times& out)
{
    out = Times{};
    if (m_timeFrames <= 0) return false;
    const double n = (double)m_timeFrames;
    out.frames = m_timeFrames; out.weaveMs = m_sumWeave / n; out.maskMs = m_sumMask / n;
    out.startMs = m_sumStart / n; out.doneMs = m_sumDone / n; out.worstDoneMs = m_maxDone;
    m_sumWeave = m_sumMask = m_sumStart = m_sumDone = m_maxDone = 0.0; m_timeFrames = 0;
    return true;
}

// The GPU tracker's results (a shareable 16x1 Direct3D 11 texture, one int4 a
// slot) for the mask's pictures that follow the page.
bool Present12::SetGpuResults(ID3D11Texture2D* tex)
{
    if (!tex || !m_gpuHeap) return false;
    if (tex == m_gpuSrc && m_gpu12) return true;
    WaitIdle();
    SAFE_RELEASE(m_gpu12); m_gpuSrc = nullptr;
    IDXGIResource1* r1 = nullptr; HANDLE h = nullptr;
    HRESULT hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
    if (SUCCEEDED(hr)) hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (SUCCEEDED(hr)) hr = m_device->OpenSharedHandle(h, IID_PPV_ARGS(&m_gpu12));
    if (h) CloseHandle(h);
    SAFE_RELEASE(r1);
    if (FAILED(hr)) { Log("Present12: the tracker offsets could not be opened (0x%08X)", (unsigned)hr); return false; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R32G32B32A32_SINT; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_gpu12, &sv, m_gpuHeap->GetCPUDescriptorHandleForHeapStart());
    m_gpuSrc = tex;
    Log("Present12: the mask follows the GPU tracker's offsets");
    return true;
}

void Present12::Shutdown()
{
    WaitIdle();
    ReleaseShared();
    ReleaseKept();
    SAFE_RELEASE(m_fenceTake11); SAFE_RELEASE(m_fenceTake);
    SAFE_RELEASE(m_fenceFrame11);
    SAFE_RELEASE(m_probeBuf);
    SAFE_RELEASE(m_timeBuf); SAFE_RELEASE(m_timeHeap);
    for (UINT i = 0; i < kBuffers; ++i)
    {
        if (m_upload[i] && m_uploadPtr[i]) m_upload[i]->Unmap(0, nullptr);
        m_uploadPtr[i] = nullptr;
        SAFE_RELEASE(m_upload[i]);
    }
    SAFE_RELEASE(m_maskPSO); SAFE_RELEASE(m_maskRoot);
    SAFE_RELEASE(m_gpu12); SAFE_RELEASE(m_gpuHeap); m_gpuSrc = nullptr;
    SAFE_RELEASE(m_rowsBuf);
    ReleaseBuffers();
    SAFE_RELEASE(m_rtvHeap);
    SAFE_RELEASE(m_swapChain3); SAFE_RELEASE(m_swapChain);
    SAFE_RELEASE(m_list);
    SAFE_RELEASE(m_setupAlloc);
    for (auto& a : m_alloc) SAFE_RELEASE(a);
    SAFE_RELEASE(m_fenceFrame); SAFE_RELEASE(m_fenceShared);
    SAFE_RELEASE(m_fence11); SAFE_RELEASE(m_c11_4);
    SAFE_RELEASE(m_queue);
    SAFE_RELEASE(m_device);
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    m_inFrame = false; m_probe = 0;
    m_sharedValue = m_frameValue = 0; m_slotValue[0] = m_slotValue[1] = 0;
}

// ---- Conversion apart from the weave ---------------------------------------
// A slow conversion (Recovered Colour on video) used to hold the weave back:
// the weave waited for it on the GPU. Here the weave reads a picture of its
// own ("kept"), always the same texture; when a conversion has finished and
// changed the converter's output, the next frame first copies that output into
// it (on this queue, so never while it is being woven). The weave, and the eye
// tracking in it, then go out every refresh whatever the conversion takes; a
// slow conversion shows a refresh later. No conversion starts while the one
// before is still running, nor before its picture has been copied out.
void Present12::ReleaseKept()
{
    // (Direct3D 11 may be waiting for a copy that now won't happen: let it go.)
    if (m_queue && m_fenceTake && m_takeIssued) m_queue->Signal(m_fenceTake, m_takeIssued);
    SAFE_RELEASE(m_kept);
    m_keptValid = false; m_takeNow = false; m_tookNow = false; m_pending = false; m_waitValue = 0;
}

void Present12::SetAsync(bool on)
{
    if (m_async && !on) { WaitIdle(); ReleaseKept(); }
    m_async = on;
}

// Before a conversion is drawn: Direct3D 11 waits (on the GPU) until every
// copy asked for so far has been made -- the conversion writes what they read.
void Present12::AsyncBeforeConvert()
{
    if (m_fenceTake11 && m_takeIssued) m_c11_4->Wait(m_fenceTake11, m_takeIssued);
}

// After it: out is the converter's output (shareable). False: could not be set
// up (the caller weaves the output directly, as without all this).
bool Present12::AsyncSubmitted(ID3D11Texture2D* out)
{
    ID3D12Resource* out12 = Share(out);
    if (!out12 || m_copyTex || !m_fenceTake11) return false;
    const D3D12_RESOURCE_DESC od = out12->GetDesc();
    D3D12_RESOURCE_DESC kd{};
    if (m_kept) kd = m_kept->GetDesc();
    if (!m_kept || kd.Width != od.Width || kd.Height != od.Height || kd.Format != od.Format)
    {
        WaitIdle();
        ReleaseKept();
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = od; d.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_kept))))
        { Log("Present12: the weave's own picture could not be made"); return false; }
        Log("Present12: conversion apart from the weave -- the weave keeps a %llux%u picture of its own", (unsigned long long)od.Width, od.Height);
    }
    m_c11_4->Signal(m_fence11, ++m_sharedValue);
    m_c11->Flush();   // (started now, not with the next batch of commands)
    if (!m_keptValid)
    {
        // (The first picture: nothing to show meanwhile -- this frame copies it
        // out and waits for it on the GPU, as without all this.)
        m_keptValid = true; m_pending = false; m_waitValue = m_sharedValue;
        if (!m_takeNow) { m_takeNow = true; ++m_takeIssued; }
        return true;
    }
    m_pending = true; m_pendingValue = m_sharedValue;
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q); m_pendingQpc = q.QuadPart;
    return true;
}

bool Present12::ConversionPending()
{
    return m_pending && m_fenceShared->GetCompletedValue() < m_pendingValue;
}

bool Present12::AsyncReady(double waitMs)
{
    if (!m_pending) return false;
    if (m_fenceShared->GetCompletedValue() < m_pendingValue && waitMs > 0.05 &&
        SUCCEEDED(m_fenceShared->SetEventOnCompletion(m_pendingValue, m_event)))
        WaitForSingleObject(m_event, (DWORD)(waitMs + 0.5));
    return m_fenceShared->GetCompletedValue() >= m_pendingValue;
}

void Present12::AsyncResolve(int changed)
{
    if (changed < 0) return;   // (not known yet: asked again next loop)
    m_pending = false;
    if (changed == 0) { ++m_asyncSame; return; }   // (nothing drawn: the picture kept is still right)
    if (!m_takeNow) { m_takeNow = true; ++m_takeIssued; }
    LARGE_INTEGER q{}; QueryPerformanceCounter(&q);
    const double ms = (double)(q.QuadPart - m_pendingQpc) * 1000.0 / (double)m_qpcHz;
    m_asyncSumMs += ms; if (ms > m_asyncMaxMs) m_asyncMaxMs = ms;
    ++m_asyncFlips;
    if (m_asyncLoopsPending > 0) ++m_asyncLate;
    m_asyncLoopsPending = 0;
}

// (BeginFrame: the finished output copied into the picture kept, ahead of the weave.)
void Present12::TakeIfAsked()
{
    if (!m_async || !m_takeNow || !m_kept || !m_shared12) return;
    Transition(m_kept, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    m_list->CopyResource(m_kept, m_shared12);
    Transition(m_kept, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    m_takeNow = false; m_tookNow = true;
}

bool Present12::TakeAsyncStats(AsyncStats& out)
{
    out = AsyncStats{ m_asyncFlips, m_asyncLate, m_asyncSame, m_asyncSkipped,
                      m_asyncFlips ? m_asyncSumMs / m_asyncFlips : 0.0, m_asyncMaxMs };
    const bool any = m_asyncFlips || m_asyncSame || m_asyncSkipped;
    m_asyncFlips = m_asyncLate = m_asyncSame = m_asyncSkipped = 0; m_asyncSumMs = m_asyncMaxMs = 0.0;
    return any;
}

// The swap chain, made from our queue (again, when the kind changes). As the
// Direct3D 11 presenter's: two buffers, the frame-latency waitable; see-through
// where alpha is 0 -- or, opaque: nothing shows through, and Windows can then
// put it on the display directly instead of composing it with the desktop
// (tearing: allowed to, as that needs).
bool Present12::RecreateSwapChain(IDXGIFactory2* factory, bool opaque, bool tearing)
{
    WaitIdle();
    ReleaseBuffers();
    SAFE_RELEASE(m_swapChain3); SAFE_RELEASE(m_swapChain);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = m_width; sd.Height = m_height; sd.Format = m_hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = kBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = opaque ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (opaque && tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    HRESULT hr = factory->CreateSwapChainForComposition(m_queue, &sd, nullptr, &m_swapChain);
    if (FAILED(hr) && (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
    {
        sd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = factory->CreateSwapChainForComposition(m_queue, &sd, nullptr, &m_swapChain);
    }
    if (SUCCEEDED(hr)) hr = m_swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&m_swapChain3);
    if (FAILED(hr)) { Log("Present12: composition swap chain failed (0x%08X)", (unsigned)hr); return false; }
    m_scFlags = sd.Flags;
    if (m_hdr)
    {
        // (scRGB: linear, 1.0 = SDR white, brighter values kept -- Windows maps
        // it to whatever the display is, HDR or not.)
        const HRESULT hc = m_swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        Log("Present12: 16-bit float output (scRGB) %s (0x%08X)", SUCCEEDED(hc) ? "set" : "REFUSED", (unsigned)hc);
    }
    m_inFrame = false;
    m_slotValue[0] = m_slotValue[1] = 0;
    return CreateBuffers();
}

// A band of the frame being recorded (rows y0 .. y0 + rows, the whole width)
// copied out for reading: CopyRows while recording, then -- once the frame has
// been sent and has run (EndFrame, WaitIdle) -- FetchRows. 8-bit output only.
bool Present12::CopyRows(UINT y0, UINT rows)
{
    if (!m_inFrame || m_hdr || rows == 0 || y0 + rows > m_height) return false;
    D3D12_RESOURCE_DESC d = m_back[m_index]->GetDesc();
    d.Height = rows;
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&d, 0, 1, 0, &m_rowsFp, nullptr, nullptr, &total);
    SAFE_RELEASE(m_rowsBuf);
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1;
    bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_rowsBuf))))
        return false;
    Transition(m_back[m_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = m_back[m_index]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = m_rowsBuf; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = m_rowsFp;
    const D3D12_BOX box{ 0, y0, 0, m_width, y0 + rows, 1 };
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Transition(m_back[m_index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_rowsCount = rows;
    return true;
}

bool Present12::FetchRows(std::vector<uint8_t>& out)
{
    if (!m_rowsBuf || !m_rowsCount) return false;
    uint8_t* p = nullptr;
    if (FAILED(m_rowsBuf->Map(0, nullptr, (void**)&p)) || !p) return false;
    out.resize((size_t)m_width * m_rowsCount * 4);
    for (UINT y = 0; y < m_rowsCount; ++y)
        memcpy(&out[(size_t)y * m_width * 4], p + m_rowsFp.Offset + (size_t)y * m_rowsFp.Footprint.RowPitch, (size_t)m_width * 4);
    const D3D12_RANGE none{ 0, 0 };
    m_rowsBuf->Unmap(0, &none);
    SAFE_RELEASE(m_rowsBuf); m_rowsCount = 0;
    return true;
}
