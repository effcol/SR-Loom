// dx12bench: does Direct3D 12 run Recovered Colour's compose kernel faster than
// Direct3D 11? The same shader (bench.hlsl), on the same textures (shared
// between the two devices), drawn into a 7680x2160 target, timed on the GPU:
//   1. Direct3D 11, the fxc build (Shader Model 5) -- how the app runs today;
//   2. Direct3D 12, that same fxc build;
//   3. Direct3D 12, the dxc build (Shader Model 6.2);
//   4. Direct3D 12, the dxc build with true 16-bit floats.
// Usage: dx12bench <folder with bench.hlsl, bench_f32.dxil, bench_f16.dxil> [draws]
// Prints numbers only.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    d.resize(n > 0 ? (size_t)n : 0);
    if (n > 0) fread(d.data(), 1, (size_t)n, f);
    fclose(f);
    return d;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dx12bench <folder> [draws]\n"); return 1; }
    const std::string dir = argv[1];
    const int draws = argc > 2 ? atoi(argv[2]) : 100;
    const UINT SW = 3840, SH = 2160, OW = 7680, OH = 2160;

    IDXGIFactory4* factory = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr; factory->EnumAdapters1(0, &adapter);
    ID3D11Device* d11 = nullptr; ID3D11DeviceContext* c11 = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d11, nullptr, &c11))) return 1;
    ID3D12Device* d12 = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)))) return 1;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_2 };
    d12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    d12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    printf("shader model 0x%X, native 16-bit shader ops: %d\n", (unsigned)sm.HighestShaderModel, (int)o4.Native16BitShaderOpsSupported);

    // The textures, made in Direct3D 11 and shared: a picture with detail and
    // edges (blocks of colour, a gradient, fine noise), a disparity map, the target.
    std::vector<uint8_t> pic((size_t)SW * SH * 4);
    uint32_t rng = 12345;
    for (UINT y = 0; y < SH; ++y)
        for (UINT x = 0; x < SW; ++x)
        {
            rng = rng * 1664525u + 1013904223u;
            const UINT bx = x / 97, by = y / 61;
            uint8_t* p = &pic[((size_t)y * SW + x) * 4];
            p[0] = (uint8_t)(((bx * 53 + by * 17) & 127) + (x * 96 / SW) + ((rng >> 24) & 15));
            p[1] = (uint8_t)(((bx * 29 + by * 71) & 127) + (y * 96 / SH) + ((rng >> 16) & 15));
            p[2] = (uint8_t)(((bx * 11 + by * 37) & 127) + ((x + y) * 48 / SW) + ((rng >> 8) & 15));
            p[3] = 255;
        }
    const UINT DW = SW / 4, DH = SH / 4;
    std::vector<uint16_t> dm((size_t)DW * DH * 4);
    auto half = [](float f) -> uint16_t {   // (small positive / negative values only)
        uint32_t u; memcpy(&u, &f, 4);
        const uint32_t s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 0xFF) - 127 + 15; const uint32_t m = (u >> 13) & 0x3FF;
        if (e <= 0) return (uint16_t)s;
        return (uint16_t)(s | (e << 10) | m);
    };
    for (UINT y = 0; y < DH; ++y)
        for (UINT x = 0; x < DW; ++x)
            dm[((size_t)y * DW + x) * 4] = half(0.002f + 0.012f * (float)((x / 23 + y / 19) % 7) / 7.0f);

    auto shared = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* data, UINT pitch, ID3D11Texture2D** t11, ID3D12Resource** t12) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_SUBRESOURCE_DATA sd{ data, pitch, 0 };
        if (FAILED(d11->CreateTexture2D(&td, data ? &sd : nullptr, t11))) return false;
        IDXGIResource1* r1 = nullptr; (*t11)->QueryInterface(&r1); HANDLE h2 = nullptr;
        const bool ok = r1 && SUCCEEDED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h2)) &&
                        SUCCEEDED(d12->OpenSharedHandle(h2, IID_PPV_ARGS(t12)));
        if (h2) CloseHandle(h2); REL(r1);
        return ok;
    };
    ID3D11Texture2D *src11 = nullptr, *disp11 = nullptr, *out11 = nullptr;
    ID3D12Resource  *src12 = nullptr, *disp12 = nullptr, *out12 = nullptr;
    const UINT rtBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!shared(SW, SH, DXGI_FORMAT_R8G8B8A8_TYPELESS, rtBind, pic.data(), SW * 4, &src11, &src12) ||
        !shared(DW, DH, DXGI_FORMAT_R16G16B16A16_FLOAT, rtBind, dm.data(), DW * 8, &disp11, &disp12) ||
        !shared(OW, OH, DXGI_FORMAT_R8G8B8A8_TYPELESS, rtBind, nullptr, 0, &out11, &out12))
    { printf("shared textures failed\n"); return 1; }

    // The shader: fxc here (as the app's build), dxc ahead of time (the files).
    const std::vector<uint8_t> hlsl = ReadFile(dir + "/bench.hlsl");
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    const D3D_SHADER_MACRO fast[2] = { { "FASTM", "1" }, { nullptr, nullptr } };
    const D3D_SHADER_MACRO* defs = getenv("BENCH_FASTM") ? fast : nullptr;
    if (hlsl.empty() ||
        FAILED(D3DCompile(hlsl.data(), hlsl.size(), "bench", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)) ||
        FAILED(D3DCompile(hlsl.data(), hlsl.size(), "bench", defs, nullptr, argc > 3 ? "PSMain2" : "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err)))
    { printf("fxc compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "no bench.hlsl"); return 1; }
    const std::vector<uint8_t> dx32 = ReadFile(dir + "/bench_f32.dxil"), dx16 = ReadFile(dir + "/bench_f16.dxil");
    const float cb[4] = { (float)SW, (float)SH, (float)OW, (float)OH };

    // ---- Direct3D 11 ----
    ID3D11VertexShader* vs11 = nullptr; ID3D11PixelShader* ps11 = nullptr;
    d11->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs11);
    d11->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps11);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView *srcV = nullptr, *dispV = nullptr; ID3D11RenderTargetView* outV = nullptr;
    d11->CreateShaderResourceView(src11, &sv, &srcV);
    d11->CreateShaderResourceView(disp11, nullptr, &dispV);
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d11->CreateRenderTargetView(out11, &rv, &outV);
    D3D11_SAMPLER_DESC smp{}; smp.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT; smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* ss = nullptr; d11->CreateSamplerState(&smp, &ss);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA bi{ cb, 0, 0 };
    ID3D11Buffer* cb11 = nullptr; d11->CreateBuffer(&bd, &bi, &cb11);
    D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; ID3D11Query *qDis = nullptr, *qA = nullptr, *qB = nullptr;
    d11->CreateQuery(&qd, &qDis); qd.Query = D3D11_QUERY_TIMESTAMP; d11->CreateQuery(&qd, &qA); d11->CreateQuery(&qd, &qB);
    D3D11_TEXTURE2D_DESC st{}; st.Width = 64; st.Height = 1; st.MipLevels = 1; st.ArraySize = 1; st.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    st.SampleDesc.Count = 1; st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr; d11->CreateTexture2D(&st, nullptr, &stage);

    auto sum11 = [&]() -> unsigned {   // (64 pixels from the middle of the right eye: the same from every build?)
        D3D11_BOX box{ OW * 3 / 4, OH / 2, 0, OW * 3 / 4 + 64, OH / 2 + 1, 1 };
        c11->CopySubresourceRegion(stage, 0, 0, 0, 0, out11, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{}; unsigned s = 0;
        if (SUCCEEDED(c11->Map(stage, 0, D3D11_MAP_READ, 0, &m))) { const uint8_t* p = (const uint8_t*)m.pData; for (int i = 0; i < 256; ++i) s += p[i]; c11->Unmap(stage, 0); }
        return s;
    };
    auto run11 = [&]() -> double {
        const D3D11_VIEWPORT vp{ 0, 0, (FLOAT)OW, (FLOAT)OH, 0, 1 };
        c11->OMSetRenderTargets(1, &outV, nullptr); c11->RSSetViewports(1, &vp);
        c11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c11->VSSetShader(vs11, nullptr, 0); c11->PSSetShader(ps11, nullptr, 0);
        ID3D11ShaderResourceView* v[2] = { srcV, dispV }; c11->PSSetShaderResources(0, 2, v);
        c11->PSSetSamplers(0, 1, &ss); c11->PSSetConstantBuffers(0, 1, &cb11);
        c11->Begin(qDis); c11->End(qA);
        for (int i = 0; i < draws; ++i) c11->Draw(3, 0);
        c11->End(qB); c11->End(qDis);
        c11->OMSetRenderTargets(0, nullptr, nullptr);
        c11->Flush();
        UINT64 a = 0, b = 0; D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        while (c11->GetData(qDis, &dj, sizeof(dj), 0) != S_OK) Sleep(1);
        while (c11->GetData(qA, &a, sizeof(a), 0) != S_OK) Sleep(1);
        while (c11->GetData(qB, &b, sizeof(b), 0) != S_OK) Sleep(1);
        return dj.Disjoint ? -1.0 : (double)(b - a) * 1000.0 / (double)dj.Frequency / draws;
    };

    // ---- Direct3D 12 ----
    D3D12_COMMAND_QUEUE_DESC cq{}; cq.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr; d12->CreateCommandQueue(&cq, IID_PPV_ARGS(&queue));
    ID3D12CommandAllocator* alloc = nullptr; d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr; d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)); list->Close();
    ID3D12Fence* fence = nullptr; d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); UINT64 fv = 0;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ID3D12DescriptorHeap *rtvH = nullptr, *srvH = nullptr; d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvH));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 2; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    d12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvH));
    D3D12_RENDER_TARGET_VIEW_DESC rv12{}; rv12.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; rv12.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    d12->CreateRenderTargetView(out12, &rv12, rtvH->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC sv12{}; sv12.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sv12.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv12.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv12.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh = srvH->GetCPUDescriptorHandleForHeapStart();
    d12->CreateShaderResourceView(src12, &sv12, sh);
    sh.ptr += d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    sv12.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; d12->CreateShaderResourceView(disp12, &sv12, sh);

    D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 4; rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sp{}; sp.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; sp.AddressU = sp.AddressV = sp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sp.MaxLOD = D3D12_FLOAT32_MAX; sp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = rp; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &sp;
    ID3DBlob* rsb = nullptr; D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err);
    ID3D12RootSignature* root = nullptr; d12->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&root));
    const std::vector<uint8_t> dxvs = ReadFile(dir + "/bench_vs.dxil");
    auto pso = [&](const void* ps, size_t n, bool dxil = false) -> ID3D12PipelineState* {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root;
        pd.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() }; pd.PS = { ps, n };
        if (dxil && !dxvs.empty()) pd.VS = { dxvs.data(), dxvs.size() };
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = 0xFFFFFFFF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; pd.SampleDesc.Count = 1;
        ID3D12PipelineState* p = nullptr;
        const HRESULT hr = d12->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&p));
        if (FAILED(hr)) printf("  (pipeline failed 0x%08lX)\n", (unsigned long)hr);
        return p;
    };
    ID3D12PipelineState* psoFxc = pso(psb->GetBufferPointer(), psb->GetBufferSize());
    ID3D12PipelineState* pso32 = dx32.empty() ? nullptr : pso(dx32.data(), dx32.size(), true);
    ID3D12PipelineState* pso16 = dx16.empty() ? nullptr : pso(dx16.data(), dx16.size(), true);
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2;
    ID3D12QueryHeap* qheap = nullptr; d12->CreateQueryHeap(&qh, IID_PPV_ARGS(&qheap));
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rb.Width = 16; rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1;
    rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* qbuf = nullptr; d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&qbuf));
    UINT64 gpuHz = 1; queue->GetTimestampFrequency(&gpuHz);

    auto run12 = [&](ID3D12PipelineState* p) -> double {
        if (!p) return -1.0;
        c11->Flush();
        alloc->Reset(); list->Reset(alloc, p);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = out12;
        b.Transition.Subresource = 0; b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvH->GetCPUDescriptorHandleForHeapStart();
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT vp{ 0, 0, (FLOAT)OW, (FLOAT)OH, 0, 1 }; const D3D12_RECT sc{ 0, 0, (LONG)OW, (LONG)OH };
        list->RSSetViewports(1, &vp); list->RSSetScissorRects(1, &sc);
        list->SetGraphicsRootSignature(root); list->SetDescriptorHeaps(1, &srvH);
        list->SetGraphicsRoot32BitConstants(0, 4, cb, 0);
        list->SetGraphicsRootDescriptorTable(1, srvH->GetGPUDescriptorHandleForHeapStart());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int i = 0; i < draws; ++i) list->DrawInstanced(3, 1, 0, 0);
        list->EndQuery(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(qheap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qbuf, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* l[1] = { list }; queue->ExecuteCommandLists(1, l);
        queue->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 60000);
        UINT64* t = nullptr; const D3D12_RANGE rr{ 0, 16 }, none{ 0, 0 };
        double ms = -1.0;
        if (SUCCEEDED(qbuf->Map(0, &rr, (void**)&t)) && t) { ms = (double)(t[1] - t[0]) * 1000.0 / (double)gpuHz / draws; qbuf->Unmap(0, &none); }
        return ms;
    };

    // The same picture's two matching channels alone, 16 bits a pixel instead of 32:
    // do the taps cost less from a narrower texture? (Direct3D 11 only.)
    ID3D11ShaderResourceView* narrowV = nullptr;
    {
        std::vector<uint8_t> rg((size_t)SW * SH * 2);
        for (size_t i = 0; i < (size_t)SW * SH; ++i) { rg[i * 2] = pic[i * 4]; rg[i * 2 + 1] = pic[i * 4 + 1]; }
        D3D11_TEXTURE2D_DESC td{}; td.Width = SW; td.Height = SH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{ rg.data(), SW * 2, 0 }; ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(d11->CreateTexture2D(&td, &sd, &t))) { d11->CreateShaderResourceView(t, nullptr, &narrowV); t->Release(); }
    }
    if (narrowV && argc > 3)
    {
        ID3D11ShaderResourceView* wide = srcV; double bw = 1e9, bn = 1e9;
        for (int round = 0; round < 6; ++round)
        {
            srcV = wide;    double t = run11(); if (t > 0 && t < bw) bw = t;
            srcV = narrowV; t = run11();        if (t > 0 && t < bn) bn = t;
        }
        srcV = wide;
        printf("matching taps from the 32-bit picture %.3f ms, from a 16-bit two-channel copy %.3f ms (%+.1f%%)\n", bw, bn, (bn / bw - 1.0) * 100.0);
    }
    // Rounds, the builds taking turns (the GPU's clock speed and other programs
    // drift): the best of each is its time.
    const char* names[4] = { "Direct3D 11, fxc (today)", "Direct3D 12, fxc", "Direct3D 12, dxc 6.2", "Direct3D 12, dxc 6.2, 16-bit floats" };
    double best[4] = { 1e9, 1e9, 1e9, 1e9 }; unsigned sums[4] = {};
    run11(); run12(psoFxc);   // (warm up)
    for (int round = 0; round < 5; ++round)
    {
        double t;
        t = run11();          if (t > 0 && t < best[0]) best[0] = t; sums[0] = sum11();
        t = run12(psoFxc);    if (t > 0 && t < best[1]) best[1] = t; sums[1] = sum11();
        t = run12(pso32);     if (t > 0 && t < best[2]) best[2] = t; sums[2] = sum11();
        t = run12(pso16);     if (t > 0 && t < best[3]) best[3] = t; sums[3] = sum11();
    }
    printf("compose kernel, %ux%u, %d draws a round, best of 5 rounds:\n", OW, OH, draws);
    for (int i = 0; i < 4; ++i)
        if (best[i] < 1e8) printf("  %-38s %.3f ms a draw (%+.1f%% vs today)   check %u\n", names[i], best[i], (best[i] / best[0] - 1.0) * 100.0, sums[i]);
        else               printf("  %-38s not run\n", names[i]);
    return 0;
}
