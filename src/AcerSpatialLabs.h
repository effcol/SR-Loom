// AcerSpatialLabs.h — toggle the registry entries Acer's SpatialLabs runtime
// reads for Focus Detection, Fullscreen Detection, and Monitor Detection. When
// SR Loom is driving the weave, those auto-detect features tend to fight it
// (auto-switching 2D/3D based on focus, or auto-engaging on fullscreen apps),
// so giving the user a one-click way to disable them from our panel removes a
// whole class of "why is my 3D flickering" reports on Acer hardware.
//
// Adapted from the same author's Stereopticon (modules/displaySettings.js),
// relicensed MIT for this project. The mechanism: three DWORDs under
// HKLM\SOFTWARE\Acer\SpatialLabs (1 = on, 0 = off). Writing HKLM needs admin.
#pragma once

#include "Common.h"

namespace srw::AcerSpatialLabs
{
    struct State
    {
        int focusDetection      = -1;   // -1 = key/value not present; else 0 or 1
        int fullscreenDetection = -1;
        int monitorDetection    = -1;
    };

    enum class Setting       { Focus, Fullscreen, Monitor };
    enum class WriteResult   { OK, NeedsAdmin, Failed };

    bool        Available();                       // the SpatialLabs registry key exists
    State       Read();                            // returns -1 for any missing value
    WriteResult Write(Setting setting, bool on);   // HKLM write (admin required)
}
