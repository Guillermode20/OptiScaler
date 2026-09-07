# async-simple ongoing plan

Updated: 2026-09-06
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

Status (2026-09-07): code done, live validation pending. The same fixed
perimeter test drives an eight-iteration presenter-side binary search over the
relative camera rotation's axis-angle scale. The displayed warp uses the
largest filter-safe scale; near-180-degree discontinuities fail closed to the
source pose. The telemetry retains the *requested* coverage demand so the
limiter cannot hide the underlying deficit.

### E4. Remove the ineffective KCD2 reserve

Completed (2026-09-07). Live testing showed that the temporary `CCamera` FOV mutation did not widen the captured world image. Its presenter-side guard mapping only cropped the image and reduced displayed FOV. The config key, menu control, camera mutation, packet metadata, FSR FOV override, warp crop, and `guard=%` telemetry were removed. Do not restore this mechanism without direct captured-image proof that an earlier projection hook produces genuine additional world coverage.

### E5. Temporal border recovery from older anchors

Only after safe limiting is solid, retain up to two older compatible world anchors as a fallback for pixels that are invalid in the newest anchor.

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
