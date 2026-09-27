// anatest: offline check of the anaglyph "Recovered Colour" conversion.
// Loads an anaglyph image, runs the real Converter (the same shaders as
// SR Loom) and saves the result, so quality changes can be compared on
// the same pictures without the SR display.
//
//   anatest <in.jpg|png> <out.png> [mode=4] [combo=0] [crop x y w h] [frames=1]
//
// out.png is the left eye (x < 0: the right eye); with a crop, that part of
// it, enlarged 2x (nearest) so the pixels can be inspected.
#include "Converter.h"
#include <wincodec.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <cstring>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ole32.lib")

using namespace srw;

static bool LoadImageRGBA(const wchar_t* path, std::vector<uint8_t>& px, UINT& w, UINT& h)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* c = nullptr;
    bool ok = SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d)) &&
              SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&c)) &&
              SUCCEEDED(c->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(c->GetSize(&w, &h));
    if (ok) { px.resize((size_t)w * h * 4); ok = SUCCEEDED(c->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data())); }
    if (c) c->Release(); if (fr) fr->Release(); if (d) d->Release(); f->Release();
    return ok;
}

// A half float to float (the disparity maps are RGBA16F).
static float HalfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0)       v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else              v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// A recovery stage's disparity map, read back: 4 floats per texel.
struct StageMap { UINT w = 0, h = 0; std::vector<float> v; };
static bool ReadStage(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, StageMap& out)
{
    if (!srv) return false;
    ID3D11Resource* r = nullptr; srv->GetResource(&r);
    ID3D11Texture2D* t = nullptr; r->QueryInterface(&t); r->Release();
    D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) { t->Release(); return false; }
    ctx->CopyResource(st, t); t->Release();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
    out.w = d.Width; out.h = d.Height; out.v.resize((size_t)d.Width * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y)
    {
        const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch);
        for (UINT x = 0; x < d.Width * 4; ++x) out.v[(size_t)y * d.Width * 4 + x] = HalfToFloat(row[x]);
    }
    ctx->Unmap(st, 0); st->Release();
    return true;
}

static bool SavePNG(const wchar_t* path, const uint8_t* px, UINT w, UINT h, UINT stride)
{
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    IWICStream* s = nullptr; IWICBitmapEncoder* e = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    // (The PNG encoder takes BGRA: RGBA given to it came out with red and blue
    // swapped. Swap to BGRA here.)
    std::vector<uint8_t> bgra(px, px + (size_t)stride * h);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path, GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(fr->WritePixels(h, stride, stride * h, bgra.data())) &&
              SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
    if (fr) fr->Release(); if (e) e->Release(); if (s) s->Release(); f->Release();
    return ok;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { fwprintf(stderr, L"usage: anatest in out.png [mode] [combo] [crop x y w h] [frames]\n"); return 1; }
    const int mode  = argc > 3 ? _wtoi(argv[3]) : 4;
    const int combo = argc > 4 ? _wtoi(argv[4]) : 0;
    const bool crop = argc > 8;
    int cx = crop ? _wtoi(argv[5]) : 0, cy = crop ? _wtoi(argv[6]) : 0;
    const int cw = crop ? _wtoi(argv[7]) : 0, ch = crop ? _wtoi(argv[8]) : 0;
    const int frames = argc > 9 ? _wtoi(argv[9]) : 1;
    const bool rightEye = crop && cx < 0;
    if (rightEye) cx = -cx;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<uint8_t> px; UINT w = 0, h = 0;
    if (!LoadImageRGBA(argv[1], px, w, h)) { fwprintf(stderr, L"can't load %s\n", argv[1]); return 1; }

    // ANATEST_QUILT=cols,rows,iL,iR: the input is a Looking Glass quilt; views
    // iL (left camera) and iR make a TRUE stereo pair. A red/cyan anaglyph is
    // made from them (red = left, green + blue = right) and recovered; the
    // result is scored against the real views.
    std::vector<uint8_t> truthL, truthR;
    UINT tw = 0, th = 0;
    if (const char* q = getenv("ANATEST_QUILT"))
    {
        int qc = 0, qr = 0, iL = 0, iR = 0;
        if (sscanf_s(q, "%d,%d,%d,%d", &qc, &qr, &iL, &iR) == 4 && qc > 0 && qr > 0)
        {
            tw = w / qc; th = h / qr;
            auto view = [&](int idx, std::vector<uint8_t>& out) {
                // (Quilt views run left to right, bottom row first.)
                const UINT vx = (UINT)(idx % qc) * tw, vy = (UINT)(qr - 1 - idx / qc) * th;
                out.resize((size_t)tw * th * 4);
                for (UINT y = 0; y < th; ++y) memcpy(&out[(size_t)y * tw * 4], &px[((size_t)(vy + y) * w + vx) * 4], (size_t)tw * 4);
            };
            view(iL, truthL); view(iR, truthR);
            px.assign((size_t)tw * th * 4, 255);
            for (size_t i = 0; i < px.size(); i += 4) { px[i] = truthL[i]; px[i + 1] = truthR[i + 1]; px[i + 2] = truthR[i + 2]; }
            w = tw; h = th;
        }
    }
    // ANATEST_RIGHT=right.png: the input is a stereo pair's LEFT photo and
    // this its right one (e.g. Middlebury); scored like a quilt pair.
    // ANATEST_HALF=1 halves both first (box 2x2) -- full-size Middlebury
    // disparities are far beyond an anaglyph's.
    if (const wchar_t* rp = _wgetenv(L"ANATEST_RIGHT"))
    {
        std::vector<uint8_t> r2; UINT rw = 0, rh = 0;
        if (LoadImageRGBA(rp, r2, rw, rh) && rw == w && rh == h)
        {
            truthL = px; truthR = r2; tw = w; th = h;
            // ANATEST_SHIFT=px: centre Middlebury's all-positive disparities on
            // the screen plane, as an anaglyph maker would -- by cropping: the
            // left photo loses its first px columns, the right its last px.
            // (Every pixel is real, so the frame edges are true edges.)
            if (const char* sh = getenv("ANATEST_SHIFT"))
            {
                const UINT s = (UINT)atoi(sh), nw = tw - s;
                std::vector<uint8_t> l2((size_t)nw * th * 4), rr((size_t)nw * th * 4);
                for (UINT y = 0; y < th; ++y)
                {
                    memcpy(&l2[(size_t)y * nw * 4], &truthL[((size_t)y * tw + s) * 4], (size_t)nw * 4);
                    memcpy(&rr[(size_t)y * nw * 4], &r2[(size_t)y * tw * 4], (size_t)nw * 4);
                }
                truthL.swap(l2); truthR.swap(rr); tw = nw;
            }
            for (int halves = getenv("ANATEST_HALF") ? atoi(getenv("ANATEST_HALF")) : 0; halves > 0; --halves)
            {
                auto half = [&](std::vector<uint8_t>& v) {
                    std::vector<uint8_t> o((size_t)(tw / 2) * (th / 2) * 4);
                    for (UINT y = 0; y < th / 2; ++y)
                        for (UINT x = 0; x < tw / 2; ++x)
                            for (int c = 0; c < 4; ++c)
                                o[((size_t)y * (tw / 2) + x) * 4 + c] = (uint8_t)((v[((size_t)(2 * y) * tw + 2 * x) * 4 + c] + v[((size_t)(2 * y) * tw + 2 * x + 1) * 4 + c] +
                                                                              v[((size_t)(2 * y + 1) * tw + 2 * x) * 4 + c] + v[((size_t)(2 * y + 1) * tw + 2 * x + 1) * 4 + c] + 2) / 4);
                    v.swap(o);
                };
                half(truthL); half(truthR); tw /= 2; th /= 2;
            }
            px.assign((size_t)tw * th * 4, 255);
            for (size_t i = 0; i < px.size(); i += 4) { px[i] = truthL[i]; px[i + 1] = truthR[i + 1]; px[i + 2] = truthR[i + 2]; }
            if (getenv("ANATEST_GREYANA"))   // (a grey anaglyph: red = the left view's luminance, green + blue = the right's)
                for (size_t i = 0; i < px.size(); i += 4)
                {
                    const uint8_t yl = (uint8_t)std::lround(0.299 * truthL[i] + 0.587 * truthL[i + 1] + 0.114 * truthL[i + 2]);
                    const uint8_t yr = (uint8_t)std::lround(0.299 * truthR[i] + 0.587 * truthR[i + 1] + 0.114 * truthR[i + 2]);
                    px[i] = yl; px[i + 1] = yr; px[i + 2] = yr;
                }
            w = tw; h = th;
        }
    }
    // ANATEST_CANVAS=W,H,x,y,grey: put the picture on a W x H "screen" of that
    // grey at (x,y), as a page / fullscreen view would -- to test how the
    // result depends on where the picture sits and how big the area is.
    // Outputs, crops and scores are then of the picture's area only.
    UINT picX = 0, picY = 0, picW = w, picH = h;
    std::vector<uint8_t> picPx = px; int canvasGrey = 0;   // (the picture alone, for ANATEST_SCROLL)
    if (const char* c = getenv("ANATEST_CANVAS"))
    {
        int CW = 0, CH = 0, ox = 0, oy = 0, grey = 0;
        if (sscanf_s(c, "%d,%d,%d,%d,%d", &CW, &CH, &ox, &oy, &grey) == 5 && CW >= (int)w + ox && CH >= (int)h + oy)
        {
            std::vector<uint8_t> canvas((size_t)CW * CH * 4, (uint8_t)grey);
            for (size_t i = 3; i < canvas.size(); i += 4) canvas[i] = 255;
            for (UINT y = 0; y < h; ++y) memcpy(&canvas[((size_t)(oy + y) * CW + ox) * 4], &px[(size_t)y * w * 4], (size_t)w * 4);
            px.swap(canvas); canvasGrey = grey;
            picX = (UINT)ox; picY = (UINT)oy;
            w = (UINT)CW; h = (UINT)CH;
        }
    }

    if (getenv("ANATEST_STATS"))
    {
        double s[3] = {};
        for (size_t i = 0; i < px.size(); i += 4) for (int c = 0; c < 3; ++c) s[c] += px[i + c];
        const double n = px.size() / 4.0;
        wprintf(L"input mean r %.1f g %.1f b %.1f\n", s[0] / n, s[1] / n, s[2] / n);
    }
    if (const wchar_t* si = _wgetenv(L"ANATEST_SAVEIN")) SavePNG(si, px.data(), w, h, w * 4);

    // ANATEST_CURVE=x,y[,want] (source px; want = the true dLR in px if known):
    // the 1/4 level's match score (left view here vs right view there) for
    // every candidate, computed on the CPU as the shaders do -- tent-downsampled
    // linear colour, 4-angle gradient descriptors -- with both the current
    // (sum of differences) score and a normalised-correlation one.
    if (const char* cv = getenv("ANATEST_CURVE"))
    {
        int qx = 0, qy = 0; float want = 1e9f;
        sscanf_s(cv, "%d,%d,%f", &qx, &qy, &want);
        if (const wchar_t* gp = _wgetenv(L"ANATEST_GTDISP"))   // (or the true value from the ground truth)
        {
            FILE* f = nullptr; _wfopen_s(&f, gp, L"rb"); char hd[3] = {}; int gw = 0, gh = 0; float sc = 0;
            if (f && fscanf_s(f, "%2s %d %d %f", hd, 3, &gw, &gh, &sc) == 4)
            {
                fgetc(f); std::vector<float> gd((size_t)gw * gh); fread(gd.data(), 4, gd.size(), f);
                const int sh = getenv("ANATEST_SHIFT") ? atoi(getenv("ANATEST_SHIFT")) : 0, kk = 1 << (getenv("ANATEST_HALF") ? atoi(getenv("ANATEST_HALF")) : 0);
                const float g = gd[(size_t)(gh - 1 - qy * kk) * gw + qx * kk + sh];
                if (g < 1e9f) want = (sh - g) / kk;
            }
            if (f) fclose(f);
        }
        auto lin = [](uint8_t v) { const float c = v / 255.0f; return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
        const int W4 = (int)((w + 15) / 16) * 4, H4 = (int)((h + 15) / 16) * 4;
        std::vector<float> q4((size_t)W4 * H4 * 2);   // chanL (red), chanR (green)
        // (ANATEST_CURVE_TRUTH: match the TRUE views' luminance instead -- the best a
        // match on recovered colour could do.)
        const bool useTruth = getenv("ANATEST_CURVE_TRUTH") && !truthL.empty();
        auto pxAt = [&](int x, int y, int c) {
            x = std::clamp(x, 0, (int)w - 1); y = std::clamp(y, 0, (int)h - 1);
            if (useTruth) { const uint8_t* t = &(c == 0 ? truthL : truthR)[((size_t)y * tw + x) * 4]; return 0.299f * lin(t[0]) + 0.587f * lin(t[1]) + 0.114f * lin(t[2]); }
            return lin(px[((size_t)y * w + x) * 4 + c]); };
        const int o[4] = { -3, -1, 1, 3 }; const float wt[4] = { 1, 3, 3, 1 };
        for (int j = 0; j < H4; ++j)
            for (int i = 0; i < W4; ++i)
                for (int ch = 0; ch < 2; ++ch)
                {
                    float acc = 0;
                    for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
                    {
                        const int sx = 4 * i + 2 + o[b], sy = 4 * j + 2 + o[a];   // sample point between pixels sx-1 and sx (and rows)
                        const int c = ch == 0 ? 0 : 1;
                        const float v = 0.25f * (pxAt(sx - 1, sy - 1, c) + pxAt(sx, sy - 1, c) + pxAt(sx - 1, sy, c) + pxAt(sx, sy, c));
                        acc += v * wt[a] * wt[b];
                    }
                    q4[((size_t)j * W4 + i) * 2 + ch] = acc / 64.0f;
                }
        auto desc = [&](int px4, int py4, int ch, float d[16]) {
            const int dirs[4][2] = { {1,0},{0,1},{1,1},{-1,1} };
            for (int a = 0; a < 4; ++a)
            {
                float v[5];
                for (int t = 0; t < 5; ++t)
                {
                    const int xx = std::clamp(px4 + dirs[a][0] * (t - 2), 0, W4 - 1), yy = std::clamp(py4 + dirs[a][1] * (t - 2), 0, H4 - 1);
                    v[t] = q4[((size_t)yy * W4 + xx) * 2 + ch];
                }
                for (int t = 0; t < 4; ++t) d[a * 4 + t] = v[t + 1] - v[t];
            }
        };
        const int p4x = qx / 4, p4y = qy / 4;
        float ref[16]; desc(p4x, p4y, 0, ref);
        float ea = 0; for (float v : ref) ea += std::abs(v);
        wprintf(L"curve at (%d,%d), level px (%d,%d); ref energy %.3f; true dLR %.1f px (level %.1f)\n", qx, qy, p4x, p4y, ea, want, want / 4);
        for (int k = -24; k <= 24; ++k)
        {
            float cand[16]; desc(p4x + k, p4y, 1, cand);
            float sad = 0, en = 0.15f, ab = 0, aa = 0, bb = 0, eb = 0;
            for (int j = 0; j < 16; ++j) { sad += std::abs(ref[j] - cand[j]); en += std::abs(ref[j]) + std::abs(cand[j]); ab += ref[j] * cand[j]; aa += ref[j] * ref[j]; bb += cand[j] * cand[j]; eb += std::abs(cand[j]); }
            const float ncc = ab / std::sqrt(aa * bb + 1e-12f);
            wprintf(L"  k %+3d (%+4d px)%s  sad-cost %.3f  ncc-cost %.3f  cand energy %.3f\n", k, k * 4, (std::abs(k * 4 - want) <= 2.0f) ? L" <TRUE" : L"      ", sad / en, 0.5f * (1 - ncc), eb);
        }
        // The same at the 1/16 level (the coarse search): q4 tent-downsampled again.
        {
            const int W16 = W4 / 4, H16 = H4 / 4;
            std::vector<float> q16((size_t)W16 * H16 * 2);
            auto q4At = [&](int x, int y, int c) { x = std::clamp(x, 0, W4 - 1); y = std::clamp(y, 0, H4 - 1); return q4[((size_t)y * W4 + x) * 2 + c]; };
            for (int j = 0; j < H16; ++j) for (int i = 0; i < W16; ++i) for (int ch = 0; ch < 2; ++ch)
            {
                float acc = 0;
                for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b)
                {
                    const int sx = 4 * i + 2 + o[b], sy = 4 * j + 2 + o[a];
                    acc += 0.25f * (q4At(sx - 1, sy - 1, ch) + q4At(sx, sy - 1, ch) + q4At(sx - 1, sy, ch) + q4At(sx, sy, ch)) * wt[a] * wt[b];
                }
                q16[((size_t)j * W16 + i) * 2 + ch] = acc / 64.0f;
            }
            auto desc16 = [&](int x0, int y0, int ch, float d[16]) {
                const int dirs[4][2] = { {1,0},{0,1},{1,1},{-1,1} };
                for (int a = 0; a < 4; ++a)
                {
                    float v[5];
                    for (int t = 0; t < 5; ++t)
                    {
                        const int xx = std::clamp(x0 + dirs[a][0] * (t - 2), 0, W16 - 1), yy = std::clamp(y0 + dirs[a][1] * (t - 2), 0, H16 - 1);
                        v[t] = q16[((size_t)yy * W16 + xx) * 2 + ch];
                    }
                    for (int t = 0; t < 4; ++t) d[a * 4 + t] = v[t + 1] - v[t];
                }
            };
            const int c16x = qx / 16, c16y = qy / 16;
            float r16[16]; desc16(c16x, c16y, 0, r16);
            wprintf(L"coarse curve, level px (%d,%d), true %.1f coarse px:\n", c16x, c16y, want / 16);
            for (int kk = -7; kk <= 7; ++kk)
            {
                float cand[16]; desc16(c16x + kk, c16y, 1, cand);
                float ab = 0, aa = 0, bb = 0; for (int j = 0; j < 16; ++j) { ab += r16[j] * cand[j]; aa += r16[j] * r16[j]; bb += cand[j] * cand[j]; }
                const float ncc = ab / std::sqrt(aa * bb + 1e-12f);
                wprintf(L"  k %+d (%+4d px)%s  signed-ncc %.3f  |ncc| cost %.3f  energies %.3f %.3f\n", kk, kk * 16, (std::abs(kk * 16 - want) <= 8.0f) ? L" <TRUE" : L"      ", 0.5f * (1 - ncc), 1 - std::abs(ncc), std::sqrt(aa), std::sqrt(bb));
            }
        }
    }
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { fwprintf(stderr, L"no D3D11 device\n"); return 1; }

    // The source as SR Loom sees a capture: RGBA8, read through an sRGB view.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);

    Converter conv;
    if (!conv.Initialize(dev, ctx)) { fwprintf(stderr, L"converter init failed\n"); return 1; }
    conv.SetFormat(StereoFormat::Anaglyph, false, combo, mode);
    bool resized = false;
    // ANATEST_SCROLL=dx,dy,n (with a canvas): n frames, the picture moving
    // (dx,dy) px each -- a scroll. Reports the flicker: between each frame
    // and the one before, lined up on the picture, how many of the picture's
    // pixels changed by more than 20 (left eye). The last frame is the output.
    if (const char* sc = getenv("ANATEST_SCROLL"))
    {
        int sdx = 0, sdy = 0, sn = 0;
        sscanf_s(sc, "%d,%d,%d", &sdx, &sdy, &sn);
        std::vector<uint8_t> prevOut, curOut; double flick = 0; int flickN = 0;
        const UINT x0 = picX, y0 = picY;
        for (int f = 0; f < sn; ++f)
        {
            const int ox = (int)x0 + f * sdx, oy = (int)y0 + f * sdy;
            if (ox < 0 || oy < 0 || ox + (int)picW > (int)w || oy + (int)picH > (int)h) break;
            std::vector<uint8_t> canvas((size_t)w * h * 4, (uint8_t)canvasGrey);
            for (size_t i = 3; i < canvas.size(); i += 4) canvas[i] = 255;
            for (UINT y = 0; y < picH; ++y) memcpy(&canvas[((size_t)(oy + y) * w + ox) * 4], &picPx[(size_t)y * picW * 4], (size_t)picW * 4);
            ctx->UpdateSubresource(src, 0, nullptr, canvas.data(), w * 4, 0);
            conv.Convert(srv, (int)w, (int)h, resized);
            // Read the left eye's picture area back.
            ID3D11Resource* r = nullptr; conv.OutputSRV()->GetResource(&r);
            ID3D11Texture2D* t = nullptr; r->QueryInterface(&t); r->Release();
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&d, nullptr, &st);
            ctx->CopyResource(st, t); t->Release();
            D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
            curOut.resize((size_t)picW * picH * 4);
            for (UINT y = 0; y < picH; ++y) memcpy(&curOut[(size_t)y * picW * 4], static_cast<const uint8_t*>(mm.pData) + (size_t)(oy + y) * mm.RowPitch + (size_t)ox * 4, (size_t)picW * 4);
            ctx->Unmap(st, 0); st->Release();
            if (!prevOut.empty())
            {
                size_t off = 0;
                for (size_t i = 0; i < curOut.size(); i += 4)
                {
                    int worst = 0; for (int c = 0; c < 3; ++c) worst = (std::max)(worst, std::abs((int)curOut[i + c] - (int)prevOut[i + c]));
                    if (worst > 20) ++off;
                }
                flick += 100.0 * off / (picW * picH); ++flickN;
            }
            prevOut.swap(curOut);
            picX = (UINT)ox; picY = (UINT)oy;
        }
        wprintf(L"scroll (%d,%d) x%d: flicker %.3f%% of the picture's px change >20 per frame\n", sdx, sdy, flickN + 1, flickN ? flick / flickN : 0.0);
    }
    else
    for (int i = 0; i < frames; ++i) conv.Convert(srv, (int)w, (int)h, resized);
#ifndef ANATEST_HEAD   // (the old converter has no timing read-back)
    // ANATEST_TIME=n: n more frames, then the recovery's GPU time per stage.
    if (const char* tn = getenv("ANATEST_TIME"))
    {
        const int n = atoi(tn);
        double ms[8] = {}; int cnt = 0;
        conv.TakeRecoveryTimes(ms, cnt);
        for (int i = 0; i < n; ++i) { conv.Convert(srv, (int)w, (int)h, resized); ctx->Flush(); if (i % 8 == 7) Sleep(20); }
        Sleep(100);
        conv.TakeRecoveryTimes(ms, cnt);
        double tot = 0; for (double v : ms) tot += v;
        wprintf(L"time (%d frames): down+coarse %.2f, desc %.2f, refine %.2f, fill %.2f, smooth %.2f, compose %.2f, -, - | total %.2f ms\n",
                cnt, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], tot);
    }
#endif

#ifndef ANATEST_HEAD
    // Recovery stage diagnostics. Disparities are printed in source pixels
    // (dLR: where the left view's pixel is in the right view; dRL back).
    //  ANATEST_PROBE=x,y[;x,y..]: each stage's value at those source pixels.
    //  ANATEST_STAGESAVE=file: save every stage's map (for STAGEDIFF).
    //  ANATEST_STAGEDIFF=file,dx,dy: this run's picture sits (dx,dy) source px
    //  from the saved run's -- per stage, how many of the picture's pixels
    //  now get a disparity more than 2 px different.
    {
        StageMap sm[4]; bool have = true;
        for (int s = 0; s < 4; ++s) have = ReadStage(dev, ctx, conv.DebugStageSRV(s), sm[s]) && have;
        const char* names[4] = { "coarse", "refine", "fill", "smooth" };
        // A map's value at a source position (nearest texel), disparities
        // (c 0/1) in source px. Maps cover whole 4/16-px blocks rounded up
        // past the picture (or, in older builds, are stretched over it).
        auto mapAt = [](const StageMap& s, UINT srcW, UINT srcH, float sx, float sy, int c) {
            const float ppt = (s.w * 8 > srcW) ? 4.0f : 16.0f;
            const bool exact = s.w * ppt >= srcW;
            const float fx = exact ? sx / ppt : sx * s.w / srcW, fy = exact ? sy / ppt : sy * s.h / srcH;
            const UINT tx = (UINT)std::clamp((int)fx, 0, (int)s.w - 1), ty = (UINT)std::clamp((int)fy, 0, (int)s.h - 1);
            const float v = s.v[((size_t)ty * s.w + tx) * 4 + c];
            return c < 2 ? v * (exact ? s.w * ppt : (float)srcW) : v;
        };
        auto at = [&](const StageMap& s, float sx, float sy, int c) { return mapAt(s, w, h, sx, sy, c); };
        if (const char* pr = have ? getenv("ANATEST_PROBE") : nullptr)
        {
            std::string spec(pr); size_t pos = 0;
            while (pos < spec.size())
            {
                size_t e = spec.find(';', pos); if (e == std::string::npos) e = spec.size();
                int x = 0, y = 0;
                if (sscanf_s(spec.substr(pos, e - pos).c_str(), "%d,%d", &x, &y) == 2)
                {
                    const uint8_t* i = &px[((size_t)y * w + x) * 4];
                    wprintf(L"probe (%d,%d) in %u %u %u:", x, y, i[0], i[1], i[2]);
                    for (int s = 0; s < 4; ++s)
                        wprintf(L"  %S dLR %+.1f dRL %+.1f (%.2f %.2f)", names[s], at(sm[s], x + 0.5f, y + 0.5f, 0), at(sm[s], x + 0.5f, y + 0.5f, 1),
                                at(sm[s], x + 0.5f, y + 0.5f, 2), at(sm[s], x + 0.5f, y + 0.5f, 3));
                    wprintf(L"\n");
                }
                pos = e + 1;
            }
        }
        if (const char* sv = have ? getenv("ANATEST_STAGESAVE") : nullptr)
        {
            FILE* f = nullptr; fopen_s(&f, sv, "wb");
            if (f)
            {
                const UINT hdr[4] = { w, h, picX, picY }; fwrite(hdr, 4, 4, f);
                for (int s = 0; s < 4; ++s) { fwrite(&sm[s].w, 4, 1, f); fwrite(&sm[s].h, 4, 1, f); fwrite(sm[s].v.data(), 4, sm[s].v.size(), f); }
                fclose(f);
            }
        }
        if (const char* sd = have ? getenv("ANATEST_STAGEDIFF") : nullptr)
        {
            char file[512] = {}; int dx = 0, dy = 0;
            const char* c1 = strchr(sd, ',');
            if (c1) { memcpy(file, sd, (std::min)((size_t)(c1 - sd), sizeof(file) - 1)); sscanf_s(c1 + 1, "%d,%d", &dx, &dy); }
            FILE* f = nullptr; fopen_s(&f, file, "rb");
            if (f)
            {
                UINT hdr[4] = {}; fread(hdr, 4, 4, f);
                StageMap om[4];
                for (int s = 0; s < 4; ++s) { fread(&om[s].w, 4, 1, f); fread(&om[s].h, 4, 1, f); om[s].v.resize((size_t)om[s].w * om[s].h * 4); fread(om[s].v.data(), 4, om[s].v.size(), f); }
                fclose(f);
                const UINT ow = hdr[0], oh = hdr[1];
                wprintf(L"stage diff (picture moved %d,%d):", dx, dy);
                for (int s = 0; s < 4; ++s)
                {
                    size_t n = 0, off = 0;
                    for (UINT y = 0; y < picH; y += 2)
                        for (UINT x = 0; x < picW; x += 2)
                        {
                            const float nx = picX + x + 0.5f, ny = picY + y + 0.5f;   // this run
                            const float ox = nx - dx, oy = ny - dy;                   // the saved run, same picture pixel
                            const float a = at(sm[s], nx, ny, 0), b = mapAt(om[s], ow, oh, ox, oy, 0);
                            ++n; if (std::abs(a - b) > 2.0f) ++off;
                        }
                    wprintf(L"  %S %.1f%%", names[s], 100.0 * off / n);
                }
                wprintf(L"\n");
            }
        }
    }
#endif
    // ANATEST_FULLOUT: output (and score) the whole screen, not just the picture.
    if (getenv("ANATEST_FULLOUT")) { picX = picY = 0; picW = w; picH = h; }

    // Read the SBS output back.
    ID3D11Resource* outRes = nullptr; conv.OutputSRV()->GetResource(&outRes);
    ID3D11Texture2D* outTex = nullptr; outRes->QueryInterface(&outTex);
    D3D11_TEXTURE2D_DESC od{}; outTex->GetDesc(&od);
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ID3D11Texture2D* st = nullptr;
    D3D11_TEXTURE2D_DESC sd2{}; outTex->GetDesc(&sd2);
    sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd2.MiscFlags = 0; sd2.MipLevels = 1;
    if (FAILED(dev->CreateTexture2D(&sd2, nullptr, &st))) { fwprintf(stderr, L"staging failed\n"); return 1; }
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, outTex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    const UINT eyeW = sd2.Width / 2, eyeH = sd2.Height;
    const uint8_t* base = static_cast<const uint8_t*>(m.pData);
    std::vector<uint8_t> out;
    UINT ow, oh;
    if (!crop)
    {
        // The picture's area (the whole eye without a canvas); ANATEST_BOTH
        // puts the right eye beside the left.
        const bool both = getenv("ANATEST_BOTH") != nullptr;
        ow = picW * (both ? 2 : 1); oh = picH;
        out.resize((size_t)ow * oh * 4);
        for (UINT y = 0; y < oh; ++y)
        {
            memcpy(&out[(size_t)y * ow * 4], base + (size_t)(picY + y) * m.RowPitch + (size_t)picX * 4, (size_t)picW * 4);
            if (both) memcpy(&out[((size_t)y * ow + picW) * 4], base + (size_t)(picY + y) * m.RowPitch + (size_t)(eyeW + picX) * 4, (size_t)picW * 4);
        }
    }
    else
    {
        const UINT x0 = (rightEye ? eyeW : 0) + picX + (UINT)cx;
        cy += (int)picY;
        ow = (UINT)cw * 2; oh = (UINT)ch * 2;
        out.resize((size_t)ow * oh * 4);
        for (UINT y = 0; y < oh; ++y)
            for (UINT x = 0; x < ow; ++x)
                memcpy(&out[((size_t)y * ow + x) * 4], base + (size_t)(cy + y / 2) * m.RowPitch + (size_t)(x0 + x / 2) * 4, 4);
    }
    // Blob score (left eye): on a near-greyscale anaglyph (the infrared test
    // photo) any colour in the output is an error. Mean chroma, how many
    // pixels are strongly coloured, and mean brightness (to catch darkening).
    {
        double chroma = 0, luma = 0; size_t blobs = 0, n = 0;
        for (UINT y = picY; y < picY + picH; ++y)
            for (UINT x = picX; x < picX + picW; ++x)
            {
                const uint8_t* o = base + (size_t)y * m.RowPitch + (size_t)x * 4;
                const int mx = (std::max)({ o[0], o[1], o[2] }), mn = (std::min)({ o[0], o[1], o[2] });
                chroma += mx - mn; luma += (o[0] + o[1] + o[2]) / 3.0;
                if (mx - mn > 40) ++blobs;
                ++n;
            }
        wprintf(L"score: mean chroma %.2f, strongly coloured px %.3f%%, mean brightness %.1f\n",
                chroma / n, 100.0 * blobs / n, luma / n);
    }
    // Against the true views (quilt mode): mean abs error, and how many pixels
    // are off by more than 40 in any channel (visible blobs).
    if (!truthL.empty())
    {
        auto scoreEye = [&](const std::vector<uint8_t>& t, UINT eyeX0, const wchar_t* name) {
            double sum = 0; size_t bad = 0, n = 0;
            for (UINT y = 0; y < picH; ++y)
                for (UINT x = 0; x < picW; ++x)
                {
                    const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(eyeX0 + picX + x) * 4;
                    const uint8_t* g = &t[((size_t)y * tw + x) * 4];
                    int worst = 0;
                    for (int c = 0; c < 3; ++c) { const int d = std::abs((int)o[c] - (int)g[c]); sum += d; worst = (std::max)(worst, d); }
                    if (worst > 40) ++bad;
                    ++n;
                }
            wprintf(L"truth %s: mean abs err %.2f, px off >40: %.3f%%\n", name, sum / (n * 3.0), 100.0 * bad / n);
            // The same in the outer 60 px of each side (frame-edge streaks).
            {
                const UINT B = (std::min)(60u, picW / 4);
                double es[2] = {}; size_t bs[2] = {}, ns[2] = {};
                for (UINT y = 0; y < picH; ++y)
                    for (UINT x = 0; x < picW; ++x)
                    {
                        const int side = (x < B) ? 0 : (x >= picW - B) ? 1 : -1;
                        if (side < 0) continue;
                        const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(eyeX0 + picX + x) * 4;
                        const uint8_t* g = &t[((size_t)y * tw + x) * 4];
                        int worst = 0;
                        for (int c = 0; c < 3; ++c) { const int d = std::abs((int)o[c] - (int)g[c]); es[side] += d; worst = (std::max)(worst, d); }
                        if (worst > 40) ++bs[side];
                        ++ns[side];
                    }
                wprintf(L"edges %s: left band err %.2f, >40 %.2f%%; right band err %.2f, >40 %.2f%%\n", name,
                        es[0] / (ns[0] * 3.0), 100.0 * bs[0] / ns[0], es[1] / (ns[1] * 3.0), 100.0 * bs[1] / ns[1]);
            }
            // ANATEST_CONF: split by the shader's alpha (a build whose PSMain
            // passes the recovery's confidence through).
            if (getenv("ANATEST_CONF"))
            {
                double es[5] = {}; size_t ns[5] = {}, bs[5] = {};
                for (UINT y = 0; y < picH; ++y)
                    for (UINT x = 0; x < picW; ++x)
                    {
                        const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(eyeX0 + picX + x) * 4;
                        const uint8_t* g = &t[((size_t)y * tw + x) * 4];
                        const int bin = (std::min)(4, o[3] * 5 / 256);
                        int worst = 0;
                        for (int c = 0; c < 3; ++c) { const int d = std::abs((int)o[c] - (int)g[c]); es[bin] += d; worst = (std::max)(worst, d); }
                        if (worst > 40) ++bs[bin];
                        ++ns[bin];
                    }
                for (int b = 0; b < 5; ++b)
                    wprintf(L"   conf %.1f-%.1f: %5.1f%% of px, err %5.2f, >40 %.2f%% (of all px)\n", b * 0.2, b * 0.2 + 0.2,
                            100.0 * ns[b] / n, ns[b] ? es[b] / (ns[b] * 3.0) : 0.0, 100.0 * bs[b] / n);
            }
        };
        // ANATEST_GTDISP=disp0.pfm (with ANATEST_RIGHT/SHIFT/HALF): the true
        // left-view disparity. A diagnostic shader build writes its left-eye
        // disparity to alpha as (px + 128); report how often it is right, and
        // how the colour error splits between right and wrong disparities.
        if (const wchar_t* gp = _wgetenv(L"ANATEST_GTDISP"))
        {
            FILE* f = nullptr; _wfopen_s(&f, gp, L"rb");
            char hdr[3] = {}; int gw = 0, gh = 0; float scale = 0;
            if (f && fscanf_s(f, "%2s %d %d %f", hdr, 3, &gw, &gh, &scale) == 4)
            {
                fgetc(f);
                std::vector<float> gd((size_t)gw * gh);
                fread(gd.data(), 4, gd.size(), f);
                const int shift = getenv("ANATEST_SHIFT") ? atoi(getenv("ANATEST_SHIFT")) : 0;
                const int halves = getenv("ANATEST_HALF") ? atoi(getenv("ANATEST_HALF")) : 0;
                const int k = 1 << halves;
                size_t n = 0, ok1 = 0, ok3 = 0, bad = 0, badOk = 0; double errOk = 0, errBad = 0; size_t nOk = 0, nBad = 0;
                std::vector<uint8_t> dm((size_t)picW * 2 * picH * 4, 255);
                for (UINT y = 0; y < picH; ++y)
                    for (UINT x = 0; x < picW; ++x)
                    {
                        const float g = gd[(size_t)(gh - 1 - y * k) * gw + x * k + shift];   // (PFM rows run bottom-up; the left photo was cropped by shift)
                        if (!(g < 1e9f) || g <= 0) continue;
                        const float want = (shift - g) / k;                              // dLR in this image's px
                        const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4;
                        const float got = (float)o[3] - 128.0f;
                        const float de = std::abs(got - want);
                        ++n; if (de <= 1) ++ok1; if (de <= 3) ++ok3;
                        const uint8_t* t = &truthL[((size_t)y * tw + x) * 4];
                        int worst = 0; for (int c = 0; c < 3; ++c) worst = (std::max)(worst, std::abs((int)o[c] - (int)t[c]));
                        if (de <= 3) { errOk += worst; ++nOk; if (worst > 40) ++badOk; } else { errBad += worst; ++nBad; }
                        if (worst > 40) ++bad;
                        uint8_t* p0 = &dm[((size_t)y * picW * 2 + x) * 4];
                        const uint8_t gv = (uint8_t)std::clamp(128.0f + want * 2.0f, 0.0f, 255.0f), ov = (uint8_t)std::clamp(128.0f + got * 2.0f, 0.0f, 255.0f);
                        p0[0] = p0[1] = p0[2] = gv; p0[picW * 4] = p0[picW * 4 + 1] = p0[picW * 4 + 2] = ov;
                        if (de > 3) { p0[picW * 4] = 255; p0[picW * 4 + 1] = 0; p0[picW * 4 + 2] = 0; }
                    }
                wprintf(L"disparity: within 1px %.1f%%, within 3px %.1f%%; worst-channel err: disp ok %.1f, disp wrong %.1f; >40 errors with disp ok: %.0f%%\n",
                        100.0 * ok1 / n, 100.0 * ok3 / n, errOk / (std::max)(nOk, (size_t)1), errBad / (std::max)(nBad, (size_t)1), 100.0 * badOk / (std::max)(bad, (size_t)1));
                if (const wchar_t* dp = _wgetenv(L"ANATEST_DISPMAP")) SavePNG(dp, dm.data(), picW * 2, picH, picW * 2 * 4);
#ifndef ANATEST_HEAD
                // ANATEST_BLOBS: which stage first got each BLOB pixel's
                // disparity wrong. A blob pixel: the left eye's HUE (rgb / sum,
                // so a brightness rescale doesn't count) is off from the true
                // view's by > 0.2 (L1), both not dark. Categories, first match:
                // occluded (only the left camera sees it -- no right answer
                // exists), then the first stage off by > 3 px; "all right" =
                // the colour step itself. The same for all pixels, for scale.
                if (getenv("ANATEST_BLOBS"))
                {
                    // ANATEST_GF=r,eps: prototype -- the left eye's borrowed
                    // channels (green, blue) re-estimated by a guided filter
                    // on its own channel (red): per window, borrowed = a x own
                    // + b (the local colour line), a and b fitted from the
                    // window's borrows, then averaged. Detail comes from the own
                    // channel; colour from the neighbourhood's consensus.
                    std::vector<uint8_t> gfOut;
                    if (const char* gs = getenv("ANATEST_GF"))
                    {
                        int R = 8; float eps = 0.001f; sscanf_s(gs, "%d,%f", &R, &eps);
                        const int W = (int)picW, Hh = (int)picH;
                        std::vector<float> I((size_t)W * Hh), P[2] = { std::vector<float>((size_t)W * Hh), std::vector<float>((size_t)W * Hh) };
                        for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                        {
                            const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4;
                            I[(size_t)y * W + x] = o[0] / 255.0f; P[0][(size_t)y * W + x] = o[1] / 255.0f; P[1][(size_t)y * W + x] = o[2] / 255.0f;
                        }
                        auto box = [&](const std::vector<float>& src) {   // mean over the (2R+1)^2 window, via an integral image
                            std::vector<double> S2((size_t)(W + 1) * (Hh + 1), 0.0);
                            for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                                S2[(size_t)(y + 1) * (W + 1) + x + 1] = src[(size_t)y * W + x] + S2[(size_t)y * (W + 1) + x + 1] + S2[(size_t)(y + 1) * (W + 1) + x] - S2[(size_t)y * (W + 1) + x];
                            std::vector<float> out((size_t)W * Hh);
                            for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                            {
                                const int x0 = (std::max)(0, x - R), x1 = (std::min)(W, x + R + 1), y0 = (std::max)(0, y - R), y1 = (std::min)(Hh, y + R + 1);
                                const double s = S2[(size_t)y1 * (W + 1) + x1] - S2[(size_t)y0 * (W + 1) + x1] - S2[(size_t)y1 * (W + 1) + x0] + S2[(size_t)y0 * (W + 1) + x0];
                                out[(size_t)y * W + x] = (float)(s / ((x1 - x0) * (y1 - y0)));
                            }
                            return out;
                        };
                        std::vector<float> II((size_t)W * Hh); for (size_t i = 0; i < II.size(); ++i) II[i] = I[i] * I[i];
                        const auto mI = box(I), mII = box(II);
                        gfOut.assign((size_t)W * Hh * 4, 255);
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            std::vector<float> IP((size_t)W * Hh); for (size_t i = 0; i < IP.size(); ++i) IP[i] = I[i] * P[ch][i];
                            const auto mP = box(P[ch]), mIP = box(IP);
                            std::vector<float> A((size_t)W * Hh), B((size_t)W * Hh);
                            for (size_t i = 0; i < A.size(); ++i) { const float v = mII[i] - mI[i] * mI[i], c = mIP[i] - mI[i] * mP[i]; A[i] = c / (v + eps); B[i] = mP[i] - A[i] * mI[i]; }
                            const auto mA = box(A), mB = box(B);
                            for (size_t i = 0; i < A.size(); ++i) gfOut[i * 4 + 1 + ch] = (uint8_t)std::clamp(std::lround((mA[i] * I[i] + mB[i]) * 255.0f), 0L, 255L);
                        }
                        for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                        {
                            const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4;
                            gfOut[((size_t)y * W + x) * 4] = o[0]; gfOut[((size_t)y * W + x) * 4 + 3] = o[3];
                        }
                        if (const wchar_t* gp2 = _wgetenv(L"ANATEST_GFOUT")) SavePNG(gp2, gfOut.data(), picW, picH, picW * 4);
                    }
                    // ANATEST_ROBUST=R,tau,delta: prototype "least wrong" --
                    // each left-eye pixel's HUE (chromaticity) compared with the
                    // weighted median hue of neighbours within R px whose own
                    // channel (the anaglyph's red, as seen) is within tau; if
                    // it's off by more than delta, the neighbours' hue replaces
                    // it at the pixel's own brightness. No detection needed.
                    if (const char* rs = getenv("ANATEST_ROBUST"))
                    {
                        int R = 12; float tau = 0.06f, delta = 0.12f; sscanf_s(rs, "%d,%f,%f", &R, &tau, &delta);
                        const int W = (int)picW, Hh = (int)picH;
                        std::vector<uint8_t> src(gfOut.empty() ? std::vector<uint8_t>() : gfOut);
                        if (src.empty())
                        {
                            src.resize((size_t)W * Hh * 4);
                            for (int y = 0; y < Hh; ++y) memcpy(&src[(size_t)y * W * 4], base + (size_t)(picY + y) * m.RowPitch + (size_t)picX * 4, (size_t)W * 4);
                        }
                        gfOut = src;
                        auto own = [&](int x, int y) { return px[((size_t)(picY + y) * w + picX + x) * 4] / 255.0f; };   // the anaglyph's red here
                        size_t changed = 0;
                        for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                        {
                            const uint8_t* p = &src[((size_t)y * W + x) * 4];
                            const float sp = (float)p[0] + p[1] + p[2];
                            if (sp < 30) continue;
                            const float r0 = own(x, y);
                            float cg[80], cb[80], cw[80]; int n = 0;
                            for (int dy = -R; dy <= R && n < 80; dy += 3) for (int dx = -R; dx <= R && n < 80; dx += 3)
                            {
                                const int qx = x + dx, qy = y + dy;
                                if ((dx == 0 && dy == 0) || qx < 0 || qy < 0 || qx >= W || qy >= Hh) continue;
                                if (std::abs(own(qx, qy) - r0) > tau) continue;
                                const uint8_t* q = &src[((size_t)qy * W + qx) * 4];
                                const float sq = (float)q[0] + q[1] + q[2]; if (sq < 30) continue;
                                cg[n] = q[1] / sq; cb[n] = q[2] / sq; cw[n] = 1.0f; ++n;
                            }
                            if (n < 6) continue;
                            // weighted median of each chromaticity coordinate
                            auto med = [&](float* v) { std::vector<float> t(v, v + n); std::nth_element(t.begin(), t.begin() + n / 2, t.end()); return t[n / 2]; };
                            const float mg = med(cg), mb = med(cb);
                            const float pg = p[1] / sp, pb = p[2] / sp;
                            if (std::abs(pg - mg) + std::abs(pb - mb) > delta)
                            {
                                // keep brightness (sum) and the pixel's red share scaled to fit
                                const float mr = 1.0f - mg - mb;
                                uint8_t* o = &gfOut[((size_t)y * W + x) * 4];
                                o[0] = (uint8_t)std::clamp(std::lround(mr * sp), 0L, 255L); o[1] = (uint8_t)std::clamp(std::lround(mg * sp), 0L, 255L); o[2] = (uint8_t)std::clamp(std::lround(mb * sp), 0L, 255L);
                                ++changed;
                            }
                        }
                        wprintf(L"robust hue: changed %.2f%% of px\n", 100.0 * changed / (W * Hh));
                        if (const wchar_t* gp2 = _wgetenv(L"ANATEST_GFOUT")) SavePNG(gp2, gfOut.data(), picW, picH, picW * 4);
                    }
                    std::vector<float> gd1;
                    {
                        std::wstring p1(gp); const size_t at = p1.rfind(L"disp0"); if (at != std::wstring::npos) p1.replace(at, 5, L"disp1");
                        FILE* f1 = nullptr; _wfopen_s(&f1, p1.c_str(), L"rb"); char h1[3] = {}; int w1 = 0, hh1 = 0; float s1 = 0;
                        if (f1 && fscanf_s(f1, "%2s %d %d %f", h1, 3, &w1, &hh1, &s1) == 4 && w1 == gw && hh1 == gh) { fgetc(f1); gd1.resize(gd.size()); fread(gd1.data(), 4, gd1.size(), f1); }
                        if (f1) fclose(f1);
                    }
                    StageMap sm[4];
                    for (int s = 0; s < 4; ++s) ReadStage(dev, ctx, conv.DebugStageSRV(s), sm[s]);
                    auto stageAt = [&](const StageMap& s, float sx, float sy) {
                        const float ppt = (s.w * 8 > w) ? 4.0f : 16.0f;
                        const UINT tx = (UINT)std::clamp((int)(sx / ppt), 0, (int)s.w - 1), ty = (UINT)std::clamp((int)(sy / ppt), 0, (int)s.h - 1);
                        return s.v[((size_t)ty * s.w + tx) * 4] * s.w * ppt;
                    };
                    // ANATEST_OCCORACLE: upper bound for occlusion handling -- the
                    // TRULY occluded left-eye pixels (ground truth) take the hue of
                    // the nearest non-occluded pixel on the background side (left)
                    // whose own channel is within 0.06, at their own brightness.
                    if (getenv("ANATEST_OCCORACLE") && !gd1.empty())
                    {
                        const int W = (int)picW, Hh = (int)picH;
                        if (gfOut.empty()) { gfOut.resize((size_t)W * Hh * 4); for (int y = 0; y < Hh; ++y) memcpy(&gfOut[(size_t)y * W * 4], base + (size_t)(picY + y) * m.RowPitch + (size_t)picX * 4, (size_t)W * 4); }
                        std::vector<uint8_t> occ((size_t)W * Hh, 0);
                        for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                        {
                            const float g = gd[(size_t)(gh - 1 - y * k) * gw + x * k + shift];
                            if (!(g < 1e9f) || g <= 0) continue;
                            const int xR = (int)(x * k + shift) - (int)std::lround(g);
                            if (xR >= 0 && xR < gw) { const float g1 = gd1[(size_t)(gh - 1 - y * k) * gw + xR]; if (g1 < 1e9f && g1 > g + 1.5f) occ[(size_t)y * W + x] = 1; }
                        }
                        auto own = [&](int x, int y) { return px[((size_t)(picY + y) * w + picX + x) * 4] / 255.0f; };
                        const std::vector<uint8_t> src = gfOut;
                        size_t fixed = 0;
                        for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x)
                        {
                            if (!occ[(size_t)y * W + x]) continue;
                            const float r0 = own(x, y);
                            for (int s = 1; s <= 200; ++s)
                            {
                                const int qx = x - s; if (qx < 0) break;
                                if (occ[(size_t)y * W + qx] || std::abs(own(qx, y) - r0) > 0.06f) continue;
                                const uint8_t* q = &src[((size_t)y * W + qx) * 4]; uint8_t* o = &gfOut[((size_t)y * W + x) * 4];
                                const float sq = (float)q[0] + q[1] + q[2], so = (float)o[0] + o[1] + o[2];
                                if (sq < 1) break;
                                for (int c = 0; c < 3; ++c) o[c] = (uint8_t)std::clamp(std::lround(q[c] / sq * so), 0L, 255L);
                                ++fixed; break;
                            }
                        }
                        wprintf(L"oracle: recoloured %.2f%% of px\n", 100.0 * fixed / (W * Hh));
                    }
                    const char* cat[7] = { "occluded", "never-found", "lost@refine", "lost@fill", "lost@smooth", "lost@full-res", "depth-right" };
                    size_t cb[7] = {}, ca[7] = {}, nb = 0, na = 0;
                    std::vector<uint8_t> bm((size_t)picW * picH * 4, 0);
                    // ANATEST_OCCSTATS (a build writing an occlusion flag to alpha): how
                    // well the flag finds the truly occluded pixels.
                    const bool occStats = getenv("ANATEST_OCCSTATS") != nullptr;
                    size_t oFl = 0, oGt = 0, oTp = 0, oFlBlob = 0, oGtBlob = 0, oTpBlob = 0;
                    for (size_t i = 3; i < bm.size(); i += 4) bm[i] = 255;
                    for (UINT y = 0; y < picH; ++y)
                        for (UINT x = 0; x < picW; ++x)
                        {
                            const size_t gi = (size_t)(gh - 1 - y * k) * gw + x * k + shift;
                            const float g = gd[gi];
                            if (!(g < 1e9f) || g <= 0) continue;
                            const float want = (shift - g) / k;
                            const uint8_t* o = gfOut.empty() ? base + (size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4 : &gfOut[((size_t)y * picW + x) * 4];
                            const uint8_t* t = &truthL[((size_t)y * tw + x) * 4];
                            const float so = (float)o[0] + o[1] + o[2], st = (float)t[0] + t[1] + t[2];
                            bool blob = false;
                            if (so > 60 && st > 60)
                            {
                                float dh = 0; for (int c = 0; c < 3; ++c) dh += std::abs(o[c] / so - t[c] / st);
                                blob = dh > 0.2f;
                            }
                            int c = 6;
                            // occluded: the left pixel's match in the right view belongs to something nearer
                            const int xR = (int)(x * k + shift) - (int)std::lround(g);
                            if (!gd1.empty() && xR >= 0 && xR < gw)
                            {
                                const float g1 = gd1[(size_t)(gh - 1 - y * k) * gw + xR];
                                if (g1 < 1e9f && g1 > g + 1.5f) c = 0;
                            }
                            if (occStats) { const bool fl = o[3] > 127; if (fl) ++oFl; if (c == 0) ++oGt; if (fl && c == 0) ++oTp; if (fl && blob) ++oFlBlob; if (blob && c == 0) ++oGtBlob; if (fl && blob && c == 0) ++oTpBlob; }
                            if (c != 0)
                            {
                                const float sx = picX + x + 0.5f, sy = picY + y + 0.5f;
                                const float st4[4] = { stageAt(sm[0], sx, sy), stageAt(sm[1], sx, sy), stageAt(sm[2], sx, sy), stageAt(sm[3], sx, sy) };
                                const bool finalOk = std::abs(((float)o[3] - 128.0f) - want) <= 3.0f;
                                if (!finalOk)
                                {
                                    int last = -1; for (int s = 0; s < 4; ++s) if (std::abs(st4[s] - want) <= 3.0f) last = s;
                                    c = (last < 0) ? 1 : (last == 3) ? 5 : 2 + last;   // lost at the stage after the last right one
                                }
                            }
                            ++ca[c]; ++na;
                            if (blob) { ++cb[c]; ++nb; uint8_t* b = &bm[((size_t)y * picW + x) * 4]; const uint8_t col[7][3] = { {255,255,255},{255,0,0},{255,160,0},{255,255,0},{0,255,0},{0,160,255},{255,0,255} }; b[0] = col[c][0]; b[1] = col[c][1]; b[2] = col[c][2]; }
                        }
                    wprintf(L"blobs: %.2f%% of px.  first wrong at:", 100.0 * nb / (std::max)(na, (size_t)1));
                    for (int i = 0; i < 7; ++i) wprintf(L"  %S %.0f%%", cat[i], 100.0 * cb[i] / (std::max)(nb, (size_t)1));
                    wprintf(L"\n  (all px:");
                    for (int i = 0; i < 7; ++i) wprintf(L"  %S %.0f%%", cat[i], 100.0 * ca[i] / (std::max)(na, (size_t)1));
                    wprintf(L")\n");
                    if (const wchar_t* bp = _wgetenv(L"ANATEST_BLOBMAP")) SavePNG(bp, bm.data(), picW, picH, picW * 4);
                    if (occStats)
                        wprintf(L"occlusion flag: flags %.2f%% of px; finds %.0f%% of the truly occluded (%.0f%% of occluded BLOB px); %.0f%% of flagged px are truly occluded; flagged px that are blobs %.0f%%\n",
                                100.0 * oFl / (std::max)(na, (size_t)1), 100.0 * oTp / (std::max)(oGt, (size_t)1), 100.0 * oTpBlob / (std::max)(oGtBlob, (size_t)1), 100.0 * oTp / (std::max)(oFl, (size_t)1), 100.0 * oFlBlob / (std::max)(oFl, (size_t)1));
                }
#endif
            }
            if (f) fclose(f);
        }
        // ANATEST_ERRMAP=path: truth | left eye | error x3 (grey) for the left eye.
        if (const wchar_t* em = _wgetenv(L"ANATEST_ERRMAP"))
        {
            std::vector<uint8_t> im((size_t)picW * 3 * picH * 4, 255);
            for (UINT y = 0; y < picH; ++y)
                for (UINT x = 0; x < picW; ++x)
                {
                    const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4;
                    const uint8_t* g = &truthL[((size_t)y * tw + x) * 4];
                    uint8_t* r0 = &im[((size_t)y * picW * 3 + x) * 4];
                    int worst = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        r0[c] = g[c]; r0[picW * 4 + c] = o[c];
                        worst = (std::max)(worst, std::abs((int)o[c] - (int)g[c]));
                    }
                    const uint8_t v = (uint8_t)(std::min)(255, worst * 3);
                    r0[picW * 8] = v; r0[picW * 8 + 1] = v; r0[picW * 8 + 2] = v;
                }
            SavePNG(em, im.data(), picW * 3, picH, picW * 3 * 4);
        }
        scoreEye(truthL, 0, L"L");
        scoreEye(truthR, eyeW, L"R");
    }
    // ANATEST_STREAKS: in the outer 64 px of the picture, each eye, count
    // pixels in horizontal streaks -- the BORROWED channel(s) flat over 8+ px
    // while the eye's own channel there has texture (a single column copied
    // across). Red/cyan only.
    if (getenv("ANATEST_STREAKS"))
    {
        const UINT B = (std::min)(64u, picW / 4);
        const char* side[4] = { "L-left", "L-right", "R-left", "R-right" };
        for (int s = 0; s < 4; ++s)
        {
            const int eye = s / 2; const bool rightSide = s % 2;
            size_t streak = 0, n = 0;
            for (UINT y = 0; y < picH; ++y)
                for (UINT x0 = 0; x0 + 8 <= B; ++x0)
                {
                    const UINT x = rightSide ? picX + picW - B + x0 : picX + x0;
                    int bMin = 255, bMax = 0, oMin = 255, oMax = 0;
                    for (UINT k = 0; k < 8; ++k)
                    {
                        const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(eye * eyeW + x + k) * 4;
                        const int bor = eye == 0 ? o[1] : o[0], own = eye == 0 ? o[0] : o[1];
                        bMin = (std::min)(bMin, bor); bMax = (std::max)(bMax, bor); oMin = (std::min)(oMin, own); oMax = (std::max)(oMax, own);
                    }
                    ++n; if (bMax - bMin <= 2 && oMax - oMin > 24) ++streak;
                }
            wprintf(L"%S %.2f%%  ", side[s], 100.0 * streak / n);
        }
        wprintf(L"(streaky windows at the edges)\n");
    }
    // ANATEST_OUTSIDE (with a canvas): coloured pixels (chroma > 40) in the
    // left eye OUTSIDE the picture -- where any colour is an error -- and a
    // few of them to probe.
    if (getenv("ANATEST_OUTSIDE"))
    {
        size_t n = 0, bad = 0; std::vector<std::pair<UINT, UINT>> at;
        for (UINT y = 0; y < eyeH; ++y)
            for (UINT x = 0; x < eyeW; ++x)
            {
                if (x >= picX && x < picX + picW && y >= picY && y < picY + picH) continue;
                const uint8_t* o = base + (size_t)y * m.RowPitch + (size_t)x * 4;
                const int mx = (std::max)({ o[0], o[1], o[2] }), mn = (std::min)({ o[0], o[1], o[2] });
                ++n; if (mx - mn > 40) { ++bad; if (at.size() < 400) at.push_back({ x, y }); }
            }
        wprintf(L"outside the picture: %zu coloured px of %zu", bad, n);
        for (size_t i = 0; i < at.size(); i += 50) wprintf(L"  (%u,%u)", at[i].first, at[i].second);
        wprintf(L"\n");
    }
    // ANATEST_ALPHAMAP=path: the left eye's alpha as grey, stretched x2 about
    // 128 (a disparity-out build: 128 + px disparity).
    if (const wchar_t* am = _wgetenv(L"ANATEST_ALPHAMAP"))
    {
        std::vector<uint8_t> im((size_t)picW * picH * 4, 255);
        for (UINT y = 0; y < picH; ++y)
            for (UINT x = 0; x < picW; ++x)
            {
                const int a = base[(size_t)(picY + y) * m.RowPitch + (size_t)(picX + x) * 4 + 3];
                const uint8_t v = (uint8_t)std::clamp(128 + (a - 128) * 2, 0, 255);
                uint8_t* d = &im[((size_t)y * picW + x) * 4]; d[0] = d[1] = d[2] = v;
            }
        SavePNG(am, im.data(), picW, picH, picW * 4);
    }
    // ANATEST_CMP=ref.png: how far this left eye is from a saved one.
    if (const wchar_t* ref = _wgetenv(L"ANATEST_CMP"))
    {
        std::vector<uint8_t> rp; UINT rw = 0, rh = 0;
        // (A ref the picture's size is compared with the picture's area, so
        // runs with the picture at different places line up.)
        const bool pic = LoadImageRGBA(ref, rp, rw, rh) && rw == picW && rh == picH;
        if (pic || (rw == eyeW && rh == eyeH))
        {
            const UINT ox = pic ? picX : 0, oy = pic ? picY : 0;
            int maxd = 0; double sum = 0; size_t over2 = 0, px20 = 0;
            for (UINT y = 0; y < rh; ++y)
                for (UINT x = 0; x < rw; ++x)
                {
                    int worst = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        const int rc = getenv("ANATEST_CMP_SWAPRB") && c != 1 ? 2 - c : c;   // (refs saved before the PNG fix)
                        const int d = std::abs((int)base[(size_t)(oy + y) * m.RowPitch + (size_t)(ox + x) * 4 + c] - (int)rp[((size_t)y * rw + x) * 4 + rc]);
                        maxd = (std::max)(maxd, d); sum += d; if (d > 2) ++over2;
                        worst = (std::max)(worst, d);
                    }
                    if (worst > 20) ++px20;
                }
            wprintf(L"vs %s: max diff %d, mean %.4f, values off by >2: %zu, px off >20: %.3f%%\n", ref, maxd, sum / (rw * rh * 3.0), over2, 100.0 * px20 / (rw * rh));
            // ANATEST_CMPMAP=path: ref | this | difference (x3), side by side.
            if (const wchar_t* cm = _wgetenv(L"ANATEST_CMPMAP"))
            {
                std::vector<uint8_t> im((size_t)rw * 3 * rh * 4, 255);
                for (UINT y = 0; y < rh; ++y)
                    for (UINT x = 0; x < rw; ++x)
                    {
                        const uint8_t* o = base + (size_t)(oy + y) * m.RowPitch + (size_t)(ox + x) * 4;
                        const uint8_t* r = &rp[((size_t)y * rw + x) * 4];
                        uint8_t* d = &im[((size_t)y * rw * 3 + x) * 4];
                        int worst = 0;
                        for (int c = 0; c < 3; ++c) { d[c] = r[c]; d[rw * 4 + c] = o[c]; worst = (std::max)(worst, std::abs((int)o[c] - (int)r[c])); }
                        const uint8_t v = (uint8_t)(std::min)(255, worst * 3);
                        d[rw * 8] = d[rw * 8 + 1] = d[rw * 8 + 2] = v;
                    }
                SavePNG(cm, im.data(), rw * 3, rh, rw * 3 * 4);
            }
        }
    }
    // ANATEST_ROW=y,x0,x1: print input vs left-eye output along that row.
    if (const char* rowSpec = getenv("ANATEST_ROW"))
    {
        int ry = 0, rx0 = 0, rx1 = 0;
        if (sscanf_s(rowSpec, "%d,%d,%d", &ry, &rx0, &rx1) == 3)
            for (int x = rx0; x <= rx1 && x < (int)w && ry < (int)h; ++x)
            {
                const uint8_t* i = &px[((size_t)ry * w + x) * 4];
                const uint8_t* o = base + (size_t)ry * m.RowPitch + (size_t)x * 4;
                wprintf(L"x=%4d in %3u %3u %3u   L-out %3u %3u %3u\n", x, i[0], i[1], i[2], o[0], o[1], o[2]);
            }
    }
    ctx->Unmap(st, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    const bool ok = SavePNG(argv[2], out.data(), ow, oh, ow * 4);
    wprintf(L"%s: %ux%u per eye -> %s (%s)\n", argv[1], eyeW, eyeH, argv[2], ok ? L"saved" : L"FAILED");
    return ok ? 0 : 1;
}
