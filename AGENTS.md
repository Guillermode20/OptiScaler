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
- Compile errors are not caught locally; read them with `gh run view <run_id> --log-failed`.

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

1. For **Unreal Engine** games, install next to the *real* executable — usually `<game>/<Project>/Binaries/Win64/` — not the root launcher `.exe`.
2. Copy `OptiScaler.dll` -> `dxgi.dll` (default injection name), plus `OptiScaler.ini`, `OptiScaler/`, and `Licenses/` into that folder.
3. Add to Steam launch options: `WINEDLLOVERRIDES=dxgi=n,b %COMMAND%`.

## Config / hotkey

- Overlay hotkey: `ShortcutKey` in `Config.h` (default `VK_HOME`; was `VK_INSERT`). INI equivalent: `[Menu] ShortcutKey=0x24`.
- The repo-root `OptiScaler.ini` is the shipped default. `auto` values resolve to the source default in `Config.h`.

## Async Reprojection (in-progress feature in this repo)

- Design docs: `AsyncReprojection.md` covers the original implementation; `AsyncReprojection_Continuation_Plan.md` covers the earlier roadmap; `docs/NativeAsyncTimewarpPlan.md` supersedes its presentation architecture. The virtualized-main-swapchain path is implemented but remains unvalidated in games.
- Code: `OptiScaler/framegen/reproj/AReproj_Dx12.{h,cpp}` and `OptiScaler/shaders/reprojection/RP_*`.
- Enabled by `FGOutput::Reproj` (`OptiScaler/State.h`).

Known issues / limitations:

- `AReproj_Dx12::Present()` uses `GetIndexWillBeDispatched()`, matching Streamline/FSR3/FfxApi's ahead-of-present resource slot and still resolving to the current upscaler slot when it has resources. The non-upscaler paths still need end-to-end validation.
- `ForceVsync` is applied before `AReproj_Dx12::Present()`, so the internally presented real frame receives the configured interval and tearing flags.
- The v2 depth-aware warp is sketch-quality; disocclusion confidence and the HUD epsilon need real-footage tuning.
- `ReprojCapAtHalfRefresh` controls whether the existing `FrameLimit` half-rate behavior is used for reprojection. Reprojection bypasses Reflex/XeLL limiter gating so that the selected cap is applied even when those limiters are inactive.
- Reproj emits zero to `ReprojMaxWarpFrames` warps per real frame. The validated synchronous path blocks the game present thread while emitting them. With `ReprojAsync=true`, the wrapper exposes private virtual backbuffers to the game and a worker owns every present on the real main swapchain; unavailable capabilities fall back synchronously. For a 120 Hz target, `ReprojMaxWarpFrames=3` covers a 30 FPS source (three warps plus one real frame); the existing cadence formula requests fewer warps at higher real FPS.
- The worker may use real DXGI backbuffers only while virtualization is active: the game must never receive or render into them. Without that ownership boundary, post-`Present` worker access remains unsafe.
- Async packets transition `FREE -> CAPTURING -> READY -> PRESENTING -> RETIRED -> FREE`; reuse requires both capture and presenter fences to complete. Stop/join the presenter before draining and releasing D3D12/DXGI objects. `IFGFeature::_cameraTimestamp` records source-pose age when camera data is captured.
- Main-swapchain virtualization requires flip model plus a frame-latency waitable object and remains experimental on Windows and Proton. Keep `ReprojAsync=false` as the shipped default until more games validate the path. On 2026-08-22, Deep Rock Galactic on Proton live-validated the async virtual-swapchain presenter: 3 virtual game buffers, successful worker presents, ~60 real + ~60 warp FPS, and ~0.1 ms game-present blocking. It can briefly fall back/reinitialize across context resets. Virtual buffers belong to the swapchain, so an FG context reset must stop the presenter but must not destroy/recreate those buffers unless the swapchain itself resizes or is released.
- The menu may disable an unavailable Reproj option, but must never reset `FGOutput=Reproj` to `NoFG`: runtime capability can be transient during DX12/VKD3D startup. VKD3D's Vulkan overlay API is not the DX12 feature state; `AReproj_Dx12::EvaluateState()` / `currentFG->IsActive()` are authoritative.
- Raw-mouse late latching uses the shared `OptiInput` cumulative relative-motion history, sampled at `_cameraTimestamp` and again immediately before warp dispatch. Input can come from raw-input reads, DirectInput, or cursor polling/recentering (needed by some Proton games). It only affects depth/camera modes (1/2) and remains opt-in. `ReprojAutoCalibrate` fits a 2x2 mouse-to-yaw/pitch map across 0–50 ms lag bins from real camera poses; `MouseDegreesX/Y` are fallback values while confidence is low. Recoil, nonlinear acceleration, and controller-only motion remain limits.
- Reproj reports real/fake frame types at its internal present sites; the generic wrapped-swapchain fakenvapi block intentionally excludes it. The FGHooks present-skip flags are `thread_local`, so a worker present cannot bypass a concurrent game present. Reflex markers/sleeps remain intentionally limited to DLSSG.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).
