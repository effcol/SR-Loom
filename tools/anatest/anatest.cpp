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
#ifndef ANATEST_HEAD
#include "ScreenAnalysis.h"
#endif
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

// Zero-copy check (ANATEST_RAW=1 with ANATEST_FORMATS): the converter given a
// plain UNORM view of the same pixels plus SetSourceEncoded -- what SR Loom does
// with the capture's own frame -- must match the _SRGB view's output. Prints the
// largest byte difference, how many bytes differ, and both GPU times.
static std::vector<uint8_t> ReadOutputBytes(ID3D11Device* dev, ID3D11DeviceContext* ctx, Converter& cv)
{
    std::vector<uint8_t> out;
    ID3D11Resource* r = nullptr; cv.OutputSRV()->GetResource(&r);
    ID3D11Texture2D* ot = nullptr; r->QueryInterface(&ot); r->Release();
    D3D11_TEXTURE2D_DESC od{}; ot->GetDesc(&od);
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&od, nullptr, &st);
    ctx->CopyResource(st, ot); ot->Release();
    D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
    out.resize((size_t)od.Width * od.Height * 4);
    for (UINT y = 0; y < od.Height; ++y)
        memcpy(&out[(size_t)y * od.Width * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, (size_t)od.Width * 4);
    ctx->Unmap(st, 0); st->Release();
    return out;
}

static double TimeConversions(ID3D11Device* dev, ID3D11DeviceContext* ctx, Converter& cv, ID3D11ShaderResourceView* v, int w, int h)
{
    D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, tq{ D3D11_QUERY_TIMESTAMP, 0 };
    ID3D11Query *dj = nullptr, *q0 = nullptr, *q1 = nullptr;
    dev->CreateQuery(&dq, &dj); dev->CreateQuery(&tq, &q0); dev->CreateQuery(&tq, &q1);
    ctx->Begin(dj); ctx->End(q0);
    for (int k = 0; k < 40; ++k) { cv.SetSourceVersion(0); bool r2 = false; cv.Convert(v, w, h, r2); }
    ctx->End(q1); ctx->End(dj);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{}; UINT64 a = 0, b = 0;
    while (ctx->GetData(dj, &dd, sizeof(dd), 0) != S_OK) Sleep(1);
    ctx->GetData(q0, &a, sizeof(a), 0); ctx->GetData(q1, &b, sizeof(b), 0);
    dj->Release(); q0->Release(); q1->Release();
    return (!dd.Disjoint && dd.Frequency) ? (double)(b - a) / dd.Frequency * 1000.0 / 40.0 : -1.0;
}

// setup: puts a fresh converter into the mode under test.
template <typename Setup>
static void RawCompare(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, ID3D11ShaderResourceView* srgbView,
                       int w, int h, const char* name, Setup setup)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* raw = nullptr; dev->CreateShaderResourceView(tex, &vd, &raw);
    Converter a, b; a.Initialize(dev, ctx); b.Initialize(dev, ctx);
    setup(a); setup(b);
    if (const char* cvg = getenv("ANATEST_CONV")) { a.SetConvergence((float)atof(cvg)); b.SetConvergence((float)atof(cvg)); }
    b.SetSourceEncoded(true);
    bool rs = false;
    a.Convert(srgbView, w, h, rs); a.Convert(srgbView, w, h, rs);
    b.Convert(raw, w, h, rs); b.Convert(raw, w, h, rs);
    const auto oa = ReadOutputBytes(dev, ctx, a), ob = ReadOutputBytes(dev, ctx, b);
    int maxd = 0; size_t over1 = 0, diff = 0;
    for (size_t i = 0; i < oa.size() && i < ob.size(); ++i)
    {
        if ((i & 3) == 3) continue;   // (alpha)
        const int d = std::abs((int)oa[i] - (int)ob[i]);
        maxd = (std::max)(maxd, d); if (d) ++diff; if (d > 1) ++over1;
    }
    const double ta = TimeConversions(dev, ctx, a, srgbView, w, h), tb = TimeConversions(dev, ctx, b, raw, w, h);
    wprintf(L"  zero-copy %-22S max diff %d, bytes differing %.4f%% (>1: %.4f%%) | GPU sRGB view %.3f ms, raw+decode %.3f ms\n",
            name, maxd, 100.0 * diff / (oa.size() * 0.75), 100.0 * over1 / (oa.size() * 0.75), ta, tb);
    a.Shutdown(); b.Shutdown(); raw->Release();
}

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
    // ANATEST_SCALE=w: the input box-scaled to w px wide first (a thumbnail).
    if (const char* sc = getenv("ANATEST_SCALE"))
    {
        const UINT nw = (UINT)atoi(sc), nh = (UINT)((double)h * nw / w);
        if (nw > 0 && nw < w)
        {
            std::vector<uint8_t> o((size_t)nw * nh * 4, 255);
            for (UINT y = 0; y < nh; ++y) for (UINT x = 0; x < nw; ++x)
            {
                const UINT x0 = x * w / nw, x1 = (std::max)(x0 + 1, (x + 1) * w / nw), y0 = y * h / nh, y1 = (std::max)(y0 + 1, (y + 1) * h / nh);
                UINT acc[3] = {}, n = 0;
                for (UINT yy = y0; yy < y1; ++yy) for (UINT xx = x0; xx < x1; ++xx) { for (int c = 0; c < 3; ++c) acc[c] += px[((size_t)yy * w + xx) * 4 + c]; ++n; }
                for (int c = 0; c < 3; ++c) o[((size_t)y * nw + x) * 4 + c] = (uint8_t)(acc[c] / n);
            }
            px.swap(o); w = nw; h = nh;
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
            // ANATEST_UI: a browser-like interface on the canvas -- a coloured
            // tab bar across the top and a column of colourful icons down the left.
            if (getenv("ANATEST_UI"))
            {
                const uint8_t pal[6][3] = { {220,40,40}, {40,160,60}, {50,90,220}, {240,190,30}, {150,60,200}, {30,180,190} };
                for (int y = 0; y < 36 && y < CH; ++y) for (int x = 0; x < CW; ++x)
                { uint8_t* p = &canvas[((size_t)y * CW + x) * 4]; const uint8_t* c = pal[(x / 180) % 6]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; }
                for (int k = 0; k < 14; ++k)
                    for (int y = 60 + k * 56; y < 60 + k * 56 + 36 && y < CH; ++y) for (int x = 10; x < 46; ++x)
                    { uint8_t* p = &canvas[((size_t)y * CW + x) * 4]; const uint8_t* c = pal[k % 6]; p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; }
            }
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
        // (Greyscale under a red/cyan anaglyph: green = blue everywhere.)
        double gb = 0, rc = 0;
        for (size_t i = 0; i < px.size(); i += 4)
        {
            gb += std::abs((int)px[i + 1] - (int)px[i + 2]);
            rc += std::abs((int)px[i] - ((int)px[i + 1] + (int)px[i + 2]) / 2);
        }
        wprintf(L"input mean |g-b| %.2f, mean |r-cyan| %.2f\n", gb / n, rc / n);
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
    // (ANATEST_DEBUG: the D3D debug layer, its messages printed after the change test.)
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, getenv("ANATEST_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { fwprintf(stderr, L"no D3D11 device\n"); return 1; }

    // The source as SR Loom sees a capture: RGBA8, read through an sRGB view.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 };
    ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, &sd, &src);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    // (ANATEST_ENCODED: read as the app reads a zero-copy capture -- a plain UNORM
    // view, the shaders decoding sRGB themselves: Converter::SetSourceEncoded.)
    const bool encodedSrc = getenv("ANATEST_ENCODED") != nullptr;
    vd.Format = encodedSrc ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(src, &vd, &srv);

    Converter conv;
    if (!conv.Initialize(dev, ctx)) { fwprintf(stderr, L"converter init failed\n"); return 1; }
#ifndef ANATEST_HEAD
    // ANATEST_RGBDTEST=strength,look: the picture taken as colour | depth and
    // each eye's view drawn from the depth. Reports how much the two eyes differ
    // (none = no depth being applied) and the GPU time.
    if (const char* rt = getenv("ANATEST_RGBDTEST"))
    {
        float strength = 50.0f, look = 0.0f; sscanf_s(rt, "%f,%f", &strength, &look);
        Converter q; q.Initialize(dev, ctx);
        q.SetFormat(StereoFormat::RGBD, false, 0, 0);
        q.SetQuilt(4, 1, 0, 0, 0.0f, 0.0f);
        q.SetTargetPaneSize(3840, 2160);
        q.SetVRView(look, 0.0f, strength * 0.0008f);
        q.SetFramePacking(0.5f, 1.0f, 0.0f);
        const double ms = TimeConversions(dev, ctx, q, srv, (int)w, (int)h);
        const auto o = ReadOutputBytes(dev, ctx, q);
        const int ow = q.OutputPerEyeWidth() * 2, oh = q.OutputHeight();
        long long differ = 0, n = 0; double sum = 0.0;
        if ((int)o.size() >= ow * oh * 4)
            for (int y = 0; y < oh; y += 5)
                for (int x = 0; x < ow / 2; ++x)
                {
                    const int a = o[((size_t)y * ow + x) * 4 + 1], b = o[((size_t)y * ow + x + ow / 2) * 4 + 1];
                    if (abs(a - b) > 2) ++differ;
                    sum += a; ++n;
                }
        wprintf(L"RGB + depth, strength %.0f, look %.2f: output %dx%d, %.2f ms | the eyes differ at %.1f%% of pixels | mean green %.1f\n",
                strength, look, ow, oh, ms, n ? 100.0 * differ / n : 0.0, n ? sum / n : 0.0);
        q.Shutdown();
        return 0;
    }
    // ANATEST_LFTEST=cols,rows,pitch,slant: the picture taken as a quilt and
    // interlaced as a light field for a 3840x2160 panel. Checks that both
    // halves of the output are the same picture (the SR weave of two equal eyes
    // leaves it as it is), and how much neighbouring pixels differ (views
    // alternating under the lens); the GPU time.
    if (const char* lt = getenv("ANATEST_LFTEST"))
    {
        int qc = 8, qr = 6; float pitch = 1.8f, slant = 0.3f; sscanf_s(lt, "%d,%d,%f,%f", &qc, &qr, &pitch, &slant);
        Converter q; q.Initialize(dev, ctx);
        q.SetFormat(StereoFormat::Quilt, false, 0, 0);
        q.SetQuilt(qc, qr, 0, 0, 0.0f, 0.0f);
        q.SetTargetPaneSize(3840, 2160);
        q.SetLightField(pitch, slant, 3840, 2160);
        const double ms = TimeConversions(dev, ctx, q, srv, (int)w, (int)h);
        const auto o = ReadOutputBytes(dev, ctx, q);
        const int ow = q.OutputPerEyeWidth() * 2, oh = q.OutputHeight();
        long long differ = 0, step = 0, n = 0;
        if ((int)o.size() >= ow * oh * 4)
            for (int y = 0; y < oh; y += 7)
                for (int x = 0; x + 1 < ow / 2; ++x)
                    for (int c = 0; c < 3; ++c)
                    {
                        const int a = o[((size_t)y * ow + x) * 4 + c], b = o[((size_t)y * ow + x + ow / 2) * 4 + c], r = o[((size_t)y * ow + x + 1) * 4 + c];
                        if (a != b) ++differ;
                        step += abs(a - r); ++n;
                    }
        wprintf(L"light field %dx%d views, pitch %.4f px, slant %.4f: output %dx%d, %.2f ms | halves differ in %lld of %lld samples | mean step between neighbouring pixels %.2f\n",
                qc, qr, pitch, slant, ow, oh, ms, differ, n, n ? (double)step / n : 0.0);
        q.Shutdown();
        return 0;
    }
    // ANATEST_QUILTTEST=cols,rows: the picture taken as a quilt, converted for a
    // 3840x2160 pane, cross-fading between views: the two-pass resampling against
    // the one-pass (must match), and both GPU times.
    if (const char* qt = getenv("ANATEST_QUILTTEST"))
    {
        int qc = 8, qr = 6; sscanf_s(qt, "%d,%d", &qc, &qr);
        auto run = [&](bool two, double& ms) {
            Converter q; q.Initialize(dev, ctx);
            q.SetFormat(StereoFormat::Quilt, false, 0, 0);
            q.SetQuilt(qc, qr, qc * qr / 2 - 1, qc * qr / 2 + 1, 0.4f, 0.6f);
            q.SetTargetPaneSize(3840, 2160);
            q.SetQuiltTwoPass(two);
            ms = TimeConversions(dev, ctx, q, srv, (int)w, (int)h);
            auto o = ReadOutputBytes(dev, ctx, q);
            q.Shutdown();
            return o;
        };
        double m1 = 0, m2 = 0;
        const auto a = run(false, m1), b = run(true, m2);
        int worst = 0; size_t diff = 0;
        for (size_t k = 0; k < a.size() && k < b.size(); ++k) { const int d = std::abs((int)a[k] - (int)b[k]); worst = (std::max)(worst, d); diff += d > 1; }
        wprintf(L"quilt %dx%d: one pass %.3f ms, two passes %.3f ms | largest difference %d, bytes off by >1: %zu of %zu\n", qc, qr, m1, m2, worst, diff, a.size());
        return 0;
    }
    // ANATEST_FORMATS=right.png (the input is the pair's left picture): every
    // packed layout checked end to end. The pair packed the way each layout
    // defines it, converted, and each output eye scored against the true left
    // and right pictures (PSNR, at the eye's size). Right: the "left" eye
    // matches the left picture far better than the right one, and vice versa.
    if (const wchar_t* fr = _wgetenv(L"ANATEST_FORMATS"))
    {
        std::vector<uint8_t> R; UINT rw = 0, rh = 0;
        if (!LoadImageRGBA(fr, R, rw, rh) || rw != w || rh != h) { fwprintf(stderr, L"right picture must match the left's size\n"); return 1; }
        const std::vector<uint8_t>& L = px;
        auto at = [&](const std::vector<uint8_t>& v, int x, int y) { return &v[((size_t)y * w + x) * 4]; };
        // A picture resampled (box) to ow x oh.
        auto resample = [&](const std::vector<uint8_t>& v, int ow, int oh) {
            std::vector<uint8_t> o((size_t)ow * oh * 4);
            for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
                const int x0 = x * (int)w / ow, x1 = (std::max)(x0 + 1, (x + 1) * (int)w / ow), y0 = y * (int)h / oh, y1 = (std::max)(y0 + 1, (y + 1) * (int)h / oh);
                int s[4] = {}, n = 0;
                for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { const uint8_t* p = at(v, xx, yy); for (int c = 0; c < 4; ++c) s[c] += p[c]; ++n; }
                for (int c = 0; c < 4; ++c) o[((size_t)y * ow + x) * 4 + c] = (uint8_t)(s[c] / n);
            }
            return o;
        };
        auto psnr = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            double s = 0; size_t n = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); i += 4) for (int c = 0; c < 3; ++c) { const double d = (double)a[i + c] - b[i + c]; s += d * d; ++n; }
            return 10.0 * std::log10(255.0 * 255.0 / (std::max)(s / (std::max)(n, (size_t)1), 1e-9));
        };
        struct Case { const char* name; StereoFormat f; int sw, sh; };
        const Case cases[] = {
            { "half SBS",           StereoFormat::HalfSBS,           (int)w,     (int)h },
            { "half top-bottom",    StereoFormat::HalfTAB,           (int)w,     (int)h },
            { "full top-bottom",    StereoFormat::FullTAB,           (int)w,     (int)h * 2 },
            { "row interleaved",    StereoFormat::RowInterleaved,    (int)w,     (int)h },
            { "column interleaved", StereoFormat::ColumnInterleaved, (int)w,     (int)h },
            { "checkerboard",       StereoFormat::Checkerboard,      (int)w,     (int)h },
            { "full SBS (32:9 in 16:9)", StereoFormat::FullSBS,       (int)w,     (int)h },
            { "frame packing",      StereoFormat::FramePacking,      (int)w,     (int)h * 2 + (int)h * 45 / 1080 },
        };
        for (const Case& c : cases)
        {
            // Packed as the layout defines it.
            std::vector<uint8_t> s((size_t)c.sw * c.sh * 4, 255);
            auto put = [&](int x, int y, const uint8_t* p) { memcpy(&s[((size_t)y * c.sw + x) * 4], p, 4); };
            std::vector<uint8_t> Lh, Rh;
            if (c.f == StereoFormat::HalfSBS) { Lh = resample(L, w / 2, h); Rh = resample(R, w / 2, h); }
            if (c.f == StereoFormat::HalfTAB) { Lh = resample(L, w, h / 2); Rh = resample(R, w, h / 2); }
            if (c.f == StereoFormat::FullSBS) { Lh = resample(L, w / 2, h / 2); Rh = resample(R, w / 2, h / 2); }
            for (int y = 0; y < c.sh; ++y)
                for (int x = 0; x < c.sw; ++x)
                {
                    const uint8_t* p = nullptr;
                    switch (c.f)
                    {
                    case StereoFormat::HalfSBS: p = x < (int)w / 2 ? &Lh[((size_t)y * (w / 2) + x) * 4] : &Rh[((size_t)y * (w / 2) + (std::min)(x - (int)w / 2, (int)w / 2 - 1)) * 4]; break;
                    case StereoFormat::HalfTAB: p = y < (int)h / 2 ? &Lh[((size_t)y * w + x) * 4] : &Rh[((size_t)(std::min)(y - (int)h / 2, (int)h / 2 - 1) * w + x) * 4]; break;
                    case StereoFormat::FullTAB: p = y < (int)h ? at(L, x, y) : at(R, x, y - h); break;
                    case StereoFormat::RowInterleaved: p = (y & 1) ? at(R, x, y) : at(L, x, y); break;
                    case StereoFormat::ColumnInterleaved: p = (x & 1) ? at(R, x, y) : at(L, x, y); break;
                    case StereoFormat::Checkerboard: p = ((x + y) & 1) ? at(R, x, y) : at(L, x, y); break;
                    case StereoFormat::FullSBS:   // (black above and below the 32:9 strip)
                    {
                        static const uint8_t black[4] = { 0, 0, 0, 255 };
                        const int sy = y - (int)h / 4;
                        if (sy < 0 || sy >= (int)h / 2) { p = black; break; }
                        p = x < (int)w / 2 ? &Lh[((size_t)sy * (w / 2) + x) * 4] : &Rh[((size_t)sy * (w / 2) + (std::min)(x - (int)w / 2, (int)w / 2 - 1)) * 4];
                        break;
                    }
                    case StereoFormat::FramePacking:   // left eye, gap (black), right eye
                    {
                        static const uint8_t black[4] = { 0, 0, 0, 255 };
                        const int gap = c.sh - 2 * (int)h;
                        p = y < (int)h ? at(L, x, y) : (y < (int)h + gap ? black : at(R, x, y - (int)h - gap));
                        break;
                    }
                    default: break;
                    }
                    put(x, y, p);
                }
            D3D11_TEXTURE2D_DESC sd2{};
            sd2.Width = c.sw; sd2.Height = c.sh; sd2.MipLevels = 1; sd2.ArraySize = 1; sd2.SampleDesc.Count = 1;
            sd2.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; sd2.Usage = D3D11_USAGE_DEFAULT; sd2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sdd{ s.data(), (UINT)c.sw * 4, 0 };
            ID3D11Texture2D* t2 = nullptr; dev->CreateTexture2D(&sd2, &sdd, &t2);
            D3D11_SHADER_RESOURCE_VIEW_DESC vd2{}; vd2.Format = encodedSrc ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd2.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd2.Texture2D.MipLevels = 1;
            ID3D11ShaderResourceView* v2 = nullptr; dev->CreateShaderResourceView(t2, &vd2, &v2);
            Converter cv; cv.Initialize(dev, ctx); cv.SetSourceEncoded(encodedSrc); if (const char* cvg = getenv("ANATEST_CONV")) cv.SetConvergence((float)atof(cvg));
            if (getenv("ANATEST_NOCS")) cv.SetComputeBothEyes(false);
            cv.SetFormat(c.f, false, 0, 4);
            if (c.f == StereoFormat::FramePacking) cv.SetFramePacking((float)h / c.sh, (float)(c.sh - 2 * (int)h) / c.sh, 0.0f);
            bool rs = false; cv.Convert(v2, c.sw, c.sh, rs);
            // The output, both eyes.
            ID3D11Resource* r = nullptr; cv.OutputSRV()->GetResource(&r);
            ID3D11Texture2D* ot = nullptr; r->QueryInterface(&ot); r->Release();
            D3D11_TEXTURE2D_DESC od{}; ot->GetDesc(&od);
            od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&od, nullptr, &st);
            ctx->CopyResource(st, ot); ot->Release();
            D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
            const int ew = (int)od.Width / 2, eh = (int)od.Height;
            std::vector<uint8_t> eL((size_t)ew * eh * 4), eR(eL.size());
            for (int y = 0; y < eh; ++y)
            {
                memcpy(&eL[(size_t)y * ew * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, (size_t)ew * 4);
                memcpy(&eR[(size_t)y * ew * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch + (size_t)ew * 4, (size_t)ew * 4);
            }
            ctx->Unmap(st, 0); st->Release();
            const auto tL = resample(L, ew, eh), tR = resample(R, ew, eh);
            const double ll = psnr(eL, tL), lr = psnr(eL, tR), rr = psnr(eR, tR), rl = psnr(eR, tL);
            const bool ok = ll > lr + 3 && rr > rl + 3;
            wprintf(L"%-20S eyes %dx%d | left eye: vs left %.1f dB, vs right %.1f | right eye: vs right %.1f, vs left %.1f  -> %s\n",
                    c.name, ew, eh, ll, lr, rr, rl, ok ? L"OK" : L"WRONG");
            // (GPU time of one conversion: 40 in a row between timestamps.)
            {
                D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, tq{ D3D11_QUERY_TIMESTAMP, 0 };
                ID3D11Query *dj = nullptr, *t0 = nullptr, *t1 = nullptr;
                dev->CreateQuery(&dq, &dj); dev->CreateQuery(&tq, &t0); dev->CreateQuery(&tq, &t1);
                ctx->Begin(dj); ctx->End(t0);
                for (int k = 0; k < 40; ++k) { cv.SetSourceVersion(0); bool r2 = false; cv.Convert(v2, c.sw, c.sh, r2); }
                ctx->End(t1); ctx->End(dj);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{}; UINT64 a = 0, b = 0;
                while (ctx->GetData(dj, &dd, sizeof(dd), 0) != S_OK) Sleep(1);
                ctx->GetData(t0, &a, sizeof(a), 0); ctx->GetData(t1, &b, sizeof(b), 0);
                if (!dd.Disjoint && dd.Frequency) wprintf(L"%-20S GPU %.3f ms per conversion\n", c.name, (double)(b - a) / dd.Frequency * 1000.0 / 40.0);
                dj->Release(); t0->Release(); t1->Release();
            }
            if (_wgetenv(L"ANATEST_RAW"))
                RawCompare(dev, ctx, t2, v2, c.sw, c.sh, c.name, [&](Converter& x) {
                    x.SetHalfWidthEyes(true);
                    x.SetFormat(c.f, false, 0, 4);
                    if (c.f == StereoFormat::FramePacking) x.SetFramePacking((float)h / c.sh, (float)(c.sh - 2 * (int)h) / c.sh, 0.0f); });
            cv.Shutdown(); v2->Release(); t2->Release();
        }
        // The anaglyph modes' GPU time (a red/cyan anaglyph of the pair).
        {
            std::vector<uint8_t> a((size_t)w * h * 4);
            for (size_t i = 0; i < a.size(); i += 4) { a[i] = L[i]; a[i + 1] = R[i + 1]; a[i + 2] = R[i + 2]; a[i + 3] = 255; }
            D3D11_TEXTURE2D_DESC sd2{};
            sd2.Width = w; sd2.Height = h; sd2.MipLevels = 1; sd2.ArraySize = 1; sd2.SampleDesc.Count = 1;
            sd2.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; sd2.Usage = D3D11_USAGE_DEFAULT; sd2.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sdd{ a.data(), w * 4, 0 };
            ID3D11Texture2D* t2 = nullptr; dev->CreateTexture2D(&sd2, &sdd, &t2);
            D3D11_SHADER_RESOURCE_VIEW_DESC vd2{}; vd2.Format = encodedSrc ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; vd2.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd2.Texture2D.MipLevels = 1;
            ID3D11ShaderResourceView* v2 = nullptr; dev->CreateShaderResourceView(t2, &vd2, &v2);
            struct M { const char* name; int mode; bool skip; };
            const M ms[] = { { "anaglyph DeAnaglyph", 0, false }, { "anaglyph Filtered", 1, false }, { "anaglyph Half Colour", 2, false },
                             { "anaglyph Mono", 3, false }, { "Recovered, all redrawn", 4, false }, { "Recovered, still page", 4, true } };
            for (const M& m : ms)
            {
                Converter cv; cv.Initialize(dev, ctx); cv.SetSourceEncoded(encodedSrc); if (const char* cvg = getenv("ANATEST_CONV")) cv.SetConvergence((float)atof(cvg));
                if (getenv("ANATEST_NOCS")) cv.SetComputeBothEyes(false);
                cv.SetFormat(StereoFormat::Anaglyph, false, 0, m.mode);
                cv.SetHalfWidthEyes(getenv("ANATEST_HALFEYES") != nullptr);
                cv.SetChangeSkip(m.skip);
                bool r2 = false; cv.Convert(v2, (int)w, (int)h, r2); cv.Convert(v2, (int)w, (int)h, r2);
                D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, tq{ D3D11_QUERY_TIMESTAMP, 0 };
                ID3D11Query *dj = nullptr, *q0 = nullptr, *q1 = nullptr;
                dev->CreateQuery(&dq, &dj); dev->CreateQuery(&tq, &q0); dev->CreateQuery(&tq, &q1);
                ctx->Begin(dj); ctx->End(q0);
                for (int k = 0; k < 40; ++k) { cv.SetSourceVersion(0); cv.Convert(v2, (int)w, (int)h, r2); }
                ctx->End(q1); ctx->End(dj);
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{}; UINT64 x0 = 0, x1 = 0;
                while (ctx->GetData(dj, &dd, sizeof(dd), 0) != S_OK) Sleep(1);
                ctx->GetData(q0, &x0, sizeof(x0), 0); ctx->GetData(q1, &x1, sizeof(x1), 0);
                if (!dd.Disjoint && dd.Frequency) wprintf(L"%-24S GPU %.3f ms per conversion\n", m.name, (double)(x1 - x0) / dd.Frequency * 1000.0 / 40.0);
                dj->Release(); q0->Release(); q1->Release(); cv.Shutdown();
                if (_wgetenv(L"ANATEST_RAW"))
                    RawCompare(dev, ctx, t2, v2, (int)w, (int)h, m.name, [&](Converter& x) {
                        x.SetHalfWidthEyes(true);
                        x.SetFormat(StereoFormat::Anaglyph, false, 0, m.mode);
                        x.SetChangeSkip(m.skip); });
            }
            // The formats that had no shader of their own: their GPU time.
            {
                struct F { const char* name; StereoFormat f; int kind; };
                const F fs[] = { { "Pulfrich (ND filter)", StereoFormat::Pulfrich, 1 }, { "Pulfrich (time delay)", StereoFormat::Pulfrich, 2 },
                                 { "frame sequential", StereoFormat::FrameSequential, 0 }, { "360 (VR180 SBS)", StereoFormat::VR180SBS, 0 },
                                 { "360 (VR360 TAB)", StereoFormat::VR360TAB, 0 }, { "RGB + depth", StereoFormat::RGBD, 3 }, { "RGB + depth, side found", StereoFormat::RGBD, 4 } };
                for (const F& m : fs)
                {
                    Converter cv; cv.Initialize(dev, ctx); cv.SetSourceEncoded(encodedSrc); if (const char* cvg = getenv("ANATEST_CONV")) cv.SetConvergence((float)atof(cvg));
                    cv.SetFormat(m.f, false, 0, 4);
                    if (m.kind == 1) cv.SetPulfrich(PulfrichMode::NDFilter, 0, 0.25f, 1);
                    if (m.kind == 2) cv.SetPulfrich(PulfrichMode::TimeDelay, 0, 1.0f, 2);
                    if (m.kind == 3) { cv.SetVRView(0.0f, 0.02f, 1.0f); cv.SetFramePacking(0.5f, 1.0f, 0.0f); cv.SetQuilt(4, 1, 0, 0); }
                    if (m.kind == 4) { cv.SetVRView(0.0f, 0.02f, 1.0f); cv.SetFramePacking(0.5f, 1.0f, 0.0f); cv.SetQuilt(8, 1, 0, 0); }
                    bool r2 = false; cv.Convert(v2, (int)w, (int)h, r2); cv.Convert(v2, (int)w, (int)h, r2);
                    D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, tq{ D3D11_QUERY_TIMESTAMP, 0 };
                    ID3D11Query *dj = nullptr, *q0 = nullptr, *q1 = nullptr;
                    dev->CreateQuery(&dq, &dj); dev->CreateQuery(&tq, &q0); dev->CreateQuery(&tq, &q1);
                    ctx->Begin(dj); ctx->End(q0);
                    for (int k = 0; k < 40; ++k) { cv.SetSourceVersion(0); cv.Convert(v2, (int)w, (int)h, r2); }
                    ctx->End(q1); ctx->End(dj);
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{}; UINT64 x0 = 0, x1 = 0;
                    while (ctx->GetData(dj, &dd, sizeof(dd), 0) != S_OK) Sleep(1);
                    ctx->GetData(q0, &x0, sizeof(x0), 0); ctx->GetData(q1, &x1, sizeof(x1), 0);
                    if (!dd.Disjoint && dd.Frequency) wprintf(L"%-24S GPU %.3f ms per conversion\n", m.name, (double)(x1 - x0) / dd.Frequency * 1000.0 / 40.0);
                    // (ANATEST_SUM: a checksum of the output, to tell a changed picture from the same one.)
                    if (getenv("ANATEST_SUM"))
                    {
                        ID3D11Resource* r = nullptr; cv.OutputSRV()->GetResource(&r);
                        ID3D11Texture2D* ot = nullptr; r->QueryInterface(&ot); r->Release();
                        D3D11_TEXTURE2D_DESC od{}; ot->GetDesc(&od);
                        od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
                        ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&od, nullptr, &st);
                        ctx->CopyResource(st, ot); ot->Release();
                        D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
                        uint64_t sum = 1469598103934665603ull;
                        for (UINT y = 0; y < od.Height; ++y) { const uint8_t* p = (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch; for (UINT x = 0; x < od.Width * 4; ++x) { sum ^= p[x]; sum *= 1099511628211ull; } }
                        ctx->Unmap(st, 0); st->Release();
                        wprintf(L"%-24S output %ux%u checksum %016llx\n", m.name, od.Width, od.Height, (unsigned long long)sum);
                    }
                    dj->Release(); q0->Release(); q1->Release(); cv.Shutdown();
                }
            }
            v2->Release(); t2->Release();
        }
        return 0;
    }
#endif
    conv.SetFormat(StereoFormat::Anaglyph, false, combo, mode);
    conv.SetHalfWidthEyes(getenv("ANATEST_HALFEYES") != nullptr);   // (full-width eyes: the measurements below assume them)
    conv.SetSourceEncoded(encodedSrc);
    if (getenv("ANATEST_NOSKIP")) conv.SetChangeSkip(false);   // (every frame redrawn whole: worst-case timing)
    if (getenv("ANATEST_NOCSREC")) conv.SetComputeRecover(false);   // (Recovered Colour's compose by the pixel shader)
    if (getenv("ANATEST_NOVIDEO")) conv.SetVideoAuto(false);   // (block-by-block tracking kept even while the whole picture changes)
    if (getenv("ANATEST_NOPAIR")) conv.SetPairRefine(false);   // (full-width Recovered Colour: refine per pixel, not per pair)
    if (getenv("ANATEST_NOSCROLL")) conv.SetScrollReuse(false);   // (a scrolled block redrawn, not last frame's moved)
    if (getenv("ANATEST_NOPYRSKIP")) conv.SetPyramidSkip(false);   // (the 1/4 passes in full whenever anything changed)
    if (getenv("ANATEST_NOREACH")) conv.SetReach(false);   // (a change redraws everything within the whole search range)
    if (getenv("ANATEST_NOPRECHECK")) conv.SetPreCheck(false);   // (an encoded source read as it is, every pass decoding)
    if (getenv("ANATEST_NOCS")) conv.SetComputeBothEyes(false);   // (each eye drawn by a pixel shader)
    if (getenv("ANATEST_NOFLAT")) conv.SetFlatSkip(false);   // (each pixel pair tested for plain grey itself)
#ifndef ANATEST_HEAD
    // Mode 3 or 5: what the app's check (AnalyseAnaPicture) makes of the whole
    // image; 5 decodes with its tint tables.
    if (mode == 3 || mode == 5)
    {
        LumaImage li; li.width = (int)w; li.height = (int)h;
        li.pixels.resize((size_t)w * h); li.red.resize(li.pixels.size()); li.green.resize(li.pixels.size());
        li.blue.resize(li.pixels.size()); li.cyan.resize(li.pixels.size());
        for (size_t i = 0; i < li.pixels.size(); ++i)
        {
            li.red[i] = px[i * 4]; li.green[i] = px[i * 4 + 1]; li.blue[i] = px[i * 4 + 2];
            li.cyan[i] = (uint8_t)((li.green[i] + li.blue[i] + 1) / 2);
            li.pixels[i] = (uint8_t)((li.red[i] * 77 + li.green[i] * 150 + li.blue[i] * 29) >> 8);
        }
        std::shared_ptr<const AnaTint> tint;
        const int kind = AnalyseAnaPicture(li, RECT{ 0, 0, (LONG)w, (LONG)h }, combo, &tint);
        wprintf(L"picture under the anaglyph: %s\n", kind == 1 ? L"black-and-white" : kind == 2 ? L"one colour" : kind == 0 ? L"colour" : L"can't tell");
        if (mode == 5 && tint) conv.SetAnaTint(tint->single, tint->missing);
    }
    // ANATEST_BOXES (mode 4): a page of several anaglyphs, as the app's manual
    // check does it -- Auto Stereo's scan for the picture boxes, each checked
    // (AnalyseAnaPicture), the black-and-white / one-colour ones decoded so
    // inside their box.
    static std::vector<std::shared_ptr<const AnaTint>> boxTints;
    static Converter::AnaBox g_testBoxes[32]; static int g_testBoxCount = 0;
    if (mode == 4 && getenv("ANATEST_BOXES"))
    {
        // (ANATEST_BOXFROM=page.png: the boxes judged on that frame -- e.g. before
        // a scroll -- and used on this one.)
        std::vector<uint8_t> bpx = px; UINT bw = w, bh = h;
        if (const wchar_t* bf = _wgetenv(L"ANATEST_BOXFROM")) { if (!LoadImageRGBA(bf, bpx, bw, bh) || bw != w || bh != h) bpx = px; }
        LumaImage li; li.width = (int)w; li.height = (int)h;
        li.pixels.resize((size_t)w * h); li.red.resize(li.pixels.size()); li.green.resize(li.pixels.size());
        li.blue.resize(li.pixels.size()); li.cyan.resize(li.pixels.size());
        for (size_t i = 0; i < li.pixels.size(); ++i)
        {
            li.red[i] = bpx[i * 4]; li.green[i] = bpx[i * 4 + 1]; li.blue[i] = bpx[i * 4 + 2];
            li.cyan[i] = (uint8_t)((li.green[i] + li.blue[i] + 1) / 2);
            li.pixels[i] = (uint8_t)((li.red[i] * 77 + li.green[i] * 150 + li.blue[i] * 29) >> 8);
        }
        ScanWindow sw; sw.bounds = { 0, 0, (LONG)w, (LONG)h };
        ScanResult res;
        StereoScanner::Scan(li, { sw }, {}, res);
        Converter::AnaBox boxes[32]; int nb = 0;
        for (const ScanHit& hit : res.hits)
        {
            std::shared_ptr<const AnaTint> t;
            const int k = AnalyseAnaPicture(li, hit.rect, combo, &t);
            wprintf(L"box (%ld,%ld)-(%ld,%ld): %s\n", hit.rect.left, hit.rect.top, hit.rect.right, hit.rect.bottom,
                    k == 1 ? L"black-and-white" : k == 2 ? L"one colour" : k == 0 ? L"colour" : L"can't tell");
            if (k < 1 || nb == 32) continue;
            boxTints.push_back(t);
            Converter::AnaBox& b = boxes[nb++];
            const int pad = k == 1 ? 16 : 2;   // (as the app: a black-and-white box reaches past the picture)
            b.u0 = (hit.rect.left - pad) / (float)w; b.v0 = (hit.rect.top - pad) / (float)h;
            b.u1 = (hit.rect.right + pad) / (float)w; b.v1 = (hit.rect.bottom + pad) / (float)h;
            b.checkInsetU = getenv("ANATEST_NOINSET") ? 0.0f : (pad + 2) / (float)w;   // (NOINSET: as before the fix)
            b.checkInsetV = getenv("ANATEST_NOINSET") ? 0.0f : (pad + 2) / (float)h;
            b.kind = k;
            if (t) { b.single = t->single; b.missing = t->missing; b.pairA = t->pairA; b.pairB = t->pairB; }
        }
        // (With ANATEST_BOXFROM: that frame converted first, as the app does --
        // its snapshot the boxes' reference -- so they follow the page to this one.)
        if (_wgetenv(L"ANATEST_BOXFROM") && bpx.size() == px.size())
        {
            ctx->UpdateSubresource(src, 0, nullptr, bpx.data(), w * 4, 0);
            conv.RequestAnaSnapshot();
            bool rs0 = false; conv.Convert(srv, (int)w, (int)h, rs0);
            conv.CommitAnaSnapshot();
            ctx->UpdateSubresource(src, 0, nullptr, px.data(), w * 4, 0);
        }
        conv.SetAnaBoxes(boxes, nb);
        memcpy(g_testBoxes, boxes, sizeof(boxes)); g_testBoxCount = nb;   // (for the change test's full redraw)
    }
#endif
#ifndef ANATEST_HEAD
    // ANATEST_NOISETEST=n,amp: the same picture n times with a little noise
    // (+-amp levels a channel, as video has), every frame redrawn. How steady
    // is the output? Per frame, the share of the left eye's pixels that changed
    // by more than 16 from the frame before -- all, and those dark in the
    // picture (luma < 64): a picture that isn't changing shouldn't flicker.
    if (const char* nt = getenv("ANATEST_NOISETEST"))
    {
        int nn = 20, amp = 2; sscanf_s(nt, "%d,%d", &nn, &amp);
        // (ANATEST_NOISESKIP: the change tracking left on -- with enough noise every block changes every frame, as video.)
        if (!getenv("ANATEST_NOISESKIP")) conv.SetChangeSkip(false);
        { double t0[8]; int c0; conv.TakeRecoveryTimes(t0, c0); }
        auto readEye = [&]() {
            ID3D11Resource* r = nullptr; conv.OutputSRV()->GetResource(&r);
            ID3D11Texture2D* t = nullptr; r->QueryInterface(&t); r->Release();
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&d, nullptr, &st);
            ctx->CopyResource(st, t); t->Release();
            D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
            std::vector<uint8_t> o((size_t)w * h * 4);
            for (UINT y = 0; y < h; ++y) memcpy(&o[(size_t)y * w * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, (size_t)w * 4);
            ctx->Unmap(st, 0); st->Release();
            return o;
        };
        uint32_t rng = 12345;
        std::vector<uint8_t> prev; double all = 0, dark = 0; size_t nDark = 0; int cmp = 0;
        for (size_t i = 0; i < px.size(); i += 4) nDark += (0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]) < 64;
        for (int f = 0; f < nn; ++f)
        {
            std::vector<uint8_t> fr(px);
            for (size_t i = 0; i < fr.size(); ++i)
            {
                if ((i & 3) == 3) continue;
                rng = rng * 1664525u + 1013904223u;
                const int v = (int)fr[i] + (int)((rng >> 16) % (2 * amp + 1)) - amp;
                fr[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            ctx->UpdateSubresource(src, 0, nullptr, fr.data(), w * 4, 0);
            bool rs3 = false; conv.Convert(srv, (int)w, (int)h, rs3);
            auto cur = readEye();
            if (!prev.empty())
            {
                size_t ca = 0, cd = 0;
                for (size_t i = 0; i < cur.size(); i += 4)
                {
                    int dmax = 0; for (int k = 0; k < 3; ++k) dmax = (std::max)(dmax, std::abs((int)cur[i + k] - (int)prev[i + k]));
                    if (dmax > 16) { ++ca; if ((0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]) < 64) ++cd; }
                }
                all += (double)ca / (w * h); dark += nDark ? (double)cd / nDark : 0; ++cmp;
            }
            prev.swap(cur);
        }
        {
            uint64_t sum = 1469598103934665603ull; for (uint8_t b : prev) { sum ^= b; sum *= 1099511628211ull; }
            ctx->Flush(); Sleep(200); { bool rs4 = false; conv.Convert(srv, (int)w, (int)h, rs4); }
            double tm[8] = {}; int tc = 0; conv.TakeRecoveryTimes(tm, tc); double tot = 0; for (double v : tm) tot += v;
            wprintf(L"noise test: last output checksum %016llx | video mode %d | recovery %.2f ms a frame (first stage %.2f, %d timed)\n", (unsigned long long)sum, conv.IsVideoMode() ? 1 : 0, tot, tm[0], tc);
        }
        wprintf(L"noise test (%d frames, +-%d): flicker %.3f%% of pixels a frame, %.3f%% of the dark ones (%.0f%% of the picture is dark)\n",
                nn, amp, 100.0 * all / (std::max)(cmp, 1), 100.0 * dark / (std::max)(cmp, 1), 100.0 * nDark / ((double)w * h));
        return 0;
    }
    // ANATEST_CHANGETEST=dy,n: only the changed blocks redrawn (PSChange) must
    // give the same picture as drawing it all. n frames where the right 40% of
    // the image scrolls dy px a frame (a scrolling pane; the rest still), then
    // the last frame's output against a fresh converter's single full frame.
    // Also each frame's GPU time.
    if (const char* ct = getenv("ANATEST_CHANGETEST"))
    {
        int cdy = 0, cn = 0, cfull = 0; sscanf_s(ct, "%d,%d,%d", &cdy, &cn, &cfull);   // (,1: the whole picture scrolls, wrapping round)
        auto frame = [&](int f) {
            std::vector<uint8_t> o(px);
            const UINT x0 = cfull ? 0 : w * 6 / 10;
            for (UINT y = 0; y < h; ++y)
            {
                const int sy = cfull ? (((int)y + f * cdy) % (int)h + (int)h) % (int)h : (int)y + f * cdy;
                for (UINT x = x0; x < w; ++x)
                {
                    const size_t d = ((size_t)y * w + x) * 4;
                    if (sy < 0 || sy >= (int)h) { o[d] = o[d + 1] = o[d + 2] = 245; o[d + 3] = 255; }
                    else memcpy(&o[d], &px[((size_t)sy * w + x) * 4], 4);
                }
            }
            return o;
        };
        auto readOut = [&](Converter& c) {
            ID3D11Resource* r = nullptr; c.OutputSRV()->GetResource(&r);
            ID3D11Texture2D* t = nullptr; r->QueryInterface(&t); r->Release();
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
            ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&d, nullptr, &st);
            ctx->CopyResource(st, t); t->Release();
            D3D11_MAPPED_SUBRESOURCE mm{}; ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm);
            std::vector<uint8_t> o((size_t)d.Width * d.Height * 4);
            for (UINT y = 0; y < d.Height; ++y) memcpy(&o[(size_t)y * d.Width * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, (size_t)d.Width * 4);
            ctx->Unmap(st, 0); st->Release();
            return o;
        };
        bool rs = false;
        conv.SetChangeStats(true);
        double ms[8] = {}; int cnt = 0;
        double fs[4] = {};
        conv.TakeRecoveryTimes(ms, cnt);
        for (int f = 0; f < cn; ++f)
        {
            const auto fr = frame(f);
            ctx->UpdateSubresource(src, 0, nullptr, fr.data(), w * 4, 0);
            conv.Convert(srv, (int)w, (int)h, rs);
            ctx->Flush();
            if (f % 8 == 7) Sleep(20);
        }
        Sleep(100);
        conv.TakeRecoveryTimes(ms, cnt);
        double tot = 0; for (double v : ms) tot += v;
        const auto a = readOut(conv);
        { Sleep(50); Converter::ChangeStats cs; conv.Convert(srv, (int)w, (int)h, rs); if (conv.TakeChangeStats(cs))
            printf("  frames: %d converted, %d whole, %d with changes (%.1f%% redrawn, %.1f%% kept and moved), %d scrolls (%.0f rows), reuse %d%c", cs.frames, cs.full, cs.changed, cs.redrawn * 100, cs.moved * 100, cs.scrolled, cs.rows, cs.reuse ? 1 : 0, 10); }
        // (Still frames: the same picture again and again.)
        double msS[8] = {}; int cntS = 0;
        conv.TakeRecoveryTimes(msS, cntS);
        for (int f = 0; f < 40; ++f) { conv.Convert(srv, (int)w, (int)h, rs); ctx->Flush(); if (f % 8 == 7) Sleep(20); }
        Sleep(100);
        conv.TakeRecoveryTimes(msS, cntS);
        double totS = 0; for (double v : msS) totS += v;
        // ANATEST_BOXSWAP: the boxes judged afresh (the first one gone) on a still
        // picture -- only their blocks are redrawn (g_boxesNew); the result must
        // be a full redraw's with those boxes.
        std::vector<uint8_t> a2;
        if (getenv("ANATEST_BOXSWAP") && g_testBoxCount > 1)
        {
            conv.SetAnaBoxes(g_testBoxes + 1, g_testBoxCount - 1);
            for (int f = 0; f < 3; ++f) { conv.Convert(srv, (int)w, (int)h, rs); ctx->Flush(); }
            a2 = readOut(conv);
        }
        // The same frames with every frame drawn in full.
        Converter fresh;
        fresh.Initialize(dev, ctx);
        fresh.SetFormat(StereoFormat::Anaglyph, false, combo, mode);
        fresh.SetHalfWidthEyes(getenv("ANATEST_HALFEYES") != nullptr);
        fresh.SetChangeSkip(false);
        fresh.SetSourceEncoded(encodedSrc);
        if (g_testBoxCount > 0) fresh.SetAnaBoxes(g_testBoxes, g_testBoxCount);
        double msF[8] = {}; int cntF = 0;
        for (int f = 0; f < cn; ++f)
        {
            const auto fr = frame(f);
            ctx->UpdateSubresource(src, 0, nullptr, fr.data(), w * 4, 0);
            fresh.Convert(srv, (int)w, (int)h, rs);
            ctx->Flush();
            if (f % 8 == 7) Sleep(20);
        }
        Sleep(100);
        fresh.TakeRecoveryTimes(msF, cntF);
        double totF = 0; for (double v : msF) totF += v;
        wprintf(L"  (every frame in full: %.2f ms/frame, %d timed)\n", totF, cntF);
        const auto b = readOut(fresh);
        long bad = 0; int worst = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i += 4)
            for (int k = 0; k < 3; ++k) { const int dd = std::abs((int)a[i + k] - (int)b[i + k]); worst = (std::max)(worst, dd); if (dd > 8) { ++bad; break; } }
        // (The whole picture scrolled, the true views known: both outputs against
        // them -- is last frame's output, moved, as good as a fresh one?)
        if (cfull && !truthL.empty() && tw == w && th == h && a.size() == (size_t)w * 2 * h * 4 && b.size() == a.size())
        {
            auto score = [&](const std::vector<uint8_t>& o, const wchar_t* name) {
                double sum = 0; size_t off = 0, n = 0;
                for (int eye = 0; eye < 2; ++eye)
                    for (UINT y = 0; y < h; ++y)
                    {
                        const UINT ty = (UINT)((((int)y + (cn - 1) * cdy) % (int)h + (int)h) % (int)h);
                        for (UINT x = 0; x < w; ++x)
                        {
                            const uint8_t* p = &o[((size_t)y * w * 2 + (size_t)eye * w + x) * 4];
                            const uint8_t* g = &(eye ? truthR : truthL)[((size_t)ty * w + x) * 4];
                            int worstC = 0;
                            for (int c = 0; c < 3; ++c) { const int d = std::abs((int)p[c] - (int)g[c]); sum += d; worstC = (std::max)(worstC, d); }
                            if (worstC > 40) ++off;
                            ++n;
                        }
                    }
                wprintf(L"  scroll truth, %s: mean abs err %.3f, px off >40: %.3f%%\n", name, sum / (n * 3.0), 100.0 * off / n);
            };
            score(a, L"as scrolled");
            score(b, L"drawn in full");
        }
        wprintf(L"  (stages: search %.2f, desc %.2f, refine %.2f, fill %.2f, smooth %.2f, compose %.2f, end %.2f %.2f)\n", ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], ms[7]);
        wprintf(L"change test: %d frames, part scrolling: recovery %.2f ms/frame (%d timed) | still frames %.2f ms/frame (%d timed) | "
                L"vs a full redraw: %ld px differ by > 8 of %zu (worst %d)\n", cn, tot, cnt, totS, cntS, bad, a.size() / 4, worst);
        if (!a2.empty())
        {
            fresh.SetAnaBoxes(g_testBoxes + 1, g_testBoxCount - 1);
            fresh.Convert(srv, (int)w, (int)h, rs); ctx->Flush();
            const auto b2 = readOut(fresh);
            long bad2 = 0, moved = 0;
            for (size_t i = 0; i < a2.size() && i < b2.size(); i += 4)
            {
                for (int k = 0; k < 3; ++k) if (std::abs((int)a2[i + k] - (int)b2[i + k]) > 8) { ++bad2; break; }
                for (int k = 0; k < 3; ++k) if (std::abs((int)a2[i + k] - (int)a[i + k]) > 8) { ++moved; break; }
            }
            wprintf(L"box swap: %ld px changed by the new boxes; vs a full redraw with them: %ld px differ by > 8\n", moved, bad2);
        }
        fresh.Shutdown();
        if (getenv("ANATEST_DEBUG"))
        {
            ID3D11InfoQueue* iq = nullptr; dev->QueryInterface(&iq);
            const UINT64 nm = iq ? iq->GetNumStoredMessages() : 0;
            for (UINT64 mi = 0; mi < nm && mi < 12; ++mi)
            {
                SIZE_T len = 0; iq->GetMessage(mi, nullptr, &len);
                std::vector<char> buf(len); auto* dm = (D3D11_MESSAGE*)buf.data();
                if (SUCCEEDED(iq->GetMessage(mi, dm, &len))) printf("  d3d: %.*s\n", (int)dm->DescriptionByteLength, dm->pDescription);
            }
            printf("  d3d: %llu messages\n", nm);
            if (iq) iq->Release();
        }
    }
#endif
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
#ifndef ANATEST_HEAD
    if (getenv("ANATEST_BOXES"))
    {
        float ok[32];
        if (conv.ReadAnaBoxValid(ok)) { wprintf(L"boxes on this frame (per-frame check):"); for (int i = 0; i < (int)boxTints.size() && i < 32; ++i) wprintf(L" %d", ok[i] > 0.5f ? 1 : 0); wprintf(L"\n"); }
    }
#endif
#ifndef ANATEST_HEAD   // (the old converter has no timing read-back)
    // ANATEST_TIME=n: n more frames, then the recovery's GPU time per stage.
    if (const char* tn = getenv("ANATEST_TIME"))
    {
        const int n = atoi(tn);
        double ms[8] = {}; int cnt = 0;
        double fs[4] = {};
        conv.TakeRecoveryTimes(ms, cnt);
        for (int i = 0; i < n; ++i) { conv.Convert(srv, (int)w, (int)h, resized); ctx->Flush(); if (i % 8 == 7) Sleep(20); }
        Sleep(100);
        conv.TakeFirstStageTimes(fs);
        conv.TakeRecoveryTimes(ms, cnt);
        double tot = 0; for (double v : ms) tot += v;
        wprintf(L"time (%d frames): down+coarse %.2f, desc %.2f, refine %.2f, fill %.2f, smooth %.2f, pair %.2f, compose %.2f | total %.2f ms\n",
                cnt, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], tot);
        wprintf(L"  (the first stage: check for change %.2f, copies %.2f, averaging down %.2f, the rest %.2f)\n", fs[0], fs[1], fs[2], fs[3]);
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
            // The same by how bright the TRUE pixel is: dark (< 64), mid, bright
            // (>= 160) -- whether a change helps or hurts the dark areas.
            {
                double bs[3] = {}; size_t bb[3] = {}, bn[3] = {};
                for (UINT y = 0; y < picH; ++y)
                    for (UINT x = 0; x < picW; ++x)
                    {
                        const uint8_t* o = base + (size_t)(picY + y) * m.RowPitch + (size_t)(eyeX0 + picX + x) * 4;
                        const uint8_t* g = &t[((size_t)y * tw + x) * 4];
                        const int lum = (g[0] + g[1] + g[2]) / 3;
                        const int b = lum < 64 ? 0 : lum < 160 ? 1 : 2;
                        int worst = 0;
                        for (int c = 0; c < 3; ++c) { const int d = std::abs((int)o[c] - (int)g[c]); bs[b] += d; worst = (std::max)(worst, d); }
                        if (worst > 40) ++bb[b];
                        ++bn[b];
                    }
                const wchar_t* nm[3] = { L"dark", L"mid", L"bright" };
                wprintf(L"bright %s:", name);
                for (int b = 0; b < 3; ++b)
                    wprintf(L" %s err %.2f, >40 %.2f%% (%.0f%% of px) |", nm[b], bn[b] ? bs[b] / (bn[b] * 3.0) : 0.0,
                            bn[b] ? 100.0 * bb[b] / bn[b] : 0.0, 100.0 * bn[b] / (double)n);
                wprintf(L"\n");
            }
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
                    size_t flatAll[5] = {}, flatBlob[5] = {}, tflatAll[5] = {}, tflatBlob[5] = {}, bothFlatBlob = 0;
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
                            // (How flat the left eye's own channel -- the anaglyph's red -- and
                            // the TRUE view's borrowed channels are around this pixel: the largest
                            // difference to the 8 pixels 3 away. For blob pixels vs all.)
                            {
                                int od = 0, td = 0;
                                const uint8_t* a0 = &px[((size_t)(picY + y) * w + picX + x) * 4];
                                for (int fy = -1; fy <= 1; ++fy) for (int fx = -1; fx <= 1; ++fx)
                                {
                                    const int qx = (std::min)((std::max)((int)x + fx * 3, 0), (int)picW - 1), qy = (std::min)((std::max)((int)y + fy * 3, 0), (int)picH - 1);
                                    od = (std::max)(od, std::abs((int)px[((size_t)(picY + qy) * w + picX + qx) * 4] - (int)a0[0]));
                                    const uint8_t* tq = &truthL[((size_t)qy * tw + qx) * 4];
                                    td = (std::max)(td, (std::max)(std::abs((int)tq[1] - (int)t[1]), std::abs((int)tq[2] - (int)t[2])));
                                }
                                const int ob = od < 4 ? 0 : od < 8 ? 1 : od < 16 ? 2 : od < 32 ? 3 : 4, tb = td < 4 ? 0 : td < 8 ? 1 : td < 16 ? 2 : td < 32 ? 3 : 4;
                                ++flatAll[ob]; ++tflatAll[tb]; if (blob) { ++flatBlob[ob]; ++tflatBlob[tb]; if (od < 8 && td < 8) ++bothFlatBlob; }
                            }
                        }
                    {
                        const wchar_t* bn[5] = { L"<4", L"4-8", L"8-16", L"16-32", L">=32" };
                        wprintf(L"blob px by OWN-channel variation (levels):");
                        for (int i = 0; i < 5; ++i) wprintf(L"  %s %.0f%% (all px %.0f%%)", bn[i], 100.0 * flatBlob[i] / (std::max)(nb, (size_t)1), 100.0 * flatAll[i] / (std::max)(na, (size_t)1));
                        wprintf(L"\nblob px by TRUE borrowed-colour variation:");
                        for (int i = 0; i < 5; ++i) wprintf(L"  %s %.0f%% (all px %.0f%%)", bn[i], 100.0 * tflatBlob[i] / (std::max)(nb, (size_t)1), 100.0 * tflatAll[i] / (std::max)(na, (size_t)1));
                        wprintf(L"\nblob px flat in both (< 8): %.0f%%\n", 100.0 * bothFlatBlob / (std::max)(nb, (size_t)1));
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
