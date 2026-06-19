# WebXR support for SR Loom — research

## 1. What WebXR actually delivers

When a page calls `navigator.xr.requestSession('immersive-vr')`, Chromium routes the
call through `device/vr/openxr/` ([source tree](https://chromium.googlesource.com/chromium/src/+/HEAD/device/vr/openxr/)).
The browser creates an `XrSession` against an OpenXR runtime, allocates per-eye
swapchains via `XR_KHR_D3D11_enable` (Chromium on desktop is **D3D11 only**),
hands the page an `XRWebGLLayer` whose framebuffer is a single wide RTV holding
both eye views, and submits the texture back to the runtime via
`xrEndFrame`. The runtime owns the final pixels; the browser never exposes a
stereo stream to the OS, never produces a window the compositor can capture
sensibly, and never lets the page reach the swapchain texture from JS. The only
way to intercept the per-eye textures is to **be** the OpenXR runtime, or to
wrap `navigator.xr` in JS before the page sees it (polyfill model).
`'inline'` sessions just render to a normal canvas in the page — those are
already capturable by WGC today.

## 2. Browser WebXR runtime requirements

Chrome and Edge use the system's OpenXR active runtime, selected by
`HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` (path to a runtime JSON
manifest), enumerated under `AvailableRuntimes`
([spec](https://registry.khronos.org/OpenXR/specs/1.0/loader.html)).
Firefox WebXR on desktop is effectively unmaintained — Mozilla pulled real WebXR
support; for Firefox you ship a JS polyfill.

Chromium has one extra non-negotiable: the runtime must implement
**`XR_EXT_win32_appcontainer_compatible`** because Chrome runs the XR device
code in a sandboxed utility process
([extension spec](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XR_EXT_win32_appcontainer_compatible.html),
[Chromium VR README](https://chromium.googlesource.com/chromium/src/+/HEAD/device/vr/README.md)).
Runtimes missing it can still be forced via command-line flags
([writeup](https://abhijeetk.github.io/Testing-WebXR-on-Windows/)) but normal
users won't do that. So: yes, a "fake HMD" approach works, but only if it
ships a conformant OpenXR runtime DLL with the appcontainer extension and
registers itself via the Khronos registry key.

## 3. What Acer "WebXR support" actually is

Two pieces, both already shipping:

1. **Acer OpenXR Runtime** for SpatialLabs Pro — Khronos-conformant OpenXR 1.0
   runtime registered as `ActiveRuntime`. Same model as SteamVR's runtime, but
   the compositor weaves to the lenticular display instead of an HMD
   ([Acer dev portal](https://spatiallabs.acer.com/developer/),
   [announcement](https://news.acer.com/acer-expands-support-for-spatiallabs-developers-with-suite-of-tools-that-enable-stereoscopic-3d-experiences)).
2. **WebXR-OpenXR Bridge** Chrome extension by Leia Inc
   ([Web Store listing](https://chromewebstore.google.com/detail/webxr-openxr-bridge/kbhoohhidfimoheieoibjlecmkcodipm),
   [Leia publisher page](https://chromewebstore.google.com/publisher/leia-inc/ub47183edcbb5db08c412f7a81ac6c593)).
   This is the user-facing piece. It is **not** a Chromium fork — it's a
   regular extension that exposes IPD / parallax / zoom / FOV controls and
   "connects WebXR applications to the native Leia OpenXR runtime". The
   runtime does the weave; the extension is just glue/UI.

So Acer's WebXR story is: *install our OpenXR runtime, set it active, install
the extension, launch Chrome.* No browser fork, no Chromium patches.

## 4. Existing third-party projects

- **[Looking Glass WebXR](https://github.com/Looking-Glass/looking-glass-webxr)**
  ([npm](https://www.npmjs.com/package/@lookingglass/webxr),
  [docs](https://docs.lookingglassfactory.com/software/creator-tools/webxr))
  — pure JS `LookingGlassWebXRPolyfill` that overrides `navigator.xr` *in the
  page*. The page (using three.js / Babylon / etc.) must explicitly import
  it. It renders a multi-view quilt into a popup window the user drags onto
  the Looking Glass; the quilt-to-lenticular weave happens in Looking Glass
  Bridge, talked to via a localhost WebSocket at `ws://127.0.0.1:11222/`
  ([bridge.js](https://github.com/Looking-Glass/bridge.js)). Polyfill model
  — **opt-in per site**, doesn't work on arbitrary WebXR pages.
- **[Mozilla WebXR Emulator](https://github.com/MozillaReality/WebXR-emulator-extension)**
  / **[Meta Immersive Web Emulator](https://github.com/meta-quest/immersive-web-emulator)**
  — extensions that inject a polyfill into every page; for development only,
  no real device output.
- **[VRto3D](https://github.com/oneup03/VRto3D)** — OpenVR (SteamVR) driver
  exposing a virtual HMD, rendering SBS/TaB to a normal window. WebXR-via-
  SteamVR-OpenXR works; SR Loom captures and weaves the SBS window. Today's
  baseline. Output is post-bake stereo, full WebXR feature set works.
- **[XRGameBridge](https://github.com/JoeyAnthony/XRGameBridge)** — **the
  direct precedent**. OpenXR runtime by Joey Anthony, D3D12-only, registers
  as `ActiveRuntime`, renders to a window on the SR screen, talks to the
  LeiaSR/Dimenco SDK for the actual weave. Built for UEVR but architecturally
  identical to what a WebXR runtime would need. Verify whether it implements
  `XR_EXT_win32_appcontainer_compatible` — required for Chrome.
- **[Monado](https://monado.freedesktop.org/)** — full FOSS OpenXR runtime;
  Windows support is "mostly simulated HMD" only and direct-mode display is a
  blocker. Forking Monado to weave via LeiaSR SDK is plausible but heavy.

Closest model to "WebXR → SR Loom direct" = the Leia bridge extension + their
OpenXR runtime. The Looking Glass polyfill is the closest *opt-in* model.

## 5. Implementation paths for SR Loom, easiest → hardest

1. **Do-nothing / document the chain.** WebXR page in Chrome → SteamVR
   OpenXR → VRto3D's virtual HMD → SBS window → SR Loom captures + weaves.
   Zero code in SR Loom. Add a `docs/webxr-via-vrto3d.md` walkthrough and a
   capture preset for VRto3D's headset window. UX: configure SteamVR active
   runtime, install VRto3D, point SR Loom at the window. Already works for
   the subset of users on that stack.
2. **Bundle / shell out to Leia's official chain.** If the Leia OpenXR
   runtime + WebXR-OpenXR Bridge extension work standalone on user hardware,
   SR Loom does *nothing* for WebXR itself — just detect them and surface a
   "Open WebXR in Chrome" tray action. Code: tray menu item + runtime
   detection (registry read of `ActiveRuntime`, check for extension ID
   `kbhoohhidfimoheieoibjlecmkcodipm`). Cleanest UX, zero new rendering
   code. Caveat: depends on Leia's runtime being installable next to LeiaSR
   SDK 1.36.2 without conflict — needs testing.
3. **Ship a "use this site" JS polyfill recipe.** Author a Looking-Glass-
   style polyfill that targets SR Loom's existing window-capture path: page
   loads the polyfill, polyfill creates a popup, renders SBS into a canvas,
   SR Loom auto-detects the window by title and weaves it. Code: a small TS
   package (`@srloom/webxr-polyfill`) plus a window-title heuristic in
   SR Loom's capture picker. Only works for sites that opt in (three.js
   demos, custom apps) — not a general "every WebXR site works" solution.
4. **Build an SR Loom OpenXR runtime.** Write a C++ OpenXR 1.0 runtime DLL
   that implements `XR_KHR_D3D11_enable` + `XR_KHR_D3D12_enable` +
   `XR_EXT_win32_appcontainer_compatible`, registers under
   `HKLM\SOFTWARE\Khronos\OpenXR\1`, accepts the per-eye D3D textures in
   `xrEndFrame`, hands them to the existing LeiaSR weave pipeline via shared
   handles, and adds head pose from the LeiaSR eye tracker. This *is*
   "WebXR → SR Loom direct" and works for every OpenXR app — not just web.
   XRGameBridge proves it's feasible at the scale of one developer. The
   sandbox extension and the Chrome conformance dance are the real cost.
   Multi-month effort, but it would obsolete VRto3D-for-WebXR on SR hardware.
5. **(Anti-option) Fork Chromium.** Building a stereoscopic Chromium fork
   that bypasses OpenXR. Off the table — multi-engineer-year commitment to
   chase Chrome releases, with no advantage over option 4.

Recommendation: ship option 1 immediately (docs only), evaluate option 2 once
the Leia runtime is tested against LeiaSR 1.36.2, keep option 4 as a long-term
"SR Loom is the OpenXR runtime on this machine" play if XRGameBridge stalls.
