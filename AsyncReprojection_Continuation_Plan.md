# Async Reprojection Continuation Plan

## Purpose

This document defines the next stages of OptiScaler's DX12 async reprojection work.
It starts from the current working implementation and describes the path toward
late-latched, asynchronous camera reprojection similar to the ComradeStinger demo.

The current implementation is a synchronous, bounded warp-per-real-frame system.
The target implementation separates expensive scene rendering from presentation so
that the latest completed frame can be warped and presented multiple times per
real render frame.

## Implementation status

M0–M4 are implemented. The safe synchronous presenter now has GPU-queue fence
retirement on allocator reuse and teardown, rate-limited real/warp/drop/pose-age
metrics, bounded adaptive warp scheduling, and fakenvapi FG reporting. The
depth-aware shader also has conservative depth-edge, motion-disagreement, and
screen-edge confidence tests, plus a rotation-only camera mode.

M5 now has an experimental, opt-in DirectComposition implementation. The game
continues to own and present its DXGI backbuffers while the render thread publishes
owned color/depth/velocity/UI packets to a worker. The worker writes only to a
separate composition swapchain, avoiding the unsafe next-backbuffer race. Creation
or runtime failure falls back to the existing synchronous presenter. The live UI
reports the active presenter and actual packet queue depth.

## Current baseline

The current path is:

```text
Game renders frame N
    -> game calls Present
    -> OptiScaler copies the backbuffer
    -> OptiScaler presents the real frame
    -> waits until each bounded display deadline
    -> dispatches and presents zero or more warped frames
```

The current implementation:

- Is DX12-only.
- Uses `FGOutput::Reproj`.
- Uses `FGInput::Upscaler`.
- Uses the active upscaler's motion vectors and depth.
- Works with the DLSS/XeSS input path replaced by the FSR2 backend.
- Owns a replacement swapchain with at least three buffers.
- Uses one real frame plus zero to `ReprojMaxWarpFrames` warped frames per game
  present; default is one.
- Uses a synchronous wait in the present path.
- Uses motion-vector warp as the reliable baseline.
- Has an experimental depth-aware mode.
- Does not create frames containing genuinely newly rendered geometry.

Recommended baseline configuration:

```ini
[Upscalers]
Dx12Upscaler=fsr22

[FrameGen]
Enabled=true
FGInput=upscaler
FGOutput=reproj
FGNvngxReplacement=none

[Inputs]
EnableDlssInputs=true
EnableFsr2Inputs=true
UseFsr2Inputs=true
```

The game should use DLSS or XeSS as its input upscaler. OptiScaler replaces that
upscaler with FSR2, captures the resulting motion vectors/depth, and uses them for
reprojection. Native in-game FSR2 is not the primary injection path for games that
OptiScaler does not otherwise hook.

## Design goals

### Primary goals

1. Keep the game render thread from waiting for every fake frame.
2. Present the latest completed frame at display refresh rate when possible.
3. Sample camera pose as late as possible before each warp.
4. Support multiple warped presentations per real rendered frame.
5. Preserve the current FSR2/DLSS/XeSS input workflow.
6. Keep the implementation independent of vendor frame-generation libraries.
7. Fail safely to real-frame presentation when resources or pose data are invalid.
8. Keep the current synchronous implementation available as a fallback.

### Non-goals

- Creating fully correct new geometry in disoccluded regions.
- Replacing the game's simulation tick.
- Guaranteeing perfect output at arbitrary low render rates.
- Supporting DX11 or Vulkan in the first asynchronous implementation.
- Adding a neural frame-generation model.
- Hiding unavoidable artifacts from large camera translations or independently
  moving objects.

## Important terminology

### Real frame

A frame produced by the game's normal simulation and rendering pipeline. It has
new scene geometry, lighting, particles, and post-processing results.

### Warp frame

A presentation produced by transforming a previously rendered image using depth,
motion vectors, and a camera pose. It is not a newly rendered scene frame.

### Timewarp

A late camera reprojection technique that updates the displayed viewpoint without
rerendering the scene. It is especially effective for camera rotation and modest
camera translation.

### Late-latched pose

The newest camera pose available immediately before presentation. It can be newer
than the pose used when the scene was rendered.

## Phase 1: Stabilize the current synchronous implementation

### Objectives

Make the current implementation predictable before introducing threading and
additional presentation complexity.

### 1.1 Verify pacing

Test with fixed refresh rates and known game limits:

| Display refresh | Real render rate | Expected warp output |
|---:|---:|---:|
| 120 Hz | 60 FPS | approximately 120 FPS |
| 120 Hz | 40 FPS | approximately 80 FPS |
| 144 Hz | 72 FPS | approximately 144 FPS |
| 144 Hz | 60 FPS | approximately 120 FPS |

The current implementation emits exactly one warp frame per real frame. It cannot
maintain the display refresh rate once real rendering falls below half-refresh.

### 1.2 Verify OptiScaler's limiter

The reprojection output must not be blocked by Reflex or XeLL limiter selection.
The limiter should apply to the real-frame cadence while the reprojection path
adds the fake presentation.

Expected behavior when `FramerateLimit=120` on a 120 Hz display:

```text
Real render cadence: approximately 60 FPS
Warp presentation cadence: approximately 120 FPS
```

If the limiter is disabled, the game may render as fast as possible and the
reprojection pacing becomes unstable. The game limit can remain the fallback while
the OptiScaler limiter is validated.

### 1.3 Validate resource lifetime

Test repeatedly:

- Starting and stopping the game.
- Toggling reprojection off and on.
- Changing resolution.
- Switching windowed/borderless/fullscreen modes.
- Alt-tabbing.
- Minimizing and restoring the window.
- Loading a new level.
- Triggering a scene reset.
- Device removal or driver reset where practical.

Every path must release or invalidate:

- Cached color resources.
- Private warp outputs.
- Command allocators.
- Command lists.
- Fences and fence events.
- Resource readiness flags.
- Camera history.
- Published frame metadata.

### 1.4 Validate input sources

Test each source separately:

1. DLSS input replaced by FSR2.
2. XeSS input replaced by FSR2.
3. FSR2 input hooks where supported.
4. FSR3/FFX input path.
5. Streamline resources, if explicitly supported.

`AReproj_Dx12::Present()` selects `GetIndexWillBeDispatched()`, matching the
Streamline/FSR3/FFX input paths while still selecting the current slot for the
upscaler path. These paths still need end-to-end validation before being treated
as supported.

### 1.5 Improve diagnostic logging

The current per-frame debug logging is too large for crash diagnosis. Add concise
rate-limited events for:

- Reprojection activation/deactivation.
- Swapchain creation and buffer count.
- Resource readiness failures.
- Fence waits exceeding a threshold.
- Present failures.
- Device removal.
- Frame-packet publication failures.
- Warp-frame drops.

Avoid logging every successful dispatch at error or info level.

Implemented diagnostic summary once per second:

```text
Reproj: real=59.8 FPS, warp=59.8 FPS, dropped=0
```

Pose age and queue depth require the published-frame worker.

## Phase 2: Improve warp quality

### 2.1 Depth-aware reprojection

The depth-aware path should reconstruct a world-space position for each output
pixel:

```text
screen UV + depth
    -> view-space position
    -> world-space position using the source camera
    -> target-camera projection
    -> source color sample
```

Required data:

- Source depth texture.
- Source color texture.
- Source view matrix or camera basis.
- Source projection parameters.
- Target camera pose.
- Depth convention, including inverted depth.
- Motion-vector fallback.

If depth reconstruction fails, use motion-vector warp rather than aborting the
entire frame.

### 2.2 Disocclusion detection

Depth reprojection exposes regions that were not visible in the source frame.
Add a confidence test using:

- Reprojected depth.
- Neighboring source depth.
- Motion-vector disagreement.
- Screen-edge distance.
- Depth discontinuity.

Low-confidence pixels should use one of:

1. Motion-vector fallback.
2. Neighbor expansion.
3. A conservative edge color.
4. A previous-frame sample.
5. A debug color in diagnostic mode.

Do not introduce a complex hole-filling model until the basic confidence mask is
measured against real footage.

### 2.3 Camera-only mode

Add a mode that intentionally ignores object motion vectors and uses only camera
reprojection. This is useful for testing late-latched camera behavior and avoids
mixing two different motion sources while debugging.

Suggested modes:

```text
0 = motion-vector warp
1 = depth-aware camera warp with motion-vector fallback
2 = camera-only timewarp
```

### 2.4 HUD and UI handling

HUD and UI should not be treated like world geometry. Options:

- Use the existing HUDless/UI resources where available.
- Keep UI in screen space and composite it after the world warp.
- Detect static UI using near-zero motion vectors.
- Add a small UI exclusion mask.
- Fall back to the original image in UI regions.

The first production-safe choice should prefer stable UI over aggressive world
reprojection. HUD ghosting is more noticeable than small disocclusion errors.

## Phase 3: Introduce a published-frame model

### 3.1 Frame packet

Create a frame packet representing one completed real frame:

```cpp
struct ReprojFramePacket
{
    ID3D12Resource* color;
    ID3D12Resource* depth;
    ID3D12Resource* velocity;

    CameraPose sourcePose;
    CameraPose latestKnownPose;
    Matrix4x4 sourceView;
    Matrix4x4 sourceProjection;

    UINT width;
    UINT height;
    UINT64 frameId;
    double renderTimestamp;
    double frameDelta;

    D3D12_RESOURCE_STATES colorState;
    D3D12_RESOURCE_STATES depthState;
    D3D12_RESOURCE_STATES velocityState;

    ID3D12Fence* completionFence;
    UINT64 completionFenceValue;
};
```

The exact structure should reuse existing project types where possible. Do not
introduce duplicate camera or resource abstractions if `IFGFeature` can be
extended safely.

### 3.2 Ring-buffer ownership

Use a small ring of packets, likely three or four entries:

- One packet being rendered or copied.
- One packet ready for presentation.
- One packet currently being warped/presented.
- Optional spare packet for resize or pipeline latency.

Ownership transitions must be explicit:

```text
FREE -> CAPTURING -> READY -> PRESENTING -> RETIRED -> FREE
```

A packet must not be reused until the GPU fence confirms that all reads are
complete.

### 3.3 Publication rules

At the end of a real frame:

1. Finish required resource copies.
2. Capture source pose and matrices.
3. Signal a completion fence.
4. Publish the packet atomically.
5. Allow the game render path to continue.

If no packet is ready, the presentation thread should reuse the last valid packet
rather than reading an incomplete resource.

## Phase 4: Dedicated reprojection thread

### 4.1 Thread responsibilities

The reprojection thread owns:

- Selecting the newest completed packet.
- Sampling the latest known camera pose.
- Waiting until the next presentation deadline.
- Dispatching the warp pass.
- Presenting the warped image.
- Dropping stale warp work when a newer packet arrives.
- Handling occlusion and device-removal results.

The game thread owns:

- Simulation.
- Scene rendering.
- Upscaling.
- Publishing completed frame packets.

### 4.2 Present-thread lifecycle

Add explicit states:

```text
Stopped
Starting
Running
Paused
Draining
Stopping
Failed
```

State changes must be synchronized and must not happen while holding a D3D12
resource mutex longer than necessary.

### 4.3 Shutdown sequence

The safe shutdown order is:

1. Stop accepting new packets.
2. Signal the reprojection thread to stop.
3. Wake it if it is waiting on a timer or fence.
4. Join the thread.
5. Wait for outstanding GPU work.
6. Release packet resources.
7. Release warp resources.
8. Release the swapchain.
9. Destroy fences and events.

Never destroy the device-facing resources while the reprojection thread is still
able to submit work.

### 4.4 Presentation timing

Use a high-resolution wait strategy:

1. Calculate the next display deadline.
2. Sleep using a waitable timer until close to the deadline.
3. Busy-wait only for the final small interval.
4. Dispatch and present the warp.
5. Advance the deadline using refresh timing rather than accumulating frame error.

Avoid using `Sleep()` for the whole interval because timer granularity and scheduler
jitter will cause visible cadence errors.

## Phase 5: Late camera sampling

### 5.1 Why current camera data is insufficient

The current camera values are generally captured during the upscaler dispatch. They
represent the pose associated with the last rendered frame, not necessarily the
latest camera input at presentation time.

To get true timewarp behavior, the reprojection thread needs a newer pose.

### 5.2 Candidate pose sources

Evaluate in this order:

1. Camera data already supplied by the game's FSR2/DLSS parameters.
2. Camera constants captured from known engine buffers.
3. Engine-specific camera hooks.
4. Input-based orientation extrapolation.
5. Extrapolation from the previous two captured poses.

The generic fallback should be pose extrapolation, but it must be clearly marked as
an estimate.

### 5.3 Rotation-first implementation

Start with camera rotation only:

- Keep camera position fixed.
- Update orientation from the newest pose.
- Reproject using depth.
- Measure visual stability and latency.

Rotation-only timewarp is substantially less prone to holes than translation
because it does not require revealing large amounts of previously hidden geometry.

### 5.4 Translation support

Add translation only after the rotation path is stable. Translation requires:

- Correct world scale.
- Accurate camera position.
- Reliable near/far parameters.
- Stronger disocclusion detection.
- More aggressive hole handling.

The existing meter-factor and camera basis data should be reused rather than
creating a second world-scale configuration system.

## Phase 6: Adaptive multiple warp frames

### 6.1 Basic scheduling

Given:

```text
refreshPeriod = display interval
realFramePeriod = time between published real frames
```

Estimate the number of warp presentations required:

```text
warpCount = clamp(ceil(realFramePeriod / refreshPeriod) - 1,
                  0,
                  ReprojMaxWarpFrames)
```

The actual number must also consider whether the next real frame is already
available and whether the previous warp completed in time.

### 6.2 Example

At 120 Hz:

| Real FPS | Real period | Approximate warp count |
|---:|---:|---:|
| 120 | 8.3 ms | 0 |
| 60 | 16.7 ms | 1 |
| 40 | 25.0 ms | 2 |
| 30 | 33.3 ms | 3 |
| 20 | 50.0 ms | 5, usually capped |

These are presentation counts, not new rendered frames.

### 6.3 Artifact and latency limits

Never allow unlimited warp accumulation. Add a configurable maximum, for example:

```ini
ReprojMaxWarpFrames=3
```

When the real frame rate becomes very low:

- Prefer repeating the newest warp rather than extrapolating indefinitely.
- Display a diagnostic warning.
- Avoid queueing stale warps after a new real frame arrives.
- Consider reducing warp strength as pose extrapolation grows.

### 6.4 Frame drops

If the warp misses its deadline:

- Do not block the game thread.
- Drop the late warp if the next deadline is imminent.
- Reuse the last completed warp if safe.
- Record a dropped-warp counter.

## Phase 7: Swapchain and synchronization hardening

### 7.1 Backbuffer ownership

The current replacement swapchain exists because fake presentation needs safe buffer
availability. The asynchronous design must make ownership explicit across:

- Game render submission.
- Real presentation.
- Warp command submission.
- Fake presentation.
- Resize.
- Shutdown.

Never assume that the current swapchain backbuffer is free merely because a present
returned.

### 7.2 Resource states

Track every resource state explicitly:

```text
Color source:
  COPY_DEST -> NON_PIXEL_SHADER_RESOURCE

Depth / velocity:
  game-provided state -> NON_PIXEL_SHADER_RESOURCE

Warp output:
  COMMON -> UNORDERED_ACCESS -> COPY_SOURCE -> COMMON

Backbuffer:
  PRESENT -> COPY_DEST -> PRESENT
```

The caller must account for the existing `CreateBufferResource` behavior: matching
resources are reused without automatic state transitions.

### 7.3 Fence model

Use separate fence responsibilities where useful:

- Capture-copy completion.
- Warp command completion.
- Packet retirement.
- Swapchain resize drain.

Do not use a single fence value ambiguously for both allocator reuse and packet
lifetime.

## Phase 8: UI and configuration

Add controls only after the underlying behavior works.

Suggested options:

```ini
ReprojMode=0
ReprojTimeStep=0.5
ReprojStrength=1.0
ReprojMaxWarpFrames=1
ReprojRotationOnly=false
ReprojUseDepth=true
ReprojTargetRefresh=0
ReprojDebugView=false
```

UI should expose:

- Current real FPS.
- Current warp FPS.
- Target refresh rate.
- Number of warps per real frame.
- Dropped warp count.
- Pose age.
- Pose age (source-pose age in the safe synchronous presenter).
- Resource readiness.
- Whether late camera data is available.

Avoid presenting a generic “infinite FPS” claim. The UI should label warped frames
as synthesized camera reprojections and show their actual cadence.

## Phase 9: Reflex and fakenvapi integration

Once the asynchronous path is stable:

1. Mark real frames separately from warp presentations.
2. Report the correct frame type to fakenvapi. Reproj is included in
   `reportFGPresent`; each internal fake present is identified through the normal
   present counter. Worker-side reporting belongs to the future compositor path.
3. Avoid reporting every warp as a new game-rendered frame.
4. Associate latency markers with the real render frame.
5. Ensure Reflex sleeps do not stall the presentation thread.
6. Ensure the game thread is not blocked by presentation-thread pacing.

This is important because incorrectly reporting warp frames can distort latency
metrics and cause external frame limiters to fight the reprojection scheduler.

## Phase 10: Test matrix

### Hardware and API

- AMD DX12.
- NVIDIA DX12.
- Intel DX12 where supported.
- VSync on and off.
- Tearing allowed and unavailable.
- Borderless and exclusive fullscreen.
- 60, 120, 144, and 240 Hz displays where available.

### Input paths

- DLSS input replaced by FSR2.
- XeSS input replaced by FSR2.
- Native FSR2 hook path.
- FSR3/FFX input path.
- Low-resolution motion vectors.
- Display-resolution motion vectors.
- Inverted depth.
- Jittered motion vectors.
- Resource-flip enabled and disabled.

### Motion scenarios

- Static camera.
- Pure camera rotation.
- Forward/backward camera translation.
- Strafing.
- Fast camera pan.
- Independent moving objects.
- Particles and transparency.
- Shadows and reflections.
- Heavy disocclusion.
- Static HUD.
- Animated HUD.
- Menus and cutscenes.

### Failure scenarios

- Missing motion vectors.
- Missing depth.
- Scene reset.
- Resolution change.
- Window resize.
- Alt-tab.
- Minimize/restore.
- Device removed.
- Game shutdown during an active warp.
- Reprojection toggled during a frame.
- Upscaler switched while reprojection is active.

## Suggested implementation order

1. Keep the current synchronous path as the fallback.
2. Add concise runtime metrics.
3. Add frame-packet ownership without a second thread.
4. Add explicit fences and packet retirement.
5. Add a presentation worker that still emits only one warp per real frame.
6. Verify no game-thread deadlock or resource reuse bugs.
7. Add display-deadline scheduling.
8. Add rotation-only late pose sampling.
9. Add multiple warp presentations.
10. Add depth-aware translation.
11. Add HUD/UI composition.
12. Add Reflex/fakenvapi reporting.
13. Enable the asynchronous path by default only after extended gameplay testing.

## Definition of done

The continuation is successful when all of the following are true:

- The game can render below display refresh without blocking on every warp.
- The presentation thread can produce multiple warp frames per real frame.
- Camera rotation responds using a pose newer than the source render frame.
- The system remains stable through resize, alt-tab, and shutdown.
- World depth and camera reprojection are used where valid.
- HUD/UI is not permanently smeared.
- Real FPS and warp FPS are reported independently.
- The system gracefully degrades when motion vectors, depth, or late pose data are
  unavailable.
- No vendor frame-generation library is required.
- The synchronous implementation remains available as a safe fallback.

## Known quality ceiling

This system can make a low-rate rendered scene appear much smoother, especially for
camera rotation. It cannot reveal geometry that was never rendered. Large camera
translations, independently moving objects, particles, transparency, shadows, and
post-processing will expose the limits of image-space reprojection.

The goal is therefore not mathematically infinite correct frames. The goal is a
low-latency, high-refresh camera presentation layer that uses the latest available
scene frame and fails gracefully when reprojection confidence is low.
