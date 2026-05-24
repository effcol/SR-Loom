// Common.h — shared types, enums and small helpers for SR Weaver.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>

// Release a COM pointer and null it.
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

namespace srw
{
    // How the woven output window is presented.
    enum class OutputMode
    {
        Fullscreen,   // borderless window covering the SR display
        Windowed      // normal resizable window
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
        FrameSequential     // alternating frames over time
    };

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
