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
- **Required reprojection workflow (always): rebuild → reinstall → start KCD2:** commit every change, push it, run and wait for **Build (No Signing)**, then install the successful artifact into KCD2 (DRG as regression) via `scripts/install_latest.py --both` and launch KCD2 for live validation. Do not claim a reprojection change is validated before that game test; ask for its install path if it is not known.

## Versioning & Release (auto-update)

- **Single source of truth:** `OptiScaler/resource.h` — `VER_MAJOR_VERSION`, `VER_MINOR_VERSION`, `VER_HOTFIX_VERSION`, `VER_BUILD_NUMBER` (current `10.0.0.1`). `OptiScaler.rc` and `version_check.cpp` derive `VER_FILE_VERSION` / `CurrentVersion()` from it. `resource_build_date.h` / `resource_build_commit.h` are *generated* at build time by the MSVC pre-build PowerShell (date `yyyyMMdd_HHmmss` + `git rev-parse --short HEAD`) — never edit them by hand.
- **Workflow naming:** all `.github/workflows/*.yml` (`build.yml`, `just_build*.yml`, `release_debug.yml`, `test.yml`) run the same *Extract OptiScaler Version* PowerShell — it `Select-String` the four `VER_*` defines (first match) and produces `vMAJOR.MINOR.HOTFIX-preBUILD` → filename `OptiScaler_v10.0.0-pre1_YYYYMMDD.7z` (uploaded with `archive:false`, see Building).
- **Canonical bump script:** `scripts/bump_version.py` (Python 3, no extra deps).
  ```bash
  python scripts/bump_version.py --current                 # 10.0.0.1 (v10.0.0-pre1)
  python scripts/bump_version.py --bump-build              # 10.0.0.1 → 10.0.0.2 (most common: nightly)
  python scripts/bump_version.py --bump-hotfix             # 10.0.0.2 → 10.0.1.1
  python scripts/bump_version.py --bump-minor              # 10.0.1.1 → 10.1.0.0
  python scripts/bump_version.py --bump-major              # 10.1.0.0 → 11.0.0.1
  python scripts/bump_version.py --set 10.0.1.5            # explicit
  python scripts/bump_version.py --bump-build --changelog "Reproj telemetry phase 2"
  python scripts/bump_version.py --bump-build --dry-run    # preview without writing
  ```
  It rewrites only the four defines (preserving whitespace/comments), validates `0..65535`, and optionally prepends `Changelog.md` with `## vX.Y.Z (YYYY-MM-DD)`. Dry-run prints `[dry-run]` and touches nothing. Commit the two files it touches:
  ```bash
  git add OptiScaler/resource.h Changelog.md && git commit -m "Bump version to v10.0.0.2"
  ```
- **When to bump:**
  - `build` (pre) for every merge to `async-timewarp` / nightly — the normal case.
  - `hotfix` for a user-visible fix, `minor` for a feature (e.g., reproj telemetry), `major` for a breaking drop (rare — current `10`).
  - `AGENTS.md` and `docs/` describe *behavior*, not the version number — update them manually when behavior changes; the script only guarantees `resource.h` ↔ workflow filename ↔ `Changelog.md` stay in sync.
- **AGENTS.md auto-update hook:** the script can be extended to patch `AGENTS.md` if you add a marker `<!-- VERSION: 10.0.0.1 -->`. Today it only prints a hint (`git add …`) — keep AGENTS.md factual about architecture, not about the numeric version.
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

- **Deep Rock Galactic live-validation target:** `/var/home/whick/.local/share/Steam/steamapps/common/Deep Rock Galactic/FSD/Binaries/Win64/` next to `FSD-Win64-Shipping.exe`. Use this after every successful reprojection build unless told otherwise; do not install beside the root `FSD.exe` launcher.
- **Kingdom Come: Deliverance II secondary testbed:** `/var/home/whick/.local/share/Steam/steamapps/common/KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/` next to `KingdomCome.exe`.

1. For **Unreal Engine** games, install next to the *real* executable — usually `<game>/<Project>/Binaries/Win64/` — not the root launcher `.exe`.
2. Copy `OptiScaler.dll` -> `dxgi.dll` (default injection name), plus `OptiScaler.ini`, `OptiScaler/`, and `Licenses/` into that folder.
3. Add to Steam launch options: `WINEDLLOVERRIDES=dxgi=n,b %COMMAND%`.

**Auto-update script:** `scripts/install_latest.py` automates the Building → Installing flow (find latest successful `Build (No Signing)` run, `gh api …/zip > build.7z` (raw .7z, `archive:false`), `7z x`, copy to both games, backup old `dxgi.dll`):
```bash
python scripts/install_latest.py --both                  # DRG+KCD2, branch async-timewarp, fork Guillermode20/OptiScaler
python scripts/install_latest.py --both --dry-run          # preview
python scripts/install_latest.py --drg --ref my-feature --repo myfork/OptiScaler
python scripts/install_latest.py --both --run-id 32769762279  # explicit run
```
It respects `GH_TOKEN`/`gh auth` (`repo`+`workflow`), requires `7z`, and leaves `OptiScaler.ini` untouched (patch `[Reproj] Telemetry` manually or via `scripts/bump_version.py`). For the reprojection workflow always run the full chain: `git push` → `gh workflow run` → `gh run watch` → `python scripts/install_latest.py --both` → launch KCD2 (DRG as regression).

## Config / hotkey

- Overlay hotkey: `ShortcutKey` in `Config.h` (default `VK_HOME`; was `VK_INSERT`). INI equivalent: `[Menu] ShortcutKey=0x24`.
- The repo-root `OptiScaler.ini` is the shipped default. `auto` values resolve to the source default in `Config.h`.

## Async Reprojection (in-progress feature in this repo)

- **Current primary target is KCD2**; use DRG only as a regression comparison unless explicitly requested. KCD2-specific camera/UI/guard-band work is tracked in `docs/KCD2AsyncReprojectionPlan.md`.
- `docs/KCD2RealAsyncTimewarpPlan.md` defines the current late-pose architecture. The existing virtual-swapchain
  presenter is genuinely asynchronous, but packet camera poses are frozen at capture and the raw-mouse target is a
  prediction. `Kcd2Input.{h,cpp}` is the behavior-neutral M0 foundation: a passive, fail-closed hook of KCD2's generic
  input dispatcher records post-input-map mouse look and gamepad right-stick values for timing/calibration. It must
  never edit or block an event. Do not let it drive warps until its counts, timestamps, release behavior, and shutdown
  have been live-validated in KCD2.
- KCD2 camera acquisition uses a read-only `WHGame.dll` `CCamera::UpdateFrustumPlanes` hook in `framegen/reproj/Kcd2Camera.{h,cpp}`. KCD2 loads `WHGame.dll` after OptiScaler initializes, so installation retries lazily from reprojection packet capture once the module exists. It validates the gameplay camera through `CView` MSVC RTTI, then publishes Matrix34 pose/FOV via seqlock. The pose also carries the raw CCamera projection block (`+0x30..+0x7C`), live-validated on retail 1.5.6: near plane = float at `+0x54` (near-edge y, 0.05), far plane = float at `+0x6C` (far-edge y, 8000), projection-plane edge at `+0x5C` (`+0x60 = (1/tan(fov/2))·height/2`, x/z are raw half-extents), pixel aspect at `+0x40`; the ints at `+0x34/+0x38` are repurposed and must not be read as viewport dims. Near/far now feed `constants.cameraNear/Far` for validated depth math (RPD already corrected for D3D 0..1 depth + axial view-Z; the old OpenGL-style symmetric formula plus normalized-ray reconstruction was one cause of the earlier mode-1 double-warp). Depth mode is selectable again (configured mode honored with per-pixel rotation fallback for disocclusions) and input-predicted timewarp composes onto its target pose: `RP_Constants.targetYaw/targetPitch/targetFromInput` (2026-08-27, RPD/RP CSOs regenerated with dxc 1.9) replace the shader's basis extrapolation with the predicted rotation while the position lerp keeps rendered velocity; reconstruction always uses the rendered pose. Frustum callbacks are grouped into render-frame bursts: exact duplicates never advance history, but a changed pose or a >=8 ms gap does, so a stationary camera publishes a zero-velocity pair instead of extrapolating the last turn forever. Optional EMA smoothing (`[Reproj] Smoothing=0..0.95`, 0=off) low-passes the camera angular velocity to counter jitter at the cost of a few ms lag. KCD2 poses currently force reprojection mode 2 (rotation-only): its depth exists but near/far are unavailable, and mode 1 caused severe camera+MV/depth double-warp artifacts. KCD2 async rotation warps use per-slot VELOCITY limiting (growth bounded by ReprojMaxTimeStep frame-units per source frame) instead of a hard displacement clamp: a displacement clamp freezes the image mid-turn during source stalls and snaps on anchor arrival, while the growth limiter keeps rotation smooth at bounded speed. The absolute step cap stays generic because KCD2's natural steps exceed 1.5 in normal play. Do not enable KCD2 depth mode until projection conventions and near/far are verified. Signatures/layout derive from MIT-licensed KCD2Tools/TPVCamera. Unknown builds must fail closed; never accept every frustum camera because the function also receives shadows/reflections/portals.
- Design docs: `AsyncReprojection.md` covers the original implementation; `AsyncReprojection_Continuation_Plan.md` covers the earlier roadmap; `docs/NativeAsyncTimewarpPlan.md` supersedes its presentation architecture. The virtualized-main-swapchain path is implemented but remains unvalidated in games.
- Code: `OptiScaler/framegen/reproj/AReproj_Dx12.{h,cpp}` and `OptiScaler/shaders/reprojection/RP_*`.
- Enabled by `FGOutput::Reproj` (`OptiScaler/State.h`).

Known issues / limitations:

- `AReproj_Dx12::Present()` uses `GetIndexWillBeDispatched()`, matching Streamline/FSR3/FfxApi's ahead-of-present resource slot and still resolving to the current upscaler slot when it has resources. The non-upscaler paths still need end-to-end validation.
- `ForceVsync` is applied before `AReproj_Dx12::Present()`, so the internally presented real frame receives the configured interval and tearing flags.
- The v2 depth-aware warp is sketch-quality; disocclusion confidence and the HUD epsilon need real-footage tuning.
- `ReprojCapAtHalfRefresh` controls whether the existing `FrameLimit` half-rate behavior is used for reprojection. Reprojection bypasses Reflex/XeLL limiter gating so that the selected cap is applied even when those limiters are inactive.
- `ReprojSourceFramerateLimit` (`[Reproj] SourceFramerateLimit`, `TargetRefresh` companion) caps **only** the virtualized async-reprojection game thread after a packet is published; `0 = uncapped`. Use `60` on a 120 Hz display for one new + one repeated anchor per refresh cycle without touching global `FramerateLimit`/`Reflex`/`XeLL`. Pacing uses an absolute deadline grid with late-frame/reset avoidance and is reported as `source.capHz`/`source.capError` in telemetry/overlay. The presenter thread is never paced.
- A nonzero `ReprojSourceFramerateLimit` is the sole source/anchor clock: every paced game present publishes an anchor. `NonBlockingAnchorSampling` only subsamples an **uncapped** source; running independent 60 Hz pacing and sampling grids aliases sub-millisecond phase jitter into periodic 30–33 ms anchor gaps. Source pacing uses a 0.2 ms spin tail while the latency-critical Proton presenter retains its separate 1 ms tail.
- Async reprojection is DISPLAY-anchored, not render-rate-anchored: the presenter fills every refresh slot after the newest real frame with a warp until a newer packet preempts it. Warp count is whatever the display needs (never bounded by `ReprojMaxWarpFrames`, which only bounds the synchronous fallback), and pose age never disables async warping, so aiming cadence is independent of render rate (warping an old anchor is exactly ATW over a stalled renderer). Async pacing is gated by BOTH the DXGI frame-latency waitable object AND a software deadline of `lastPresent + refreshPeriod`: Proton can signal the waitable whenever queue capacity is available (every ~3–4 ms with tearing), so the waitable alone must not drive present cadence - the software gate prevents warp floods and frozen output.
- The worker may use real DXGI backbuffers only while virtualization is active: the game must never receive or render into them. Without that ownership boundary, post-`Present` worker access remains unsafe.
- Async packets transition `FREE -> CAPTURING -> READY -> PRESENTING -> RETIRED -> FREE`; reuse requires both capture and presenter fences to complete. Stop/join the presenter before draining and releasing D3D12/DXGI objects. `IFGFeature::_cameraTimestamp` records source-pose age when camera data is captured.
- Main-swapchain virtualization requires flip model plus a frame-latency waitable object and remains experimental on Windows and Proton. Keep `ReprojAsync=false` as the shipped default until more games validate the path. On 2026-08-22, Deep Rock Galactic on Proton live-validated the async virtual-swapchain presenter: 3 virtual game buffers, successful worker presents, ~60 real + ~60 warp FPS, and ~0.1 ms game-present blocking. It can briefly fall back/reinitialize across context resets. Virtual buffers belong to the swapchain, so an FG context reset must stop the presenter but must not destroy/recreate those buffers unless the swapchain itself resizes or is released.
- The 2026-08-23 display-clock revision supersedes the earlier real-anchor-plus-warp cadence: publication only supplies anchors, the worker selects the newest completed anchor at a refresh slot, every valid visible output is warped, and it issues exactly one `Present(1, 0)` per slot. The revised cadence is not validated until a fresh build is visibly tested in DRG.
- Input-side late latch, auto-calibration, and input pose lag were REMOVED on 2026-08-23 (user testing: they made feel worse; the per-packet motion-grid readback on the presenter thread also degraded pacing). Warps then extrapolated the last two rendered poses by `TimeStep` only (RPD `s = 1.0 + TimeStep`); mode 3 and `RP_Constants::extrapolate` are gone. Root-caused 2026-08-27: the old auto-tracked sensitivity was fed by a broken `dot` lambda (`a[i]*a[i]`, ignoring `b`), calibrating gain against `atan2(|fwd|²,|fwd|²) = π/4` — a garbage ~4× oversensitive value, which explains the bad feel. Revisited and replaced by the opt-in input-predicted timewarp: `ReprojInputPredictor.{h,cpp}` calibrates per-axis mouse-counts→radians gain (median/MAD robust rings over rendered pose pairs) and, at warp dispatch, composes the rotation the camera will have at the display deadline from fresh raw mouse deltas via `OptiInput::GetRawMouseMotionAt` (atomics + one mutex, no GPU readback). Prediction REPLACES the velocity-extrapolation term in `PrepareRotationConstants(cb, true, yaw, pitch)` — never add it on top. Fallback ladder: prediction (confident + input-driven + hysteresis 0.55/0.35) → legacy `ApplyLateInput` (async) → velocity extrapolation → hold. Gates: mouse idle over the window or camera not recently responding (gamepad/cutscene/menus) falls back. `[Reproj] InputPredictor` (default off) + `InputPredictorResponse` (0.05..1 under-rotation knob for smoothed aim); `MouseSensitivityX/Y` > 0 bypasses calibration. Telemetry slots record `inputPredicted`/predicted yaw+pitch; both paths log `Reproj input predictor:` stats every 10 s (sync from DispatchWarp, async from the presenter loop with an applied-slots ratio). CRITICAL (fixed 2026-08-27): raw-input relative motion (WM_INPUT/GetRawInputData/GetRawInputBuffer via `AccumulateRelativeMouseMotionLocked`) now records into the timestamped `RawMouseMotionState`/`RawMouseHistory` — previously only the polled GetCursorPos delta stream did, which stays empty for cursor-locked raw-input games like KCD2, starving the predictor AND the legacy late latch of mouse deltas (dead totals → no calibration → feature silently inert). Sync-path limitation: the game thread is blocked inside Present during warp bursts, so WM_INPUT is not pumped mid-burst — deltas cover input up to the last pump; async (presenter thread) sees fully fresh totals.
- KCD2 does not use the generic confidence-flapping predictor. It evaluates fixed 0–40 ms post-map-input phase candidates against hooked camera-pair rotation with normalized least-squares residuals, requires a stable winner plus both calibrated axes, then locks the phase and robust median gains until reprojection reset. Late warps query input from `poseTimestamp-phaseOffset` onward; a recently stopped mouse produces an explicit hold instead of resuming stale rendered velocity. KCD2 velocity EMA smoothing is bypassed from the start of phase calibration so engagement cannot change the camera response used to fit the model. Logs report `KCD2 phase ... LOCKED` and the async applied-slot ratio.
- Synchronous reprojection now supports the same HUD composition as the packet path (`DrawUIOverFG=true` + Hudfix): `CopyLastFrame` copies the HUD-less source into `_lastColor` and stashes UI in `_uiColor`; `DispatchWarp` composites UI after the warp copy. Result: exactly one unwarped HUD instead of a timewarped baked-in HUD.
- Async-timewarp-on-KCD2 verdict (2026-08-22): wine/Proton's d3d12core access-violates on ANY use of a `FRAME_LATENCY_WAITABLE_OBJECT` swapchain in KCD2 — confirmed by minidump (AV in d3d12core, called from wrapped dxgi) after the redundant-resize skip already worked. DRG async works on the same Proton, so it is a CryEngine-usage-specific wine bug. Do not retry KCD2 with `Reproj.Async=true` unless wine is updated or the waitable requirement is redesigned. KCD2 runs sync reprojection (`Reproj.Async=false`) as its stable config. A later KCD2 startup repro reproduced at `CreateShaderResourceView` with `pResource=0x3`; the SRV hook now ignores low-address placeholders instead of forwarding them to VKD3D, but the next repro still crashed. Root cause found 2026-08-24: KCD2 requests a 2-buffer swapchain, and its factory path PRE-wraps the raw chain at a hook site (`WrappedIDXGISwapChain4` constructed with `gameBufferCount=0`); AReproj then reuses that wrapper via QueryInterface, so constructor-passed counts never apply and virtualization exposed all 3 coerced real buffers to a game that only fills 2 (its third slot held garbage `0x3`). Fix: the requested count is recorded in `State::Instance().reprojRequestedBufferCount` by FGHooks before coercion, and `WrappedIDXGISwapChain4::EffectiveGameBufferCount()` falls back to it for virtualization visibility plus GetDesc/GetDesc1 clamping. Also fixed an out-of-bounds loop in `InitializeReprojectionVirtualization` (it iterated over the real buffer count while writing into a vector sized to the visible count).
- KCD2 async live test (2026-08-24, 120 Hz display): async warping works (~55 FPS source -> ~114 FPS display, game-present block ~0.1-4 ms) but is juddery: `missed=6-37` slots per window and present-interval p95 13-17 ms vs the 8.33 ms slot. Root cause identified in logs: under Wine, `DXGI_FRAME_STATISTICS` advances per presented output, so missed slots inflate `_measuredRefreshPeriodMs` (observed drift 8.4 -> 9.6 ms), stretching the deadline grid and causing more misses - a self-reinforcing loop. Fix: presenter now only accepts a measured period SHORTER than the configured/target one (`std::min`), and KCD2's INI pins `TargetRefresh = 119.95`. Further anti-judder changes: presenter thread raised to `THREAD_PRIORITY_TIME_CRITICAL` (late wakes convert directly into skipped vblank slots), and dispatch lead adapts from actual post-wait headroom (up to 8 ms); never set it from GPU duration because that repeatedly reset it to 3 ms and defeated adaptation. KCD2 rotation extrapolation uses the exact timestamp interval of the previous/current hooked camera pair rather than source-present cadence, so caps and render jitter do not change angular prediction velocity. Frustum callbacks are grouped into render-frame bursts with an 8 ms duplicate gap: exact duplicates inside a burst do not advance history, but a stationary pose on a later frame does publish a zero-velocity pair so the last turn delta cannot creep indefinitely. Toggle-off behavior is expected, not a hang: disabling Timewarp removes FG so the game falls back to its own ~30 FPS sync presentation; re-enable activates within ~200 ms. The poseAge >1 s rows during menu toggling are ATW correctly holding the last anchor while the paused game produces no new frames.
- The menu may disable an unavailable Reproj option, but must never reset `FGOutput=Reproj` to `NoFG`: runtime capability can be transient during DX12/VKD3D startup. VKD3D's Vulkan overlay API is not the DX12 feature state; `AReproj_Dx12::EvaluateState()` / `currentFG->IsActive()` are authoritative.
- Reprojection screen-edge protection must preserve the real-frame pixel rather than clamp-sampling an off-screen warp coordinate: clamping stretches edge texels into obvious smears. The RP/RPD shaders feather to the original frame by source coverage. HUD composition requires *both* a compatible HUDless source and UI; compare normalized reprojection formats on both resources (sRGB/typeless normalize to UNORM) before choosing that path.
- KCD2 Scaleform HUD Isolation (live-validated 2026-08-26): KCD2 renders its Scaleform UI directly into the active DXGI swapchain backbuffer right before `Present()`. `Kcd2Scaleform` hooks `CScaleformPlayback::BeginDisplay`/`EndDisplay` via Detours and sets thread-local active state. Inside `ResTrack_dx12::hkOMSetRenderTargets`, the first OM binding within a display scope snapshots the clean 3D world into `hudlessTexture`, clears `uiTexture` with `(0,0,0,0)`, and redirects Scaleform's RTV to `uiTexture`. `AReproj_Dx12` feeds `hudlessTexture` for world timewarp and `uiTexture` for UI composition, executing `_renderUI->Dispatch()` post-warp on both real and warped frames to produce a crisp, unwarped HUD over the reprojected 3D scene.
- Reproj reports real/fake frame types at its internal present sites; the generic wrapped-swapchain fakenvapi block intentionally excludes it. The FGHooks present-skip flags are `thread_local`, so a worker present cannot bypass a concurrent game present. Reflex markers/sleeps remain intentionally limited to DLSSG.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).

## Reprojection Telemetry (telemetry_plan.md)

- Dedicated component `OptiScaler/framegen/reproj/ReprojTelemetry.{h,cpp}` owns a 512-slot fixed ring (~4.2 s at 120 Hz, <256 KB). Presenter thread is sole writer; menu/log reads a published snapshot. No allocation/vector/sort/format per slot; telemetry failure never fails presentation.
- QPC is canonical time domain (`ReprojClock`). GPU timestamps are calibrated via `ID3D12CommandQueue::GetClockCalibration` once per second; conversion uses `cpuQpc = calibratedCpu + (gpu - calibratedGpu) * qpcFreq / gpuFreq`. Invalid calibration is marked unavailable, never forced to zero.
- Per-slot record (`ReprojSlotRecord`) stores timing QPCs (loop, deadline, wake, waitable, packet selection, command recording, queue submit, present), DXGI stats, packet/prediction fields (raw/selected interval, anchorAge, timestep, mvScale, HUD flags), and result (outcome, miss cause). Sequence distinguishes stale ring data.
- Clean accounting: one terminal `ReprojSlotOutcome` per scheduled slot (Presented, SoftwareSkipped, WaitableTimeout, etc.). `representedSlots` captures coalesced skips without ring spam. Legacy `_metricsMissedDisplaySlots` is kept temporarily for comparison then removed.
- GPU heap is trace-sequence-indexed (`queryStart = traceIndex*2`). `ReserveGpuQueries` checks prior fence completion and skips telemetry for that slot rather than waiting. Fence association via SC fence; only after fence completes is readback mapped.
- Classifier assigns exactly one `ReprojMissCause` (CpuWakeLate, WaitableLate, CaptureNotReady, PresentQueueBacklog, WarpGpuSlow, PresentSlip, ClockCorrection, Unknown) with secondary bitmask. Successful present with `interval > 1.5*refresh` is a slipped slot.
- Aggregation publishes once per second: cadence, CPU, GPU, DXGI, prediction, cause totals. Uses fixed arrays and nth_element for p50/p95/p99 only at publish time. Log line is `ReprojTelemetry v=1 ...` with stable keys for parser.
- INI: `[Reproj] Telemetry` (master enable) and `TelemetryMissDump` (rate-limited 16-before/after slot dump via async log, 10 s throttle).
- Analysis tool: `tests/reprojection/analyze_telemetry.py` parses `OptiScaler.log` for telemetry and slot dumps, prints FPS/interval/queue distributions, clamp rate, path distribution, correlations, worst windows/slots. Fixture at `tests/reprojection/fixtures/telemetry_sample.log` guards log format regressions.
- Hard invariants for Commit 1: no per-slot allocation/log/fence wait/readback-before-fence, no presenter motion-grid readback, no display-clock/packet/timestep behavior change—instrumentation only. Behavior changes are isolated follow-ups.
- Overlay shows effective path (MV/depth/rotation) and truthful resource lines (velocity/depth/camera basis/constants) with warnings for missing camera, invalid depth, frequent clamping, queue dominance, polluted DXGI, missing calibration.
