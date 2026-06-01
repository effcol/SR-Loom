# SR Loom

<img width="210" height="258" alt="Screenshot 2026-05-26 220250" src="https://github.com/user-attachments/assets/82918258-978b-43bf-a060-85bf2056b88b" /> <img width="210" height="258" alt="Screenshot 2026-05-26 221234" src="https://github.com/user-attachments/assets/7b5f002d-dbea-4f95-866d-6e68d2a09e62" />




A lightweight Windows tray app that weaves stereo 3D onto **Simulated Reality**
displays (Samsung Odyssey 3D, Acer SpatialLabs, and other LeiaSR / Dimenco
panels). Point it at your screen, a window, or a floating "looking glass," pick
the stereo format your content is in, and it converts and weaves it with
head-tracked depth — no glasses.

> **v1.3** — single self-contained `SRLoom.exe`. Requires a Simulated Reality
> display and the SR Platform runtime installed.

## What it does

The LeiaSR weaver always takes a **full side-by-side (SBS)** texture and
produces the lenticular, eye-tracked output. SR Loom captures your source,
converts whatever stereo layout it's in to SBS with a GPU shader, then weaves:

```
capture (screen / window)  ->  convert any stereo format to SBS  ->  weave  ->  present
```

### Stereo input formats
- **Side-by-Side** (Full / Half)
- **Top-and-Bottom** (Full / Half)
- **Interleaved** (Row / Column)
- **Checkerboard**
- **Anaglyph** — with colour-recovery decode modes (recovered / filtered / half / mono)
- **Frame Sequential** (temporal)
- **Pulfrich Effect** (time-delay or ND-filter)
- **Frame Packing**

### Display modes
- **Monitor** — fullscreen passthrough weave of the whole SR display.
- **Window** — pick a window; the weave overlays and tracks it.
- **Make active window 3D** — weave whatever window you're using (Ctrl+Alt+C).
- **Looking Glass** — a floating, draggable/resizable 3D viewport.

## Controls

Left-click the tray icon for the control panel; right-click for a quick menu.

- **Ctrl+Alt+W** — toggle weaving on/off
- **Ctrl+Alt+F** — switch Fullscreen ⇄ Looking Glass
- **Ctrl+Alt+C** — make the active window 3D (press again to turn it off)

The panel has a compact mode (just the on/off switch + a status line) and an
expanded mode with the display, stereo-input, and depth (convergence) controls,
plus a light/dark theme toggle.

## Requirements

- **64-bit Windows 10 (1903+) or Windows 11** — older builds lack the
  screen-capture APIs SR Loom relies on.
- A Simulated Reality display + a **current SR Platform runtime** installed (the
  SR Service must be running — it provides the `SimulatedReality*.dll`s and the
  eye-tracking). An out-of-date runtime can be missing the modern weaver API.

## Troubleshooting

- **A "fullscreen" game shows a frozen 3D image / doesn't update / has no input.**
  SR Loom captures and overlays via Windows' screen-capture API, which can't see
  or draw over a game in **exclusive fullscreen**. Run the game in **borderless**
  (or windowed) instead. Most modern games default to borderless-flip anyway; only
  true exclusive fullscreen is unsupported. Games that gate their own 3D mode
  behind exclusive fullscreen can't be driven yet.
- **`CreateDX11Weaver ... could not be located`** — your **SR Platform runtime is
  out of date**. Update it (and make sure the SR Service is running).
- **`CreateDirect3D11DeviceFromDXGIDevice ... could not be located`** — your
  **Windows is too old**; update to a current Windows 10/11.
- **Windows Defender / SmartScreen flags it.** It's an unsigned app that captures
  the screen and draws overlays, which trips antivirus heuristics — a false
  positive. The source is here for review; you can "Allow on device," and the
  detection has been reported to Microsoft.
- **A yellow border around the captured area.** That's Windows' capture indicator.
  SR Loom asks the OS to hide it, but Windows only grants that to packaged apps,
  so it may remain.
- **Variable refresh rate (G-Sync / FreeSync) causes judder.** SR Loom captures
  the source and re-presents to the SR display, so the SR display's VRR ends up
  synced to *SR Loom's* present cadence rather than the source game's. If the
  game runs below the SR display's refresh rate, you'll see judder. This is a
  fundamental limitation of capture-based mirroring on Windows — Sunshine,
  ShaderGlass, OBS, and every other tool in this category have the same
  constraint, and the DXGI API that could fix it (`SetPresentDuration`) is
  explicitly restricted by Microsoft to internal panels. For best results: cap
  the game at (or just below) the SR display's refresh rate so VRR isn't engaged,
  or disable VRR on the SR display and run a clean integer multiple
  (e.g. 60 fps source on a 120 Hz panel).
- **RTSS (RivaTuner Statistics Server) causes stutter or drops.** RTSS hooks
  `IDXGISwapChain::Present` on every D3D11 app, which interferes with SR Loom's
  output. If you use RTSS, **add `SRLoom.exe` to its exclusion list**
  (RTSS Settings → "Application detection level" → exclude, or per-process
  exclusion). Same trick OBS users use for the same reason.

## Releasing / running

The build is a **single self-contained `SRLoom.exe`** — the UI fonts are embedded
and the MSVC runtime is statically linked, so there's nothing to ship beside it.
The only external dependency is the SR Platform runtime, which the user already
has installed to use the display.

## Building

Requires Visual Studio 2022/2026 (Desktop C++) and CMake ≥ 3.21. The build is
**x64 only** (the SR Platform runtime only installs on 64-bit Windows).

> The `lib/` folder (the proprietary SR SDK) is **not** tracked in git. Place the
> LeiaSR SDK locally at `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64`.

```powershell
cmake -B build/x64 -A x64
cmake --build build/x64 --config Release
# -> build/x64/Release/SRLoom.exe
```

## License

MIT (see `LICENSE`). Bundles the **Inter** font (SIL Open Font License). Links
against the proprietary **SR SDK**, which keeps its own license and must be
installed separately.
