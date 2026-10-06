// copybench: does a big texture copy on Direct3D 12's COPY queue run at the same
// time as shader work on the main queue -- and how fast is it there?
//
// SR Loom's Direct3D 12 presenter copies each finished 7680x2160 picture into
// the weave's own texture, in the weave's command list, ahead of the weave. The
// question is whether that copy could go to a copy queue instead, off the
// weave's path. Measured here, with a compute shader standing in for the
// conversion:
//   1. the copy alone on the main (direct) queue
//   2. the copy alone on a copy queue
//   3. the shader work alone
//   4. copy then shader work, one after the other on the main queue
//   5. copy on the copy queue while the shader work runs on the main queue
// If 5 is near the larger of 2 and 3, they do run together; near their sum,
// they do not.
//
//   copybench [width height] [rounds]
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>

static const char* kShader =
    "RWTexture2D<float4> o : register(u0);\n"
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    float a = id.x * 0.001 + id.y * 0.002;\n"
    "    for (int i = 0; i < 40; ++i) a = sin(a * 1.7 + 0.3);\n"
    "    o[id.xy] = float4(a, a, a, 1);\n"
    "}\n";

struct Q
{
    ID3D12CommandQueue* queue = nullptr; ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; HANDLE ev = nullptr; UINT64 value = 0;
    bool Make(ID3D12Device* d, D3D12_COMMAND_LIST_TYPE type)
    {
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = type;
        if (FAILED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(d->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(d->CreateCommandList(0, type, alloc, nullptr, IID_PPV_ARGS(&list)))) return false;
        if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return ev != nullptr;
    }
    void Send() { ID3D12CommandList* l[1] = { list }; queue->ExecuteCommandLists(1, l); queue->Signal(fence, ++value); }
    void Wait() { if (fence->GetCompletedValue() < value) { fence->SetEventOnCompletion(value, ev); WaitForSingleObject(ev, INFINITE); } }
};

static ID3D12Resource* Tex(ID3D12Device* d, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static double Ms(LARGE_INTEGER a, LARGE_INTEGER b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart; }

int main(int argc, char** argv)
{
    const UINT W = argc > 2 ? (UINT)atoi(argv[1]) : 7680, H = argc > 2 ? (UINT)atoi(argv[2]) : 2160;
    const int rounds = argc > 3 ? atoi(argv[3]) : 200;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) { printf("no Direct3D 12 device\n"); return 1; }
    Q direct, copy, both;
    if (!direct.Make(dev, D3D12_COMMAND_LIST_TYPE_DIRECT) || !copy.Make(dev, D3D12_COMMAND_LIST_TYPE_COPY) || !both.Make(dev, D3D12_COMMAND_LIST_TYPE_DIRECT))
    { printf("queues failed\n"); return 1; }

    ID3D12Resource* src = Tex(dev, W, H, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* dst = Tex(dev, W, H, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* out = Tex(dev, 3840, 2160, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!src || !dst || !out) { printf("textures failed\n"); return 1; }

    // The shader work: a compute pass over a 3840x2160 texture.
    ID3DBlob* cs = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) { printf("shader failed\n"); return 1; }
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp{}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp.DescriptorTable.NumDescriptorRanges = 1; rp.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters = 1; rs.pParameters = &rp;
    ID3DBlob* rsb = nullptr; D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, nullptr);
    ID3D12RootSignature* root = nullptr; dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&root));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = root; pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ID3D12PipelineState* pso = nullptr;
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) { printf("pipeline failed\n"); return 1; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr; dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    dev->CreateUnorderedAccessView(out, nullptr, nullptr, heap->GetCPUDescriptorHandleForHeapStart());

    auto work = [&](ID3D12GraphicsCommandList* l)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        l->ResourceBarrier(1, &b);
        l->SetPipelineState(pso); l->SetComputeRootSignature(root);
        l->SetDescriptorHeaps(1, &heap);
        l->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        l->Dispatch(3840 / 8, 2160 / 8, 1);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        l->ResourceBarrier(1, &b);
    };
    // (Copies: the textures in the common state -- promoted for the copy, and
    // back to common when the list has run, on either kind of queue.)
    // direct: the shader work alone. copy: the copy alone, on the copy queue.
    // both: on the main queue, the copy alone / the copy then the work.
    work(direct.list); direct.list->Close();
    copy.list->CopyResource(dst, src); copy.list->Close();

    auto time = [&](const char* name, int n, auto&& one) {
        LARGE_INTEGER a, b; double best = 1e9;
        for (int r = 0; r < 5; ++r)
        {
            QueryPerformanceCounter(&a);
            for (int i = 0; i < n; ++i) one();
            QueryPerformanceCounter(&b);
            const double ms = Ms(a, b) / n; if (ms < best) best = ms;
        }
        printf("  %-62s %.3f ms each\n", name, best);
        return best;
    };

    printf("copy %ux%u (%.0f MB), shader work 3840x2160, %d a round, best of 5 rounds:\n", W, H, W * (double)H * 4 / 1048576.0, rounds);
    both.list->CopyResource(dst, src); both.list->Close();
    const double t1 = time("1. the copy alone, on the main queue", rounds, [&] { both.Send(); both.Wait(); });
    const double t2 = time("2. the copy alone, on a copy queue", rounds, [&] { copy.Send(); copy.Wait(); });
    const double t3 = time("3. the shader work alone", rounds, [&] { direct.Send(); direct.Wait(); });
    both.alloc->Reset(); both.list->Reset(both.alloc, nullptr);
    both.list->CopyResource(dst, src);
    {
        // (The copy's textures back to common before other work in the same list.)
        D3D12_RESOURCE_BARRIER b[2]{};
        for (int k = 0; k < 2; ++k) { b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b[k].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; }
        b[0].Transition.pResource = dst; b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[1].Transition.pResource = src; b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        both.list->ResourceBarrier(2, b);
    }
    work(both.list); both.list->Close();
    const double t4 = time("4. copy then shader work, one after the other on the main queue", rounds, [&] { both.Send(); both.Wait(); });
    const double t5 = time("5. copy on the copy queue while the shader work runs on the main", rounds, [&] { copy.Send(); direct.Send(); copy.Wait(); direct.Wait(); });
    printf("together they take %.0f%% of one after the other (sum of the two alone: %.3f ms; the larger alone: %.3f ms)\n",
           100.0 * t5 / t4, t2 + t3, t2 > t3 ? t2 : t3);
    printf("the copy queue's copy is %.0f%% of the main queue's time for the same copy\n", 100.0 * t2 / t1);
    return 0;
}
