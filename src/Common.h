// Common.h — shared types, enums and small helpers for SR Weaver.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// Release a COM pointer and null it.
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

namespace srw
{
    // Current SR Loom version. Compared (after stripping any leading 'v') to
    // the GitHub Releases latest-tag by the update checker. Bump in lockstep
    // with the git tag for new releases.
    constexpr const char* kAppVersion = "1.6";

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

    // Append a line to srweaver.log (next to the exe) and the debugger output.
    inline void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
        va_end(ap);
        ::OutputDebugStringA(buf);
        ::OutputDebugStringA("\n");
        FILE* f = nullptr;
        if (fopen_s(&f, ExePath("srweaver.log").c_str(), "a") == 0 && f)
        {
            fputs(buf, f);
            fputc('\n', f);
            fclose(f);
        }
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
            // StereoFormat::Katanga intentionally omitted -- the feature
            // is implemented end-to-end but parked from the UI until the
            // remaining UX issues (cursor on weave, focus/Z-pin via
            // publisher discovery) are properly sorted. Re-add this entry
            // to expose it again.
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
            "Green / Magenta", "Amber / Blue", "Cyan / Magenta",
        };
        count = (int)(sizeof(combos) / sizeof(combos[0]));
        return combos;
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
