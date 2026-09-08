# async-simple ongoing plan

Updated: 2026-09-07
Branch: `async-simple`
Primary live target: Kingdom Come: Deliverance II (KCD2)
Regression target: Deep Rock Galactic (DRG)

This is the single source of truth for active async-timewarp development planning on `async-simple`. Update this file as work lands or priorities change. Do not create separate roadmap, handoff, continuation-plan, artifact-options, or experiment-plan Markdown files. Durable implementation facts belong in `AGENTS.md`; active work and decisions belong here.

## 1. Project goal

Build a simple, low-latency asynchronous timewarp path that keeps KCD2's renderer independent from the display presenter, presents at display cadence, applies the freshest practical rotational camera correction, leaves KCD2's HUD unwarped, and fails closed to ordinary presentation when data or capabilities are invalid.

The branch should stay deliberately smaller than `async-timewarp`. Complexity is allowed only when a measured KCD2 A/B demonstrates that it improves latency, cadence, stability, or visible quality.

## 2. Current baseline

The simplified architecture is already established and should not be rewritten without evidence:

- The game renders into wrapper-owned virtual backbuffers while the presenter owns the real DXGI swapchain.
- Game `Present()` captures an anchor inline on the game's DIRECT queue. Packet pressure or a busy capture allocator drops the anchor instead of CPU-waiting on reprojection GPU work.
- The presenter uses one NORMAL-priority DIRECT queue, selects the newest completed anchor, and performs one real present per display slot.
- Repeated slots are real rotation warps of the active anchor, not blits.
- `FrameSlot[3]` and the existing capture/retirement fences define packet ownership.
- The final display warp is rotation-only. Depth and motion vectors are not part of the final homography.
- A CPU-signalled deferred latch parks already-submitted warp work and releases it at a fixed late-sample lead, 3 ms by default. Raw mouse motion and the newest valid KCD2 camera state provide the late rotational correction.
- KCD2 camera acquisition is build-gated and RTTI-validated. Unknown builds fail closed.
- KCD2 Scaleform HUD isolation is live-validated. World content is warped and the isolated HUD is composited unwarped.
- `[AsyncTimewarp] SourceFramerateLimit` is opt-in. The 60 -> 120 A/B has been live-validated and is the preferred controlled test mode.
- `[AsyncTimewarp] ContentInterpolation` is an opt-in one-midpoint FSR content-generation experiment. Generated and real world content receive the same final late rotation and unwarped HUD composite.

## 3. Hard invariants

These rules take priority over feature work.

1. The game thread never CPU-waits for reprojection GPU work. Drop an anchor rather than stall rendering.
2. Latest completed anchor wins. Never block the presenter on a newer incomplete anchor.
3. One paced real present per display slot. No catch-up bursts and no packet-arrival presents.
4. Repeated slots are re-warped with fresh rotational input.
5. KCD2 HUD isolation remains single-submit and the HUD remains unwarped whenever isolation is available.
6. Presenter stop and failure downgrade must be deadlock-proof. Release any outstanding late-latch gate before join/drain.
7. The final warp remains rotation-only until a separate measured project justifies positional reprojection.
8. Failures degrade to plain passthrough. Do not restore the old synchronous generated-frame fallback machinery.
9. Do not restore the parent branch's COPY worker, capture worker, COMPUTE queue, UI borrowing, repeat-warp shedding, hitch hold, heavy per-slot telemetry, target-pose stack, or positional depth/MV warp unless a controlled A/B proves a specific need.
10. Every reprojection change follows the full build, install, KCD2 live-test chain before it is called validated.

## 4. Priority 0: screen-edge artifacts

This is the current quality blocker.

Live KCD2 testing established that the attempted render-reserve hook did not widen the rasterized world image. The presenter mapping only cropped the existing image and reduced the displayed FOV, so the feature and all associated guard plumbing were removed on 2026-09-07. Edge work must not assume offscreen source coverage that has not been proven in the captured color.

### E0. Establish a repeatable baseline

Use the same KCD2 scene and camera motion for every A/B. Start with `SourceFramerateLimit=60` on a 120 Hz display so cadence is controlled.

Capture short tests with slow pans, ordinary mouse turns, fast flicks, vertical turns, diagonal turns, first-person geometry near the edge, foliage, and a static skyline/building edge. Record the normal 1 Hz reprojection log with each run. Compare `ContentInterpolation=false` and `true` separately so final-warp behavior is not confused with midpoint generation.

### E1. Measure required source coverage

Add cheap aggregate diagnostics without putting GPU readback or per-slot logging into the hot path.

For each requested rotation, use the same CPU-baked homography as the warp and evaluate a small fixed set of boundary samples, for example the four corners plus 8-16 points around the screen perimeter. Track at least:

- maximum source-UV overrun per edge;
- percentage or count of boundary samples outside valid source coverage;
- requested angular delta;
- anchor pose age;
- eventual safe-warp scale once E3 exists.

Aggregate into the existing 1 Hz health line or similarly cheap counters. The goal is to quantify the 95th and 99th percentile source-coverage deficit in KCD2 rather than tuning by feel alone.

Status (2026-09-07): code done, live validation pending. The presenter now
samples a fixed 16-point output perimeter from the CPU-baked homography and
reports the requested raw-UV coverage demand once per second: invalid sample
count, per-edge maximum overrun, requested rotation, safe scale, and limiter
activations. It does not add GPU readback, per-slot logging, a game-thread
wait, or a queue/fence dependency. The subsequent live sweep showed that the
camera hook changed a field but did not widen captured world coverage; the
reserve control and guard mapping were therefore removed.

Correction (2026-09-07): the first live build evaluated the NDC homography
with pixel coordinates, falsely reporting coverage loss and clamping every
late rotation to zero. The evaluator now converts the perimeter pixel centers
to NDC before testing. The affected artifact is invalid for feel evaluation;
rebuild before resuming E0/E3 testing.

### E2. Verify shader validity and filtering

Audit `RPD` so validity is determined from the raw reprojected coordinate before any clamp/saturate operation. Sampling coordinates may be clamped for safety, but a clamped coordinate must never be treated as valid warp coverage.

The valid rectangle must account for the sampler footprint. For bilinear sampling, keep at least half a texel inside the source bounds. If a wider reconstruction filter is introduced later, expand the inset accordingly.

Keep a diagnostic mode that can display invalid coverage distinctly. Remove or disable it for normal builds if it adds hot-path cost.

Acceptance: no offscreen coordinate can turn into a stretched last-row/last-column smear through clamp sampling.

Status (2026-09-06): code done, live validation pending. `RPD.hlsl` / `RP_Common.h` / `RPD_Shader.h`(+`.cso`) now use a half-texel-inset valid rect with the feather measured from the inset edge, and `DebugView == 1` paints invalid coverage magenta ahead of the UI composite (`debugView == 0` in normal builds). Pinned by `test_rpd_edge_validity_is_filter_safe_with_debug_view`. Still needs the E0 `DebugView=1` footage check in KCD2.

Controls (2026-09-07): `[AsyncTimewarp] DebugView` (default off) plus menu `Edge debug view` now drive `RP_Constants::debugView` instead of the hardcoded 0. Visualization only; recommended off for play, on for short edge footage.

### E3. Add maximum-safe-warp limiting

Prevent the final rotation from exposing more source area than the current packet actually contains.

For the requested source-to-target rotation, find the largest scale `s` in `[0, 1]` for which the sampled output boundary remains inside the filter-safe source region. Apply:

```text
safeRotation = slerp(identity, requestedRotation, s)
```

A small fixed-iteration binary search over `s` is acceptable initially because the boundary test is tiny and CPU-side. An analytic bound can replace it only if measurement shows this matters.

Normal motion should remain `s = 1`. On an extreme flick, deliberately leave a little residual rotational latency instead of revealing a large invalid border.

Acceptance:

- ordinary mouse motion is normally full-warp;
- large flicks degrade by reducing warp magnitude, not by producing a large smeared/black/lagging strip;
- the limiter never changes anchor ownership, queue topology, or game-thread behaviour.

Status (2026-09-07, revised same day): live telemetry proved the
zero-overrun policy wrong. Every motion second showed `scale=0.000` with
`limited`~=all slots (`late=120/120` but the applied warp clamped to
identity), i.e. the warp was fully neutralized while frames displayed
25-45 ms stale — floaty and worse than native. Root cause: any real rotation
moves uncovered content in at one edge (~22 px/degree), so demanding zero
invalid boundary samples can only ever return `s=0`; the policy assumed the
removed render reserve would supply margin that does not exist. The validity
criterion is now an edge-width budget (12 px, per-axis): ordinary turns pass
at `s=1`, large flicks clamp to partial, raw overruns still report the true
demand, and identity verdicts are float-stable. Pinned by
`tests/reprojection/test_edge_limiter.py`. Re-run the E0 motion sweep and
expect `scale=1.000 limited=0` on gentle pans with bounded strips on flicks.

Controls (2026-09-07): `[AsyncTimewarp] SafeWarpBudget` (default 12 px, 0 = off/full warp, clamp 0..32) plus menu `Safe warp budget` replace the hardcoded 12 px constant. `0` forces `invalidSamples = 0` so the binary search is skipped. Recommended 12; 0 only to diagnose floatiness vs edge smear; 16-20 for looser flicks. `[AsyncTimewarp] MaxTimeStep` (default 2.5 frames, clamp 1.0..4.0) plus menu `Max warp step` replace the hardcoded 2.5 cap. Recommended 2.5; 1.5 if anchor-switch snaps appear.

### E4. Remove the ineffective KCD2 reserve

Completed (2026-09-07). Live testing showed that the temporary `CCamera` FOV mutation did not widen the captured world image. Its presenter-side guard mapping only cropped the image and reduced displayed FOV. The config key, menu control, camera mutation, packet metadata, FSR FOV override, warp crop, and `guard=%` telemetry were removed. Do not restore this mechanism without direct captured-image proof that an earlier projection hook produces genuine additional world coverage.

### E5. Temporal border recovery from older anchors

Only after safe limiting is solid, retain up to two older compatible world anchors as a fallback for pixels that are invalid in the newest anchor.

Status (2026-09-07): code done, Windows CI and live validation pending. The
public opt-in is `[AsyncTimewarp] HistoryBorderFallback`, default off, with
matching menu, INI, and startup-effective-value logging. Two presenter-owned
textures snapshot real HUDless anchors on their first real display; the same
DIRECT queue orders each snapshot after prior history reads and the packet's
normal retirement fence covers the copy. History accepts only matching
dimensions/format/sample count/HDR/cut generation, <=0.25 degree FOV drift,
<=0.5% aspect drift, and <=75 ms age. The shader samples current-valid content
first, then newest/older valid history, then the bounded spatial fallback; it
never blends history over valid current content and composites the current HUD
last. The existing safe-warp budget remains active independently.

Conceptually:

```text
newest anchor valid -> sample newest
otherwise history N-1 valid -> sample N-1
otherwise history N-2 valid -> sample N-2
otherwise -> spatial fallback
```

This must use explicit resource/fence ownership. Do not sample a packet after it has been recycled.

Reject history across camera cuts, FOV/aspect changes, stale resources, or excessive age. Start with a conservative maximum age around 50-100 ms and tighten from footage. History is for invalid border pixels only. Do not blend previous-anchor imagery across already-valid current content, because that previously produced visible ghost/double images during rotation.

Moving NPCs, weapons, foliage, particles, lighting, and exposure changes can make historical pixels wrong, so history fill must remain a fallback rather than the baseline warp.

Live decision (2026-09-07): rejected. The KCD2 run never produced an
eligible history sample (`hist=0/0/0/0`), and temporal color cannot solve the
dominant sustained-turn case even if eligibility is repaired: the newly
revealed leading edge lies outside every older view. Remove E5 after the
replacement experiment proves viable rather than spending another iteration
on history thresholds.

### E5 replacement: predictive render-pose steering

Move source coverage toward the expected scanout pose before KCD2 renders,
then retain the existing late rotation only as a residual correction. This is
the same broad ordering disclosed for NVIDIA Reflex Frame Warp: predict the
render camera, render world/G-buffer content near that pose, then correct to
the newest input at display time.

Implementation is gated on direct captured-image proof. The first diagnostic
records the live gameplay `CView` caller of the existing
`CCamera::UpdateFrustumPlanes` observer. Locate an earlier render-view
construction point from that call path and add a fixed, opt-in one-degree yaw
probe. The probe passes only if it shifts genuine captured world coverage and
culling while an inverse final warp restores the original presented center.
It fails if it merely crops/shifts the completed image or mutates gameplay
camera state.

Only after that proof should the probe become bounded prediction from the
existing passive raw-input totals and rendered-camera velocity. Store the
exact steered render basis per packet and late-warp from that basis to the
fresh target. Keep HUD isolation last, keep the game thread GPU-wait-free,
disable `ContentInterpolation` during initial validation, reject unknown game
builds, and fail closed to the unmodified camera path on any ambiguity.

Status (2026-09-08): implemented first diagnostic probe.
- The gameplay frustum caller probe in commit `9dcd5034` reported caller RVA `0x7F12E6`. Reverse engineering `WHGame.dll` located `CView::Update` at `0x7F0EC0`. It performs two calls to `CCamera::UpdateFrustumPlanes`: the first via `CCamera::SetFrustum` (`0x7F12E6`) before camera matrix computation, and the second directly at `0x7F1A63` (caller RVA `0x7F1A68`) immediately after writing camera matrix rows `0x00..0x2F` into `CCamera` (`cview + 0xE8`).
- Added opt-in knob `[AsyncTimewarp] PredictiveProbe=auto` (`ReprojPredictiveProbe`, exposed in in-game menu).
- When active on gameplay cameras at caller RVA `0x7F1A68`, applies a fixed +1.0 degree yaw bias around CryEngine world Z (`[0, 0, 1]`) directly to the camera matrix before `UpdateFrustumPlanes` updates frustum culling. Because `CView::Update` reconstructs the camera matrix each frame from internal view parameters, game state is not permanently mutated.
- Packet captures both the biased render basis and the unbiased target basis.
- Presenter `ApplyLateInput` detects `biasYaw != 0` and warps from the biased render basis to the unbiased target pose, producing an exact -1.0 degree residual yaw warp that restores the presented center while shifting genuine rasterized coverage.
- Next step: live validate in KCD2 with `PredictiveProbe=true` and `ContentInterpolation=false`. Confirm log message, stable center/crosshair, and edge shift.

### E5.1 Predictive overscan investigation gate (2026-09-08)

Full predictive overscan must satisfy the required pipeline: widen the *rendered* world (not the displayed crop), preserve nominal pixels-per-degree, capture matching oversized colour/depth + projection metadata, warp/crop late, composite HUD normal. The 2026-09-06/07 reserve failed exactly because it violated this: mutating `CCamera` FOV at `+0x30` without enlarging the viewport/render-target only re-mapped more world onto the same pixels and the presenter guard then cropped it back — FOV loss, no extra geometry (see `45ff0db3`).

**Exact KCD2/CryEngine path traced (retail 1.5.6, `WHGame.dll`):**
- `CView::Update @0x7F0EC0` is the per-frame render-view constructor. It writes the world-space camera basis into `CCamera` at `cview+0xE8` (`matrix[0..2][0..3]` = right/forward/up/position), then calls `CCamera::UpdateFrustumPlanes` at `0x7F1A68` (second call) to rebuild culling frustum planes from `CCamera +0x30..0x7C` (`vertical FOV @0x30`, `pixelAspect @0x40`, `nearEdge @0x50..0x58`, `projEdge @0x5C..0x64`). The first call at `0x7F12E6` via `SetFrustum` occurs *before* the matrix is built and is not a viable injection point.
- Viewport and render-target extent are *not* set inside that `CView` call. Viewport comes later from the D3D12 `RSSetViewports`/`OMSetRenderTargets` path driven by the game's swapchain-sized virtual backbuffer (`WrappedIDXGISwapChain4::InitializeReprojectionVirtualization` allocates virtual buffers sized to `desc.Width/Height`). HUD isolation confirms this split: `ResTrack_dx12::hkOMSetRenderTargets` already intercepts Scaleform to redirect HUD to a private transparent `uiTexture` (`Kcd2HudIsolation::TryRedirect`), while world rendering still targets the virtual backbuffer. Thus widening the frustum alone leaves the viewport at nominal size — the precise failure mode observed.
- `Kcd2Camera.cpp` already notes the KCD2-shifted `CCamera` layout (20 floats at `+0x30..0x7C`, Viewport ints repurposed to `1,0` at `+0x34/38`), validated via `DescribeProjection`. TAA jitter, depth, MV, post, and dynamic-resolution all derive from the same `CCamera` projection + viewport; changing one without the other breaks motion-vector scale (`mvScaleX/Y`, `jitterX/Y` in `ContentFrame`/`RP_Constants`) and the `HistoryBorderFallback` dimension checks.

**Gate answers:**
1. *Where KCD2 builds projection/culling:* `CView::Update → CCamera matrix @0xE8 → UpdateFrustumPlanes @0x7F1A68` (culling). Projection derived from `CCamera +0x30..0x64` plus matrix.
2. *Is existing `CView` callback early enough?* For *orientation* yes, for *extent* no. The `0x7F1A68` hook is the latest point before culling but before viewport binding. It can shift culling toward prediction (proven by the +1° probe) but cannot by itself enlarge rasterisation.
3. *Can projection/culling/viewport/render-target be changed consistently?* Only with a second hook. Options: (a) enlarge the virtual backbuffer itself (simplest but enlarges HUD too — defeated by isolation), or (b) keep virtual backbuffer nominal and add an oversized private world RT (`hudlessTexture` enlarged) with a world-phase `OMSetRenderTargets`/`RSSetViewports` redirect. (b) is required to preserve HUD at normal scale (`ContentInterpolation` already keeps HUD unwarped). Both require proving which `RSSetViewports` calls correspond to world vs HUD — currently unproven.
4. *Dependencies:* TAA jitter (`jitterX/Y`, `jitterCancelled`), depth/MV (`mvWidth/Height`, `mvScaleX/Y`, `invertedDepth`, `hdr`), post, dynamic resolution scale, and HUD scale all couple to `cameraVFov`/`cameraAspect`/`displayWidth/Height` in `RP_Constants` and `HybridFsrGenerator`. Overscan must publish both the *rendered* frustum (widened/asymmetric, for sampling) and the *nominal* display frustum (for inverse warp), and keep MV scale consistent with the enlarged render size.
5. *Build gating:* Only `WHGame.dll` RTTI-validated `CView` (`IsCViewVtable` → `.?AVCView@@`) with signature-bound `UpdateFrustumPlanes` is safe. Unknown builds must fail closed to unmodified camera (existing behaviour in `Kcd2Camera::Hook`). The viewport-hook path must inherit the same build gate — no new signature, no enable.

**Decision:** Do **not** implement full predictive overscan yet. The pre-culling hook exists and the +1° probe validates the steer-then-inverse-warp plumbing, but genuine overscan requires proven viewport/render-target enlargement. A second fake guard (FOV-only) is prohibited. Next reverse-engineering step is to instrument the live KCD2 frame with diagnostic logging of `RSSetViewports`/`RSSetScissorRects`/`OMSetRenderTargets` extents during world vs Scaleform phases (behind `PredictiveProbe` diagnostics), capture a debug frame-dump showing distinct world geometry outside the nominal rect, and only then gate an opt-in oversized world RT + asymmetric frustum (`SetAsymmetry`-style `l/r/b/t` from `CCamera` near/proj edges) with predicted bias (`latest validated camera motion + passive raw mouse`, capped ~10-12% total, symmetric safety 6-8% + predictive up to 6% toward turn, horizon ~1 frame + latch lead). Prediction must steer reserve placement only, never the visible gameplay camera. Preserve `async-simple` invariants (inline game-DIRECT capture, 3 packet slots, one NORMAL DIRECT presenter queue, fixed 3 ms late latch, non-blocking game thread, `SafeWarpBudget`/`MaxTimeStep` unchanged). Requires `PLAN.md` approval after captured-image proof.

Status (2026-09-08, unvalidated): viewport diagnostic implemented, live test pending.
- `ResTrack_dx12` now hooks `RSSetViewports` (vtable 21) + `RSSetScissorRects` (22) alongside `OMSetRenderTargets` (46). Passive observers only: never edit args, call original unconditionally, no allocation/wait.
- Logging gated by `[AsyncTimewarp] PredictiveProbe`; rate-limited to first 24 viewport + 24 scissor calls, each tagged `phase=world|hud` via `Kcd2Scaleform::IsActiveOnThisThread()`. Probe off = single Config branch overhead.
- Attach/detach mirrors existing command-list hooks (`HookCommandList`, `ReleaseDeviceHooks`, `ReleaseHooks`); failure path nulls new originals.
- Static check only: `pytest tests/reprojection/ -q` 84 passed. No Windows build yet. Live gate: KCD2 with `PredictiveProbe=true`, confirm `KCD2 viewport/scissor` lines split world vs hud and viewport matches virtual backbuffer size before any oversized-RT work.

### E6. Tiny spatial fallback for the final gap

If a few invalid pixels remain after current/history sampling, use a deliberately small peripheral fallback. Candidate order:

1. short smooth feather into nearby valid content;
2. bounded nonlinear edge stretch over only a few pixels;
3. small push-pull/pyramid fill if it materially beats stretching.

Do not allow a spatial fill to cover a large fraction of the screen. If the hole is large, E3 should reduce the warp instead.

### Edge-artifact definition of done

- Normal KCD2 camera movement does not show obvious clamped smear, black wedges, or a persistent lag band at screen edges.
- Fast flicks remain visually bounded by safe-warp limiting.
- Historical fill does not create obvious edge ghosts on moving objects.
- 60 -> 120 cadence and source performance remain within the established healthy range.
- No edge-quality change adds a game-thread GPU wait or changes the simplified queue topology.

## 5. Priority 1: walking judder and content motion

Rotation-only timewarp cannot synthesize translation parallax, walking, head bob, animation, recoil geometry, or newly revealed scene content. Treat those as content-motion problems rather than stretching the rotation warp into a positional one.

### W0. Validate the existing FSR midpoint path

Run controlled KCD2 60 -> 120 tests with `ContentInterpolation` off/on in the same walking and hill scenes. Verify:

- the midpoint is generated only from valid depth/MV/dimension/scale inputs;
- generated and real frames share the intended camera interval and rendered FOV;
- final late rotation and isolated HUD composition behave identically on both;
- failure falls closed to ordinary timewarp;
- source FPS and presenter cadence do not regress materially;
- walking motion is visibly smoother rather than merely different.

Do not enable interpolation by default until it is live-validated.

Status (2026-09-07): failed the first KCD2 visual gate. The 2026-09-06
`ContentInterpolation=true` run was a real midpoint run, not a presenter
fallback: after warm-up it held roughly 60 source FPS and 120 display FPS with
about 60 generated outputs per second (`new≈120`, `repeat≈0`, `generated≈60`).
The captured FSR inputs were internally shape-consistent (1706x960 depth/MV,
1706x960 render size, 2560x1440 output, MV scale 1706/960), but the generated
content was reported visibly smeary/ugly. Keep the feature explicitly off for
KCD2. Do not tune warp constants to hide this: before another opt-in trial,
prove the KCD2 depth/MV semantics and frame pairing with short matched footage
in the same walking/hill scene. The ordinary timewarp segment that followed
returned to approximately 60 new + 60 repeat outputs with `generated=0`.

Follow-up (2026-09-07, runtime pending): fixed a generated-output reuse state
violation. The presenter samples each generated texture as an SRV, but the next
FSR dispatch previously declared that recycled texture as a UAV without a
matching transition. The packet retirement fence makes the ownership safe; the
new transition restores its actual state before FSR writes it. Re-run the same
on/off footage after the Windows artifact is installed. If it is still smeary,
the next gate is KCD2 MV/depth/frame-pair semantics, not presenter cadence.

### W1. Separate parallax from anchor-refresh discontinuity

With a stable 60 Hz source, determine whether residual judder is smooth near-geometry parallax or a periodic snap when the presenter switches anchors.

If the dominant artifact is an anchor-switch rotational snap, test a small cut-gated pose-domain transition on the displayed rotation baseline. Do not blend previous-anchor images. Any smoothing must be disabled across `sourceCutGeneration` changes and must not noticeably delay genuine mouse look.

Status (2026-09-07): camera-feel diagnosis. Reported symptom was
wobbly/viscous/delayed camera response. Two mechanisms found, both fixed
without changing queue topology or the game-thread contract:

- `ReprojSmoothing` defaulted to 0.25, so every session ran an EMA on camera
  angular velocity. The EMA lags motion onset and leaves a geometric creep
  tail after stop, and it biases the FSR midpoint through the smoothed prev
  pose. No A/B justified default-on smoothing, so the default is now 0 (off,
  still opt-in via config/menu). Pinned by `test_smoothing_defaults_off`.
- `ContentInterpolation` generated slots warped with the real anchor's mouse
  baseline and a naive linearly-averaged midpoint orientation. The fallback
  late warp therefore dropped half an interval of mouse motion on every
  generated slot (alternating under-rotation vs neighbouring real slots), and
  lerp shortens larger rotations. Generated frames now carry their own
  midpoint mouse baseline (`GetRawMouseMotionAt` at the midpoint timestamp),
  `ApplyLateInput` warps the actually-displayed content from its own
  baseline/timestamp/cut, and the midpoint orientation is a halfway
  axis-angle slerp. Pinned by
  `test_late_input_warps_selected_content_from_its_own_baseline`,
  `test_midpoint_pose_is_a_halfway_slerp_with_own_mouse_baseline`, and
  `test_halfway_slerp_preserves_angle_while_lerp_shortens_it`.

Live gate: same-scene 60 -> 120 mouse-turn A/B, interpolation off then on;
expect tight onset, no creep after stop, and no alternating judder on
generated slots.

### W2. Positional/depth work remains deferred

Do not revive the old one-pass depth positional warp merely because walking remains imperfect. It previously produced disocclusion tearing, halos, and poor hill/stair behaviour. Revisit positional reprojection only as a separate project with repeatable footage, clear acceptance criteria, and evidence that content interpolation cannot provide the required result.

## 6. Priority 2: performance and cadence proof

Run the controlled matrix before adding new scheduling machinery:

| Test | Warp | HUD isolation | Late latch | Content interpolation |
| --- | --- | --- | --- | --- |
| B0 | off | n/a | n/a | off |
| B1 | identity presenter | off | off | off |
| B2 | rotation | off | fixed | off |
| B3 | rotation | on | fixed | off |
| B4 | rotation + chosen edge handling | on | fixed | off |
| B5 | same as B4 | on | fixed | on |

Run 60 -> 120 first, then the monitor's native/high target refresh.

Record source FPS/frame-time median/p95/p99, maximum game `Present()` block time, display FPS, display interval mean/p95, missed slots, new/repeat ratio, dropped anchors, capture-not-ready count, pose age, effective latch lead, safe-warp scale distribution, and temporary warp GPU duration if needed.

Targets:

- no repeatable source-FPS loss caused by CPU blocking;
- source median/p95 remain close to reprojection-off baseline;
- steady presenter miss rate below roughly 1% after warm-up;
- 60 -> 120 gives approximately equal new/repeat outputs;
- higher target refresh adds repeat warps without throttling the source.

If the identity presenter is clean but rotation hurts source cadence, investigate warp GPU/queue contention rather than adding a scheduler that hides it.

## 7. Priority 3: shorten the post-latch critical path

Only optimize what the performance matrix measures. Investigate in this order:

1. `RPD` shader execution time;
2. warp-output to real-backbuffer copy cost;
3. HUD composite cost;
4. presenter DIRECT queue scheduling under VKD3D;
5. unnecessary barriers or allocator waits.

Every millisecond removed after late-latch release can move the input sample roughly one millisecond closer to scanout.

A COMPUTE-queue warp experiment is allowed only if measurements show the second DIRECT queue is materially damaging KCD2 source cadence. Keep the existing packet model, inline capture, latest-completed-anchor policy, and minimal latch. Do not restore the parent branch's queue graph wholesale.

## 8. Priority 4: input coverage

Mouse remains the first-class sub-frame latency path. Keep raw mouse totals and the KCD2 camera snapshot as the authoritative rendered-pose baseline.

After edge quality, content motion, and cadence are stable:

- validate sensitivity tracking across FOV changes, menus, mounted states, and sensitivity settings;
- keep cut/reset rejection strict;
- investigate controller support separately;
- do not treat stick values as mouse counts.

Sub-frame controller response likely requires a KCD2-specific input-to-camera understanding. Until then, rendered-camera angular-velocity extrapolation is the safe generic fallback.

## 9. Explicitly deferred ideas

These remain outside the current implementation unless a future measured problem justifies them:

- neural inpainting;
- predictive rendering that changes KCD2's render camera ahead of time;
- final positional depth/MV reprojection;
- large spatial hole filling;
- adaptive late-sample controller before fixed-latch measurements demand it;
- KCD2 input-event/target-pose resolver stacks removed from `async-simple`;
- heavy per-slot logging, GPU readback, or allocation in the display hot path;
- parent-branch COPY/COMPUTE/capture-worker architecture.

Depth can help internal disocclusions if positional reprojection returns in the future, but it cannot reconstruct world content that was never rendered outside the source frustum. For the current outer-edge problem, safe limiting, compatible history, and tiny fallback are the intended ladder.

## 10. Validation workflow

A code change is not live-validated until the full chain succeeds:

```text
commit -> push async-simple -> Build (No Signing) -> install artifact -> KCD2 test
```

Use `scripts/install_latest.py --both --ref async-simple` for the normal KCD2 + DRG deployment flow. KCD2 is the primary subjective/telemetry validation target. DRG is the generic presenter/swapchain regression target.

Keep the existing once-per-second `Reproj:` health line cheap and stable. Add only aggregate fields needed to answer an active question. Temporary detailed instrumentation must be removed or disabled once that question is answered.

Do not call a quality change successful from logs alone. Edge fill, walking motion, anchor switches, and latency feel require direct KCD2 footage/play testing.

## 11. Completed and rejected work

Keep this section short. It is a decision memory, not a second changelog.

Completed:

- simplified async architecture and virtual swapchain ownership;
- inline game-DIRECT anchor capture and three-slot packet model;
- one NORMAL-priority DIRECT presenter queue;
- display-clocked one-present-per-slot presenter;
- rotation-only final warp and repeated-anchor re-warping;
- KCD2 gameplay camera hook;
- KCD2 Scaleform HUD isolation;
- fixed deferred late latch;
- opt-in stable source-FPS cap for controlled 60 -> 120 tests;
- opt-in one-midpoint FSR content interpolation path;

Rejected or removed unless new evidence changes the decision:

- default-on source pacing;
- repeat-warp shedding/blit repeats;
- high-priority presenter DIRECT queue on Proton;
- CPU-blocking queue-arrival latching;
- previous-anchor image blending;
- old positional depth residual warp;
- capture worker/COPY queue/mid-frame world-fence complexity;
- UI borrowing and hitch-hold machinery;
- old KCD2 input-prediction/target-pose stack;
- heavy per-slot telemetry.
- attempted KCD2 render reserve and presenter guard crop; live testing showed only reduced displayed FOV, not wider captured coverage.

When one of these decisions changes, record the measured reason here and update the relevant active section above.
