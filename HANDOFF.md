# Handoff — KCD2 HUD Isolation / Timewarp

Date: 2026-08-26  
Branch: `async-timewarp`  
Objective: keep KCD2 HUD/UI unwarped while reprojection warps only the world.

## Current state

- Latest commit: `c295fd68 Reproj: composite isolated UI on real sync present frames`.
- Latest build: GitHub Actions run `33018023225`, artifact `OptiScaler_v10.0.0-pre25_20260826.7z`.
- **Status:** **LIVE VALIDATED on KCD2 1.5.6 (retail)**.
  - Scaleform UI isolation, world snapshotting, and post-warp composition render crisply and cleanly.
  - 3D camera rotation is timewarped while all HUD elements (subtitles, reticle, compass, inventory, menus) remain sharp and unwarped.
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
   - 100% clean separation of world color and Scaleform UI draws.
   - Zero artifacts, zero UI warping, subtitles and full UI render with correct alpha and contrast.

## Config recommendations for KCD2

- `[Reproj] Async = false` (keep sync reprojection active; virtual waitable async remains Wine/Proton-stalled).
- `[FrameGen] DrawUIOverFG = true`
- `[Reproj] Kcd2HudIsolation = true`
- `[Reproj] RotationOnly = true`

## Relevant code

- `OptiScaler/framegen/reproj/Kcd2Scaleform.{h,cpp}`: RTTI/prologue-gated Scaleform hook and TLS scope.
- `OptiScaler/framegen/reproj/Kcd2HudIsolation.{h,cpp}`: Backbuffer snapshot, UI target allocation, clearing, and RTV redirection.
- `OptiScaler/resource_tracking/ResTrack_dx12.cpp`: `hkOMSetRenderTargets` redirection hook.
- `OptiScaler/framegen/reproj/AReproj_Dx12.cpp`: Consumes hudless world and UI textures, composites UI on both real and warped frames.
- `OptiScaler/framegen/reproj/Kcd2Camera.cpp`: Gameplay camera pose acquisition hook.
