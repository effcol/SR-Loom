// AcerSpatialLabs.h — surface the handful of HKLM registry switches that are
// most useful to tweak alongside SR Loom on an Acer SpatialLabs display.
//
// Focus Detection is Acer's own auto-detect (HKLM\SOFTWARE\Acer\SpatialLabs)
// -- when on, Acer's runtime turns weaving off whenever the user looks away,
// which fights SR Loom; the toggle lets the user disable it from our panel.
//
// CameraPopup is the Windows-level "Your camera is in use" notification that
// fires every time the SR display's eye-tracking camera engages. SR displays
// have a hardware LED indicator for the camera, so the OS popup is redundant
// and noisy. The toggle writes to HKLM\SOFTWARE\Microsoft\OEM\Device\Capture
// \NoPhysicalCameraLED (DWORD; 0 = "display HAS an LED, no popup needed";
// 1 = "no LED, show popup" -- the Windows default).
//
// Adapted from the same author's Stereopticon (modules/displaySettings.js),
// relicensed MIT for this project. Writing HKLM needs admin.
#pragma once

#include "Common.h"

namespace srw::AcerSpatialLabs
{
    struct State
    {
        int focusDetection = -1;   // -1 = key/value not present; else 0 or 1
        int cameraPopup    = -1;   // mirrors the on-screen popup state: 1 = shown
                                   //  (default / NoPhysicalCameraLED=1 or missing),
                                   //  0 = hidden (NoPhysicalCameraLED=0).
    };

    enum class Setting       { Focus, CameraPopup };
    enum class WriteResult   { OK, NeedsAdmin, Failed };

    bool        Available();                       // the Acer SpatialLabs registry key exists
    State       Read();                            // returns -1 for any missing value
    WriteResult Write(Setting setting, bool on);   // HKLM write (admin required)
}
