// dx12test: the first building block of the DX12 weaver option. SR Loom's
// capture and conversion stay Direct3D 11; the DX12 weaver needs the converted
// picture as a Direct3D 12 texture. This checks, on this machine's GPU and
// driver, that a D3D11 texture can be handed to D3D12 without a copy and read
// there in step:
//   1. a D3D11 texture made shareable (NT handle), filled with a known colour;
//   2. opened on a D3D12 device on the same adapter;
//   3. a fence shared both ways, so D3D12 waits for D3D11's drawing;
//   4. D3D12 reads the texture back and the colour is compared.
// Prints numbers only. Build: cmake --build build/x64 --config Release --target dx12test
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdint>
#include <chrono>

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

int main()
{
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { printf("no DXGI factory\n"); return 1; }
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapters1(0, &adapter);
    DXGI_ADAPTER_DESC1 ad{}; if (adapter) adapter->GetDesc1(&ad);
    printf("adapter: %ls\n", ad.Description);

    // Direct3D 11 (as SR Loom's renderer) and Direct3D 12 on the same adapter.
    ID3D11Device* d11 = nullptr; ID3D11DeviceContext* c11 = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d11, nullptr, &c11);
    if (FAILED(hr)) { printf("D3D11 device failed 0x%08lX\n", (unsigned long)hr); return 1; }
    ID3D11Device5* d11_5 = nullptr; ID3D11DeviceContext4* c11_4 = nullptr;
    d11->QueryInterface(&d11_5); c11->QueryInterface(&c11_4);
    ID3D12Device* d12 = nullptr;
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr)) { printf("D3D12 device failed 0x%08lX\n", (unsigned long)hr); return 1; }
    if (!d11_5 || !c11_4) { printf("no ID3D11Device5 / DeviceContext4 (shared fences): Windows too old\n"); return 1; }

    // 1. The shareable D3D11 texture (the converter's output, in the app): 4K side by side.
    const UINT W = 7680, H = 2160;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;   // (as the converter's: sRGB views, a plain view for compute)
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D* tex11 = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    hr = d11->CreateTexture2D(&td, nullptr, &tex11);
    if (FAILED(hr)) { printf("shareable D3D11 texture failed 0x%08lX\n", (unsigned long)hr); return 1; }
    D3D11_RENDER_TARGET_VIEW_DESC rvd{}; rvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d11->CreateRenderTargetView(tex11, &rvd, &rtv);
    IDXGIResource1* res1 = nullptr; tex11->QueryInterface(&res1);
    HANDLE hTex = nullptr;
    hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hTex);
    if (FAILED(hr)) { printf("CreateSharedHandle failed 0x%08lX\n", (unsigned long)hr); return 1; }

    // 2. Opened in D3D12.
    ID3D12Resource* tex12 = nullptr;
    hr = d12->OpenSharedHandle(hTex, IID_PPV_ARGS(&tex12));
    printf("texture opened in D3D12: %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "NO", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    // 3. One fence both APIs see.
    ID3D12Fence* f12 = nullptr; HANDLE hFence = nullptr; ID3D11Fence* f11 = nullptr;
    hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12));
    if (SUCCEEDED(hr)) hr = d12->CreateSharedHandle(f12, nullptr, GENERIC_ALL, nullptr, &hFence);
    if (SUCCEEDED(hr)) hr = d11_5->OpenSharedFence(hFence, IID_PPV_ARGS(&f11));
    printf("shared fence: %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "NO", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    ID3D12CommandQueue* queue = nullptr; ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    hr = d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    printf("high-priority D3D12 queue: %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "NO", (unsigned long)hr);
    if (FAILED(hr)) { qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL; hr = d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)); }
    if (FAILED(hr)) return 1;
    d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));

    // A read-back buffer for one row's worth around the middle (enough to check).
    D3D12_RESOURCE_DESC rd12 = tex12->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    d12->GetCopyableFootprints(&rd12, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    hr = d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { printf("read-back buffer failed 0x%08lX\n", (unsigned long)hr); return 1; }

    // 4. Twenty rounds: D3D11 fills the texture with a new colour and signals;
    // D3D12 waits for that, copies it out, and the colour must be the new one.
    ID3D12Fence* done = nullptr; d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int ok = 0; double ms = 0;
    for (int i = 1; i <= 20; ++i)
    {
        const float col[4] = { i / 32.0f, 1.0f - i / 32.0f, (i % 4) / 4.0f, 1.0f };
        const auto t0 = std::chrono::steady_clock::now();
        c11->ClearRenderTargetView(rtv, col);
        c11_4->Signal(f11, (UINT64)i);
        c11->Flush();

        queue->Wait(f12, (UINT64)i);
        if (i > 1) { alloc->Reset(); list->Reset(alloc, nullptr); }
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex12;
        b.Transition.Subresource = 0; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = tex12; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[1] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(done, (UINT64)i);
        done->SetEventOnCompletion((UINT64)i, ev);
        WaitForSingleObject(ev, 2000);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        uint8_t* p = nullptr; D3D12_RANGE rr{ 0, (SIZE_T)total };
        if (SUCCEEDED(rb->Map(0, &rr, (void**)&p)) && p)
        {
            const uint8_t* px = p + (size_t)(H / 2) * fp.Footprint.RowPitch + (size_t)(W / 2) * 4;
            const int want[3] = { (int)(col[0] * 255.0f + 0.5f), (int)(col[1] * 255.0f + 0.5f), (int)(col[2] * 255.0f + 0.5f) };
            if (abs(px[0] - want[0]) <= 1 && abs(px[1] - want[1]) <= 1 && abs(px[2] - want[2]) <= 1) ++ok;
            D3D12_RANGE none{ 0, 0 }; rb->Unmap(0, &none);
        }
    }
    printf("D3D11 draw -> D3D12 read, in step: %d of 20 right (%.2f ms a round, read-back included)\n", ok, ms / 20.0);
    printf("%s\n", ok == 20 ? "RESULT: sharing works -- the DX12 weaver can take the converter's picture without a copy"
                            : "RESULT: sharing did NOT work reliably on this system");

    // 5. The presenter's swap chain: made from the D3D12 queue, for composition,
    // see-through (premultiplied alpha), with the frame-latency waitable; an sRGB
    // view of its back buffer (what the weaver draws into).
    {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 3840; sd.Height = 2160; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.Scaling = DXGI_SCALING_STRETCH; sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        IDXGIFactory2* f2 = nullptr; factory->QueryInterface(&f2);
        IDXGISwapChain1* sc = nullptr;
        hr = f2 ? f2->CreateSwapChainForComposition(queue, &sd, nullptr, &sc) : E_FAIL;
        printf("composition swap chain from the D3D12 queue (see-through, waitable): %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "NO", (unsigned long)hr);
        if (sc)
        {
            ID3D12Resource* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
            D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
            ID3D12DescriptorHeap* heap = nullptr; d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
            D3D12_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            if (bb && heap) d12->CreateRenderTargetView(bb, &rv, heap->GetCPUDescriptorHandleForHeapStart());
            printf("  sRGB view of its back buffer: %s\n", SUCCEEDED(d12->GetDeviceRemovedReason()) && bb ? "yes" : "NO");
            // ... and an sRGB view of the shared (typeless) picture, as the weaver makes.
            D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 1;
            ID3D12DescriptorHeap* sheap = nullptr; d12->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&sheap));
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
            if (sheap) d12->CreateShaderResourceView(tex12, &sv, sheap->GetCPUDescriptorHandleForHeapStart());
            printf("  sRGB view of the shared picture: %s (its format in D3D12: %d, flags 0x%X)\n",
                   SUCCEEDED(d12->GetDeviceRemovedReason()) ? "yes" : "NO", (int)rd12.Format, (unsigned)rd12.Flags);
            hr = sc->Present(0, 0);
            printf("  present: %s (0x%08lX)\n", SUCCEEDED(hr) ? "yes" : "NO", (unsigned long)hr);
            queue->Signal(done, 100); done->SetEventOnCompletion(100, ev); WaitForSingleObject(ev, 2000);
            REL(sheap); REL(heap); REL(bb); REL(sc);
        }
        REL(f2);
    }
    // 6. What else this GPU offers Direct3D 12 (for later stages).
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
        if (SUCCEEDED(d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
            printf("variable-rate shading tier %d (tile %u px, extra rates %d)\n", (int)o6.VariableShadingRateTier, o6.ShadingRateImageTileSize, (int)o6.AdditionalShadingRatesSupported);
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
        if (SUCCEEDED(d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))))
            printf("casting fully typed formats: %d\n", (int)o3.CastingFullyTypedFormatSupported);
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        ID3D12CommandQueue* cq = nullptr;
        printf("second (compute) queue: %s\n", SUCCEEDED(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&cq))) ? "yes" : "NO");
        REL(cq);
    }

    CloseHandle(ev); CloseHandle(hFence); CloseHandle(hTex);
    REL(done); REL(rb); REL(list); REL(alloc); REL(queue); REL(f11); REL(f12); REL(tex12); REL(res1); REL(rtv); REL(tex11);
    REL(c11_4); REL(d11_5); REL(d12); REL(c11); REL(d11); REL(adapter); REL(factory);
    return ok == 20 ? 0 : 2;
}
