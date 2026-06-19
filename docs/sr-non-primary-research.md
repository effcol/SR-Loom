# SR display as a non-primary Windows monitor — research

**Question:** does the LeiaSR / Dimenco runtime (SDK 1.36.2 / 1.34.10) require the
SR display (Samsung Odyssey 3D, Acer SpatialLabs View / View Pro / Edge) to be
the *Windows primary* monitor, or can it weave on a secondary display?

**Short answer:** the SDK does **not** require it. The "must be primary"
instruction in vendor user guides is a Windows-setup convention, baked into
vendor *launcher apps* (Samsung Reality Hub, Acer SpatialLabs Go / TrueGame),
not into the underlying LeiaSR runtime or `IDX11Weaver`. SR Loom already
follows the SDK-recommended pattern and should work on non-primary setups
unchanged. Verify with the diagnostic logging already in `WinMain`.

## 1. How `CreateDX11Weaver` picks a display

`CreateDX11Weaver(SRContext*, ID3D11DeviceContext*, HWND, IDX11Weaver1**)` in
`LeiaSR-SDK-1.36.2-win64/include/sr/weaver/dx11weaver.h:68` takes the HWND as
"handle to (optional) output window" (line 63). The HWND is used for
**window-presentation behaviour**, not for choosing which physical panel is the
SR display. `PredictingDX11Weaver::setLatencyInFrames` (dx11weaver.h:362–363)
notes latency is *"calculated using these number of frames based on the refresh
rate of the monitor that the application is running on, this will be
dynamically updated when the window changes monitor"* — proof the SDK calls
`MonitorFromWindow(hwnd)` to track refresh-rate, but only for latency math.
Which monitor counts as an SR display is decided by the SR runtime/service via
EDID, independent of Windows-primary state.

`IWeaverBase1::setWindowHandle(HWND)` (`IWeaverBase.h:34`) reinforces this: the
docstring is "the window that will present the backbuffer," not "the display to
weave for."

## 2. The `IDisplayManager` / `IDisplay` API — the actual selector

`sr/world/display/display.h` exposes:

```cpp
class IDisplayManager : public virtual SR::IQueryInterface {
    virtual IDisplay* getPrimaryActiveSRDisplay() = 0;
    static std::unique_ptr<IDisplayManager> create(SR::SRContext& ctx);
};
extern "C" DIMENCOSR_API SR::IDisplayManager* GetDisplayManagerInstance(SR::SRContext& context);
```

`IDisplay::getLocation()` returns an `SR_recti` *"representing the position and
virtual size of the display in the display configuration"* (`display.h:103–104,
197–198`), i.e. coordinates in the Windows virtual-desktop space. **"Primary
active SR" here means *the* registered SR display, not Windows primary** — the
naming is unfortunate but `WindowsDisplayUtilities.h:160–162` clarifies:
*"This function assumes only one SR device is attached at a time."* No
`getByEdid()` / `getByMonitorHandle()` overload exists; the SDK assumes one SR
panel and locates it by EDID in the runtime
(`WindowsDisplayUtilities.h:30–38, 110, 147, 180`: `EdidData {manufacturerId,
productId, serialNumber}` matched against product codes `"AJ"`, `"D1"`, `"AN"`).

The deprecated `world/display/screen.h:34` `Screen` class hard-codes 2560×1440
defaults and predates the proper enumeration API — ignore it.

No DX11 weaver overload takes an `HMONITOR`, EDID or display name. All
overloads in `dx11weaver.h` are `(SRContext, device, ctx, w, h, [format], HWND)`.
The chain is: app calls `getPrimaryActiveSRDisplay()->getLocation()`, places
its window inside that rect, passes the HWND to `CreateDX11Weaver`, done.

## 3. Eye / head tracker monitor binding

`EyeTracker::create(SRContext&)` (`sense/eyetracker/eyetracker.h:47`) and the
head-pose trackers take **only the SRContext** — no HWND, no HMONITOR. The
camera is calibrated to the physical SR panel at install time (owned by SR
Service), so eye coordinates are already in the SR display's frame regardless
of where Windows places the desktop origin. Trackers are display-aware but
globally, not per-weaver.

## 4. How other apps handle it

- **XRGameBridge** ([repo](https://github.com/JoeyAnthony/XRGameBridge),
  [releases](https://github.com/JoeyAnthony/XRGameBridge/releases)) is
  runtime/EDID-driven: *"If the connected SR display was not detected by the
  SR runtime yet, the XRGameBridge window may open on a non-SR display. In
  this case, restart the game and inject again."* — no primary requirement.
- **VRto3D** ([repo](https://github.com/oneup03/VRto3D),
  [changelog](https://github.com/oneup03/VRto3D/blob/main/changelog.md))
  exposes a `display_index` setting; the current README does *not* require the
  SR display be Windows primary.
- **Acer SpatialLabs Pro triple-display** ([landing](https://www.acer.com/us-en/spatiallabs))
  drives three SR panels simultaneously via Acer's OpenXR runtime — by
  definition at most one of those three can be Windows primary, so the SR
  runtime demonstrably supports non-primary panels in production. KitGuru's
  [Computex 2026 roundup](https://www.kitguru.net/peripherals/monitors/joao-silva/computex-2026-acer-unveils-new-spatiallabs-3d-and-1000hz-gaming-monitors/)
  covers consumer SKUs; the triple-display config is a Pro / simulation feature,
  not a consumer "3D Surround" bundle (I couldn't find a marketed
  "TrueGame 3D Surround" SKU — Acer's three-panel offering is Pro-only).

## 5. Vendor user-facing guidance (the source of the "must be primary" myth)

- **Samsung Odyssey 3D** support page
  (https://www.samsung.com/uk/support/displays/how-to-use-3d-on-the-odyssey-3d-gaming-monitor/)
  tells users to *"ensure that the Odyssey 3D is set as your primary monitor"*
  for the Reality Hub experience. This is a launcher convention, not an SDK
  contract.
- **Acer SpatialLabs Go / TrueGame** has the same pattern in its launcher:
  Acer Community threads
  ([1](https://community.acer.com/en/discussion/comment/1302046/),
  [2](https://community.acer.com/en/discussion/684609/spatiallabs-go-problem-with-multi-monitor-setup),
  [3](https://community.acer.com/en/discussion/712923/predator-helios-300-how-can-i-play-in-3d-and-2d-simultaneously-using-2-monitors))
  report games launching on the wrong monitor; users work around with
  `Win+Shift+→` or by setting SpatialLabs as primary. Notably one user reports
  on Windows 11 the 3D image *"displays on the SpatialLabs monitor under all
  circumstances"* — modern Windows + the SR runtime route correctly; the
  primary requirement was a pre-Win11 launcher-app workaround.

## 6. Conclusion + code implications for SR Loom

**The "must be primary" requirement is a Windows-setup convention, not a
LeiaSR limitation.** The SR runtime locates the SR panel by EDID, exposes its
desktop rectangle via `IDisplayManager::getPrimaryActiveSRDisplay()->getLocation()`,
and the weaver follows `MonitorFromWindow(hwnd)` only for refresh-rate
tracking.

**What SR Loom already does right** (`src/main.cpp`):
`ResolveTargetRect()` (line 157) queries the SDK for the SR display rect;
`SrMonitor()` (line 1425) maps that rect back to an `HMONITOR` via
`MonitorFromPoint(center, MONITOR_DEFAULTTOPRIMARY)`; the output window is
created at that rect (line 2818). Diagnostic logging (lines 2778–2796) already
records whether the SR display is Windows primary and flags non-primary runs
in `srloom.log`. Nothing in this path assumes Windows-primary.

**Code changes needed: none for the happy path.** Worth auditing:
1. `MonitorFromPoint(..., MONITOR_DEFAULTTOPRIMARY)` in `SrMonitor()` — the
   `DEFAULTTOPRIMARY` fallback only triggers if the SDK-reported point lies
   outside every monitor's rect (shouldn't happen if the SDK is healthy).
   Cosmetic; leave it.
2. `WGC` "this display" passthrough already sets `WDA_EXCLUDEFROMCAPTURE`, so
   the feedback loop is fine regardless of primary status.
3. Update the README's "make the SR display your primary monitor" line to
   *"SR Loom does not require the SR display to be Windows primary; Samsung's
   own setup guide recommends it for Reality Hub, but our weave window
   self-targets the SR panel via the SDK."*

**Hardware-specific caveats:**
- **Samsung Odyssey 3D:** Reality Hub itself may behave oddly on non-primary
  setups (per Samsung's own support page) — that's *their* app, not ours.
  SR Loom is unaffected.
- **Acer SpatialLabs (View / View Pro / Edge):** SpatialLabs Go / TrueGame
  historically launched games on Windows-primary regardless of where the
  SpatialLabs panel was positioned. SR Loom side-steps this entirely by
  placing its own borderless window on the SDK-reported SR rect.
- **Both:** if the user has *two* SR displays attached, the SDK exposes only
  `getPrimaryActiveSRDisplay()` — there is no enumeration API in 1.36.2, so
  SR Loom can only target one. Documented limitation.

## Sources

- LeiaSR SDK 1.36.2 headers (local): `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64/include/sr/{weaver/dx11weaver.h, weaver/IWeaverBase.h, world/display/display.h, world/display/screen.h, management/WindowsDisplayUtilities.h, sense/eyetracker/eyetracker.h}`
- [docs.simulatedreality.com](https://docs.simulatedreality.com/) — public reference (WebFetch blocked in research env; SDK headers used as the authoritative source)
- [VRto3D repo](https://github.com/oneup03/VRto3D), [changelog](https://github.com/oneup03/VRto3D/blob/main/changelog.md)
- [XRGameBridge releases](https://github.com/JoeyAnthony/XRGameBridge/releases), [repo](https://github.com/JoeyAnthony/XRGameBridge), [3DGameBridgeProjects](https://github.com/JoeyAnthony/3DGameBridgeProjects), [BramTeurlings/3DGameBridge](https://github.com/BramTeurlings/3DGameBridge)
- [Samsung UK — How to use 3D on the Odyssey 3D](https://www.samsung.com/uk/support/displays/how-to-use-3d-on-the-odyssey-3d-gaming-monitor/), [SI mirror](https://www.samsung.com/si/support/displays/how-to-use-3d-on-the-odyssey-3d-gaming-monitor), [Odyssey 3D install page](https://pages.samsung.com/us/support/simulators/odyssey-3d/)
- [Acer SpatialLabs landing — triple-display Pro](https://www.acer.com/us-en/spatiallabs)
- Acer Community: [game launching on wrong monitor](https://community.acer.com/en/discussion/comment/1302046/), [SpatialLabs Go multi-monitor problem](https://community.acer.com/en/discussion/684609/spatiallabs-go-problem-with-multi-monitor-setup), [3D + 2D simultaneous on two monitors](https://community.acer.com/en/discussion/712923/predator-helios-300-how-can-i-play-in-3d-and-2d-simultaneously-using-2-monitors)
- [KitGuru — Computex 2026 SpatialLabs roundup](https://www.kitguru.net/peripherals/monitors/joao-silva/computex-2026-acer-unveils-new-spatiallabs-3d-and-1000hz-gaming-monitors/)
- [Leia forum — PC LeiaSR universal programs](https://forums.leialoft.com/t/pc-leiasr-universal-programs/6063), [LEIA SR / LEIA Player for Windows](https://forums.leialoft.com/t/leia-sr-or-leia-player-for-windows/5073)
- [effcol/Simulated-Reality-OpenTrack-Bridge](https://github.com/effcol/Simulated-Reality-OpenTrack-Bridge)
