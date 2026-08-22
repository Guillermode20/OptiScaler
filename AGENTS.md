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
- **Required reprojection workflow:** commit every change, push it, run and wait for **Build (No Signing)**, then install the successful artifact into Deep Rock Galactic for live validation. Do not claim a reprojection change is validated before that game test; ask for its install path if it is not known.

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
- Synchronous reprojection now supports the same HUD composition as the packet path (`DrawUIOverFG=true` + Hudfix): `CopyLastFrame` copies the HUD-less source into `_lastColor` and stashes UI in `_uiColor`; `DispatchWarp` composites UI after the warp copy. Result: exactly one unwarped HUD instead of a timewarped baked-in HUD.
- Async-timewarp-on-KCD2 verdict (2026-08-22): wine/Proton's d3d12core access-violates on ANY use of a `FRAME_LATENCY_WAITABLE_OBJECT` swapchain in KCD2 — confirmed by minidump (AV in d3d12core, called from wrapped dxgi) after the redundant-resize skip already worked. DRG async works on the same Proton, so it is a CryEngine-usage-specific wine bug. Do not retry KCD2 with `Reproj.Async=true` unless wine is updated or the waitable requirement is redesigned. KCD2 runs sync reprojection (`Reproj.Async=false`) as its stable config.
- The menu may disable an unavailable Reproj option, but must never reset `FGOutput=Reproj` to `NoFG`: runtime capability can be transient during DX12/VKD3D startup. VKD3D's Vulkan overlay API is not the DX12 feature state; `AReproj_Dx12::EvaluateState()` / `currentFG->IsActive()` are authoritative.
- Raw-mouse late latching uses the shared `OptiInput` cumulative relative-motion history, sampled at `_cameraTimestamp` and again immediately before warp dispatch. Input can come from raw-input reads, DirectInput, or cursor polling/recentering (needed by some Proton games). It only affects depth/camera modes (1/2) and remains opt-in. `ReprojAutoCalibrate` fits a 2x2 mouse-to-yaw/pitch map across 0–50 ms lag bins from real camera poses; `MouseDegreesX/Y` are fallback values while confidence is low. Recoil and nonlinear acceleration remain limits. XInput hooks retain the current right-stick rate and late-latch dispatches refresh it directly, so controller aim is extrapolated from source-pose time with tunable `GamepadDegreesPerSecondX/Y`; DirectInput/GameInput-only controllers are not yet covered.
- Reprojection screen-edge protection must preserve the real-frame pixel rather than clamp-sampling an off-screen warp coordinate: clamping stretches edge texels into obvious smears. The RP/RPD shaders feather to the original frame by source coverage. HUD composition requires *both* a compatible HUDless source and UI; compare normalized reprojection formats on both resources (sRGB/typeless normalize to UNORM) before choosing that path.
- Reproj reports real/fake frame types at its internal present sites; the generic wrapped-swapchain fakenvapi block intentionally excludes it. The FGHooks present-skip flags are `thread_local`, so a worker present cannot bypass a concurrent game present. Reflex markers/sleeps remain intentionally limited to DLSSG.
- Warp responsiveness (aligned with the Comrade Stinger ATW reference in `/home/whick/Coding/AsyncTimewarp/`): `ReprojLateLatch` defaults to ON so warps target the freshest input rather than only extrapolated rendered poses; `ApplyLateLatch` skips the focus gate on Wine/Proton (Win32 focus messages are unreliable there, same exemption as the presenter loop); `ReprojMaxPoseAgeMs` default is 100 ms so late-cadence warps at 30 FPS sources are not dropped mid-sequence; the RPD HUD exclusion uses a knee (`saturate((length(delta)-0.02)*8)`) instead of a linear ramp from zero so far geometry under slow aim stays depth-warped.
- Single-prediction ATW rule (OpenXR/Oculus): never combine pose extrapolation with late-latched input — both model `[source pose -> display time]`, and their sum is ~2x rotation during steady turns (sawtooth overshoot/snap-back = rotational jitter). `RP_Constants::extrapolate=1` restores the legacy extrapolation only when `ReprojLateLatch=false`; with late latch on, the warp target pose is the current camera basis rotated solely by LateYaw/LatePitch.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).
