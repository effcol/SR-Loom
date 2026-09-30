// captest: zero-copy capture (Capture::SetZeroCopy) against the real Windows
// capture. It opens its own animated window, then captures (1) that window --
// the Window weave mode -- and (2) the whole monitor it's on, and checks every
// few frames that:
//   - the captured frame itself (DirectSRV) and the copy made from it on
//     demand (SRV, built from the changed parts only) hold the same bytes --
//     this also catches WGC drawing only the changed parts into its buffers;
//   - the converter's output from the frame itself (decoded in the shader)
//     matches the output from the copy, for Half SBS, Half TAB and Row.
// Only numbers are printed: no pixels are saved or shown.
//
// usage: captest [seconds per test]   (built on request: --target captest)
#include "Capture.h"
#include "Converter.h"
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace srw;

namespace
{
    int g_tick = 0;

    LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
    {
        if (m == WM_PAINT)
        {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            // A gradient that shifts, a moving box and a counter: changes
            // everywhere some frames, in small parts on others.
            const int band = (g_tick / 30) % 4;
            for (int y = 0; y < rc.bottom; y += 8)
            {
                HBRUSH b = CreateSolidBrush(RGB((y * 255 / (rc.bottom + 1) + band * 40) & 255, 90 + band * 30, 200 - y * 150 / (rc.bottom + 1)));
                RECT r{ 0, y, rc.right, y + 8 }; FillRect(dc, &r, b); DeleteObject(b);
            }
            const int x = (int)((std::sin(g_tick * 0.05) * 0.4 + 0.5) * (rc.right - 120));
            HBRUSH bx = CreateSolidBrush(RGB(250, 250, 40));
            RECT box{ x, rc.bottom / 3, x + 120, rc.bottom / 3 + 90 }; FillRect(dc, &box, bx); DeleteObject(bx);
            wchar_t t[64]; swprintf(t, 64, L"captest frame %d", g_tick);
            SetBkMode(dc, TRANSPARENT); TextOutW(dc, 20, 20, t, (int)wcslen(t));
            EndPaint(h, &ps);
            return 0;
        }
        if (m == WM_ERASEBKGND) return 1;
        return DefWindowProcW(h, m, w, l);
    }

    void Pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

    // Bytes of a texture (whole, 8-bit RGBA / BGRA as it is), via a staging copy.
    std::vector<uint8_t> ReadTex(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT& w, UINT& h)
    {
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        w = d.Width; h = d.Height;
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        std::vector<uint8_t> out;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return out;
        ctx->CopyResource(st, tex);
        D3D11_MAPPED_SUBRESOURCE mm{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mm)))
        {
            out.resize((size_t)w * h * 4);
            for (UINT y = 0; y < h; ++y) memcpy(&out[(size_t)y * w * 4], (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, (size_t)w * 4);
            ctx->Unmap(st, 0);
        }
        st->Release();
        return out;
    }

    ID3D11Texture2D* TexOf(ID3D11ShaderResourceView* v)
    {
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        ID3D11Texture2D* t = nullptr; r->QueryInterface(&t); r->Release();
        return t;   // (caller releases)
    }

    struct Diff { int maxd = 0; size_t differ = 0, total = 0; };
    Diff Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        Diff d;
        if (a.size() != b.size()) { d.maxd = 999; return d; }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if ((i & 3) == 3) continue;   // (alpha)
            const int x = std::abs((int)a[i] - (int)b[i]);
            ++d.total; if (x) { ++d.differ; d.maxd = (std::max)(d.maxd, x); }
        }
        return d;
    }

    struct Stats { int checks = 0, frames = 0, directFrames = 0, rawBad = 0; int rawMax = 0; int convMax[3] = {}; size_t convDiffer[3] = {}, convTotal[3] = {}; };

    void RunTest(const char* name, ID3D11Device* dev, ID3D11DeviceContext* ctx, HWND anim, bool asWindow, int seconds)
    {
        Capture cap;
        cap.Initialize(dev, ctx);
        cap.SetZeroCopy(true);
        const bool ok = asWindow ? cap.StartWindow(anim) : cap.StartMonitor(MonitorFromWindow(anim, MONITOR_DEFAULTTOPRIMARY));
        if (!ok) { printf("%s: capture didn't start\n", name); return; }

        const StereoFormat fmts[3] = { StereoFormat::HalfSBS, StereoFormat::HalfTAB, StereoFormat::RowInterleaved };
        const char* fmtNames[3] = { "Half SBS", "Half TAB", "Row interleaved" };
        Converter convDirect[3], convCopy[3];
        for (int f = 0; f < 3; ++f)
        {
            convDirect[f].Initialize(dev, ctx); convCopy[f].Initialize(dev, ctx);
            convDirect[f].SetHalfWidthEyes(true); convCopy[f].SetHalfWidthEyes(true);
            convDirect[f].SetFormat(fmts[f], false, 0, 4); convCopy[f].SetFormat(fmts[f], false, 0, 4);
        }

        Stats s;
        const DWORD start = GetTickCount();
        bool resized = false;
        while (GetTickCount() - start < (DWORD)seconds * 1000)
        {
            ++g_tick;
            InvalidateRect(anim, nullptr, FALSE);
            Pump();
            // Halfway: resize the window (a new frame size; the pool is remade).
            if (!resized && GetTickCount() - start > (DWORD)seconds * 500)
            {
                SetWindowPos(anim, nullptr, 0, 0, 1000, 760, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                resized = true;
            }
            cap.WaitForNewFrame(20);
            bool sizeChanged = false;
            if (!cap.Update(sizeChanged)) continue;
            ++s.frames;
            // Every 4th new frame: check (the copy in between builds up from
            // several frames' changed parts, as it does in the app).
            if (s.frames % 4) continue;
            bool enc = false;
            ID3D11ShaderResourceView* direct = cap.DirectSRV(enc);
            if (!enc) continue;   // (not a zero-copy frame: e.g. the one after a resize)
            ++s.directFrames;
            ID3D11ShaderResourceView* copy = cap.SRV();
            ID3D11Texture2D* dt = TexOf(direct);
            UINT w1 = 0, h1 = 0, w2 = 0, h2 = 0;
            const auto a = ReadTex(dev, ctx, dt, w1, h1);
            const auto b = ReadTex(dev, ctx, cap.Texture(), w2, h2);
            dt->Release();
            const Diff raw = Compare(a, b);
            ++s.checks;
            if (raw.differ) { ++s.rawBad; s.rawMax = (std::max)(s.rawMax, raw.maxd); }
            for (int f = 0; f < 3; ++f)
            {
                bool rs = false;
                convDirect[f].SetSourceEncoded(true, &cap);
                const bool okA = convDirect[f].Convert(direct, cap.Width(), cap.Height(), rs);
                const bool okB = convCopy[f].Convert(copy, cap.Width(), cap.Height(), rs);
                if (getenv("CAPTEST_DEBUG") && s.checks == 1) printf("  [%d] convert direct %d copy %d\n", f, okA, okB);
                UINT ow = 0, oh = 0;
                ID3D11Texture2D* ta = TexOf(convDirect[f].OutputSRV());
                ID3D11Texture2D* tb = TexOf(convCopy[f].OutputSRV());
                const auto oa = ReadTex(dev, ctx, ta, ow, oh), ob = ReadTex(dev, ctx, tb, ow, oh);
                const Diff d = Compare(oa, ob);
                if (s.checks == 1 && getenv("CAPTEST_DEBUG"))
                {
                    D3D11_TEXTURE2D_DESC da{}, db{}; ta->GetDesc(&da); tb->GetDesc(&db);
                    printf("  [%d] out %ux%u fmt %d/%d; src %ux%u\n", f, ow, oh, (int)da.Format, (int)db.Format, w1, h1);
                    for (int k = 0; k < 4; ++k)
                    {
                        const size_t px = ((size_t)(oh / 2) * ow + ow / 8 + k * ow / 4) * 4;
                        printf("    px %zu: direct %3d %3d %3d %3d | copy %3d %3d %3d %3d\n", px / 4, oa[px], oa[px + 1], oa[px + 2], oa[px + 3], ob[px], ob[px + 1], ob[px + 2], ob[px + 3]);
                    }
                }
                ta->Release(); tb->Release();
                s.convMax[f] = (std::max)(s.convMax[f], d.maxd);
                s.convDiffer[f] += d.differ; s.convTotal[f] += d.total;
            }
        }
        printf("%s: %d new frames, %d checked (zero-copy), frame size %dx%d at the end\n", name, s.frames, s.checks, cap.Width(), cap.Height());
        printf("  captured frame vs the copy built from its changed parts: %s", s.rawBad ? "DIFFERENT" : "identical every time");
        if (s.rawBad) printf(" (%d of %d checks, max diff %d)", s.rawBad, s.checks, s.rawMax);
        printf("\n");
        for (int f = 0; f < 3; ++f)
            printf("  %-16s converted from the frame vs from the copy: max diff %d, %.4f%% of values differ\n",
                   fmtNames[f], s.convMax[f], s.convTotal[f] ? 100.0 * s.convDiffer[f] / s.convTotal[f] : 0.0);
        for (int f = 0; f < 3; ++f) { convDirect[f].Shutdown(); convCopy[f].Shutdown(); }
        cap.Shutdown();
    }
}

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? (std::max)(2, atoi(argv[1])) : 6;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | (getenv("CAPTEST_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0),
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { printf("no D3D11 device\n"); return 1; }

    // (CAPTEST_DEBUG: the D3D debug layer's messages, printed after each test.)
    ID3D11InfoQueue* iq = nullptr;
    if (getenv("CAPTEST_DEBUG")) dev->QueryInterface(&iq);
    auto dumpMessages = [&]() {
        if (!iq) return;
        const UINT64 n = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < n && i < 12; ++i)
        {
            SIZE_T len = 0; iq->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len); auto* m = (D3D11_MESSAGE*)buf.data();
            if (SUCCEEDED(iq->GetMessage(i, m, &len))) printf("  d3d: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
        }
        if (n) printf("  d3d: %llu messages\n", n);
        iq->ClearStoredMessages();
    };
    WNDCLASSW wc{}; wc.lpfnWndProc = TestWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SRLoomCapTest"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"SR Loom capture test", WS_OVERLAPPEDWINDOW,
                               120, 120, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    UpdateWindow(wnd);
    Pump();

    RunTest("Window (the test window only)", dev, ctx, wnd, true, seconds);
    dumpMessages();
    SetWindowPos(wnd, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RunTest("Whole monitor", dev, ctx, wnd, false, seconds);
    dumpMessages();
    DestroyWindow(wnd);
    ctx->Release(); dev->Release();
    return 0;
}
