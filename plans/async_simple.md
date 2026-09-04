# async-simple: Brutally Minimal Async Timewarp Baseline

> **Branch:** `async-simple` (created from `async-timewarp` @ `b6668d95`).
> **Reference:** keep `async-timewarp` untouched; it is the machinery museum this
> branch strips from. Every line/function name below is from `b6668d95`.

## Implementation status

- **P1 (pacing removal + A0 gate): landed.** The reproj game thread is never paced:
  every `paceReprojectionSource` call site in `AReproj_Dx12.cpp` is gone along with
  the `FrameLimit` pacer itself (`sleepForReprojectionSourceMs`, `SourcePacingStats`,
  `reprojectionSourcePacingStats`); `FG_Hooks` never lets a reprojection output reach
  `FrameLimit::sleep`, so the fgActive half-rate rule cannot apply; `ReprojSourceFramerateLimit`
  defaults to `0` (INI `SourceFramerateLimit=0`); and `kAsyncSimpleStage = 0` implements the
  A0 identity stage below (presenter live, warp shader never dispatched). `ReprojTelemetry`
  reports no active source cap. Test pins updated (`tests/reprojection/test_display_clock.py`).
- **P2 (single-queue composed capture + FrameSlot[3]): landed.** `CaptureFramePacket` now
  copies exactly one composed frame (HUD included) inline on the game DIRECT queue's UI
  command list and records the `_uiFence` value as the single warp/readiness/recycle gate.
  The capture COPY queue, its worker thread, the mid-frame world fence, and the deferred
  handoff fences are deleted (`handoff` is unconditionally `nullptr/0` — same-queue
  ordering makes it fence-free); `ReprojHudIsolation` defaults to `false` so the isolation
  code is inert. The packet ring is trimmed to `kReprojFrameSlots = 3` (`FrameSlot[3]`).
  Warping is still gated behind `kAsyncSimpleStage` (0 = A0 identity-blit; 1 = warps on).
- **P3.4 (PresenterMain slim): landed.** `EvaluateRepeatWarpShed` + all shed state
  (`_repeatWarpShed`, stall/cadence EMAs, `_latestGameStallMs`, feed sites in the game
  present path) are deleted; `shouldWarp` is unconditional per slot when `kAsyncSimpleStage
  >= 1` (RepeatWarp absorbed — repeated slots always warp). ui-borrow (`_heldPacketIndex`,
  `_metricsUiBorrows`) and hitch hold (`_metricsHitchHolds`) are gone — on a real switch the
  previous anchor retires immediately. The dispatch lead is the fixed `kDispatchLeadMs =
  3.0` constant (the `_dispatchLeadMs` adaptive controller is deleted); the 1 Hz log dropped
  its `uiBorrow=`/`hold=`/`shed=`/`stallEma=` keys and `lead=` prints the constant.  Parser
  tests pinned the deletions (shed/borrow tests inverted to absence); 35/35 pass.
- **P3.5 (sync-fallback deletion): landed.** `PresentVirtualFrameSync`, `CopyLastFrame`,
  `DispatchWarp`, and the sync arrays (`_lastColor`/`_uiColor`/`_syncHasUi`/`_syncFence`)
  are deleted; `_renderUI` is never created by AReproj (RUI composite + the vestigial UI
  packet param are gone from `DisplayPacket`/`DispatchPacketWarp`). `Present()`'s inactive /
  paused / failed / non-virtualized paths are plain passthrough: presenter stopped, one
  same-queue `BlitGameFrameToReal` (virtual buffer → real backbuffer on the game DIRECT
  queue) when virtualization is up, then `PresentFrame`. No generated frame is ever
  presented. `_warpOutput` is kept. Parser tests re-pinned (sync-fallback pins inverted to
  absence, dispatch split re-anchored at `DrainGpuWork`); 35/35 pass.

  **P3 complete** — the runtime is Capture (one DIRECT UI list) + FrameSlot[3] + Presenter
  (one DIRECT SC queue, one warp shader, one `_scFence`). Warps stay gated behind
  `kAsyncSimpleStage` (0 = blit) until a CI + KCD2 pass validates this queue model.
- **P4 (telemetry trim): landed.** `ReprojTelemetry.{h,cpp}` deleted (slot records,
  snapshots, GPU queries, `TRACE_SLOT_COUNT`); the query heap/readback and
  `_presentTimestampFrequency` are gone from `CreateAsyncPresenter`; `_currentTelemetrySlot`,
  `GetTelemetry/GetTelemetrySnapshot`, every `RecordPipeline*` + `_pipe*` atomic, and the
  `ReprojPipe v=1` 4 Hz line are deleted, along with the now-dead
  `_reprojectionAdvanceWait*` stats in the wrapped swapchain (their only consumer was
  ReprojPipe). `DispatchPacketWarp`/`DisplayPacket` lost their `telemetryQueryStart`
  plumbing. The 1 Hz line is now:
  `Reproj: source=… display=… (new=… repeat=…) missed=… interval=mean/p95 lead=3.0 poseAge=… queue=… late=applied/samples maxDeg=… dropAnchor=… capC=… capWait=… (mode, block=…)`
  — dropped `sampLead`, `hud`, `latch/lateAge`, `sensX`, `pace` keys and their dead
  counters (`_metricsLateCam*`, `_metricsHudComposites`, `_metricsGamePresentPaceMaxMs`).
  `_trackedMouseSensitivityX` stays (ApplyLateInput steering still uses it).  Parser tests
  re-pinned (sampLead/hud/ReprojPipe → absence, new key set); 35/35 pass.
- **P5 (menu/config sweep) + stage flip: landed.** Live KCD2 session (2026-09-04)
  confirmed the A0 identity pipeline runs clean (async presenter ~100–115 display FPS,
  zero errors/downgrades) but by design never dispatches the warp — the user-visible
  "timewarp does nothing". `kAsyncSimpleStage` flipped **0 → 1**: rotation warps now run
  on the single DIRECT queue. The live reproj menu block (menu_common.cpp, outside the
  `#if 0` experimental block) toggled machinery deleted in P2/P3 — Source FPS cap,
  HUD Isolation, Allow Composed Warp, Warp Repeated Slots, Async Compute Queue,
  Non-blocking handoff, Adaptive late sample — and is trimmed to the real controls
  (Enable, Target refresh, Smoothing + metrics line). Dead config keys
  `ReprojAllowComposedWarp`/`ReprojNonBlockingHandoff`/`ReprojRepeatWarp`/
  `ReprojAsyncComputeWarp` removed from Config.h/Config.cpp;
  `ReprojHudIsolation`/`ReprojLateSampleLead`/`ReprojSourceFramerateLimit` stay as inert
  compat reads (Kcd2HudIsolation.cpp compiles reads of the first; tests pin the latter two).
- **P6 (HUD fix + adaptive late-sample rollover): landed.** The two pieces of parent
  machinery the user asked back are re-rolled onto the single-queue model.
  (a) **HUD isolation is live again**: `ReprojHudIsolation` defaults back to `true`
  (INI `HudIsolation=auto`; the stale `AllowComposedWarp`/`NonBlockingHandoff`/
  `RepeatWarp`/`AsyncComputeWarp` keys are dropped from the shipped INI).
  `CaptureFramePacket` queries `Kcd2HudIsolation::GetHudlessColor/GetUIColor` and, when
  valid for this backbuffer, copies the HUD-less world → `packet.color` AND the isolated
  UI → `packet.ui` in the **same inline submit** (one list, one `_uiFence` gate — the UI
  is always as fresh as the color, so no borrow, no world fence, no MarkFrameCaptured:
  same-queue ordering protects the copies). Composed capture remains the fallback.
  `DispatchPacketWarp` passes `packet.ui` to the warp so RPD composites the UI unwarped
  after the rotation warp (`hudlessSource` = premultiplied by default, parent semantics).
  (b) **Adaptive late-sample lead is back**: the `SAMPLE_LEAD_*` constants and
  `_lateSampleLeadMs` return, but the controller rides the presenter's own DIRECT queue —
  after submit the presenter waits for the warp on `_scFence`, measures headroom to the
  present deadline, and slides the next slot's dispatch/sample lead ±0.25 ms within
  [2.0, 6.0] (reduce > 2.0 ms headroom, grow < 0.9 ms), so the mouse is sampled as late
  as the warp allows. `ReprojLateSampleLead` > 0.5 overrides with a constant; `lead=` in
  the 1 Hz line reports the effective value. Parser tests re-pinned (isolation/controller
  pins inverted back to presence); 35/35 pass.

## 1. Goal

Reduce `AReproj` to the smallest runtime that still demonstrates async timewarp in
KCD2, then prove — with boring acceptance tests, not quality eyeballing — that none
of the machinery *around* the warp is what poisons the source cadence (the recurring
57–59 FPS dips). One captured texture, one presenter, one output:

```text
KCD2 renders frame
    ↓
copy final color (composed backbuffer)        ← Capture: ONE command queue
    ↓
publish latest frame (color + source camera + fence)
    ↓
return from Present immediately               ← NO pacing calls on the game thread

separate presenter thread:
    ↓
wait for display slot
    ↓
warp latest color (rotation-only)             ← Presenter: ONE queue, ONE warp shader, ONE fence
    ↓
present
```

Target data model:

```text
AReproj
│
├── Capture
│   └── one command queue                     (the game's DIRECT queue)
│
├── FrameSlot[3]                              (kReprojFrameSlots = 3)
│   ├── color                                 (full-res composed copy)
│   ├── source camera                         (RP_Constants pose data @ capture time)
│   └── fence                                 (one capture-completion value on _uiFence)
│
└── Presenter
    ├── one queue                             (_presentQueue, DIRECT)
    ├── one warp shader                       (RP_Dx12 + RPD.hlsl, unchanged)
    └── one fence                             (retirement on _scFence)
```

### Non-goals (cut, not ported)

- Source pacing of any kind (`paceReprojectionSource`, `SourceFramerateLimit`).
- Depth / MV warping. Already gone on the parent branch; stays gone.
- Translation warp. Rotation-only, exactly like the parent's proven baseline.
- Dedicated COPY capture queue + capture worker thread + world fence (mid-frame gating).
- Deferred COMPUTE warp + late-latch fence (CPU `_lateLatchFence` constant rewrite).
- Adaptive controllers: repeat-warp shed, adaptive dispatch lead, auto-tracked mouse
  sensitivity, KCD2 calibration. The adaptive **sample** lead is back (P6) — it rides the
  presenter's own DIRECT queue instead of the deleted compute deferred-latch path.
- Per-slot telemetry and the `ReprojPipe v=1` aggregate line (keep only the 1 Hz `Reproj:` line).
- Synchronous generated-frame fallback (`PresentVirtualFrameSync`, `DispatchWarp`,
  `_lastColor`/`_uiColor` sync capture). Non-virtualized → plain passthrough present.
- Generic (non-KCD2) upscaler-isolation resource capture (`FG_ResourceType::HudlessColor/UiColor`).
- The `FrameLimit::sleep(fgActive)` half-rate rule for reprojection outputs.
- Reproj menu controls (the surviving non-`#if 0` ones) — keys go inert, then out.

The warp math, KCD2 camera hook, swapchain virtualization, and DXGI present ownership
are **not** up for debate on this branch — they are the pieces that work.

### Deliberate deviations from mainline `AGENTS.md` invariants (branch-local)

1. **The HUD is composited unwarped via parent-validated isolation (P6).**
   `ReprojHudIsolation` defaults to `true`; when the KCD2 Scaleform split is live,
   CaptureFramePacket copies the HUD-less world + isolated UI in one submit and RPD
   composites the UI after the rotation warp — the parent's validated behavior on the
   simplified single-submit model (no borrow, no world fence). A composed frame (world +
   HUD) is warped only as the isolation-unavailable fallback; that fallback remains a
   deliberate deviation from mainline (mainline never warps a composed HUD) but is now
   the exception rather than the rule.
2. **The game thread is never paced by OptiScaler in reproj mode.** `SourceFramerateLimit`
   effectively becomes 0; KCD2's own limiter (or an external cap) owns source cadence.
3. **The packet ring is 3 slots** (`FrameSlot[3]`), not `BUFFER_COUNT == 4`.

## 2. Current inventory (what we are stripping)

Baseline `b6668d95`. All line numbers refer to that commit.

### 2.1 Queues (four today, plus the game queue)

| # | Queue | Owned by | Work it does today | async-simple |
|---|-------|----------|--------------------|--------------|
| 1 | Game DIRECT (`_gameCommandQueue`) | game | rendering; `_uiCommandList` capture fallback; sync UI submits | **Capture queue** (kept) |
| 2 | Present DIRECT (`_presentQueue`, in `CreateAsyncPresenter`) | presenter | SC lists: blits, unwarped UI raster, DIRECT-fallback warps | **Presenter queue** (kept, sole) |
| 3 | Warp COMPUTE (`_computeQueue`, `CreateAsyncPresenter`) | presenter | deferred-latch rotation warps via `RPD` | **delete** |
| 4 | Capture COPY (`_captureQueue`, `CreateAsyncPresenter`; falls back COPY→COMPUTE) | capture worker | DMA world+UI copies, world-fence gated | **delete** |

### 2.2 Fences / events / threads

| Object | Location | async-simple |
|--------|----------|--------------|
| `_uiFence` + `_uiFenceEvent`, `_uiAllocator[]`, `_uiCommandList[]` (base `IFGFeature_Dx12` path, game DIRECT) | `CreateObjects` | **keep** — this is the single capture queue+fence |
| `_scFence` + `_scFenceEvent`, `_scAllocator[]`, `_scCommandList[]` (DIRECT, per real output index) | `CreateObjects` | **keep** — the single presenter fence |
| `_captureFence`, `_captureInputFence`, `_worldFence`, `_captureFenceEvent` | `CreateAsyncPresenter` / `CreateObjects` | **delete** |
| `_lateLatchFence` | `CreateObjects` | **delete** (late latch becomes inline) |
| `_computeFence` + compute allocators/lists | `CreateAsyncPresenter` | **delete** |
| `_captureThread` / `CaptureWorkerMain` + `_captureWork*` | `AReproj_Dx12.cpp:1507–1680` | **delete** |
| `_presentThread` / `PresenterMain` | `AReprojPresenter.cpp:575–988` | keep, rewritten slim |
| Input pump (`OptiInput::StartRawInputPump`) | `StartAsyncPresenter` | delete for v0 (see §5.4); re-add later if wanted |

### 2.3 Frame slot → packet fields to delete (`AReproj_Dx12.h`, `ReprojFramePacket` + `ContentFrame`)

Delete: `ui`, `uiState`, `worldFenceValue`, `handoffFence/handoffFenceValue` (always null in
v0), `captureSrcColor/Ui/Composed` + states + `captureInputFenceValue`, `captureViaWorker`,
`colorFenceValue`, `sourceCutGeneration`, `inputLatchReady` (v0), `syncInterval`/`presentFlags`
(if unused after rewrite).

Keep: `color`+`colorState`, `captureFenceValue`, `frameId`, `constants` (RP_Constants =
source camera), `renderTimestamp`, `sourcePoseTimestamp`, `sourcePoseInterval`, `frameDelta`/
`rawFrameDelta`, `hasCamera`, `warpAllowed`, `sourceMouseX/Y/Timestamp` (for a later inline
late-latch phase), `state`.

### 2.4 Features and where they live

| Feature | Files / functions | Fate |
|---------|-------------------|------|
| Source pacing | `FrameLimit.cpp:174–259` (`paceReprojectionSource`), call sites `AReproj_Dx12.cpp:1118, 2536, 2566, 2659, 2739, 2795`; `ReprojSourceFramerateLimit` (Config.h:616) | delete calls; drop default to 0; keep `FrameLimit.cpp` helpers only for presenter sleeps |
| FG half-rate | `FG_Hooks.cpp:1395–1403` (`FrameLimit::sleep(reprojActive)`) | reproj passes `false` (or is excluded) so `min_interval_us *= 2` (`FrameLimit.cpp:108`) never applies |
| Capture worker path | `CaptureFramePacket` (1128–1496): isolation lookup (1138–1166), worker enqueue (1432–1467), UI fallback; `EnqueueCapture`/`ProcessCapturePacket`/`FailCapturePacket` | rewrite to: composed `gameBackBuffer` only → `SubmitUICommandList(packetIndex)` on `_uiFence` |
| World fence / mid-frame gate | `Kcd2HudIsolation::TakeWorldSignalValue/MarkFrameCaptured/SetWorldSignalContext`, `_worldFence` | stays deleted from AReproj (inline same-queue capture needs no mid-frame gate); the isolation files' world-fence code is inert — AReproj never calls `SetWorldSignalContext` |
| HUD isolation per-draw | `ResTrack_dx12.cpp:1144–1178`, `Hudfix_Dx12.cpp:464`, `dllmain.cpp:2101`, `CaptureFramePacket` | live: `ReprojHudIsolation=true` default; ResTrack OM hook + Hudfix `ArmForFrame` drive the split; capture copies world+UI in one submit (P6) |
| Presenter selection + capWait/uiBorrow/hold | `PresenterMain` (AReprojPresenter.cpp:575–988) | rewrite: newest READY w/ completed fence, else reuse active |
| Repeat-warp shed | `EvaluateRepeatWarpShed` (AReprojPresenter.cpp:501–573) + state in header | delete (always full warps on repeats) |
| Adaptive dispatch lead | `PresenterMain` lead control (~604–630) | constant lead (§5.5) |
| Adaptive sample lead | `SAMPLE_LEAD_*` consts + `DispatchPacketWarp` post-warp headroom | back (P6): rides the presenter DIRECT queue — `_scFence` wait after submit, ±0.25 ms in [2.0, 6.0] |
| Deferred late latch | `DispatchPacketWarp` | inline only: constants baked at dispatch; the adaptive lead makes the dispatch (and so the mouse sample) land as late as the warp allows |
| Sensitivity tracking/calibration | `UpdateMouseSensitivity` (868), `_kcd2Calibration*` fields | delete for v0 |
| Hitches/hold | `PresenterMain` `hitchHold` (~790) | omit in v0 |
| Sync fallback presentation | `PresentVirtualFrameSync` (2446), `CopyLastFrame` (320), `DispatchWarp` (1982), `_lastColor`/`_uiColor`/`_syncHasUi`/`_warpOutput` sync uses | delete; non-virtualized ⇒ `PresentFrame` passthrough only |
| Per-slot + pipeline telemetry | `ReprojTelemetry.cpp/.h`, `_pipe*` atomics, `RecordPipeline*` (2115–2250), `ReprojSlotRecord` | delete; keep only 1 Hz `LogMetricsIfDue` fields the log line prints |
| Metrics plumbing for removed controllers | `_latestGameStallMs`, `_stallEmaMs`, `_cadenceEmaMs`, `_repeatWarpShed`, `shed`/`stallEma=` log keys | delete |
| RUI overlay composite (`RUI_Dx12`) | `_renderUI`, `DisplayPacket`/`PresentVirtualFrameSync` UI dispatch | delete (no UI overlay) |
| Generic upscaler-isolation capture | `GetResource(HudlessColor/UiColor)`, `IsResourceReady` | delete from capture path |

### 2.5 What stays essentially untouched

- `IFGFeature_Dx12` interface surface (virtuals `AReproj_Dx12` overrides; callers in
  `FG_Hooks`, `State`, `menu`, `wrapped_swapchain`).
- `WrappedIDXGISwapChain4` virtualization: `IsReprojectionVirtualized`,
  `GetReprojectionBuffer`, `SubmitReprojectionBuffer`, `AdvanceReprojectionBuffer`
  (`wrapped_swapchain.{h,cpp}`). The game keeps rendering into virtual buffers; the
  presenter keeps owning the real chain. **Non-blocking handoff becomes unconditional**
  (no capture fence ever rides the virtual-buffer handoff — the capture only reads the
  *composed copy target* the game just finished, so ring reuse needs no GPU wait).
- `CreateSwapchain`/`CreateSwapchain1`/`ReleaseSwapchain` coercions (≥3 buffers, waitable,
  wrapper install), `SetGameBufferCount`, `EvaluateState` capability rules.
- KCD2 camera: `Kcd2Camera.{h,cpp}` (frustum hook, RTTI validation, seqlock, pose→constants
  via `ApplyToConstants`, `packet.constants.mode = 2` rotation literal at 1411).
- `RP_Dx12` + `RPD.hlsl` + `RP_Common.h` + `RPD_Shader.h` (three representations must stay
  in sync; no shader edit expected — `hudlessSource` stays 0 on every slot).
- `PrepareRotationConstants`, `FillConstants` (minus calibration), `ApplyLateInput` (kept for
  the later inline-latch phase), warp-output UAV texture (`CreateWarpOutput`).
- Occlusion/minimize backoff, presenter watchdog, and completion-clock pacing shape in
  `PresenterMain` (with adaptive control removed).
- fakenvapi `reportFGPresent` on real presents; `FGHooks::SkipPresent` framing in `PresentFrame`.
- Config parse/write plumbing for the keys that survive.

## 3. v0 behavioral spec (hardcode, do not re-generalize yet)

First make the 60 → 120 sequence perfect (per the guidance this branch came from):

```text
0.000  anchor A
0.008  warp A
0.016  anchor B
0.025  warp B
0.033  anchor C
```

Concretely:

1. **Source cadence: untouched.** No `paceReprojectionSource`, no `FrameLimit::sleep` with
   reproj active, no half-rate. KCD2 renders as fast as it wants / its own cap dictates.
2. **Presenter cadence:** one `Present(1, 0)` per display slot at `TargetRefreshHz()`
   (display refresh; `TargetRefresh` override honored — don't hardcode 120 into code, hardcode
   the *policy*: presenter owns display cadence, exactly one output per slot, new anchor or
   repeat, always warped).
3. **Every output is a warp** of the newest completed anchor (repeat slots included —
   `RepeatWarp` behavior unconditional). `timeStep` = (slot deadline − anchor renderTimestamp)
   / represented period, clamped to `[0, 2.5]`; no velocity filters, no EMA extrapolation
   beyond what `PrepareRotationConstants` already does from the captured pose pair.
4. **Warp gate = capture completion:** a slot only switches to a new anchor whose
   `captureFenceValue` is already `<= _uiFence->GetCompletedValue()` (CPU check, never a
   presenter-queue wait). Color and UI are one submit, so the single value gates the whole
   anchor (`hasUi` only marks the split-capture variant). Otherwise it re-warps the active
   anchor. No `uiBorrow` (UI is always as fresh as color), no `hold`.
5. **Late input:** `ApplyLateInput` runs inline at dispatch (freshest raw mouse + latest
   KCD2 camera pose). The dispatch lead is **adaptive** (P6): the presenter waits for the
   warp on `_scFence` after submit, measures headroom to the present deadline, and slides
   the next slot's lead ±0.25 ms within `[2.0, 6.0]` ms (`SAMPLE_LEAD_*`), so the sample is
   taken as late as the warp allows. `ReprojLateSampleLead` > 0.5 overrides.
6. **Dispatch lead bounds:** adaptive lead clamped into the slot window
   (`min(lead, max(3, min(20, 0.75·period)))`). Occlusion backoff and watchdog stay as-is.
7. **Capture:** on every game `Present`, `CopyResource` the composed virtual backbuffer into
   `FrameSlot[k].color` on the game DIRECT queue (one `_uiCommandList` submit, one `_uiFence`
   signal). If the slot's allocator is still busy (previous frame in flight), **skip
   publication** — advance the virtual ring, never wait (invariant below). Camera pose is
   captured at the same `Present` via the existing constants path.
8. **Deactivate/Stop order** unchanged: stop + join presenter (and any worker) before
   draining or releasing D3D12 objects.

### Invariants that survive the strip (hard rules)

- Game thread never waits on the GPU in the capture path: allocator busy ⇒ skip anchor.
- Virtual buffers belong to the swapchain; an FG context reset stops the presenter but does
  not destroy them (`DestroyFGContext` keeps them).
- Packet lifecycle `FREE → CAPTURING → READY → PRESENTING → RETIRED → FREE`; recycling needs
  capture completion (`_uiFence`) **and** presenter retirement (`_scFence`).
- Presenter stop/join precedes any release of queues/fences/packets.
- Rotation warp math and `PrepareRotationConstants`' baked homography rows are untouched.
- Unwarped fallback only when the real swapchain isn't virtualized or the presenter failed —
  then plain passthrough `PresentFrame`, no generated frames.

## 4. Acceptance ladder (do not skip stages)

> From the design conversation: *"Async mode enabled, warp amount = zero. KCD2 must behave
> identically to vanilla and hold the exact same 60 FPS."* until that passes, don't even
> test timewarp quality.

Implement behind one compile-time stage switch, e.g. `constexpr int kAsyncSimpleStage = 0..3`
in `AReproj_Dx12.cpp`, so each stage is one line to flip and the branch never needs
uncommitted hacks:

| Stage | What runs | Acceptance |
|-------|-----------|------------|
| **A0** (`0`) | Async presenter live, virtualization on, but the warp shader is **never dispatched** — every display slot identity-blits the newest completed anchor (capture still runs; it is the blit's source). Zero warp cost through the async plumbing. | Source FPS identical to reproj-off in the same scene (±1 FPS); no new `block`; toggle-off/on clean. |
| **A1** (`1`) | + capture (one copy per game present on game DIRECT queue). | Still identical source FPS; `block` ≤ ~1 ms over the A0 value; no dropped anchors. |
| **A2** (`2`) | + presenter owns the real swapchain at display cadence (blit outputs, no warp). | Display at target (~118–120); source untouched; no missed-slot bursts; alt-tab in/out clean. |
| **A3** (`3`) | + rotation warp on every output. | 60→120 cadence, `missed` < 2/s steady, `new`≈`repeat`, late-rotations sane while turning; **then** start judging warp feel. |

Measure and record, per stage, the four timings the design conversation calls for:
real frame interval, `Present()` blocking time, capture GPU time, warp GPU time — the 1 Hz
`Reproj:` line already carries source/display/block/pace; GPU times come from the warp
duration implied by `sampLead`/dispatch headroom or a temporary `EndQuery` pair (do not ship
per-slot queries).

## 5. Implementation phases (each phase ends compilable)

Work in `OptiScaler/framegen/reproj/` unless noted. Keep `AReproj_Dx12.h` fields and method
declarations in lockstep; delete dead includes.

### P1 — Disconnect OptiScaler from game cadence (A0)

1. Delete every `FrameLimit::paceReprojectionSource(...)` call in `AReproj_Dx12.cpp`
   (lines 1118, 2536, 2566, 2659, 2739, 2795 — `Present`, `SkipAnchorPublication`,
   `CaptureFramePacket` pacing tail).
2. Set `ReprojSourceFramerateLimit` default to `0.0f` (Config.h:616) and ship
   `SourceFramerateLimit=0` in the INI; keep reading the key so old INIs don't break, but the
   code path it fed is gone.
3. `FG_Hooks.cpp:1395–1403`: for reprojection outputs never call `FrameLimit::sleep(...)`
   (the virtualized branch already skips it — make the non-virtualized fallback pass
   `false` / skip too). `FrameLimit::sleep` half-rate stays DLSSG-only.
4. Keep `FrameLimit::sleepForMs`/`sleepForPrecisePacingMs` (presenter sleeps still need them);
   delete `paceReprojectionSource`, `sleepForReprojectionSourceMs`, and their stats when no
   callers remain. Update `tests/reprojection/test_display_clock.py` expectations that pin
   the pacer (the file already needs a rewrite per AGENTS.md — do it in this phase).
5. **Stage gate A0** blit-only path; run KCD2 (see §7 workflow), record source FPS.

### P2 — Single-queue capture, FrameSlot[3] (A1)

1. Rewrite `CaptureFramePacket` to ignore isolation: `color = gameBackBuffer` (composed,
   `D3D12_RESOURCE_STATE_PRESENT`), `packet.hasUi = false` always. Delete the
   hudless/UI lookups (1138–1166), the `_captureQueue` branches (1200–1467), the
   `usingKcd2Isolation`/`allowComposed` logic — composed warping is unconditional now.
2. Capture submit = `SubmitUICommandList((UINT) packetIndex)`; `packet.completionFence =
   _uiFence`; `packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex]`. Delete
   `colorFenceValue`/`worldFenceValue`/`handoffFence` gating (handoff always
   null/0 ⇒ `wrapped->SubmitReprojectionBuffer(idx, nullptr, 0)`).
3. Delete the capture-worker subsystem: `EnqueueCapture`, `CaptureWorkerMain`,
   `ProcessCapturePacket`, `FailCapturePacket`, `StopCaptureWorker`, `_captureWork*`,
   capture allocators/lists/fences/event, `WaitForCaptureAllocator`,
   `GetCaptureCommandList`, `SubmitCaptureCommandList`. (Keep `CaptureAllocatorReady`'s
   non-blocking "allocator busy ⇒ skip" semantics, now polling the `_uiAllocator` fence.)
4. Trim the packet ring to exactly 3: `static constexpr int kReprojFrameSlots = 3;` for
   `_packets[]` and slot math; per-real-output arrays (`_scAllocator*`) keep `BUFFER_COUNT`
   sizing (real chain can be 3+). Rework `AcquirePacket`/`RetirePackets`/`PacketQueueDepth`
   and the `Present()` skip paths to the new count; verify `Deactivate`'s force-free loop and
   `ReleaseObjects` iterate the right bounds.
5. Capture camera at present into the slot's constants exactly as today
   (`Kcd2Camera::ApplyToConstants`, `SetCameraData`, `FillConstants`); delete
   `Kcd2Scaleform::Initialize()` call (1317) and calibration accumulation (1328–1395);
   `warpAllowed = warpAllowed && hasCamera` (composed always allowed).

### P3 — Slim presenter, one queue + one fence (A2)

Status: **items 1–2 + 4 done** (queue/fence deletion + PresenterMain slim); items 3 and 5
remain (DisplayPacket RUI/query drop; sync-fallback deletion) and do not change the queue model.

1. ✅ `CreateAsyncPresenter` (AReprojPresenter.cpp:18–300): delete the COMPUTE queue +
   allocators/lists/fence block (99–230) and the COPY capture queue block (231–300). Only
   create `_presentQueue` (DIRECT, normal priority) and grab the present waitable +
   `SetMaximumFrameLatency(1)`. `DestroyAsyncPresenter` mirrors: release `_presentQueue`,
   no capture/compute teardown, no `_warpTimestamp*`.
2. ✅ Delete `DispatchPacketWarp`'s compute branch + deferred late latch (AReproj_Dx12.cpp:
   1785–1980) and `SubmitComputeCommandList`/`GetComputeCommandList`/
   `WaitForComputeAllocator`. Warp path = `GetSCCommandList(outputIndex)` on `_presentQueue`
   (`RP_Dx12::Dispatch` with `ui = nullptr`), copy `_warpOutput` → real backbuffer, submit via
   `SubmitSCCommandList`, retirement value on `_scFence` (this is the existing DIRECT fallback
   shape today's code already uses when `AsyncComputeWarp=false`). Also deleted: the
   `SAMPLE_LEAD_*` adaptive-lead constants, `_lateSampleLeadMs`, and the `_lateLatchFence`
   creation/release sites. `ApplyLateInput` still runs inline at dispatch (parent DIRECT
   behavior); the raw-input pump is untouched.
3. ✅ `DisplayPacket`: drop RUI composite + UI packet param — copy `packet.color` → real
   backbuffer on the same SC list; used only when warping is disallowed. (Telemetry
   timestamp-query writes remain until P4.)
4. ✅ `PresenterMain`: delete `EvaluateRepeatWarpShed` call/state, ui-borrow selection, hitch
   hold, adaptive lead updates. Fixed lead 3 ms (`kDispatchLeadMs`); always-warp repeats;
   select newest READY packet whose capture value completed, else reuse active; retire
   previous anchor only on a real switch (drop `_heldPacketIndex`). Keep occlusion backoff,
   watchdog, completion clock, `WaitForPresentSlot`, `PresentCompositorFrame` (minus
   waitable double-wait subtleties). The 1 Hz `Reproj:` line lost its
   `uiBorrow=`/`hold=`/`shed=`/`stallEma=` keys here (P4 trims the rest).
5. ✅ Delete the sync-fallback block: `PresentVirtualFrameSync`, `CopyLastFrame`,
   `DispatchWarp`, `_lastColor`/`_uiColor`/`_syncHasUi` sync arrays (kept `_warpOutput`
   per-output), RUI `_renderUI` creation, and their use in `Present()`. Non-virtualized /
   presenter-failed ⇒ `PresentFrame` passthrough + `_asyncDowngraded` + one
   `BlitGameFrameToReal` same-queue copy when virtualization is up.

### P4 — Trim telemetry to the 1 Hz line (A2/A3)

1. Delete `ReprojTelemetry.cpp` per-slot machinery usage: `_currentTelemetrySlot`,
   `GetTelemetry/GetTelemetrySnapshot` consumers in `Present()`/`PresenterMain`, `_warpTimestamp*`
   query heap, `RecordPipeline*` + all `_pipe*` atomics, `RecordWarpFrame`'s heavy fields.
2. Keep a hand-rolled 1 Hz summary log (source/display, new/repeat, missed, mean/p95 interval,
   block, warp FPS, pose age) — field names can diverge from the current `Reproj:` line once
   this branch's semantics differ (no pace/shed/stallEma/uiBorrow). Update the log keys here,
   and the docs (AGENTS.md `Reading the once-per-second log line` section) to match at the end.

### P5 — Config, menu, docs, dead-file sweep

1. Config.h: remove deleted keys (`ReprojSourceFramerateLimit`→ keep as inert 0 read, or drop;
   `ReprojAsyncComputeWarp`, `ReprojLateSampleLead`, `ReprojRepeatWarp`(absorbed),
   `ReprojHudIsolation` → default **false** branch-wide, `ReprojAllowComposedWarp` → default
   **true** branch-wide, `ReprojNonBlockingHandoff` absorbed-true). Keep `TargetRefresh`,
   `Enabled`, smoothing keys that still exist in code. Update Config.cpp + shipped INI.
2. menu_common.cpp: delete the reproj key toggles (3923–3930, 4153–4203) or leave behind the
   same `#if 0` treatment as the experimental block.
3. Dead-file pass (only after a clean CI build with code above): remove
   `Kcd2HudIsolation.{h,cpp}`, `Kcd2Scaleform.{h,cpp}`, `ReprojTelemetry.{h,cpp}` from the
   vcxproj + filters and their includes in `dllmain.cpp`, `ResTrack_dx12.cpp`,
   `Hudfix_Dx12.cpp`, `AReprojPresenter.cpp`. **Do this as its own commit**; until then the
   isolation files stay compiled but inert because every entry point gates on
   `ReprojHudIsolation` (verified: Kcd2HudIsolation.cpp:221, 304, 326, 345, 367).

### P6 — Later phases (explicitly not v0)

- Inline late-latch mouse (sample just before dispatch on the presenter thread; re-enable
  pump + `ApplyLateInput`), then decide if auto-sensitivity returns.
- Hitch hold re-added once the baseline cadence is proven.
- Anything the A0–A3 data shows is actually needed. **Do not add ahead of data.**

## 6. Cross-cutting hooks you must not break (verify by grep before CI)

```bash
# All consumers of the pieces being removed — every hit must be explained or gone:
rg 'paceReprojectionSource|sleepForReprojectionSourceMs' OptiScaler
rg '_captureQueue|_captureFence|_captureInputFence|_worldFence|_captureThread|_computeQueue|_computeFence|_lateLatchFence' OptiScaler/framegen OptiScaler/hooks
rg 'Kcd2HudIsolation|Kcd2Scaleform' OptiScaler   # expect: Config gates + files themselves until P5
rg 'ReprojSourceFramerateLimit|ReprojLateSampleLead|ReprojAsyncComputeWarp|ReprojRepeatWarp' OptiScaler
rg 'GetTelemetrySnapshot|RecordPipeline|_pipe[A-Z]|_currentTelemetrySlot|_repeatWarpShed|_stallEmaMs|_heldPacketIndex' OptiScaler/framegen/reproj
git diff --check
```

The base-class UI command-list path (`GetUICommandList`/`SubmitUICommandList`, `_uiFence`,
`_uiAllocator*`) lives in `IFGFeature_Dx12` and is shared with other FG features — do not
rename or re-layout it; only AReproj's use of it changes.

## 7. Build / validation workflow (mandatory)

Every stage flip (A0→A3) or phase commit that touches runtime code goes through the full
chain — no exceptions, no "small change" shortcuts:

1. Commit on `async-simple`, push to the fork (`git push fork async-simple`).
2. `gh workflow run "Build (No Signing)" --repo Guillermode20/OptiScaler --ref async-simple`
   then `gh run watch --exit-status`. Any compile error is fixed before the next stage.
3. `python scripts/install_latest.py --both --run-id <run_id>` (or `--ref async-simple`).
4. KCD2: same 1440p scene used for the 57–59 FPS reports. Record the 1 Hz line + source FPS
   with reproj **off** first, then per stage. DRG only as a final regression sanity check.
5. Version bumps: use `scripts/bump_version.py --bump-build` per merged stage, per repo rules.

## 8. Risks / open decisions

- **Warped HUD artifacts** (menus, subtitles, crosshair smear) are expected in v0 — that is
  the price of dropping isolation. Decide after A3 whether KCD2 isolation deserves a minimal
  *re-add* (one snapshot copy, no UI texture, warp world + blit UI) or stays out.
- **Presenter on DIRECT vs COMPUTE:** the parent branch moved warps to COMPUTE because VKD3D
  serialized DIRECT queues 10–17 ms behind the game. v0's whole premise is that with no source
  pacing and one warp per slot the DIRECT presenter behaves; if A2/A3 regresses display
  cadence, the first fix is re-checking `_presentQueue` priority/serialization, not
  re-importing the compute + deferred-latch machinery wholesale.
- **Uncapped source saturation:** if KCD2 runs 90+ FPS uncapped on the RX 6600 XT, the
  60→120 rhythm never forms. The acceptance scenes assume the game's own 60 cap is on; record
  source behavior under both and pick the primary scene accordingly.
- **`Kcd2Camera` unknown-build fail-closed:** unchanged behavior, but now it directly gates
  `warpAllowed` (no HUD fallback crutch). Fine — that is the fail-closed design working.
- **tests/reprojection:** the display-clock parser tests pin parent-branch behavior (pacer
  calls, isolation strings). They are stale per AGENTS.md; update them to the async-simple
  source, or move them out of the way in P1 and re-target later.

## 9. Merge / rollback notes

- `async-timewarp` stays the reference. Do not merge async-simple back until the composed-HUD
  deviation and pacing policy are consciously accepted or reverted.
- If `async-timewarp` advances, re-base async-simple by cherry-picking only the *proven*
  rotation/pacing-relevant fixes — never a wholesale merge of the machinery being stripped.
