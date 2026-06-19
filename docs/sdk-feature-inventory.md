# LeiaSR SDK 1.36.2 — Feature Inventory for SR Loom

Source: `lib/Simulated Reality/LeiaSR-SDK-1.36.2-win64/include/sr/` (Copyright 2025 Leia, Inc.). All findings below were read directly from the headers; no CHANGELOG / RELEASE_NOTES file ships in the SDK root, so the "what's new in 1.36" question is answered by inference from `Copyright 2025` stamps, the existence of `IDX11Weaver1` / `IDX12Weaver1` / `Window2` / `IDisplay` / `IDisplayManager` "...1 / ...2" suffixed types (clear new-API generation), and the new `lenshints` sample.

Scoring legend: **5** = adopt now, **4** = high value moderate effort, **3** = neat-but-situational, **2** = future-feature material, **1** = academic interest.

---

## 1. Weaver — capabilities SR Loom isn't using

SR Loom currently calls only `SetInputView` + `weave(w,h[,x,y])` + `StopHeadTracker` on `IDX11Weaver1`. The modern `IDX11Weaver1` inherits from `IWeaverBase1` (`weaver/IWeaverBase.h:25`), which exposes a large surface SR Loom is ignoring:

| File:line | Signature | What SR Loom could do |
|---|---|---|
| IWeaverBase.h:40 | `virtual void weave() = 0` | The no-arg overload — uses currently-bound viewport/scissor. Lets you wrap the weave call in a custom DX state setup. **★★** |
| IWeaverBase.h:48 | `virtual void enableLateLatching(bool enable) = 0` | Updates head position for frames already in flight. The header explicitly says this "reduc[es] weaving latency". Pure win for tracked content — no other call site needed. **★★★★★** |
| IWeaverBase.h:54 | `virtual bool isLateLatchingEnabled() const = 0` | Diagnostic; pair with a tray-menu toggle. **★★★** |
| IWeaverBase.h:64 | `virtual void setShaderSRGBConversion(bool read, bool write) = 0` | Critical correctness knob. If our input texture is sRGB-typed (DXGI_FORMAT_*_SRGB) the GPU samples already linearise — `read=false`. Mismatching this is a common cause of "too dark / too bright weave". The official DX11 sample at `examples/directx11_weaving/.../main.cpp:813` calls `setShaderSRGBConversion(true, true)`. **★★★★★** |
| IWeaverBase.h:78 | `virtual void setLatencyInFrames(uint64_t) = 0` | Auto-computes microsecond latency from the current monitor's refresh rate (re-evaluates when the window moves between monitors). Better than hand-tuned `setLatency`. **★★★★** |
| IWeaverBase.h:91 | `virtual void setLatency(uint64_t latency) = 0` | Microsecond override. Useful if our render pipeline depth is known. **★★★★** |
| IWeaverBase.h:101 | `virtual uint64_t getLatency() const = 0` | Reads back current effective latency in µs. Status-display fodder. **★★** |
| IWeaverBase.h:111 | `virtual void getPredictedEyePositions(float left[3], float right[3]) = 0` | This is the cleanest path to per-eye positions — already latency-corrected — without subscribing to `HeadPoseTracker` ourselves. The DX11/DX12/GL samples all use this for virtual-camera setup. SR Loom currently polls `HeadPoseTracker`; we could likely **delete** the listener and call this once per `weave()` instead. **★★★★★** |
| dx11weaver.h:45 | `virtual void setContext(ID3D11DeviceContext* context) = 0` | Rebind the D3D11 immediate context. Lets SR Loom switch device contexts (e.g. after a device reset) without recreating the weaver. **★★★** |

### IDX11Weaver1 only — no DX11Weaver2 yet

The `1` suffix on `IDX11Weaver1`, `IDX12Weaver1`, `IDX10Weaver1`, `IDX9Weaver1`, `IGLWeaver1` plus the explicitly-marked-deprecated `DX11WeaverBase` / `PredictingDX11Weaver` (dx11weaver.h:74, 281) tells us Leia split the API in this version: the new IWeaverBase1 path is `IDX11Weaver1`, the old monolithic class is the deprecated one. **Anything on the deprecated `DX11WeaverBase` that isn't on `IWeaverBase1` is gone from the new API**:

- `setContrast(float)` / `getContrast()` (dx11weaver.h:130, 135) — **deprecated path only**. ACT replaces it. **★**
- `setACTMode(WeaverACTMode)` / `getACTMode()` (dx11weaver.h:148, 153) where `WeaverACTMode` is `Off / Static / Dynamic` (WeaverTypes.h:19) — Anti-Crosstalk control. **Critical**: this used to be on the deprecated class, and on `IDX11Weaver1` there is NO ACT setter. SR Loom currently has zero control over ACT; if the user sees ghosting they have no knob to turn. Either re-add the deprecated weaver path for ACT, or expose this once Leia adds it to `IDX11Weaver1`. **★★★★** (today: ★★ because not on new API)
- `setCrosstalkStaticFactor / setCrosstalkDynamicFactor` (dx11weaver.h:158-173) — fine-grained ACT factors. Same situation. **★★**
- `canWeave(w,h)` / `canWeave(w,h,xOff,yOff)` (dx11weaver.h:190, 201) — gated feasibility check before `weave()`. Useful for "do we actually have a tracked user / valid display state right now?". Also deprecated. **★★**
- `BehaviorWhenNotTracking` enum (WeaverTypes.h:25): `Default / ShowLeft / ShowLeftWithShader`. Set via `Dimenco::Weaver::GetBehaviorWhenNotTracking()` (Weaver.h:155). Tells you what the runtime is configured to do when the user steps out. SR Loom could surface this in the tray ("3D off — user not tracked"). **★★★**

### DX12 weaver — what migration would buy us

`dx12weaver.h` adds three things DX11 doesn't expose:

- dx12weaver.h:48 `setOutputFormat(DXGI_FORMAT)` — DX12 weaver needs an explicit output-format hint. The DX12 sample calls it at `main.cpp:1095`. **HDR matters here**: SR Loom presents through Windows' compositor; if we ever want HDR10 / scRGB stereo content we need DX12 + this call. **★★★** (only if we add HDR mode)
- dx12weaver.h:58 `setViewport(D3D12_VIEWPORT)` — viewport must be passed in, not inferred. **★★**
- dx12weaver.h:63 `setScissorRect(D3D12_RECT)` — scissor passed in. **★★**

The DX12 sample (`examples/directx12_weaving/.../main.cpp`) is otherwise structurally identical — multi-frame command allocators, explicit fence sync. There is **no DX12-only weaver feature** (late latching, latency control, ACT all live on the common base). Migrating buys us HDR support and one less DXGI interop layer with WGC's `ID3D11Texture2D`. Not free.

### Calibration / low-level math (`Weaver.h`, `WeaverC.h`)

These look intended for apps that write their own custom shaders, not for the weaver-as-a-service flow SR Loom uses. Notable getters that could be used as **diagnostics**:

- Weaver.h:124 `static float GetPattern()` — lens pattern parameter
- Weaver.h:125 `static float GetXTalkFactor()` — current crosstalk factor
- Weaver.h:135 `static float GetSlant()` — lens slant coefficient
- Weaver.h:140-150 `GetPx() / GetN() / GetDoN()` — pixel pitch, refractive index, D/N
- Weaver.h:129-130 `GetLateLatchingForceOn() / GetLateLatchingForceOff()` — read what the global INI forces. **Worth checking** because if `ForceOff=true` our `enableLateLatching(true)` will silently no-op. **★★★**

---

## 2. Sense subsystem — what we're not subscribing to

### `SystemSense` (sense/system/) — ★★★★★

This is the **single highest-value missed feature**. `SystemEvent` (systemevent.h:12-31) is an 18-value enum the SR runtime fires at apps:

```
Info, ContextInvalid, SRUnavailable, SRRestored,
USBNotConnected, USBNotConnectedResolved,
DisplayNotConnected, DisplayNotConnectedResolved,
Duplicated, DuplicatedResolved,
NonNativeResolution, NonNativeResolutionResolved,
DeviceConnectedAndReady, DeviceDisconnected,
LensOn, LensOff,
UserFound, UserLost
```

SR Loom's job is precisely to react to these conditions, and right now we react to **none** of them. Subscribe via `SystemSense::create(ctx)->openSystemEventStream(listener)` (systemsense.h:45, 53). Concrete wins:

- `ContextInvalid` → tear down + reinit our weaver instead of silently going stale.
- `LensOff / LensOn` → tray-icon state ("3D mode off") + don't bother weaving while lens is off.
- `UserLost / UserFound` → optional auto-pause; combine with `BehaviorWhenNotTracking` enum.
- `Duplicated / NonNativeResolution` → surface a tray warning ("3D is impaired: your display is duplicated"). These are silent killers today.
- `DeviceDisconnected / SRUnavailable` → graceful degrade rather than crash.

`SystemEvent.message` (systemevent.h:69) carries a human-readable string we can echo to our log/UI.

### `SwitchableLensHint` (sense/display/switchablehint.h) — ★★★★

- switchablehint.h:42 `static SwitchableLensHint* create(SRContext&)`
- switchablehint.h:52 `virtual void enable()` — request lens ON
- switchablehint.h:57 `virtual void disable()` — request lens OFF
- switchablehint.h:64 `virtual bool isEnabled()` — current actual state
- switchablehint.h:73 `virtual bool isEnabledByPreference()` — has *any* app requested ON

SR Loom should call `enable()` whenever it starts weaving and `disable()` when it pauses/stops. Currently we have no control over the lens. The `examples/lenshints/src/lenshints.cpp` sample is the canonical pattern (poll `isEnabled()` after a 1s sleep — state is asynchronous). The admin-only variant (`displays_admin_c.h:40`) also exposes `disableByForce()` and `enableDefaultSwitching()` but those are admin-tool territory.

### `ApplicationSense` (sense/application/applicationsense.h) — ★★★

- applicationsense.h:42 `static ApplicationSense* create(SRContext&)`
- applicationsense.h:49 `virtual std::vector<std::string> getApplicationNames() = 0`

Returns the names of all currently-connected SR applications. Useful for a tray submenu "Other apps using SR: …" and to avoid double-weaving when the user launches an SR-native game.

### `PredictingEyeTracker` / `PredictingWeaverTracker` (sense/eyetracker/, sense/weavertracker/) — ★★

`PredictingEyeTracker::create(ctx)` (predictingeyetracker.h:47) + `predict(latency_us, SR_eyePair& out)` (line 62) gives latency-compensated **per-eye** positions. `PredictingWeaverTracker` (predictingweavertracker.h:47, 65) gives a single `SR_weaverPosition` (cm — note the unit difference from eye-pair mm) intended as the weaver's "where is the head right now for the frame I'm about to show". 

**Important caveat**: `predictingweavertracker.h:65,75` explicitly says "for internal use only" — the weaver itself uses this internally. SR Loom should rely on `IWeaverBase1::getPredictedEyePositions()` instead. The header chain is `PredictingEyeTracker → filter+predict → fed into Weaver via late latching`. So this isn't really new functionality for us — it's a knob the weaver already pulls on our behalf.

`EyeTracker::createRaw(ctx)` (eyetracker.h:58) is interesting: unfiltered per-frame eye data. Useful for telemetry/debug overlay ("how jittery is the raw tracker?"). **★★**

`PredictingEyeTracker::setFaceLostDelay(uint64_t)` (predictingeyetracker.h:79) — milliseconds before eyes snap to default position (0,100,600) when face is lost. Tuneable. **★★**

### `HandTracker` (sense/handtracker/) — ★

The hand tracker is **fully articulated**: 21 joints per hand (handpose.h:68-90 enumerates them — wrist, palm, then metacarpal/proximal/intermediate/distal for each finger and metacarpal/proximal/distal for the thumb). Per-frame `SR_handPose` (handpose.h:119-141) gives absolute positions in mm relative to the display, indexed both by named fields (`hand.index.tip`) and by `joints[21]`. Up to N hands via per-hand `HandPoseStream` (handtracker.h:62), with `HandEventStream` (handtracker.h:69) firing `CreateHand`/`DestroyHand` (handevent.h:10) on entry/exit.

For SR Loom this is "academic interest" — we're a weaver, not a UX framework. The relevant question is whether camera coverage matters: the headers don't expose camera FOV or coverage range. **★**

`getPinching(SR_handPose)` (handpose.h:153) returns the index-tip-to-thumb-tip distance in mm — useful primitive if we ever wanted air-tap shortcuts. `getGrabbing()` (handpose.h:146) literally returns 0 — stub. **★**

### `GestureAnalyser` (sense/gestureanalyser/) — ★★

Two tiers:

- **Stream-based, runtime gestures** (gesture.h:10): `TapGesture, SwipeGesture, GrabGesture, ReleaseGesture` — fires `SR_gesture { time, position, type }` events.
- **Per-frame classification** via `GestureRecognizer` (gestureRecognizer.h:100) — neural-net pose classifier returning one of `FIST, POINT, PINCH, FLAT, PINCHGRABRELEASE` (gestureRecognizer.h:46) with probability. Two models: `NN4` (4 classes) and `NN5` (adds PINCHGRABRELEASE).

A "wave hand at the screen → toggle 3D mode" feature is **technically** trivial here (subscribe to `SwipeGesture`). Probably overkill for v1.7. **★★**

### `Camera` (sense/cameras/) — ★★

The internal SR cameras are exposed as a generic `Sense`: `Camera::create(ctx)` (camera.h:132), `openVideoStream(VideoListener*)` (camera.h:160), `getStreamCount()` (camera.h:152). `VideoFrame` (videoframe.h:45) wraps a `cv::Mat` — yes, OpenCV `Mat`. So you can subscribe to the **raw IR / RGB camera frames** the eye tracker uses.

`CameraController` (cameracontroller.h:63) lets you (exclusively, via `UniqueCameraController`) set `setShuttertime(float seconds)` and `setGain(float)`. For diagnostics: "show me what the tracker sees", or "user is in a dim room — bump gain". **★★**

---

## 3. World / Display / Window

### `Window2` vs `Window` — ★★★

`window.h` is deprecated. `Window2` (`world/display/window2.h:32`) has the same surface (`getHandle`, `getLocation`, `isWindowPartVisible`) plus one new static helper:

- window2.h:82 `static void getScreenRect(const std::shared_ptr<SR::Window2>& window, const SR_recti& monitorRect, int screenWidth, int screenHeight, int& rectX, int& rectY, int& rectW, int& rectH)`

Computes a window's rect relative to the SR display in screen coordinates. **Replaces hand-rolled `RECT` arithmetic SR Loom does today** when computing the weave region. We currently do this manually via `GetWindowRect` + the SR display rect. **★★★**

### `IDisplayManager` / `IDisplay` (display.h:127, 236) — ★★★★

This is the new-generation display API. Key extras over the legacy `Display::create()` path:

- display.h:142 `virtual uint64_t identifier() const = 0` — stable display ID, survives reconnect. Lets us bind to "the SR display" persistently. **★★★★**
- display.h:135 `virtual bool isValid() const = 0` — quick liveness check (use in tandem with `SystemSense::DisplayNotConnected`). **★★★**
- display.h:219 `virtual void getDefaultViewingPosition(float& x, float& y, float& z) const = 0` — the display's recommended sweet-spot. Could anchor a "calibrate / center" hint in the GUI. **★★★**
- display.h:265 `extern "C" SR::IDisplayManager* GetDisplayManagerInstance(SR::SRContext&)` — explicitly-exported C entry point. The lazy-binding variant `TryGetDisplayManagerInstance` (display.h:290) lets you fail gracefully if the runtime is too old. Worth using in SR Loom for forward-compat.

### `WindowsDisplayUtilities.h` — ★★★

Surprising: an entire Win32 display-introspection library shipped inside the SDK. About 25 functions. Highlights:

- WindowsDisplayUtilities.h:71 `bool isProductCodeSupported(std::string productCode)` — is this monitor's EDID product code on the known-SR list?
- WindowsDisplayUtilities.h:110 `bool getMonitorList(std::vector<MonitorData>& monitors)` — full enumeration with EDID, native res, etc.
- WindowsDisplayUtilities.h:147 `bool getKnownMonitors(...)` — filter to SR-compatible only.
- WindowsDisplayUtilities.h:224 `bool hasNonNativeResolution(const MonitorData&)` — flags the killer-of-3D condition before the SystemSense event fires.
- WindowsDisplayUtilities.h:238 `void waitForChangeMonitorRectangles(...)` — blocks on monitor topology change. Useful for "user moved windows / changed displays" handling.
- WindowsDisplayUtilities.h:248 `double getMonitorRefreshRate(HMONITOR)` — direct refresh-rate query, useful for `setLatencyInFrames` math.

SR Loom could replace its custom monitor enumeration with these. **★★★**

---

## 4. Management / Context

### `SRContext::NetworkMode` (srcontext.h:96-103) — ★★

The constructor takes a `NetworkMode` enum we've been ignoring:
```
EdgeMode, ServerMode, ClientMode, HybridMode,
StandaloneMode, NonBlockingClientMode (default)
```

Default `NonBlockingClientMode` "Try to connect once, throw exception" — explains some startup-crash reports we get when the SR service isn't running. Switching to `StandaloneMode` ("ClientMode if server reachable, else ServerMode") may let SR Loom run gracefully without the SR runtime present. Worth experimenting with for robustness. **★★★**

Also: srcontext.h:159 `SRContext(bool lensPreference, ...)` — you can request lens-ON at construction time without going through `SwitchableLensHint`.

### `Configuration` / `SenseConfiguration` (srconfiguration.h) — ★

Calibration metadata reader (`getDisplayHeight / getDisplayWidth`, `calibrate(Sense*)`). Mostly internal-use. **★**

---

## 5. Networking (network/) — ★

`NetworkInterface::send(uint64_t dest, void* payload, uint64_t size)` (networkinterface.h:33) and `Receiver::receive(SR_packet&)` (receiver.h:34). `SR_packet` is a 24-byte header + payload (packet.h:20). This is the internal IPC the SR runtime uses to ship sense data between processes — **not** a public "stream stereo over the network" API. We could theoretically piggyback custom packets, but there is no documented protocol. Skip unless we want to build a peer-to-peer SR control plane. **★**

---

## 6. Utility (utility/)

### `logging.h` — ★★★

- logging.h:38 `class Log` with `initialize(Verbosity, logPath, logFilePrefix)` (line 57), `initializeToFile(...)` (line 77).
- `Verbosity`: `HighVerbosity (0)`, `MediumVerbosity (1)`, `LowVerbosity (2, default)`, `NoLogging (3)`.
- logging.h:84-107 `debugInfo / info / warning / error` static methods.
- logging.h:120 `initializeCrashCallBack()` — installs the **Google Breakpad crash handler**. Surprising — explains why we sometimes see `crash_dumps/` directories appear. We could redirect this or disable it for our own crash-handler ownership.

This is the SR runtime's own log channel. SR Loom should call `Log::initialize(HighVerbosity, "<our-log-dir>", "srloom-")` early — gets us SR-runtime errors in our log without parsing stderr. **★★★**

### `exception.h` — ★★

- exception.h:35 `class Exception : public std::exception`
- exception.h:81 `class DeviceNotAvailableException`
- exception.h:96 `class ServerNotAvailableException` — thrown when the SR Service isn't running. SR Loom should catch this around `SRContext` construction (the SystemSense sample does, lines 105-112). Currently we let it crash to the v1.6 diagnostic logger. **★★★**

---

## 7. C admin API (admin_c.h, displays_admin_c.h, etc.) — ★★

The C API is mostly a thin wrapper over the C++ API, but a few admin-only things are not in C++:

- displays_admin_c.h:86 `char* getVersion(SR_switchableLensHintAdmin)` — lens firmware version.
- displays_admin_c.h:95 `char* getSerialNumber(SR_switchableLensHintAdmin)` — lens serial.
- displays_admin_c.h:106 `char* getAdditionalSerialNumber(...)` — up to 4 additional SNs.
- displays_admin_c.h:68 `disableByForce(SR_switchableLensHintAdmin)` — admin-tool force-off.
- displays_admin_c.h:118 `getNewDeviceSerialNumbers(...)` — blocks on display attach/detach.

These are intended for admin tools (calibration utilities, factory test). The serial-number read could be exposed in a tray "About this display" item. **★★**

`facetrackers_c.h` is **not** a separate face tracker — it bundles the C wrappers for `EyeTracker`, `HeadTracker`, and `PredictingWeaverTracker` (no extra fields beyond what's in the C++ headers). Despite the name, there is no standalone "face tracker" feature. **★ (rules out the question)**

`version_c.h:38` `const char* getSRPlatformVersion()` — returns `MAJOR.MINOR.PATCH.GITHASH`. Should be displayed in SR Loom's "About" dialog so we know what runtime users actually have. **★★★**

---

## 8. Surprises / under-documented findings

1. **Late latching is in the public API.** `enableLateLatching(true)` is one line. Most stereo-weaving apps don't realise this exists.
2. **`getPredictedEyePositions()` replaces our HeadPoseTracker subscription entirely** for the weaving path. The HeadPoseTracker we use is doing work the weaver already does internally.
3. **`SystemSense` exists and SR Loom isn't subscribed.** This is the runtime telling us "you can't weave right now because the display is duplicated / set to a non-native res / disconnected". We currently fail silently in all of these cases.
4. **`SwitchableLensHint` lets us turn the lens off when paused.** We never call this. The lens is presumably always on whenever SR Service is running, wasting power and possibly disturbing the 2D-mode experience for the user.
5. **Google Breakpad is built into the SR runtime** (logging.h:120). Could clash with v1.6's own crash handler.
6. **`Log::initialize()` must be called before `SRContext` construction** to capture early errors. We currently don't init the SR log at all.
7. **`Window2::getScreenRect()` does the screen-rect math we do by hand.**
8. **`SRContext` defaults to `NonBlockingClientMode`** which throws if SR Service is down. Switching to `StandaloneMode` may give us soft-fail behaviour.
9. **The weaver suite is mid-API-rewrite.** `IDX11Weaver1` (new) is missing ACT, contrast, and canWeave; those still live only on the deprecated `DX11WeaverBase`. If we want ACT today we must use the deprecated path; expect a `IDX11Weaver2` to add it.
10. **`OpenCV` is exposed in the public surface** (`Transformation`, `VideoFrame`, `SenseConfiguration` all use `cv::Mat`). If we ever touch the camera/calibration API we inherit an OpenCV dependency.
11. **The cameras are exposable to apps**, including shutter/gain control — useful for IR-camera debug overlays.
12. **`getPredictedEyePositions()` returns BOTH eyes** but our existing pipeline only uses head-position. Per-eye is what the weaver actually needs for stereo separation tuning; we could plumb this into a future "manual IPD override" feature.
13. **No CHANGELOG, no version.txt, no markdown docs** ship with the SDK. The only authoritative source for what's new is the headers + the `Copyright (C) 2025 Leia, Inc.` stamp on the new-style files (IWeaverBase, dx11weaver IDX11Weaver1, window2, IDisplay/IDisplayManager, lenshints sample). Everything else is `Dimenco` legacy.

---

## Top-10 "What to look at next" (ranked)

1. **`SystemSense` event subscription** (sense/system/) — react to LensOn/LensOff, UserFound/Lost, DisplayNotConnected, NonNativeResolution, Duplicated, ContextInvalid. Single biggest UX/robustness win. **★★★★★**
2. **`IWeaverBase1::enableLateLatching(true)`** — one-line latency reduction. **★★★★★**
3. **`IWeaverBase1::getPredictedEyePositions()`** — replaces our HeadPoseTracker subscription with the weaver-internal predicted positions; less code, lower latency. **★★★★★**
4. **`IWeaverBase1::setShaderSRGBConversion(read, write)`** — correctness fix for sRGB content (DX11 sample does this). **★★★★★**
5. **`SwitchableLensHint::enable() / disable()`** — turn the lens on when we start weaving, off when paused. **★★★★**
6. **`IWeaverBase1::setLatencyInFrames(n)`** — explicit, monitor-aware latency declaration. **★★★★**
7. **`IDisplayManager` / `IDisplay::identifier()` / `isValid()` / `getDefaultViewingPosition()`** — migrate from `Display::create()` to the new manager API. **★★★★**
8. **`SR::Log::initialize(HighVerbosity, …)` + catch `ServerNotAvailableException`** — capture SR runtime logs in our log file; graceful "SR service not running" handling. **★★★**
9. **`Window2::getScreenRect()`** — replaces hand-rolled screen-rect math. **★★★**
10. **`getSRPlatformVersion()` in About + `ApplicationSense::getApplicationNames()` in tray** — show users which SR runtime they have and which other SR apps are active. **★★★**

Honourable mentions outside the top 10: HDR via DX12 migration (★★★ but big effort), ACT exposure once on `IDX11Weaver1` (★★★★ when available, ★★ today), `Camera` + `CameraController` for an IR-debug overlay (★★), gesture-based UI shortcuts (★★).
