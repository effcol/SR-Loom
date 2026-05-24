# SR Weaver

A Windows system-tray app that weaves stereo 3D onto **Simulated Reality**
displays (Samsung Odyssey 3D, Acer SpatialLabs, and other LeiaSR / Dimenco
panels). It takes a stereo source — eventually captured from any window or the
whole screen — converts it to the side-by-side form the SR weaver consumes, and
weaves it with head-tracked depth, in either a window or fullscreen.

> Status: **Milestone 1** — tray icon + a static side-by-side test image woven
> into a window that toggles between fullscreen and windowed. The capture and
> format-conversion stages come next.

## How it works

The LeiaSR weaver always takes a **full side-by-side (SBS)** texture and produces
the lenticular, eye-tracked output for the SR display. So the pipeline is:

```
capture source  ->  convert any stereo format to SBS  ->  weaver.weave()  ->  present
   (later)              (later: shaders)                  (SR SDK)         (window/fullscreen)
```

Milestone 1 skips capture/convert and feeds a built-in SBS image directly to the
weaver to validate the SDK, window, and tray plumbing.

## Building

Requires Visual Studio 2022/2026 (Desktop C++), CMake ≥ 3.21, and the installed
**SR Platform runtime** (the SR Service must be running).

> The `lib/` folder (SR SDKs and helper repos) is **not** tracked in git. Place
> the LeiaSR SDKs there locally:
> `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64` (x64) and
> `lib/Simulated Reality/simulatedreality-1.34.10-win32-Release` (x86).

```powershell
# 32-bit (matches the installed 32-bit runtime; verified path)
cmake -B build/x86 -A Win32
cmake --build build/x86 --config Release

# 64-bit (requires the 64-bit SR runtime DLLs to be present at run time)
cmake -B build/x64 -A x64
cmake --build build/x64 --config Release
```

The 32-bit build links the SDK in `lib/Simulated Reality/simulatedreality-1.34.10-win32-Release`;
the 64-bit build links `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64`. Both use
the modern `IDX11Weaver1` / `CreateDX11Weaver` API (windowed + fullscreen).

## Controls

- **Tray right-click**: toggle weaving, choose Fullscreen / Windowed, Exit.
- **Ctrl+Alt+W**: enable/disable weaving.
- **Ctrl+Alt+F**: toggle fullscreen / windowed.

## License

MIT (see `LICENSE`). Links against the proprietary SR SDK, which keeps its own
license and must be installed separately.
