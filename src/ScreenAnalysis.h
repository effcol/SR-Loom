// ScreenAnalysis.h -- Auto Stereo's view of the screen.
//
// ScreenAnalyzer: each frame, draws the SR display capture into a
// half-resolution luma (R8) texture and reads it back through a small ring
// of staging textures (non-blocking), giving the CPU a fresh grey image a
// frame or two behind the screen. On request (a detection scan) it also reads
// back the red and cyan ((G+B)/2) channels, which is what an anaglyph is made
// of. Nothing is stored beyond the latest frame; nothing leaves the process.
//
// The pure-CPU helpers below work on that image (analysis pixels = capture
// pixels / Scale()):
//   ImageRectFinder -- the rectangle of image content (photo / video / picture
//                      on a web page) around a point, bounded by long straight
//                      edges on all four sides. Prepare once per window, then
//                      Find() from as many points as needed.
//   ClassifyStereo  -- is the content of a rectangle a stereo image, and in
//                      which format (anaglyph, side-by-side, top-and-bottom)?
//   RegionTracker   -- follows an image region as the page scrolls (row/column
//                      brightness profiles, mostly-vertical search, verified by
//                      a sparse spot-check; reports "lost"), and learns the
//                      page's scrolling viewport so the part of an image that
//                      has scrolled under a toolbar isn't woven.
//   StereoScanner   -- background thread: scans windows for stereo images.
#pragma once

#include "Common.h"
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srw
{
    // A grey image in analysis pixels (one byte per pixel, tightly packed),
    // optionally with the red and cyan channels alongside (detection scans).
    struct LumaImage
    {
        int                  width  = 0;
        int                  height = 0;
        std::vector<uint8_t> pixels;
        std::vector<uint8_t> red, cyan;   // empty unless read back with colour
        uint8_t at(int x, int y) const { return pixels[(size_t)y * width + x]; }
        bool    empty() const { return width <= 0 || height <= 0; }
        bool    hasColour() const { return !red.empty() && red.size() == pixels.size(); }
    };

    class ScreenAnalyzer
    {
    public:
        ~ScreenAnalyzer();
        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();

        // Queue the current capture frame (full frame, sRGB view) for readback.
        // withColour: also read back red/cyan for this frame.
        void Submit(ID3D11ShaderResourceView* src, int srcW, int srcH, bool withColour = false);
        // Newest completed readback, if one arrived since the last call.
        // waitForNewest: block until the frame just submitted is read back
        // (no frame of lag -- used while tracking a scrolling image).
        bool Latest(LumaImage& out, uint64_t& frameId, bool waitForNewest = false);

        static constexpr int Scale() { return 2; }   // capture px per analysis px
        // This frame's luma image, still on the GPU (after Submit).
        ID3D11ShaderResourceView* LumaSRV() const { return m_srv; }

    private:
        static constexpr int kRing = 3;
        bool Ensure(int w, int h);
        void Release();

        ID3D11Device*            m_device  = nullptr;
        ID3D11DeviceContext*     m_context = nullptr;
        ID3D11VertexShader*      m_vs = nullptr;
        ID3D11PixelShader*       m_ps = nullptr;       // luma + red/cyan (two targets)
        ID3D11PixelShader*       m_psLuma = nullptr;   // luma only
        ID3D11SamplerState*      m_sampler = nullptr;
        ID3D11Texture2D*         m_rt = nullptr;            // luma (R8)
        ID3D11RenderTargetView*  m_rtv = nullptr;
        ID3D11ShaderResourceView* m_srv = nullptr;          // luma, for GPU tracking
        ID3D11Texture2D*         m_rtC = nullptr;           // red, cyan (R8G8)
        ID3D11RenderTargetView*  m_rtvC = nullptr;
        ID3D11Texture2D*         m_staging[kRing] = {};
        ID3D11Texture2D*         m_stagingC[kRing] = {};
        bool                     m_slotColour[kRing] = {};
        uint64_t                 m_slotFrame[kRing] = {};   // 0 = empty
        int                      m_w = 0, m_h = 0;
        uint64_t                 m_submitted = 0;           // frames submitted so far
        uint64_t                 m_lastRead  = 0;           // newest frame returned
        int                      m_next = 0;
    };

    // Rectangle of image content around a point, within a window's bounds
    // (analysis px). Prepare() builds the edge / flat maps for the bounds once;
    // Find() is then cheap enough to call from many seed points.
    class ImageRectFinder
    {
    public:
        bool Prepare(const LumaImage& img, const RECT& bounds);
        // False if no convincing 4-sided rectangle is found around (px,py).
        // flatSides (optional): how many sides have page background outside and
        // picture inside (window edges count) -- 3-4 = a confident image rect.
        bool Find(int px, int py, RECT& out, int* flatSides = nullptr) const;
        // Flat ("page background") fraction of a rectangle (analysis px).
        float FlatFraction(const RECT& r) const;
        const RECT& Bounds() const { return m_b; }

    private:
        const LumaImage*      m_img = nullptr;
        RECT                  m_b{};
        int                   m_bw = 0, m_bh = 0;
        std::vector<uint8_t>  m_ev, m_eh;           // raw edge maps (local coords)
        std::vector<uint32_t> m_colSum, m_rowSum;   // widened-edge running totals
        std::vector<uint32_t> m_flatSum, m_lumaSum; // integral images
        bool                  m_pageLuma[256] = {}; // the page's background colours
    };

    // One-shot convenience wrapper.
    bool FindImageRect(const LumaImage& img, int px, int py, const RECT& bounds, RECT& out);

    // Is the rectangle's content a stereo image? Returns true with the format
    // (Anaglyph needs img.hasColour()). `score` is the winning test's
    // strength; `diag` (optional) receives a one-line breakdown for the log.
    // Raw half-vs-half match strengths (for a manual pick that the tests
    // above found inconclusive).
    struct StereoScores { float sbs = 0.0f, tab = 0.0f; };
    bool ClassifyStereo(const LumaImage& img, const RECT& r, StereoFormat& format, float& score,
                        char* diag = nullptr, size_t diagLen = 0, StereoScores* scores = nullptr);

    // Debug aid (Ctrl+Alt+Shift+A only): write the analysed grey image as an
    // 8-bit BMP with the search bounds, the found rectangle and the pick point
    // drawn on it. Stays on the local disk; nothing is kept otherwise.
    bool SaveLumaDebugBmp(const LumaImage& img, const RECT& bounds, const RECT* found,
                          POINT pick, const wchar_t* path, bool annotate = true);
    // Same, as a 24-bit BMP of the red / cyan channels (if the image has them).
    bool SaveColourDebugBmp(const LumaImage& img, const wchar_t* path);

    class RegionTracker
    {
    public:
        // Capture a fingerprint of `rect` (analysis px) from `img`.
        void Reset(const LumaImage& img, const RECT& rect);
        // The window area the image lives in (analysis px); call before
        // Track() each frame. Empty = the whole image.
        void SetViewport(const RECT& outer);
        // Locate the region in a new frame. Updates Rect() and returns true if
        // found; false = lost (scrolled off / page changed).
        bool Track(const LumaImage& img);
        const RECT& Rect() const { return m_rect; }
        const std::vector<float>& RowProfile() const { return m_rowProfile; }
        const std::vector<float>& ColProfile() const { return m_colProfile; }
        // The part of Rect() actually on show: inside the viewport and below /
        // above any toolbar or sticky header the page scrolls under.
        RECT Visible() const;
        bool Valid() const { return m_valid; }
        // The viewport the image is clipped to (analysis px).
        RECT ViewRect() const { LumaImage d; d.width = m_imgW; d.height = m_imgH; return View(d); }
        // Its window moved by (dx,dy) analysis px: move the expected position
        // with it (the search then only has to find the rest).
        void Shift(int dx, int dy) { OffsetRect(&m_rect, dx, dy); m_prevRows.clear(); }
        // Recent scroll speed (analysis px per tracked frame, smoothed).
        float VelocityX() const { return m_vx; }
        float VelocityY() const { return m_vy; }

    private:
        void Fingerprint(const LumaImage& img, const RECT& r);
        float SpotCheck(const LumaImage& img, int dx, int dy) const;
        void CaptureSurround(const LumaImage& img, const RECT& r);
        bool SurroundStillMatches(const LumaImage& img, const RECT& r) const;
        RECT View(const LumaImage& img) const;
        void LearnViewport(const LumaImage& img, int dy);
        void RememberRows(const LumaImage& img);

        // The page just outside each side (left, top, right, bottom): mean
        // luma where that strip was flat background at Reset. A playing video
        // changes its content every frame, but its surround stays put -- so
        // "content differs, surround matches" means "still here", not "lost".
        float              m_surLuma[4] = {};
        bool               m_surValid[4] = {};

        RECT               m_rect{};
        bool               m_valid = false;
        std::vector<float> m_rowProfile;   // mean luma per row of the region
        std::vector<float> m_colProfile;   // mean luma per column
        std::vector<uint8_t> m_spots;      // sparse grid samples for verification
        int                m_spotsX = 0, m_spotsY = 0;

        // Viewport: the window area (from the caller) minus what the page
        // scrolls under, learned from scrolling itself -- rows that stay put
        // while the image moves are a toolbar / header / input box.
        RECT               m_outer{};
        int                m_insetTop = 0, m_insetBottom = 0;
        int                m_imgW = 0, m_imgH = 0;
        std::vector<float> m_prevRows;     // last frame's row means over the region's columns
        int                m_prevX0 = 0, m_prevX1 = 0;
        float              m_vx = 0.0f, m_vy = 0.0f;   // smoothed shift per frame
    };

    // Background stereo-image scanner. The caller hands it a frame (with
    // colour) and the windows to look in; it finds image rectangles from a
    // grid of seed points, classifies each, and reports the stereo ones.
    struct ScanWindow
    {
        HWND host = nullptr;          // top-level window
        HWND view = nullptr;          // viewport window (child or host)
        RECT bounds{};                // analysis px (visible part of the viewport)
        std::vector<RECT> covered;    // parts hidden by windows above (analysis px)
    };
    struct ScanHit
    {
        HWND         host = nullptr, view = nullptr;
        RECT         rect{};          // analysis px
        StereoFormat format = StereoFormat::HalfSBS;
        float        score = 0.0f;
        bool         whole = false;   // the whole viewport (a player / fullscreen video)
    };
    struct ScanVerify             // an existing auto-detected region to re-check
    {
        int          id = 0;
        RECT         rect{};          // analysis px
        StereoFormat format = StereoFormat::HalfSBS;
    };
    struct ScanResult
    {
        uint64_t                          frameId = 0;
        std::shared_ptr<const LumaImage>  image;    // the frame the rects refer to
        std::vector<ScanHit>              hits;
        std::vector<std::pair<int, bool>> verified; // ScanVerify id -> still 3D in that format
        std::vector<std::string>          log;      // classifier diagnostics
        double                            ms = 0.0;
    };

    class StereoScanner
    {
    public:
        ~StereoScanner();
        bool Busy() const { return m_busy.load(); }
        // Start scanning (non-blocking; ignored if a scan is running).
        // `exclude`: rects not to look in (existing / suppressed regions).
        bool Start(std::shared_ptr<const LumaImage> img, uint64_t frameId,
                   std::vector<ScanWindow> windows, std::vector<RECT> exclude,
                   std::vector<ScanVerify> verify = {});
        // The finished scan's result, once.
        bool Take(ScanResult& out);

        // The scan itself (synchronous; used by the thread and offline tests).
        // Verdicts of pictures already judged, keyed by place + a content
        // sample: an unchanged picture isn't judged again next scan.
        struct CachedVerdict
        {
            RECT         rect{};
            uint64_t     hash = 0;
            bool         stereo = false;
            StereoFormat format = StereoFormat::HalfSBS;
            float        score = 0.0f;
            std::string  diag;
        };
        static void Scan(const LumaImage& img, const std::vector<ScanWindow>& windows,
                         const std::vector<RECT>& exclude, ScanResult& out,
                         const std::vector<ScanVerify>& verify = {},
                         std::vector<CachedVerdict>* cache = nullptr);

    private:
        std::thread        m_thread;
        std::atomic<bool>  m_busy{ false };
        std::mutex         m_mutex;
        bool               m_ready = false;
        std::vector<CachedVerdict> m_cache;   // used by the scan thread only
        ScanResult         m_result;
    };
}
