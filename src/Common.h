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
    // How the woven output window is presented.
    enum class OutputMode
    {
        Fullscreen,     // borderless window covering the SR display
        Windowed,       // normal resizable window
        WindowOverlay,  // borderless click-through overlay tracking a source window
        LookingGlass    // movable see-through loupe weaving the screen beneath it
    };

    // What the weaver is currently weaving.
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
        FramePacking        // HDMI 1.4: top eye, blanking gap, bottom eye
    };

    // HDMI 1.4 frame-packing presets. 720p and 1080p share the same proportions
    // (each eye 48.98% of height, ~2.04% blanking gap), so the decode is identical;
    // the selector is offered for clarity. eyeFrac/gapFrac are fractions of height.
    struct FramePackPreset { const char* label; float eyeFrac; float gapFrac; };
    inline const FramePackPreset* FramePackPresets(int& count)
    {
        static const FramePackPreset presets[] = {
            { "1080p (1920x2205, 45-line gap)", 1080.0f / 2205.0f, 45.0f / 2205.0f },
            { "720p (1280x1470, 30-line gap)",   720.0f / 1470.0f, 30.0f / 1470.0f },
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

    // How each eye is reconstructed from the anaglyph. Colour by default.
    inline const char* const* AnaglyphModeList(int& count)
    {
        static const char* const modes[] = {
            "Recovered colour",              // per-eye luminance + shared, de-fringed chroma (default)
            "Colour (filtered)",             // each eye keeps only its own channels
            "Half colour",                   // blend toward grey
            "Mono (black & white)",
            "DeAnaglyph",                    // multi-scale disparity-aligned colour recovery (red/cyan)
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

    // Show a modal error box (used for unrecoverable startup failures).
    inline void ShowError(const char* msg)
    {
        ::MessageBoxA(nullptr, msg, "SR Weaver", MB_ICONERROR | MB_OK);
#ifdef _DEBUG
        ::OutputDebugStringA(msg);
        ::OutputDebugStringA("\n");
#endif
    }
}
