// Common.h — shared types, enums and small helpers for SR Weaver.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <share.h>
#include <intrin.h>

// Release a COM pointer and null it.
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

namespace srw
{
    // Current SR Loom version. Compared (after stripping any leading 'v') to
    // the GitHub Releases latest-tag by the update checker. Bump in lockstep
    // with the git tag for new releases.
    constexpr const char* kAppVersion = "3.1";

    // GitHub repo path for the update checker + "About" links.
    constexpr const char* kRepoSlug = "effcol/SR-Loom";

    // How the woven output window is presented.
    enum class OutputMode
    {
        Fullscreen,     // borderless window covering the SR display
        Windowed,       // normal resizable window
        WindowOverlay,  // borderless click-through overlay tracking a source window
        LookingGlass    // movable see-through loupe weaving the screen beneath it
    };

    // What the weaver is currently weaving. Katanga used to live here as a
    // 4th source kind; it moved to StereoFormat (an "external stereo input"
    // format) so it composes with any source/placement.
    enum class SourceKind
    {
        TestImage,        // bundled static SBS image
        CaptureMonitor,   // live capture of the SR monitor
        CaptureWindow     // live capture of a chosen window
    };

    // Stereo layout of the SOURCE that we convert into the side-by-side
    // texture the weaver consumes. Milestone 1 implements FullSBS / HalfSBS;
    // the rest are placeholders for upcoming conversion shaders.
    enum class StereoFormat
    {
        FullSBS,            // left | right, each full width
        HalfSBS,            // left | right, each squished to half width
        FullTAB,            // left over right, each full height
        HalfTAB,            // left over right, each squished to half height
        Anaglyph,           // colour-encoded (red/cyan, etc.)
        RowInterleaved,     // alternating scanlines (a.k.a. line interleaved)
        ColumnInterleaved,  // alternating columns
        Checkerboard,       // quincunx
        FrameSequential,    // alternating frames over time
        Pulfrich,           // mono source -> depth via per-eye delay or ND darkening
        FramePacking,       // HDMI 1.4: top eye, blanking gap, bottom eye
        Quilt,              // Looking Glass quilt: cols x rows grid of views; pick a pair as L/R
        VR180TAB,           // 180° equirectangular hemisphere, L over R packing
        VR180SBS,           // 180° equirectangular hemisphere, L | R packing
        VR360TAB,           // 360° equirectangular sphere, L over R packing
        VR360SBS,           // 360° equirectangular sphere, L | R packing
        Katanga,            // external SBS handed over by Bo3b's Katanga shared
                            // texture (Geo-11 stereo game mods etc.). Bypasses the
                            // Source's captured pixels -- the Source only governs
                            // placement (which window/display the weave sits on).
        LightField          // Lytro plenoptic (.lfp/.lfr/.lfx). LFPRenderer samples
                            // per output pixel from the user's actual eye aperture
                            // position each frame. Auto-set when an LFP loads.
    };

    inline bool IsVRFormat(StereoFormat f)
    {
        return f == StereoFormat::VR180TAB || f == StereoFormat::VR180SBS
            || f == StereoFormat::VR360TAB || f == StereoFormat::VR360SBS;
    }
    // True if the format gets its SBS from an external publisher (shared
    // texture / IPC) rather than the Source's captured pixels. Today this
    // is just Katanga; future external feeds (OpenXR mirror, virtual
    // display, etc.) would slot in alongside it.
    inline bool IsExternalSourceFormat(StereoFormat f)
    {
        return f == StereoFormat::Katanga;
    }
    inline bool IsVR360(StereoFormat f)
    {
        return f == StereoFormat::VR360TAB || f == StereoFormat::VR360SBS;
    }
    inline bool IsVRSBS(StereoFormat f)
    {
        return f == StereoFormat::VR180SBS || f == StereoFormat::VR360SBS;
    }

    // HDMI 1.4 frame packing. The 720p (1280x1470) and 1080p (1920x2205) variants
    // share the same proportions (eye 48.98%, gap 2.04% of the squeezed capture
    // height) so a single preset covers both. eyeFrac/gapFrac are fractions of
    // the captured height; eyeAlign is a residual vertical shift in source pixels
    // for the bottom eye to correct capture-pipeline rounding (default 0 — works
    // out of the box for most capture devices).
    struct FramePackPreset { const char* label; float eyeFrac; float gapFrac; float eyeAlign; };
    inline const FramePackPreset* FramePackPresets(int& count)
    {
        static const FramePackPreset presets[] = {
            { "HDMI 1.4 Frame Packing", 1080.0f / 2205.0f, 45.0f / 2205.0f, 0.0f },
        };
        count = (int)(sizeof(presets) / sizeof(presets[0]));
        return presets;
    }

    // Pulfrich: how the affected eye is treated.
    enum class PulfrichMode { TimeDelay, NDFilter };

    // Affected-eye ND transmission presets (fraction of brightness kept).
    // ~0.30 ≈ 2 stops ≈ a ~9 ms perceived delay (15 ms per factor-of-10), a good
    // balance of depth vs. darkness; the other eye stays full so the fused image
    // doesn't look dim.
    struct NdLevel { const char* label; float transmission; };
    inline const NdLevel* PulfrichNdLevels(int& count)
    {
        static const NdLevel levels[] = {
            { "Light (~1 stop)",  0.50f },
            { "Medium (~2 stop)", 0.30f },   // default
            { "Strong (~3 stop)", 0.15f },
        };
        count = (int)(sizeof(levels) / sizeof(levels[0]));
        return levels;
    }

    // Resolve a filename to an absolute path next to the executable (so the app
    // doesn't depend on the current working directory).
    inline std::string ExePath(const char* filename)
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, (DWORD)ARRAYSIZE(buf));
        char* slash = strrchr(buf, '\\');
        if (slash) *(slash + 1) = '\0';
        std::string path(buf);
        path += filename;
        return path;
    }

    // srweaver.log writer. Log() only queues the line; a background thread
    // appends it to the file, which stays open. Opening, writing and closing
    // the file on the caller's thread (as Log used to) took 10-30 ms now and
    // then -- antivirus scanning each write -- and showed up as render-thread
    // hitches in whatever section happened to log something.
    class LogWriter
    {
    public:
        // Never destroyed, so it still works while the process exits.
        static LogWriter& Get() { static LogWriter* w = new LogWriter(); return *w; }

        void Push(const char* line)
        {
            {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                m_queue.emplace_back(line);
                if (!m_started)
                {
                    m_started = true;
                    std::thread([this] { Run(); }).detach();
                    std::atexit([] { LogWriter::Get().Flush(); });
                }
            }
            m_cv.notify_one();
        }

        // Write out whatever is queued, on the calling thread. (Gives up if the
        // writer thread stays stuck mid-write -- e.g. it's the one crashing.)
        void Flush()
        {
            std::unique_lock<std::mutex> fk(m_fileMutex, std::try_to_lock);
            for (int i = 0; i < 20 && !fk.owns_lock(); ++i) { Sleep(5); (void)fk.try_lock(); }
            if (fk.owns_lock()) WriteQueued();
        }

    private:
        void Run()
        {
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lk(m_queueMutex);
                    m_cv.wait(lk, [this] { return !m_queue.empty(); });
                }
                std::lock_guard<std::mutex> fk(m_fileMutex);
                WriteQueued();
            }
        }

        // (Holding m_fileMutex, so lines go out in order.)
        void WriteQueued()
        {
            std::deque<std::string> batch;
            {
                std::lock_guard<std::mutex> lk(m_queueMutex);
                batch.swap(m_queue);
            }
            if (batch.empty()) return;
            if (!m_file) m_file = _fsopen(ExePath("srweaver.log").c_str(), "a", _SH_DENYNO);   // (readable while open)
            if (!m_file) return;
            for (const std::string& s : batch) { fputs(s.c_str(), m_file); fputc('\n', m_file); }
            fflush(m_file);
        }

        std::mutex              m_queueMutex, m_fileMutex;
        std::condition_variable m_cv;
        std::deque<std::string> m_queue;
        bool                    m_started = false;
        FILE*                   m_file = nullptr;
    };

    // Append a line to srweaver.log (next to the exe) and the debugger output.
    inline void Log(const char* fmt, ...)
    {
        // "[hh:mm:ss.mmm] " first: how long things take (start-up, stalls).
        char buf[1024];
        SYSTEMTIME st;
        GetLocalTime(&st);
        const int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
        ::OutputDebugStringA(buf);
        ::OutputDebugStringA("\n");
        LogWriter::Get().Push(buf);
    }

    // Write out every queued log line now (crash handler).
    inline void LogFlush() { LogWriter::Get().Flush(); }

    // CPU time the calling thread has actually run, in ms. Next to wall time
    // it tells a slow section that was WORKING from one that was WAITING (on
    // the GPU queue, a lock, the scheduler). Thread cycle counts tick at the
    // TSC rate, calibrated against QPC since the first call (made at startup).
    inline double TscPerMs()
    {
        static const unsigned long long tsc0 = __rdtsc();
        static const LARGE_INTEGER qpc0 = [] { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q; }();
        LARGE_INTEGER qpc{}, qpf{};
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&qpf);
        const double ms = (double)(qpc.QuadPart - qpc0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        return ms > 100.0 ? (double)(__rdtsc() - tsc0) / ms : 3.0e6;   // (a rough 3 GHz until calibrated)
    }
    inline double ThreadCpuMs()
    {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        return (double)cycles / TscPerMs();
    }

    // A window message that took a while to handle, logged with where it came
    // from: part of the render thread's hitch watchdog. sendFlags is
    // InSendMessageEx() taken as the message arrived.
    inline void LogSlowMessage(const char* window, UINT msg, WPARAM wp, DWORD sendFlags,
                               std::chrono::steady_clock::time_point t0)
    {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms <= 15.0) return;
        const char* from = (sendFlags & ISMEX_SEND)   ? "sent by another thread or program"
                         : (sendFlags & ISMEX_NOTIFY) ? "notification from another thread"
                         : "from SR Loom's own thread";
        Log("Hitch: %s window message 0x%04X (wParam 0x%llX), %s, took %.1f ms",
            window, msg, (unsigned long long)wp, from, ms);
    }

    // The selectable source stereo layouts, shared by the tray menu and command
    // handling so their order stays in sync. (Anaglyph has its own submenu.)
    struct StereoFormatEntry { StereoFormat fmt; const char* label; };
    inline const StereoFormatEntry* StereoFormatList(int& count)
    {
        static const StereoFormatEntry list[] = {
            { StereoFormat::FullSBS,           "Side-by-Side (full)" },
            { StereoFormat::HalfSBS,           "Side-by-Side (half)" },
            { StereoFormat::FullTAB,           "Top-and-Bottom (full)" },
            { StereoFormat::HalfTAB,           "Top-and-Bottom (half)" },
            { StereoFormat::RowInterleaved,    "Row interleaved" },
            { StereoFormat::ColumnInterleaved, "Column interleaved" },
            { StereoFormat::Checkerboard,      "Checkerboard" },
            { StereoFormat::FrameSequential,   "Frame sequential" },
            { StereoFormat::Pulfrich,          "Pulfrich" },
            { StereoFormat::FramePacking,      "Frame packing" },
            { StereoFormat::Quilt,             "Quilt" },
            { StereoFormat::VR180TAB,          "VR180 (Top-and-Bottom)" },
            { StereoFormat::VR180SBS,          "VR180 (Side-by-Side)" },
            { StereoFormat::VR360TAB,          "VR360 (Top-and-Bottom)" },
            { StereoFormat::VR360SBS,          "VR360 (Side-by-Side)" },
            // Katanga: receive frames a game / bridge publishes over the
            // Katanga shared-texture protocol. Selecting it listens actively;
            // the "Katanga Receiver" toggle (auto-receive) listens passively
            // and switches here on its own when a sender appears.
            { StereoFormat::Katanga,           "Katanga" },
            { StereoFormat::LightField,        "Lytro Light Field" },
        };
        count = (int)(sizeof(list) / sizeof(list[0]));
        return list;
    }

    // Anaglyph colour combinations (which channels carry left vs right).
    inline const char* const* AnaglyphComboList(int& count)
    {
        static const char* const combos[] = {
            "Red / Cyan", "Red / Green", "Red / Blue",
            "Green / Magenta", "Amber / Blue", "Cyan / Magenta", "Custom",
        };
        count = (int)(sizeof(combos) / sizeof(combos[0]));
        return combos;
    }

    constexpr int kAnaComboCustom = 6;   // (AnaglyphComboList's last entry: the two colours are picked)
    // A custom anaglyph pair, from the two filter colours picked (only their hue
    // and strength of colour count: both are taken at full brightness).
    // An anaglyph made with filter colours fl and fr holds, in each pixel,
    // L x fl + R x fr -- L and R the two eyes' brightness. So each eye's
    // brightness is worked back out by least squares: L = wl . pixel, R = wr .
    // pixel (for red / cyan exactly red, and the mean of green and blue, as the
    // preset). Any two different colours work, and turning one changes the
    // result smoothly. tl / tr: the colours themselves (Filtered shows each
    // eye in its own). ml / mr: how much of each channel is each eye's own,
    // for Recovered Colour (what one has more of than the other).
    // (cl / cr: the one channel each eye's view is matched on in Recovered Colour --
    // the one most its own, green first on a tie, as the presets do: a blend of
    // channels matched worse.)
    struct AnaCustom { float wl[3], wr[3], tl[3], tr[3], ml[3], mr[3]; int cl = 0, cr = 1; };
    inline AnaCustom AnaCustomFromColours(const float l[3], const float r[3])
    {
        AnaCustom k{};
        const float lm = (std::max)((std::max)(l[0], l[1]), (std::max)(l[2], 1e-4f));
        const float rm = (std::max)((std::max)(r[0], r[1]), (std::max)(r[2], 1e-4f));
        float fl[3], fr[3];
        for (int c = 0; c < 3; ++c) { fl[c] = l[c] / lm; fr[c] = r[c] / rm; }
        float a = 0, b = 0, d = 0;
        for (int c = 0; c < 3; ++c) { a += fl[c] * fl[c]; b += fl[c] * fr[c]; d += fr[c] * fr[c]; }
        const float det = a * d - b * b;
        if (det < 0.05f)   // (the same colour twice, near enough: nothing to tell apart -- red / cyan)
        {
            const float L[3] = { 1, 0, 0 }, R[3] = { 0, 1, 1 };
            return AnaCustomFromColours(L, R);
        }
        for (int c = 0; c < 3; ++c)
        {
            k.wl[c] = (d * fl[c] - b * fr[c]) / det;
            k.wr[c] = (a * fr[c] - b * fl[c]) / det;
            k.tl[c] = fl[c]; k.tr[c] = fr[c];
            k.ml[c] = (std::max)(0.0f, fl[c] - fr[c]);
            k.mr[c] = (std::max)(0.0f, fr[c] - fl[c]);
        }
        auto pick = [](const float m[3]) { const int order[3] = { 1, 0, 2 }; int best = 1; for (int c : order) if (m[c] > m[best] + 1e-3f) best = c; return best; };
        k.cl = pick(k.ml); k.cr = pick(k.mr);
        return k;
    }

    // How each eye is reconstructed from the anaglyph. Listed in DISPLAY order, but
    // each carries its shader mode VALUE so the menu order is decoupled from the
    // shader's g_anaMode numbering (0 shared, 1 filtered, 2 half, 3 mono, 4 recovery).
    struct AnaglyphModeEntry { const char* label; int value; };
    inline const AnaglyphModeEntry* AnaglyphModeList(int& count)
    {
        static const AnaglyphModeEntry modes[] = {
            { "Recovered Colour",          4 },   // multi-scale disparity recovery (default, top)
            { "DeAnaglyph",                0 },   // per-eye luminance + shared anaglyph chroma
            { "Filtered Colour",           1 },   // each eye keeps only its own channels
            { "Half Colour",               2 },   // half saturation, full per-eye brightness
            { "Monochrome (Black & White)", 3 },
        };
        count = (int)(sizeof(modes) / sizeof(modes[0]));
        return modes;
    }

    // Index of a format within StereoFormatList (-1 if absent).
    inline int StereoFormatIndex(StereoFormat f)
    {
        int n = 0; const StereoFormatEntry* l = StereoFormatList(n);
        for (int i = 0; i < n; ++i) if (l[i].fmt == f) return i;
        return -1;
    }

    // Parse a Looking Glass quilt grid out of a filename. The LG naming convention
    // embeds the grid as "_qsCxR" or "_qsCxR_" before the extension (e.g.
    // "Foo_qs8x6_Final.png" -> 8x6). Returns true and fills cols/rows on a match.
    inline bool ParseQuiltDims(const char* path, int& cols, int& rows)
    {
        if (!path) return false;
        const char* p = path;
        while (*p) ++p;                                // end
        const char* end = p;
        // Search the filename for "_qs<digits>x<digits>" (case-insensitive on qs).
        for (const char* s = path; s + 4 < end; ++s)
        {
            if (s[0] != '_') continue;
            if ((s[1] != 'q' && s[1] != 'Q') || (s[2] != 's' && s[2] != 'S')) continue;
            const char* d = s + 3;
            int c = 0, r = 0; bool gotC = false, gotR = false;
            while (*d >= '0' && *d <= '9') { c = c * 10 + (*d++ - '0'); gotC = true; }
            if (!gotC || (*d != 'x' && *d != 'X')) continue;
            ++d;
            while (*d >= '0' && *d <= '9') { r = r * 10 + (*d++ - '0'); gotR = true; }
            if (!gotR || c <= 0 || r <= 0) continue;
            cols = c; rows = r;
            return true;
        }
        return false;
    }

    // Show a modal error box (used for unrecoverable startup failures).
    inline void ShowError(const char* msg)
    {
        ::MessageBoxA(nullptr, msg, "SR Loom", MB_ICONERROR | MB_OK);
#ifdef _DEBUG
        ::OutputDebugStringA(msg);
        ::OutputDebugStringA("\n");
#endif
    }
}
