// resource.h — resource identifiers.
#pragma once

#define IDI_TRAY 101

// Embedded UI fonts (Inter, SIL OFL) so the release is a single self-contained exe.
#define IDR_FONT_REGULAR  201
#define IDR_FONT_SEMIBOLD 202

// Embedded NPClient.dll (x64) for TrackIR-aware games. Extracted to
// %LOCALAPPDATA%\SRLoom\NPClient\ on first launch.
#define IDR_NPCLIENT64    301
