# AGENTS.md

Guidance for coding agents and contributors working on this repository.

## Project overview

- OptiScaler is a Windows DLL injected into DX11/DX12/Vulkan games to swap upscalers and add frame generation.
- Toolchain: C++20, MSVC, Visual Studio 2022. Solution: `OptiScaler.sln`.
- The current development branch for this work is `async-simple`. `async-timewarp` is a reference branch, not the architecture to reproduce by default.
- KCD2 is the primary live-validation target. DRG is regression-only.

## Planning discipline

`PLAN.md` is the single source of truth for active async-timewarp work, priorities, experiments, acceptance criteria, and decisions.

Do not create new roadmap, handoff, continuation-plan, artifact-options, or experiment-plan Markdown files. Update `PLAN.md` instead. Keep this file factual and operational. The normal OptiScaler project documentation such as `README.md`, `Config.md`, `Features.md`, `Issues.md`, `Spoofing.md`, `CONTRIBUTING.md`, and `Changelog.md` remains separate because it serves users/contributors rather than development planning.

## Building

This is an MSVC/Windows project. Do not claim a local Linux build validates the DLL.

For reprojection work:

1. Commit and push the target branch.
2. Run the unsigned GitHub Actions workflow, `Build (No Signing)`.
3. Wait for it to succeed.
4. Install the produced artifact with `scripts/install_latest.py --both --ref async-simple` or an explicit run id.
5. Launch KCD2 and test the change. Use DRG when the change touches generic swapchain/presenter behaviour.

A reprojection change is not live-validated until the game test happens.

Useful commands:

```bash
gh workflow run "Build (No Signing)" --repo <owner>/OptiScaler --ref async-simple
gh run watch <run_id> --repo <owner>/OptiScaler --exit-status
python scripts/install_latest.py --both --ref async-simple
```

The signed `Build` workflow requires signing credentials. Use `Build (No Signing)` on forks.

## Versioning and release

`OptiScaler/resource.h` is the version source of truth. `OptiScaler.rc` and version-check code derive from it. Build date/commit headers are generated during the MSVC build and should not be edited manually.

Use `scripts/bump_version.py` for version changes. The normal development bump is the build/pre number. If the script updates `Changelog.md`, commit both files it touches.

## Shaders

Each shader family has three representations that must stay synchronized:

- `OptiScaler/shaders/<name>/precompile/<Name>.hlsl`
- `OptiScaler/shaders/<name>/<Name>_Common.h`
- `OptiScaler/shaders/<name>/precompile/<Name>_Shader.h`

After HLSL changes, regenerate the precompiled shader header. Register new source/header files in both `OptiScaler/OptiScaler.vcxproj` and `.filters`.

## async-simple architecture

Enabled through the reprojection/frame-generation output path. Relevant code is under `OptiScaler/framegen/reproj/`, the wrapped DXGI swapchain, resource tracking, and `OptiScaler/shaders/reprojection/`.

Steady-state model:

1. The game renders into wrapper-owned virtual backbuffers. While virtualization is active, the presenter owns all presents to the real DXGI swapchain.
2. Game `Present()` performs one inline capture submit on the game's DIRECT queue. It captures world colour and, when KCD2 HUD isolation is active, the isolated UI. One capture fence gates the anchor.
3. Packet lifecycle is `Free -> Capturing -> Ready -> Presenting -> Retired -> Free`. There are three packet slots. The presenter chooses the newest completed anchor and never blocks on a newer incomplete capture.
4. The presenter uses one NORMAL-priority DIRECT queue and performs exactly one real present per display slot. Repeated slots are real rotation warps of the active anchor.
5. A CPU-signalled deferred latch parks submitted warp work until a fixed late-sample deadline. The default lead is 3 ms. Late rotational correction uses fresh raw mouse motion plus the latest valid KCD2 camera state, then falls back to rendered-camera angular-velocity extrapolation.
6. The final display homography is rotation-only. Depth and motion vectors may be consumed by the optional FSR midpoint generator but must not silently become a positional final warp.
7. If the presenter is unavailable or permanently fails, downgrade to plain frame presentation. Do not restore a synchronous generated-frame fallback path.

## Hard invariants

- The game thread never CPU-waits for reprojection GPU work. Packet exhaustion or a busy capture allocator drops the anchor.
- The virtual-buffer handoff remains GPU-wait-free from the game thread's perspective.
- Latest completed anchor wins. `capWait` means re-warp the active anchor, not stall.
- Repeated display slots are warps, not blits.
- The presenter owns display cadence. Packet arrival must not cause an extra present or reset the deadline grid.
- Stop/join the presenter before draining/releasing its D3D12/DXGI objects. Release any outstanding late-latch gate before join/drain so the queue cannot remain parked.
- One NORMAL-priority DIRECT presenter queue is the fixed baseline on Proton/VKD3D. A high-priority DIRECT queue previously starved KCD2 source rendering.
- Do not reintroduce CPU-blocking queue-arrival latching. It previously worsened cadence.
- Screen-edge validity must be based on raw reprojected coverage, not a coordinate after clamp/saturate.
- Do not re-add previous-anchor image blending over valid current content. It produced a visible double/ghost during rotation.
- Keep the final warp rotation-only unless `PLAN.md` explicitly moves positional reprojection into active work after a measured gate.

## KCD2-specific integration

Camera acquisition lives in `Kcd2Camera.{h,cpp}`. It signature-finds the relevant `WHGame.dll` camera callback, accepts only validated gameplay `CView` cameras using RTTI/build gates, and publishes pose/FOV through a seqlock. Unknown builds fail closed. The callback also supplies the rendered camera history used by extrapolation.

Late-latch yaw is around CryEngine world Z. Pitch is around the yawed camera-right axis. Do not yaw around the camera's local up because it tilts with pitch and produces roll during horizontal pans.

Raw mouse input is observed passively. Do not add a second `RegisterRawInputDevices` path on Wine/Proton because it can steal the game's raw stream. The input observer must never edit, block, or synthesize game input.

KCD2 HUD isolation uses the Scaleform hooks and HUD-isolation resource path under `framegen/reproj/`. The clean world is timewarped and the isolated UI is composited afterward. Keep the UI/world capture in the same inline game-DIRECT submit and readiness gate. Do not restore UI borrowing.

The attempted KCD2 render-reserve hook was removed after live testing showed that it only reduced the displayed FOV; it did not widen the captured world image. Do not restore a guard crop or claim offscreen coverage without direct captured-image proof from an earlier projection hook.

`ContentInterpolation` is opt-in. It asks the maintained FSR frame-generation path for one midpoint using the isolated world plus validated depth/MV inputs. Generated and real content both receive the same final late rotation and isolated HUD composite. Missing or suspect inputs fail closed to ordinary timewarp.

KCD2 requests fewer swapchain buffers than the virtualized path may allocate. Game-visible buffer-count logic must use the wrapper's effective game count rather than exposing the coerced real count.

## Current live controls

The active `[AsyncTimewarp]` controls include:

- `Enabled`
- `TargetRefresh`
- `SourceFramerateLimit`, opt-in, default uncapped
- `MouseSensitivityX/Y`
- `Smoothing`
- `LateSampleLead`, default fixed 3 ms when automatic/default value is selected
- `HudIsolation`
- `ContentInterpolation`, default off

Do not invent a new control before checking `Config.h`, `Config.cpp`, `OptiScaler.ini`, and the in-game menu wiring.

## Health telemetry

The once-per-second `Reproj:` line is the primary runtime health instrument. Keep it cheap and stable. Do not add allocation, blocking fence waits, GPU readback, or high-rate logging to the display hot path.

For the controlled 60 -> 120 KCD2 test, healthy behaviour is approximately:

- source around 60 FPS;
- display around 120 FPS;
- roughly equal new/repeat outputs;
- missed slots below about 1% after warm-up;
- no dropped-anchor burst;
- low game-present block time;
- late input applied during camera motion.

Add aggregate telemetry only when it answers an active question in `PLAN.md`. Temporary instrumentation should be removed or disabled after the measurement is complete.

## D3D12 gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is protected. Subclasses can use it to flush UI work before presentation.
- `LockedDx12Resource` has an explicit `operator bool`; use contextual conversion rather than pointer-style comparison.
- Swapchain backbuffers are normally in `D3D12_RESOURCE_STATE_PRESENT` at present time. Restore them to PRESENT after copy/render operations.
- `IFGFeature_Dx12::CreateBufferResource` may reuse a matching resource without transitioning it. Track the previous state if a pass leaves a custom state.
- sRGB formats cannot be UAVs. Private warp outputs should use an appropriate typeless/UNORM-compatible resource/view arrangement so copying stays byte-faithful and gamma is not applied twice.
- The CPU prepares the rotation homography used by the shader. Keep CPU and shader representations in sync when changing warp constants.

## Removed mechanisms

Do not copy mechanisms back from `async-timewarp` simply because they exist there. The following were deliberately removed from `async-simple` and require a measured A/B plus a `PLAN.md` decision before return:

- capture worker and dedicated COPY capture queue;
- mid-frame world fence;
- COMPUTE warp queue;
- UI borrowing;
- repeat-warp shedding/blit repeats;
- hitch-hold machinery;
- previous-anchor image blend;
- adaptive late-sample/dispatch controller;
- old KCD2 input predictor and target-pose resolver stack;
- full positional depth/MV final warp;
- heavy per-slot telemetry.

The opt-in source cap, one-midpoint FSR generator, and future edge-history fallback described in `PLAN.md` are self-contained exceptions only when they preserve the simplified ownership/queue model.

## Working rule

Before implementing substantial async-timewarp work, read `PLAN.md` and the relevant code. When a milestone lands, update `PLAN.md` in the same change so the repository does not accumulate contradictory plans again.
