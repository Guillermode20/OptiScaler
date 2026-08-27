# Handoff — KCD2 HUD Isolation / Timewarp

Date: 2026-08-26  
Branch: `async-timewarp`  
Objective: keep KCD2 HUD/UI unwarped while reprojection warps only the world.

## Current state

- Latest branch tip: `724ae939 Docs: record live-validated KCD2 Scaleform HUD isolation in HANDOFF.md and AGENTS.md`.
- Latest functional implementation commit: `c295fd68 Reproj: composite isolated UI on real sync present frames`.
- Latest build: GitHub Actions run `33018023225`, artifact `OptiScaler_v10.0.0-pre25_20260826.7z`.
- **HUD-isolation status:** **LIVE VALIDATED on KCD2 1.5.6 (retail)**.
  - Scaleform UI isolation, world snapshotting, and post-warp composition render crisply and cleanly.
  - In the observed retail test scenes, 3D camera rotation was timewarped while the HUD elements exercised (subtitles, reticle, compass, inventory, and menus) remained sharp and unwarped.
- **Async-presentation status:** running but not feel-validated. The 2026-08-26 KCD2 log shows a 60 Hz source and approximately 120 Hz output, but present-interval p95 was 11.25 ms with 246 missed slots across 212 telemetry windows. Treat async cadence as active work, not as a shipped-smooth result.
- `pre25` architecture:
  - `CScaleformPlayback::BeginDisplay` / `EndDisplay` Detours hook brackets Scaleform rendering.
  - In `ResTrack_dx12.cpp::hkOMSetRenderTargets`, when `Kcd2Scaleform::IsActiveOnThisThread()` is true:
    - First OM binding of a display scope snapshots the clean world backbuffer into `hudlessTexture` and clears `uiTexture` with `(0, 0, 0, 0)`.
    - Redirects Scaleform's `OMSetRenderTargets` RTV descriptor to `uiTexture`.
  - In `AReproj_Dx12`:
    - `CopyLastFrame` and `CaptureFramePacket` take `hudlessTexture` as `_lastColor` for world-only timewarp and `uiTexture` as `_uiColor`.
    - Both real frames (`PresentVirtualFrameSync` / non-virtualized sync present) and warped frames (`DispatchWarp` / `DisplayPacket` / `DispatchPacketWarp`) run `_renderUI->Dispatch(...)` to alpha-composite the unwarped UI on top of the world.

## Summary of live results

1. **Scaleform Interception:**
   - Detours cleanly attached to `BeginDisplay=WHGame.dll+0x4E901C` and `EndDisplay=WHGame.dll+0x4E8B28`.
   - Scaleform output target is the active DXGI swapchain backbuffer.
2. **HUD Isolation:**
   - The tested Scaleform draws separated cleanly into world and UI targets.
   - No UI warping or alpha/contrast artifacts were observed in the exercised subtitles and full-screen UI paths.

## Config recommendations for KCD2

- `[FrameGen] Enabled = true`
- `[FrameGen] FGInput = Upscaler`
- `[FrameGen] FGOutput = AsyncTimewarp` (required to enable reprojection; the shipped default is `nofg`).
- `[FrameGen] DrawUIOverFG = true`
- `[Reproj] Kcd2HudIsolation = true`
- `[Reproj] RotationOnly = true`
- For the latest async experiment: `[Reproj] Async = true`, `[Reproj] TargetRefresh = 119.95`, and `[Reproj] SourceFramerateLimit = 60`.
- For the conservative KCD2 fallback: `[Reproj] Async = false`. Async is not blocked/stalled on the current Proton setup, but its frame pacing still needs improvement before it can be called smooth.
- `OptiScaler.ini` currently says KCD2 isolation also requires `OptiFG HUDFix=true`; the current redirection code does not gate on that setting. Resolve that stale comment before treating HUDFix as a required configuration prerequisite.

## Relevant code

- `OptiScaler/framegen/reproj/Kcd2Scaleform.{h,cpp}`: RTTI/prologue-gated Scaleform hook and TLS scope.
- `OptiScaler/framegen/reproj/Kcd2HudIsolation.{h,cpp}`: Backbuffer snapshot, UI target allocation, clearing, and RTV redirection.
- `OptiScaler/resource_tracking/ResTrack_dx12.cpp`: `hkOMSetRenderTargets` redirection hook.
- `OptiScaler/framegen/reproj/AReproj_Dx12.cpp`: Consumes hudless world and UI textures, composites UI on both real and warped frames.
- `OptiScaler/framegen/reproj/Kcd2Camera.cpp`: Gameplay camera pose acquisition hook.

## Reproduce / inspect

- Latest tested KCD2 deployment: `/var/home/whick/.local/share/Steam/steamapps/common/KingdomComeDeliverance2/Bin/Win64MasterMasterSteamPGO/`.
- Inspect cadence after a test with: `python3 tests/reprojection/analyze_telemetry.py <KCD2-dir>/OptiScaler.log`.
- Do not call an async change validated until the required chain succeeds: commit, push, **Build (No Signing)**, `scripts/install_latest.py --both`, then a KCD2 test (DRG regression).
