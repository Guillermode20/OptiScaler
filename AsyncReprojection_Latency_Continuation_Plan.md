# Async Reprojection Latency Continuation Plan

## Current milestone

Generic raw-mouse late-latched rotation and short-horizon input prediction are implemented. An online least-squares model now learns the 2D mouse-to-yaw/pitch mapping and searches 0–50 ms input delay from captured real camera poses, leaving manual sensitivity as fallback only. These features update camera rotation immediately before each warp but do not update game simulation or independently moving objects.

## Phase 3 — Engine-specific late camera poses

### Goal

Replace calibrated raw-input estimates with the game's actual newest camera orientation, including sensitivity, acceleration, smoothing, controller input, recoil, head bob, and scripted camera motion.

### Order

1. Add diagnostics that compare raw-input prediction against the next captured game camera pose.
2. Prototype Unreal Engine view/projection constant-buffer identification and timestamped capture.
3. Store source and newest camera poses independently in each reprojection packet.
4. Select the newest valid pose immediately before warp dispatch; reject poses older than the source or belonging to a scene reset.
5. Keep raw-input prediction as the generic fallback.
6. Add engine adapters only when a stable signature and validation rule exist; do not expose arbitrary constant-buffer scanning by default.

### Success criteria

- Late pose includes mouse, controller, recoil, and camera animation.
- Pose source and age are visible in diagnostics.
- Scene cuts, FOV changes, and invalid matrices fall back safely.
- Measured camera response improves over raw-input prediction without added instability.

## Phase 4 — Presentation queue latency

### Goal

Minimize time between late pose sampling and physical scanout without blocking the game render thread.

### Order

1. Measure warp dispatch, queue completion, `Present`, and estimated scanout timestamps separately.
2. Set the composition swapchain maximum frame latency to one where DXGI supports it.
3. Use waitable swapchain/display timing when available instead of fixed sleeps.
4. Schedule warp submission at `deadline - measuredWarpGpuTime - safetyMargin`.
5. Drop stale work whenever a newer packet or missed deadline makes it obsolete.
6. Tune the safety margin adaptively from recent GPU-time variance.
7. Compare DirectComposition against supported low-latency presentation paths; retain DComp as the safe worker-owned default until another path proves safe.

### Success criteria

- No more than one compositor frame is queued.
- Warp pose is sampled within a measured small margin of submission.
- Late warps are dropped rather than queued.
- Resize, alt-tab, occlusion, and shutdown remain deadlock-free.
- Present-to-scanout latency is reported separately from source-pose age.
