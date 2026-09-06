# AGENTS.md

Guidance for AI coding agents (and contributors) working on this repository.

## Project overview

- **OptiScaler** is a Windows DLL injected into DX11/DX12/Vulkan games to swap upscalers and add frame generation.
- **Toolchain:** C++20, MSVC (Visual Studio 2022). Windows-only. Solution: `OptiScaler.sln`.
- The project builds to `x64/Release/a/`, which is the full distributable (DLL + ini + runtime libs + setup scripts).

## Building (important — there is no local build on Linux)

This is an MSVC-only project. It **cannot** be built on a Linux box (no `windows.h`/Windows SDK, no MSVC, no Wine). Use GitHub Actions instead:

1. Push the branch to your fork, e.g. `git push fork <branch>`.
2. Trigger the unsigned workflow on that branch:
   ```bash
   gh workflow run "Build (No Signing)" --repo <owner>/OptiScaler --ref <branch>
   ```
3. Watch it to completion:
   ```bash
   gh run watch <run_id> --repo <owner>/OptiScaler --exit-status
   ```
4. Download and extract the artifact (it is a `.7z`, **not** a zip):
   ```bash
   gh api repos/<owner>/OptiScaler/actions/runs/<run_id>/artifacts   # find the artifact id
   gh api repos/<owner>/OptiScaler/actions/artifacts/<id>/zip > build.7z
   7z x build.7z
   ```
   - The `/zip` endpoint returns the raw `.7z` bytes because the artifact is uploaded with `archive:false`. `gh run download` fails on it ("not a valid zip file") — use `gh api` instead.
   - The extracted contents are `OptiScaler.dll`, `OptiScaler.ini`, `OptiScaler/`, `Licenses/`, and the setup scripts.

Notes:
- The `Build` (signed) workflow needs a `SIGNPATH_API_TOKEN` secret. Use **"Build (No Signing)"** on forks.
- `gh` must be authed with `repo` + `workflow` scopes to trigger runs and download artifacts.
- **Required reprojection workflow (always): rebuild → reinstall → start KCD2:** commit every change, push it, run and wait for **Build (No Signing)**, then install the successful artifact via `scripts/install_latest.py --both` and launch KCD2 for live validation. Do not claim a reprojection change is validated before that game test.

## Versioning & Release (auto-update)

- **Single source of truth:** `OptiScaler/resource.h` — `VER_MAJOR_VERSION`, `VER_MINOR_VERSION`, `VER_HOTFIX_VERSION`, `VER_BUILD_NUMBER`. `OptiScaler.rc` and `version_check.cpp` derive `VER_FILE_VERSION` / `CurrentVersion()` from it. `resource_build_date.h` / `resource_build_commit.h` are *generated* at build time by the MSVC pre-build PowerShell (date `yyyyMMdd_HHmmss` + `git rev-parse --short HEAD`) — never edit them by hand.
- **Workflow naming:** all `.github/workflows/*.yml` run the same *Extract OptiScaler Version* PowerShell — it `Select-String` the four `VER_*` defines (first match) and produces `vMAJOR.MINOR.HOTFIX-preBUILD` → filename `OptiScaler_v10.0.1-pre19_YYYYMMDD.7z` (uploaded with `archive:false`, see Building).
- **Canonical bump script:** `scripts/bump_version.py` (Python 3, no extra deps).
  ```bash
  python scripts/bump_version.py --current                 # print current version
  python scripts/bump_version.py --bump-build              # most common: nightly
  python scripts/bump_version.py --bump-hotfix
  python scripts/bump_version.py --bump-minor
  python scripts/bump_version.py --bump-major
  python scripts/bump_version.py --set 10.0.1.5            # explicit
  python scripts/bump_version.py --bump-build --changelog "What changed"
  python scripts/bump_version.py --bump-build --dry-run    # preview without writing
  ```
  It rewrites only the four defines (preserving whitespace/comments), validates `0..65535`, and optionally prepends `Changelog.md` with `## vX.Y.Z (YYYY-MM-DD)`. Dry-run prints `[dry-run]` and touches nothing. Commit the files it touches:
  ```bash
  git add OptiScaler/resource.h Changelog.md && git commit -m "Bump version to v10.0.1.20"
  ```
- **When to bump:** `build` (pre) for every merge to the active dev branch (`async-simple` today; `async-timewarp` is the reference branch) / nightly — the normal case. `hotfix` for a user-visible fix, `minor` for a feature, `major` for a breaking drop (rare).
- **AGENTS.md auto-update hook:** the script can be extended to patch `AGENTS.md` if you add a marker `<!-- VERSION: ... -->`. Today it only prints a hint (`git add …`) — keep AGENTS.md factual about architecture, not about the numeric version.
- **CI gate:** `build.yml` nightly checks `resource.h` → filename; a mismatch fails `Extract Version`. If you edit `resource.h` by hand, run the script with `--dry-run` to sanity-check.

## Shaders

Each shader family has **three** representations that must stay in sync:

- `OptiScaler/shaders/<name>/precompile/<Name>.hlsl` — HLSL source on disk.
- `OptiScaler/shaders/<name>/<Name>_Common.h` — the same HLSL as an inline `std::string` (runtime-compile fallback when `UsePrecompiledShaders=false`).
- `OptiScaler/shaders/<name>/precompile/<Name>_Shader.h` — precompiled CSO as a C byte array.

After editing HLSL, regenerate the CSO header:

- Windows: `shaders/shader_tools/build_precompiled_shader.bat <Name>`
- Linux: download `linux_dxc_*.tar.gz` from [microsoft/DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler/releases), then:
  ```bash
  dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect <Name>.hlsl -Fo <Name>_Shader.cso
  python3 shaders/shader_tools/create_header.py <Name>_Shader.cso <Name>_Shader.h <Name>_cso
  ```

Register new files in `OptiScaler/OptiScaler.vcxproj` and `OptiScaler/OptiScaler.vcxproj.filters`.

## Installing into a game (Linux/Proton)

- **Kingdom Come: Deliverance II (primary validation target):** `/var/home/whick/.local/share/Steam/steamapps/common/KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/` next to `KingdomCome.exe`.
- **Deep Rock Galactic (regression only):** `/var/home/whick/.local/share/Steam/steamapps/common/Deep Rock Galactic/FSD/Binaries/Win64/` next to `FSD-Win64-Shipping.exe` — not the root `FSD.exe` launcher. For **Unreal Engine** games generally, install next to the *real* executable, usually `<game>/<Project>/Binaries/Win64/`.

1. Copy `OptiScaler.dll` -> `dxgi.dll` (default injection name), plus `OptiScaler.ini`, `OptiScaler/`, and `Licenses/` into that folder.
2. Add to Steam launch options: `WINEDLLOVERRIDES=dxgi=n,b %COMMAND%`.

**Auto-update script:** `scripts/install_latest.py` automates the Building → Installing flow (find latest successful `Build (No Signing)` run, `gh api …/zip > build.7z` (raw .7z, `archive:false`), `7z x`, copy to both games, backup old `dxgi.dll`):
```bash
python scripts/install_latest.py --both                  # DRG+KCD2, default branch async-timewarp, fork Guillermode20/OptiScaler
python scripts/install_latest.py --both --ref async-simple  # current dev branch
python scripts/install_latest.py --both --dry-run          # preview
python scripts/install_latest.py --drg --ref my-feature --repo myfork/OptiScaler
python scripts/install_latest.py --both --run-id 32769762279  # explicit run
```
It respects `GH_TOKEN`/`gh auth` (`repo`+`workflow`), requires `7z`, and leaves `OptiScaler.ini` untouched. For the reprojection workflow always run the full chain: `git push` → `gh workflow run` → `gh run watch` → `python scripts/install_latest.py --both` → launch KCD2 (DRG as regression).

## Config / hotkey

- Overlay hotkey: `ShortcutKey` in `Config.h` (default `VK_HOME`; was `VK_INSERT`). INI equivalent: `[Menu] ShortcutKey=0x24`.
- The repo-root `OptiScaler.ini` is the shipped default. `auto` values resolve to the source default in `Config.h`.

## Async Timewarp (async-simple; current dev branch)

Enabled by `FGOutput::Reproj` (`OptiScaler/State.h`). **KCD2 is the primary target; DRG is regression-only.** This section describes the `async-simple` pipeline that is checked out and validated here (single DIRECT presenter queue, inline game-DIRECT capture, rotation-only warps). `async-timewarp` is the reference branch carrying the richer multi-queue machinery (capture worker, COMPUTE warp queue, mid-frame world fence, adaptive controllers, UI borrow, hitch hold) — it is NOT what this branch builds; port from it only mechanisms that win a measured A/B here.
Code: `OptiScaler/framegen/reproj/AReproj_Dx12.{h,cpp}`, `AReprojPresenter.cpp`, `AReprojTiming.cpp`, `Kcd2*{.h,cpp}`; shaders `OptiScaler/shaders/reprojection/RP_*`.
Docs: `plans/async_simple.md` (roadmap: architecture freeze, hard invariants, P7–P11 phases), `plans/walking_judder_options.md`, `plans/timewarp_artifacts.md` (live options for the two remaining quality gaps), `AsyncReprojection.md` (original design), `plans/async_capture_queue_decoupling.md` (parent-branch queue architecture).

### Pipeline (steady state)

1. The game renders into virtualized backbuffers (`WrappedIDXGISwapChain4` private ring; KCD2 requests 2 buffers while virtualization coerces ≥3 — the game sees `EffectiveGameBufferCount()`, never the raw count). The async presenter owns the real DXGI swapchain and presents exactly once per display slot.
2. Each game `Present` captures inline on the game's DIRECT queue: one command-list submit copies color — the KCD2 HUD-less world snapshot when isolation is active, else the composed frame — and, with isolation, the isolated UI, into a packet slot (`CaptureFramePacket`). One `_uiFence` value gates the whole anchor (`packet.captureFenceValue`). No capture worker, no COPY/COMPUTE capture queue, no mid-frame world fence, and the virtual-buffer handoff carries no fence (`SubmitReprojectionBuffer(..., nullptr, 0)`) — the same-queue copy is already GPU-ordered before any later render into that virtual buffer.
3. The presenter (`PresenterMain`, `AReprojPresenter.cpp`) fills every display slot from a software completion clock (`nextDeadlineMs = presentedAt + refreshPeriod`, never DXGI frame statistics) plus the DXGI frame-latency waitable. Each slot picks the newest READY packet whose capture fence value has completed; if the newest capture is still in flight it re-warps the active anchor (`capWait`) — it never blocks on an unfinished capture, and pose age never disables warping (ATW over a stalled renderer is the point). Packet lifecycle `Free -> Capturing -> Ready -> Presenting -> Retired -> Free`; `AcquirePacket` sets `Capturing`, which lasts only for the inline capture inside the same game `Present()` — there is no worker phase and no standing `Capturing` state.
4. Every output is a rotation-only warp on the presenter's single DIRECT queue (`_presentQueue`, NORMAL priority — HIGH starved the game source on Proton/VKD3D, which serializes DIRECT queues), submitted via `DispatchPacketWarp`; repeated slots are warped unconditionally (no repeat-warp shed, no blit repeats, no UI borrow). The presenter thread runs `THREAD_PRIORITY_TIME_CRITICAL` (Windows). The isolated KCD2 HUD is composited unwarped in the same dispatch by RPD; without isolation the composed frame is warped (HUD included) — there is no “never warp a composed HUD” gate on this branch.
5. A CPU-signaled deferred latch fence (`_lateLatchFence`) parks the already-submitted warp command list until a fixed sample lead before the deadline, when the presenter writes the final rotation constants from fresh raw mouse motion and the latest KCD2 camera pose (`ApplyLateInput`, `DispatchPacketWarp`). The lead is FIXED, not adaptive, on this branch: `ReprojLateSampleLead` ≤ 0.5 → `LATE_LATCH_DEFAULT_MS` (3.0 ms), else the configured value clamped `[1.0, 20.0]` ms; the dispatch wake runs `min(max(latchLead + 1, 4), min(20, 0.75·period))` ms early. Per-slot late rotation is clamped to `0.11 rad` (~6.3°) and extrapolation `timeStep` to `2.5`. With no fresh input it falls back to rendered-camera angular-velocity extrapolation. Do not reintroduce adaptive leads without P8 measurements (`plans/async_simple.md`).
6. When the presenter is not running (feature inactive/paused, or permanently failed) or the chain is not virtualized, the game frame is presented unchanged: blit the virtual buffer to the real backbuffer and `Present` — never a generated-frame fallback. An occluded/minimized presenter backs off to ~20 Hz `DXGI_PRESENT_TEST` visibility probes, resets its deadline grid, and counts no missed slots (no burst on restore); sustained jammed `Present(1)` calls trip a watchdog that fails the presenter so the game downgrades instead of freezing.

### Tuning — config keys that exist on this branch

`[AsyncTimewarp] Enabled`, `TargetRefresh`, `SourceFramerateLimit`, `MouseSensitivityX/Y`, `Smoothing`, `LateSampleLead`, `HudIsolation`, `ContentInterpolation`, and `Kcd2RenderReservePercent` are the live keys. All are exposed in the in-game menu's **Output (Async Timewarp)** section. `ContentInterpolation` asks FSR frame generation for one midpoint, sends generated and real world content through the same final late rotation, and composites the isolated HUD unwarped. It is off by default and fails closed to ordinary timewarp: capture and Generate both sanity-check MV/dims/scales (a suspect anchor never dispatches), the midpoint pose/timestamp and FSR `frameTimeDelta` share one camera-interval denominator, and FSR receives the widened (rendered) FOV when a reserve is active. `Kcd2RenderReservePercent` temporarily widens the validated gameplay CView while CryEngine builds its frustum, then the presenter maps the player's original center FOV; this supplies genuinely rendered peripheral pixels without affecting Scaleform HUD. The applied reserve is captured per-packet at publication (never re-read from the hook global at display time) and reported as `guard=%` in the 1 Hz log; the hook logs reserve active/inactive transitions so a dead widen is visible without screenshots.

| INI key | Default | Meaning |
|---|---|---|
| `[AsyncTimewarp] Enabled` | true | Master switch (menu checkbox also flips `FGEnabled`) |
| `[AsyncTimewarp] TargetRefresh` | 0 (= display) | Presenter slot cadence in Hz |
| `[AsyncTimewarp] SourceFramerateLimit` | 0 (uncapped) | OPT-IN source cap for the 60→120 A/B. Paces the game-present thread onto an absolute deadline grid (`FrameLimit::paceReprojectionSource`, `OptiScaler/misc/FrameLimit.cpp`) only on the running-virtualized publication paths, after the anchor handoff and outside `block=` metrics (the metrics scopes are always closed before the pacing sleep — the lock must never be held across it). Overshoots advance the grid (no 57–58 FPS drift); no generic-`FramerateLimit` fallback. Live-validated v10.0.1-pre45. Keep 0 unless A/B testing |
| `[AsyncTimewarp] MouseSensitivityX/Y` | 0 (= auto-track from rendered pose pairs) | Radians per raw-mouse count for the late latch |
| `[AsyncTimewarp] Smoothing` | 0.25 | EMA on KCD2 camera angular velocity; `0` = off |
| `[AsyncTimewarp] LateSampleLead` | 0 (= fixed 3 ms) | ms before the deadline to release the deferred latch. `≤0.5` = auto (fixed 3 ms default); a value `>0.5` overrides, clamped `[1, 20]` ms |
| `[AsyncTimewarp] HudIsolation` | true | KCD2: separate Scaleform HUD, composited unwarped. `false` = warp the composed frame (HUD included) |
| `[AsyncTimewarp] ContentInterpolation` | false | Generate one FSR midpoint from the isolated world, depth, and motion vectors. Generated and real content both receive final late rotation; missing inputs or FFX failure fall back to ordinary timewarp |
| `[AsyncTimewarp] Kcd2RenderReservePercent` | 8 | Genuine rendered world reserve per side (`0..15%`). Widens only the validated KCD2 gameplay CView, preserves the original presented center FOV, and leaves isolated HUD unchanged |

Hardcoded constants agents must know: warp timestep clamp `2.5` (`maxTimeStep`, `AReprojPresenter.cpp`); per-slot late-rotation ceiling `0.11 rad`; deferred-latch fixed lead `LATE_LATCH_DEFAULT_MS = 3.0`, bounds `LATE_LATCH_MIN_MS/MAX_MS = 1.0/20.0` (`AReproj_Dx12.h`); presenter grid sleep keeps a 1.0 ms spin window on Proton (`FrameLimit::sleepForPrecisePacingMs`); presenter thread `THREAD_PRIORITY_TIME_CRITICAL`, present queue NORMAL priority on Linux. `kAsyncSimpleStage = 1` (`AReproj_Dx12.h`): ≥1 means every slot warps; bump it to 0 only for a one-off identity-presenter A/B.

### Reading the once-per-second log line

`Reproj: source=… FPS display=… FPS (new=… repeat=…) missed=… interval=mean/p95ms latchLead=…ms poseAge=…ms queue=… late=applied/samples maxDeg=… dropAnchor=… capC=… capWait=… (async virtual swapchain|safe sync, block=…ms) generated=…` (exact format in `AReproj_Dx12.cpp` `LogMetricsIfDue`; pinning tests assert its pieces).
Healthy at 60 Hz source / 120 Hz display (the 60→120 A/B): `source` ≈ 60, `display` ≥ ~119, `missed` < 1% of slots after warm-up, roughly equal `new`/`repeat`, `dropAnchor=0`, low `block` (the opt-in source-cap sleep is outside `block=`), `latchLead` = the fixed 3.0 ms default (or configured override), `late` applied and nonzero during motion. `capC` = inline DIRECT captures; `capWait` = slots that reused the active anchor because the newest capture fence had not completed (nonzero is fine as long as the presenter never stalls); `dropAnchor` = skipped publications (packet exhaustion / busy capture allocator); `queue` = READY packet queue depth; `maxDeg` = largest single-slot late rotation; `poseAge` = anchor pose age at target display time. Keep the keys stable — do not add per-slot allocation/log/fence-wait/readback to the hot path.

### HUD composite is single-submit (no UI borrow)

With KCD2 isolation active, color and UI are copied in the same inline submit on the game DIRECT queue, so the UI is always as fresh as the color — the presenter never borrows a previous anchor's UI (`uiBorrow` is gone) and the UI trails nothing. The swap-smooth prev-color blend is also gone (parent-branch v40+): sampling a previous anchor's image under the current anchor's homography misaligns by the full inter-anchor rotation — a visible ghost/double during look-around. Do not re-add either without a second per-anchor UV bake or a second readiness pipeline; both were removed for latency/cadence reasons documented in `plans/async_simple.md`.

### Invariants (do not break)

- The game thread never CPU-waits on reprojection GPU work: packet exhaustion AND a busy capture allocator drop the anchor (signal, advance, count `dropAnchor`) instead of stalling. The virtual-buffer handoff is always fence-free (`SubmitReprojectionBuffer(..., nullptr, 0)`) — never fall back to passing `completionFence`, which reintroduces a CPU wait. The opt-in source-cap pacer is the ONLY sleep on the game present path; it runs only on the running-virtualized publication paths, after the anchor handoff, and never waits on a GPU fence.
- Packet lifecycle is `Free -> Capturing -> Ready -> Presenting -> Retired -> Free` (`AcquirePacket` marks `Capturing`; a skipped anchor returns to `Free`). Recycling waits on the packet's capture (`_uiFence`) and retirement (`_scFence`) values. Stop/join the presenter before draining/releasing D3D12/DXGI objects, and on any drain/shutdown signal the outstanding `_lateLatchFence` value so `_presentQueue` can never stay parked behind it.
- The worker/presenter may touch real backbuffers only while virtualization is active; the game must never receive or render into them. Virtual buffers belong to the swapchain — an FG context reset stops the presenter but must not destroy them unless the swapchain resizes or is released.
- Latest completed anchor wins. The presenter never blocks on an unfinished capture (`capWait` re-warps the active anchor) and repeated display slots are real warps, never blits.
- The presenter owns cadence: the next deadline derives from Present-completion timestamps (`presentedAt + refreshPeriod`); DXGI frame statistics are never used for phase correction (Wine advances them per presented output, not per physical vblank). On Proton the frame-latency waitable alone fires on queue capacity and must not drive cadence — poll it non-blocking or with a bounded timeout.
- `PrepareRotationConstants` CPU-bakes the complete output-pixel → source-UV homography into the shader-private `prevCameraRight/Up/Forward` rows; RPD consumes those rows directly — do not restore per-pixel FOV/camera-ray reconstruction.
- Screen edges feather to the source frame by source coverage — never clamp-sample off-screen warp coordinates (edge smear). The final warp is always rotation-only. Depth and motion vectors belong only to the optional FSR content generator and never alter the final homography.
- The menu may disable an unavailable Reproj option but must never reset `FGOutput=Reproj` to `NoFG` (capability is transient during DX12/VKD3D startup; `EvaluateState()` / `currentFG->IsActive()` are authoritative).
- Reproj reports real/fake frame types at its own present sites; the generic wrapped-swapchain fakenvapi block intentionally excludes it. FGHooks present-skip flags are `thread_local`. A reprojection output never reaches the generic `FrameLimit::sleep` half-rate limiter.
- Do not retry: CPU-blocking queue-arrival latching (cadence regressed ~2% → 7% drops) or a high-priority presenter queue on Linux (starved source rendering to ~35 FPS). One DIRECT presenter queue at NORMAL priority is the fixed topology.

### KCD2 specifics

- Camera comes from a `WHGame.dll` `CCamera::UpdateFrustumPlanes` hook (`Kcd2Camera.{h,cpp}`; signatures/layout derive from MIT-licensed KCD2Tools/TPVCamera). The DLL loads after OptiScaler, so installation retries lazily from packet capture. Gameplay cameras validate via `CView` MSVC RTTI and publish Matrix34 pose/FOV via seqlock; **unknown builds fail closed** — never accept every frustum camera (the function also sees shadows/reflections/portals). When `Kcd2RenderReservePercent > 0`, the hook temporarily widens only that validated gameplay CView while the original function builds its frustum, restores the game-facing FOV immediately afterward, and publishes the applied reserve for the presenter mapping.
- Live-validated projection block (`+0x30..+0x7C`, retail 1.5.6): near = float `+0x54` (0.05), far = float `+0x6C` (8000), plane edge `+0x5C` (`+0x60 = (1/tan(fov/2))·height/2`, x/z raw half-extents), pixel aspect `+0x40`; ints at `+0x34/+0x38` are repurposed, not viewport dims. Near/far feed `cameraNear/Far`.
- The final warp is rotation-only. Translation (walking/hills) remains a limitation when content interpolation is off; the optional FSR midpoint is the only path intended to add scene-content motion.
- Frustum callbacks group into render-frame bursts: exact duplicates never advance history, but a changed pose or a ≥ 8 ms gap does, so a stationary camera publishes a zero-velocity pair instead of extrapolating the last turn forever. Extrapolation divides by the exact hooked camera-pair interval, never by present cadence.
- Late-latch yaw composes around CryEngine world Z, then pitch around the yawed camera-right axis (generic cameras keep local-up). Never yaw around the camera's local up — it tilts with pitch and rolls during horizontal pans while looking up/down.
- Packet mouse baselines come from raw-input totals captured atomically with the KCD2 camera callback; generic cameras retain the timestamped-history fallback. The polled cursor stream is empty for cursor-locked games, so steering depends on raw input: WM_INPUT accumulation plus a dedicated pump thread (`WH_MOUSE_LL` passive observer at ~1 kHz) that must never become a second `RegisterRawInputDevices` (on Wine that steals the game's raw stream and freezes its camera). The pump is the sole relative-motion accumulator while delivering, self-expiring after 250 ms quiet; it stops with the presenter.
- HUD isolation (`Kcd2Scaleform` + `Kcd2HudIsolation`, live-validated): KCD2 renders Scaleform UI into the active backbuffer just before `Present()`. `BeginDisplay`/`EndDisplay` hooks snapshot the clean 3D world into `hudlessTexture`, clear `uiTexture`, and redirect Scaleform's RTV there. `CaptureFramePacket` consumes both textures in its one inline submit (`packet.hasUi`); the unwarped HUD is composited by the warp shader. When the presenter is down and the frame blits to the real chain (`BlitGameFrameToReal`), the isolated UI is composited by `RUI_Dx12` (`_renderUI`) so the fallback also yields one unwarped HUD.
- Swapchain quirks: KCD2 requests 2 buffers while virtualization coerces 3 — visibility/clamping goes through `EffectiveGameBufferCount()` (recorded pre-coercion request), never the raw count. The SRV hook ignores low-address placeholder resources (e.g. `pResource=0x3`) instead of forwarding them to VKD3D.

### Removed on async-simple — do not re-add unless a measured A/B wins

Capture worker + dedicated COPY capture queue, mid-frame world fence, COMPUTE warp queue, UI borrow (`uiBorrow` / `_heldPacketIndex`), repeat-warp shed (`EvaluateRepeatWarpShed`, stall/cadence EMAs, `RepeatWarp` key), swap-smooth prev-color blend, adaptive late-sample/dispatch controllers, per-slot telemetry (`ReprojTelemetry v=1`, `ReprojPipe`, slot dumps), `Kcd2Input`, `ReprojInputPredictor`, `TargetPoseResolver`, velocity clamps, and the full positional depth/MV warp. The deliberate exceptions are the opt-in source cap and the self-contained one-midpoint `HybridFsrGenerator`; neither changes queue topology or the final rotation-only warp.

### Stall behavior (no hitch hold)

`Hitch hold` was deleted in the async-simple simplification. A publish stall simply clamps extrapolation to `maxTimeStep = 2.5`; late-latch mouse steering ignores the timestep and stays live through the freeze. Do not reintroduce hold machinery unless P8 (`plans/async_simple.md`) shows a need.

### Current status

- **v10.0.1-pre45 (2026-09-05, live-validated):** opt-in `[AsyncTimewarp] SourceFramerateLimit` source cap (`FrameLimit::paceReprojectionSource` — absolute deadline grid with cadence recovery, Proton spin tail) plus the live ATW menu controls (source cap, late sample lead, mouse sens X/Y, KCD2 HUD isolation) under Output (Async Timewarp). The 60→120 A/B with `SourceFramerateLimit=60` on a 120 Hz display works well in KCD2: `source` ≈ 60, `display` ≈ 120, `new`≈`repeat`, no missed-slot bursts. Architecture-pinning tests updated to the opt-in contract (3 pacer call sites on the running-virtualized publication paths, none in FG_Hooks, no generic-`FramerateLimit` fallback; 59 local tests pass).
- **Why the cap stays opt-in (parent-branch history, 2026-09-04):** on `async-timewarp` a *default-on* 60 Hz source cap and a 60 Hz repeat-warp-shed floor both failed live (pacer overshoot left the source at 56–59 FPS on a GPU-bound scene, and shed turned repeat outputs into unwarped blits that read as cadence drops). Those results shaped the async-simple design — default `0`, pacer never on the non-running/fallback paths, always-warp repeats — they do not forbid an explicit A/B cap.
- **P0 fix (2026-09-05):** the opt-in source-cap sleep no longer holds `_metricsMutex` — each paced publication site now closes its metrics scope (block= counter) *before* `FrameLimit::paceReprojectionSource(true)`, so a game-thread pacing sleep can never stall the presenter's `RecordWarpFrame` / 1 Hz log / `GetRuntimeMetrics` behind the mutex.
- **v10.0.1-pre47 candidate:** removed the unvalidated edge extension, guard crop, depth residual, and continuity latch. Added one opt-in FSR-generated midpoint within the existing inline capture submit; generated and real world content both receive final late rotation and the isolated HUD is composited unwarped. Runtime validation remains mandatory before enabling it by default.
- Earlier async-simple stages: P1–P6 removed source pacing, capture worker/COPY queue, COMPUTE warp queue, repeat shed, UI borrow, hitch hold, and per-slot telemetry; warps enabled (stage 1) and the deferred fixed-lead latch landed. Next per `plans/async_simple.md`: P8 performance/queue-contention matrix (B0–B5, 60→120 first), then P9 warp-critical-path reduction.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).

## Reprojection telemetry

Detailed per-slot telemetry (`ReprojTelemetry v=1`, slot dumps, `cause.*`/`step.*` fields) was removed 2026-09-02, and the `ReprojPipe v=1` 250 ms aggregate (the RX 6600 XT source-dip instrument) went with the P4 simplification on async-simple. The once-per-second `Reproj:` line above is the ONLY emitter and the primary health instrument — keep its keys stable and do not add per-slot allocation/log/fence-wait/readback; the pinning tests in `tests/reprojection/test_display_clock.py` assert the line's exact pieces and the absence of the removed emitters. `tests/reprojection/analyze_telemetry.py` has been rewritten (2026-09-05) for the planned **RTv=2 sparse-event grammar** (`ReprojEv`/`ReprojHit`/`ReprojMeta` lines) owned by `telemetry_plan.md` — it parses that v2 grammar and ignores the old `ReprojTelemetry v=1` format; the v2 *emitter* (V1–V4 in the plan) is not yet implemented on this branch, so the analyzer currently reports "no telemetry" on real logs. The inert one-slot bookkeeping shim in the presenter exists only until its call sites are cleaned up.
