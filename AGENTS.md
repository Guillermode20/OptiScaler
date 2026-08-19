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

- Design docs: `AsyncReprojection.md` covers the original implementation; `AsyncReprojection_Continuation_Plan.md` covers the true asynchronous timewarp roadmap. Milestones **M0–M4** are implemented; **M5** (true async thread + fakenvapi) is a stretch.
- Code: `OptiScaler/framegen/reproj/AReproj_Dx12.{h,cpp}` and `OptiScaler/shaders/reprojection/RP_*`.
- Enabled by `FGOutput::Reproj` (`OptiScaler/State.h`).

Known issues / limitations:

- `AReproj_Dx12::Present()` reads resources via `GetIndex()`, which matches the `FGInput=upscaler` path. Streamline/FSR3/FfxApi inputs route through `GetIndexWillBeDispatched()`; they converge in steady state but have not been explicitly validated end-to-end.
- `ForceVsync` is not applied to the real present — the feature presents internally before `FGPresent`'s vsync block runs.
- The v2 depth-aware warp is sketch-quality; disocclusion confidence and the HUD epsilon need real-footage tuning.
- `ReprojCapAtHalfRefresh` is parsed/saved but intentionally unused (the half-rate cap rides on `FrameLimit::sleep(true)`). Reprojection now bypasses Reflex/XeLL limiter gating so that cap is applied even when those limiters are inactive.
- Reproj emits one fake frame per real frame; it cannot sustain a fixed refresh target when the real rate falls below half-refresh without multi-frame extrapolation and increased artifacts.
- fakenvapi/Reflex `reportFGPresent` is not wired for `Reproj` yet.

## D3D12 base-class gotchas

- `IFGFeature_Dx12::SubmitUICommandList` is `protected`; subclasses call it to flush a pending UI command list (required before presenting a frame that depends on it).
- `LockedDx12Resource` has an explicit `operator bool` — use contextual conversion (`if (!res)`, `res ? ... : ...`), not `res != nullptr`.
- The swapchain backbuffer is in `D3D12_RESOURCE_STATE_PRESENT` at present time; transitions should be `PRESENT -> (COPY_SOURCE/RENDER_TARGET/COPY_DEST) -> PRESENT`.
- `IFGFeature_Dx12::CreateBufferResource` **reuses** an existing resource when the desc matches and does not transition it. If a pass leaves that resource in a custom state (e.g. `NON_PIXEL_SHADER_RESOURCE`), the caller must track and transition it.
- sRGB formats cannot be UAVs. For a private warp output, use the typeless parent (`R8G8B8A8_TYPELESS` etc.) as the UAV and sample the sRGB source via a UNORM SRV to keep the copy byte-faithful (avoids double gamma).
