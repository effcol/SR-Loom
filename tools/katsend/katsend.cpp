// katsend: a Katanga test sender. Publishes a shared side-by-side texture the
// way a game with geo-11 (direct_mode = katanga_vr) or the original Katanga
// does, so SR Loom's receiver can be tried without a game.
//
//   katsend [seconds] [texW texH] [fillW fillH]
//
// seconds: how long to send (default 30). texW x texH: the shared texture
// (default 7680x2160: two 3840x2160 eyes). fillW x fillH: the part of it that is
// drawn, from the top-left, the rest left black (default: all of it) -- a game
// rendering below the size its texture was made at. Example, a 2560x1440 game
// in a texture made for 3840x2160:  katsend 30 7680 2160 5120 1440
//
// The picture: each eye a grid on grey with a white frame round the eye, the
// left eye's bars red, the right eye's cyan and shifted (they sit behind the
// screen), and a square that moves so a frozen picture shows.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 30;
    const int tw = argc > 3 ? atoi(argv[2]) : 7680, th = argc > 3 ? atoi(argv[3]) : 2160;
    const int fw = argc > 5 ? atoi(argv[4]) : tw, fh = argc > 5 ? atoi(argv[5]) : th;
    if (tw < 16 || th < 16 || fw < 16 || fh < 16 || fw > tw || fh > th) { printf("bad sizes\n"); return 1; }

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { printf("no D3D11 device\n"); return 1; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)tw; td.Height = (UINT)th; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // (sRGB-typed: left eye first, as the 3D Slicer bridge sends)
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { printf("no texture\n"); return 1; }
    IDXGIResource* res = nullptr; HANDLE shared = nullptr;
    if (FAILED(tex->QueryInterface(&res)) || FAILED(res->GetSharedHandle(&shared)) || !shared) { printf("no shared handle\n"); return 1; }

    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(uintptr_t), "Local\\KatangaMappedFile");
    void* view = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(uintptr_t)) : nullptr;
    if (!view) { printf("no mapping (%lu)\n", GetLastError()); return 1; }
    *reinterpret_cast<volatile uintptr_t*>(view) = reinterpret_cast<uintptr_t>(shared);
    printf("sending: texture %dx%d, drawn %dx%d from the top-left, for %d s\n", tw, th, fw, fh, seconds);

    // The drawn part, built once on the CPU: both eyes, each half of it.
    const int ew = fw / 2;
    std::vector<uint32_t> px((size_t)fw * fh);
    auto build = [&](int t)
    {
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x)
            {
                const int eye = x >= ew ? 1 : 0, ex = x - eye * ew;
                const int sx = ex + (eye ? ew / 100 : 0);   // (the right eye's picture a hundredth of its width to the side)
                uint32_t c = 0xFF606060;
                if ((sx / (ew / 16 + 1) + y / (fh / 9 + 1)) & 1) c = 0xFF808080;
                if (sx % (ew / 8 + 1) < 6) c = eye ? 0xFFFFFF00 : 0xFF0000FF;   // (ABGR: cyan / red)
                const int qx = (t * 8) % (ew > 200 ? ew - 200 : 1), qy = fh / 2 - 100;
                if (ex >= qx && ex < qx + 200 && y >= qy && y < qy + 200) c = 0xFFFFFFFF;
                if (ex < 8 || ex >= ew - 8 || y < 8 || y >= fh - 8) c = 0xFFFFFFFF;   // (the frame round each eye)
                px[(size_t)y * fw + x] = c;
            }
    };
    // (The whole texture black first: what lies outside the drawn part.)
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) { const float black[4] = { 0, 0, 0, 1 }; ctx->ClearRenderTargetView(rtv, black); rtv->Release(); }

    const ULONGLONG end = GetTickCount64() + (ULONGLONG)seconds * 1000;
    for (int t = 0; GetTickCount64() < end; ++t)
    {
        build(t);
        const D3D11_BOX box{ 0, 0, 0, (UINT)fw, (UINT)fh, 1 };
        ctx->UpdateSubresource(tex, 0, &box, px.data(), (UINT)fw * 4, 0);
        ctx->Flush();
        Sleep(33);
    }
    *reinterpret_cast<volatile uintptr_t*>(view) = 0;
    UnmapViewOfFile(view); CloseHandle(map);
    res->Release(); tex->Release(); ctx->Release(); dev->Release();
    printf("done\n");
    return 0;
}
