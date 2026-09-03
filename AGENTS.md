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
- **When to bump:** `build` (pre) for every merge to `async-timewarp` / nightly — the normal case. `hotfix` for a user-visible fix, `minor` for a feature, `major` for a breaking drop (rare).
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
python scripts/install_latest.py --both                  # DRG+KCD2, branch async-timewarp, fork Guillermode20/OptiScaler
python scripts/install_latest.py --both --dry-run          # preview
python scripts/install_latest.py --drg --ref my-feature --repo myfork/OptiScaler
python scripts/install_latest.py --both --run-id 32769762279  # explicit run
```
It respects `GH_TOKEN`/`gh auth` (`repo`+`workflow`), requires `7z`, and leaves `OptiScaler.ini` untouched. For the reprojection workflow always run the full chain: `git push` → `gh workflow run` → `gh run watch` → `python scripts/install_latest.py --both` → launch KCD2 (DRG as regression).

## Config / hotkey

- Overlay hotkey: `ShortcutKey` in `Config.h` (default `VK_HOME`; was `VK_INSERT`). INI equivalent: `[Menu] ShortcutKey=0x24`.
- The repo-root `OptiScaler.ini` is the shipped default. `auto` values resolve to the source default in `Config.h`.

## Async Timewarp (in-progress feature in this repo)

One fixed pipeline, enabled by `FGOutput::Reproj` (`OptiScaler/State.h`). **KCD2 is the primary target; DRG is regression-only.**
Code: `OptiScaler/framegen/reproj/AReproj_Dx12.{h,cpp}`, `AReprojPresenter.cpp`, `AReprojTiming.cpp`, `Kcd2*{.h,cpp}`; shaders `OptiScaler/shaders/reprojection/RP_*`.
Docs: `AsyncReprojection.md` (original design), `docs/NativeAsyncTimewarpPlan.md` (presentation architecture), `docs/KCD2AsyncReprojectionPlan.md` (KCD2 work), `plans/async_capture_queue_decoupling.md` (queue architecture).

### Pipeline (steady state)

1. The game renders into virtualized backbuffers; the async presenter owns the real DXGI swapchain.
2. Each game present publishes an anchor packet on the game DIRECT queue: a full-resolution copy of the HUD-less world plus a copy of the isolated UI (`CaptureFramePacket`, completion = the submitted `_uiFence` value). No per-anchor queue pin.
3. The presenter fills every display slot with exactly one `Present(1, 0)` of the newest anchor whose capture fence has **already completed**; otherwise it re-warps its active anchor (counted as `capWait`). Warp count is whatever the display needs — never bounded by a warp cap — and pose age never disables warping (ATW over a stalled renderer is the point).
4. Warps are rotation-only camera timewarp on a dedicated normal-priority COMPUTE queue (VKD3D serializes DIRECT queues ~10–17 ms behind the game, so the warp must not run there), followed by a single backbuffer copy. The isolated HUD is composited unwarped in the same dispatch by RPD — exactly once per output.
5. A compute-fence late latch rewrites the warp constants ~3.5 ms before the present deadline from fresh raw mouse motion; with no fresh input it falls back to rendered-camera angular-velocity extrapolation.
6. If camera data or separate world/UI resources are missing, present the game frame unchanged. **Never warp a composed HUD.**

### Tuning — INI only, and only these keys

All other behavior (late latch, compute warp, input pump, timestep cap, dispatch lead) is hardcoded. The experimental reproj menu controls are compiled out (`menu_common.cpp` `#if 0` block) — do not re-enable them piecemeal.

| INI key | Default | Meaning |
|---|---|---|
| `[AsyncTimewarp] Enabled` | true | Master switch |
| `[AsyncTimewarp] TargetRefresh` | 0 (= display) | Presenter slot cadence in Hz |
| `[AsyncTimewarp] SourceFramerateLimit` | 60 | Caps only the virtualized game thread after publish; `0` = uncapped. `60` on a 120 Hz display ≈ one new + one repeated anchor per cycle |
| `[AsyncTimewarp] MouseSensitivityX/Y` | 0 (= auto-track from rendered pose pairs) | Radians per raw-mouse count for the late latch |
| `[AsyncTimewarp] Smoothing` | 0.25 | EMA on KCD2 camera angular velocity; `0` = off |
| `[FrameGen] DrawUIOverFG` | false | Required for HUD composition (with Hudfix) |

Hardcoded constants agents must know: warp timestep clamp `2.5`; late-latch lead `3.5 ms`; dispatch lead adapts from post-wait headroom within `[3.0, min(8, 0.75·period)]` ms; presenter thread runs at `THREAD_PRIORITY_TIME_CRITICAL`.

### Reading the once-per-second log line

`Reproj: source=… display=… (new=… repeat=…) missed=… interval=mean/p95 lead=… poseAge=… queue=… late=applied/sampled maxDeg=… hud=… dropAnchor=… capC=… capWait=… latch=lateCam/packetBase/fallback lateAge=…ms sensX=… hold=… (async virtual swapchain|safe sync, block=… pace=…)`.
Healthy at 60 Hz source / 120 Hz display: source 59–60, display ≥ 117, `missed` < 2/s after warm-up, roughly equal `new`/`repeat`, `dropAnchor=0`, low `block` (`pace` is the intentional cap sleep, reported separately), `late` applied and nonzero during motion, `hud` tracking displayed outputs. `capC` = DIRECT captures; `capWait` = slots that reused the active anchor because the newest capture was unfinished (nonzero is fine as long as the warp queue never stalls). `latch=` splits applied steering into late-camera-pose / packet-baseline / velocity-fallback slots; `lateAge` is the mean age of the late pose at use; `sensX` is the auto-tracked radians/count (sanity: stable, ~1–3e-4). `hold` counts slots frozen at timeStep 0 during a publish stall (see Hitch hold below); steady-state it must be 0.

### Invariants (do not break)

- Packet lifecycle is `FREE -> CAPTURING -> READY -> PRESENTING -> RETIRED -> FREE`. Recycling requires both capture completion and presenter retirement fences. Stop/join the presenter before draining/releasing D3D12/DXGI objects.
- The worker may touch real backbuffers only while virtualization is active; the game must never receive or render into them. Virtual buffers belong to the swapchain — an FG context reset stops the presenter but must not destroy them unless the swapchain resizes or is released.
- Pacing is gated by BOTH the DXGI frame-latency waitable AND a software deadline of `lastPresent + refreshPeriod` (on Proton the waitable alone fires on queue capacity, ~3–4 ms with tearing, and must not drive cadence). The presenter runs on a completion clock: the next deadline derives from Present-completion timestamps and DXGI frame-statistics phase correction is bypassed entirely, because Wine advances those statistics per presented output rather than per physical vblank.
- `PrepareRotationConstants` CPU-bakes the complete output-pixel → source-UV homography into the shader-private `prevCameraRight/Up/Forward` rows; RPD consumes those rows directly — do not restore per-pixel FOV/camera-ray reconstruction. Its interior fast path skips the original-frame load except in the edge-feather/disocclusion branch.
- Screen edges feather to the real frame by source coverage — never clamp-sample off-screen warp coordinates (edge smear). HUD composition requires *both* a compatible HUD-less source and UI; compare normalized formats (sRGB/typeless → UNORM) first.
- The menu may disable an unavailable Reproj option but must never reset `FGOutput=Reproj` to `NoFG` (capability is transient during DX12/VKD3D startup; `EvaluateState()` / `currentFG->IsActive()` are authoritative, not the Vulkan overlay API).
- Reproj reports real/fake frame types at its own present sites; the generic wrapped-swapchain fakenvapi block intentionally excludes it. FGHooks present-skip flags are `thread_local`.
- Do not retry: CPU-blocking queue-arrival latching (cadence regressed ~2% → 7% drops) or a high-priority presenter queue (starved source rendering to ~35 FPS).

### KCD2 specifics

- Camera comes from a read-only `WHGame.dll` `CCamera::UpdateFrustumPlanes` hook (`Kcd2Camera.{h,cpp}`; signatures/layout derive from MIT-licensed KCD2Tools/TPVCamera). The DLL loads after OptiScaler, so installation retries lazily from packet capture. Gameplay cameras validate via `CView` MSVC RTTI and publish Matrix34 pose/FOV via seqlock; **unknown builds fail closed** — never accept every frustum camera (the function also sees shadows/reflections/portals).
- Live-validated projection block (`+0x30..+0x7C`, retail 1.5.6): near = float `+0x54` (0.05), far = float `+0x6C` (8000), plane edge `+0x5C` (`+0x60 = (1/tan(fov/2))·height/2`, x/z raw half-extents), pixel aspect `+0x40`; ints at `+0x34/+0x38` are repurposed, not viewport dims. Near/far feed `cameraNear/Far`.
- Warp mode is hardcoded rotation-only (`mode 2`, unwarped `0` without camera); the shader's depth path is currently unreachable — do not treat it as selectable until footage-tuned.
- Frustum callbacks group into render-frame bursts: exact duplicates never advance history, but a changed pose or a ≥ 8 ms gap does, so a stationary camera publishes a zero-velocity pair instead of extrapolating the last turn forever. Extrapolation divides by the exact hooked camera-pair interval, never by present cadence.
- Late-latch yaw composes around CryEngine world Z, then pitch around the yawed camera-right axis (generic cameras keep local-up). Never yaw around the camera's local up — it tilts with pitch and rolls during horizontal pans while looking up/down.
- Packet mouse baselines use `GetRawMouseMotionAt(sourcePoseTimestamp)`, never publication-time totals. The polled cursor stream is empty for cursor-locked games, so steering depends on raw input: WM_INPUT accumulation plus a dedicated pump thread (`WH_MOUSE_LL` passive observer at ~1 kHz) that must never become a second `RegisterRawInputDevices` (on Wine that steals the game's raw stream and freezes its camera). The pump is the sole relative-motion accumulator while delivering, self-expiring after 250 ms quiet; it stops with the presenter.
- HUD isolation (`Kcd2Scaleform` + `Kcd2HudIsolation`, live-validated): KCD2 renders Scaleform UI into the active backbuffer just before `Present()`. `BeginDisplay`/`EndDisplay` hooks snapshot the clean 3D world into `hudlessTexture`, clear `uiTexture`, and redirect Scaleform's RTV there. The synchronous fallback reuses the same split (`CopyLastFrame` → `_lastColor`, UI → `_uiColor`, composite after warp) so it also yields exactly one unwarped HUD.
- Swapchain quirks: KCD2 requests 2 buffers while virtualization coerces 3 — visibility/clamping goes through `EffectiveGameBufferCount()` (recorded pre-coercion request), never the raw count. The SRV hook ignores low-address placeholder resources (e.g. `pResource=0x3`) instead of forwarding them to VKD3D.

### Removed — do not re-add while the bare-basics experiment runs

`HybridFsrGenerator` (+ FSR generated-content sequence, `FGOutput::HybridTimewarp`), `Kcd2Input`, `ReprojInputPredictor`, `TargetPoseResolver`, velocity clamps, adaptive/queue-aware dispatch lead, depth/MV warp selection, hybrid/sync-generated/subsampling/smoothing-besides-EMA/debug modes, queue/late-latch toggles, per-slot telemetry and its INI/menu controls (`Telemetry`, `TelemetryMissDump`, and the whole `#if 0` experimental menu block: `Strength`, `TimeStep`, `MaxWarpFrames`, `NonBlockingAnchorSampling`, `AnchorSampleHz`, `PresentCompletionClock`, `MaxPoseAgeMs`).

### Hitch hold

When the game stops publishing anchors for more than ~2.5 source periods (streaming stall), velocity extrapolation would dead-reckon far past the last known pose and snap back on resume. Those slots instead hold the anchor's own pose (`timeStep = 0`); late-latch mouse paths ignore the timestep, so aiming stays live through the freeze. Gated on *publish* freshness (freshest READY `renderTimestamp`), never on anchor age: fresh publishes with lagging captures keep normal extrapolation. `hold=` on the log line proves engagement; it must be 0 in steady state.

### Current status

- v10.0.1-pre21 (2026-09-03): hitch hold (above) + latch-path diagnostics. KCD2 castle walk: steady state is healthy (source 85–105, display ~118–121, missed 0–4, lateCam steering, lateAge ~5 ms) but streaming hitches show source dips, present misses (interval p95 ~15 ms), fallback rise, poseAge 40+ ms. NOTE: `sensX` has never auto-calibrated this session (stuck at the 0.00015 default) — open question whether the true sensitivity differs; lateCam residuals would then be systematically misscaled. KCD2 runs uncapped (cap=60 regressed feel); do not re-impose without a live A/B.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).

## Reprojection telemetry

Detailed per-slot telemetry (`ReprojTelemetry v=1`, slot dumps, `cause.*`/`step.*` fields) was removed 2026-09-02. The once-per-second `Reproj:` line above is the only instrument — keep its keys stable and do not add per-slot allocation/log/fence-wait/readback. `tests/reprojection/analyze_telemetry.py` and its fixture still parse the dead v=1 format, so they are stale until rewritten; the inert one-slot bookkeeping shim in the presenter exists only until its call sites are cleaned up.
