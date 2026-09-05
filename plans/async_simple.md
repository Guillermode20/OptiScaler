# async-simple: Low-Latency Async Timewarp Roadmap

> **Branch:** `async-simple`
> **Reference branch:** `async-timewarp`
> **Updated:** 2026-09-05
>
> This roadmap starts from the simplified runtime that is already working. The priority from
> here is motion-to-photon latency and stable display cadence, not adding more machinery.

## 0. Current position

`async-simple` has completed the architectural simplification. The branch no longer needs a
second rewrite.

Landed:

- **P1:** removed reprojection source pacing and the FG half-rate interaction.
- **P2:** reduced capture to inline copies on the game's DIRECT queue and `FrameSlot[3]`.
- **P3:** reduced the presenter to one DIRECT queue, one warp path, and `_scFence`; removed the
  COPY worker, COMPUTE warp queue, sync generated-frame fallback, UI borrowing, repeat-warp
  shedding, and hitch-hold machinery.
- **P4:** removed per-slot/pipeline telemetry and retained a small 1 Hz runtime summary.
- **P5:** validated the identity presenter in KCD2 at roughly 100-115 display FPS with no
  errors/downgrades, then enabled rotation warps.
- **P6:** restored KCD2 HUD isolation on the single-submit capture path and restored adaptive
  dispatch-time input sampling without restoring the old queue graph.

The current runtime is a valid minimal async-timewarp prototype. The next work should improve
latency and prove performance, while preserving this architecture.

## 1. Primary goal

Make camera rotation respond as close to scanout as the hardware and OS allow, while keeping
KCD2's renderer independent of the reprojection presenter.

The desired end state is:

```text
KCD2 source renderer                    async presenter

render source frame
      |
      v
capture newest anchor
      |
      +------------------------------------+
      |                                    |
return immediately                        v
      |                           choose newest complete anchor
render next source frame                  |
                                           v
                                  prepare warp command list
                                           |
                                           v
                                  submit behind latch gate
                                           |
                                  GPU waits on latch fence
                                           |
                              near predicted scanout deadline
                                           |
                                  sample newest input/pose
                                           |
                                  write final warp constants
                                           |
                                  signal latch fence
                                           |
                                           v
                                    rotation warp
                                           |
                                     HUD composite
                                           |
                                           v
                                        Present
```

The source game can still update simulation, animation, movement, combat, and world state at
60 FPS. The rotational view correction should instead track the presenter cadence, for
example 120, 144, 165, or 240 Hz.

"Near-zero latency" means removing most of the source render pipeline from rotational
mouse-look latency. Absolute zero motion-to-photon latency is impossible because input
polling, warp GPU time, scanout, display processing, and pixel response remain.

## 2. Architecture freeze

Do not redesign the runtime unless measurements prove one of these decisions is wrong.

### Keep

```text
Capture:
  game DIRECT queue
  one inline color copy
  optional isolated UI copy in the same submit
  _uiFence as capture completion gate

Packets:
  FrameSlot[3]
  latest completed anchor wins
  unfinished anchor never blocks presenter
  packet pressure never blocks game thread

Presenter:
  _presentQueue, DIRECT, normal priority
  RP_Dx12 rotation warp
  _scFence for retirement/completion
  one real present per display slot

Input:
  KCD2 camera hook
  raw-input pump
  ApplyLateInput
```

### Add only for P7

```text
_lateLatchFence
_lateLatchFenceValue
```

One latch fence is allowed because it directly reduces input age. It must not grow back into
the parent branch's multi-queue dependency graph.

### Do not restore by default

- source pacing or `SourceFramerateLimit` control;
- dedicated COPY capture queue or capture worker;
- mid-frame world fence;
- COMPUTE warp queue;
- UI borrowing or separate UI readiness;
- repeat-warp shedding;
- synchronous generated-frame fallback;
- heavy per-slot telemetry;
- depth, motion-vector, or translation reprojection;
- generic feature expansion before KCD2 rotation-only latency is solved.

A COMPUTE presenter may be tested later only if P8 proves that the second DIRECT queue itself
is materially damaging KCD2 source cadence.

## 3. Hard invariants

These rules are more important than individual features.

1. **The game thread never CPU-waits for reprojection GPU work.** If capture resources or
   packet slots are busy, drop the anchor and keep rendering.
2. **No reprojection source pacer.** The game, an external limiter, or the user owns source
   cadence.
3. **Latest completed anchor wins.** Never stall the presenter waiting for a newer incomplete
   anchor.
4. **Repeated display slots are real warps.** Re-warp the active anchor with a fresh target
   pose/input sample.
5. **HUD isolation stays single-submit.** World and UI copies share the game DIRECT capture
   submission and the same completion gate.
6. **Presenter stop is deadlock-proof.** Any outstanding latch gate must be released before
   joining/draining the presenter.
7. **Rotation warp remains the baseline.** No depth/MV complexity until latency and cadence
   are proven.
8. **Failures degrade to plain passthrough.** Do not resurrect synchronous generated-frame
   fallback machinery.

## 4. Latency model

The current P6 path samples input before the warp submission is allowed to execute:

```text
sample input -> build constants -> submit warp -> GPU executes -> Present
```

Its minimum achievable input age therefore includes the whole dispatch/queue/warp critical
path.

The target P7 path is:

```text
prepare + submit early
        |
GPU waits at latch fence
        |
sample input as late as safely possible
        |
rewrite constants
        |
release fence
        |
warp -> Present
```

This separates **submission lead** from **input-sample lead**. CPU scheduling and command-list
submission can happen early without making the pose old.

The latency controller should ultimately optimize:

```text
latch lead ~= measured warp + copy critical path + small safety margin
```

not:

```text
dispatch lead ~= whole CPU submission + warp critical path + safety margin
```

Software timing can measure input-sample age, queue completion, and present timing. It cannot
prove photon latency. Final motion-to-photon claims require an external high-speed camera,
LDAT-style setup, or equivalent hardware measurement.

## 5. Next implementation phases

### P7 - Minimal deferred late latch

**Priority: next.**

Reintroduce only the deferred constant latch from `async-timewarp`, adapted to the simplified
DIRECT presenter.

#### P7.1 Fence and lifecycle

Add one CPU-signallable D3D12 fence owned by AReproj:

```text
_lateLatchFence
_lateLatchFenceValue
```

Requirements:

- create/release it with the async presenter objects;
- never attach it to the game queue;
- before shutdown, failure downgrade, device reset, or drain, signal any outstanding latch
  value so `_presentQueue` can never remain parked forever;
- no new worker thread and no new command queue.

#### P7.2 Submit early, release late

For a warp slot:

1. Select the newest completed anchor exactly as today.
2. Record the normal warp + output copy on the SC command list.
3. Use a per-output constant allocation that can be updated safely while the GPU is parked.
4. Queue `_presentQueue->Wait(_lateLatchFence, latchValue)` before the warp command list.
5. Submit the warp command list early enough that presenter-thread scheduling jitter does not
   threaten the slot.
6. Wait on the CPU until the late-latch deadline.
7. Read the newest raw mouse totals and latest valid KCD2 camera state.
8. Build the final rotation constants and call `RP_Dx12::WriteConstants(...)`.
9. Publish the CPU writes, then signal `_lateLatchFence` from the CPU.
10. Let the already-submitted GPU work run immediately.
11. Complete/present using the existing `_scFence` path.

The game DIRECT queue must never wait on `_lateLatchFence`.

#### P7.3 Fixed latch lead first

Do not start with another adaptive controller.

First validate fixed latch leads, for example:

```text
3.0 ms
2.0 ms
1.5 ms
1.0 ms
```

Find the smallest lead that does not create missed display slots in the repeatable KCD2 test
scene.

Only after that works should adaptive tuning return.

#### P7.4 Adaptive latch lead

Replace the current adaptive **dispatch** lead with adaptive **latch** lead.

Use actual warp completion headroom:

```text
too much headroom -> latch later next slot
too little headroom -> latch earlier next slot
missed slot         -> immediately increase safety margin
```

Keep the controller slow and bounded. A sensible initial policy is the existing 0.25 ms step,
with conservative bounds determined from P7.3 rather than assumed in advance.

Submission itself should stay comfortably early. Only the final pose release hunts toward
scanout.

#### P7 acceptance

P7 is complete when all of the following are true:

- 60 -> 120 rotation feels at least as stable as P6;
- fixed-latch mode survives enable/disable, alt-tab, minimize, loading screens, and device
  teardown without a deadlock;
- late-latched input is measurably newer than P6 dispatch-time input;
- no new game-thread wait appears;
- presenter missed-slot rate does not materially regress;
- adaptive latch lead converges without oscillating by multiple milliseconds;
- HUD remains unwarped when KCD2 isolation is available.

### P8 - Performance and queue-contention proof

**Do this before adding another feature.**

The main unresolved performance question is whether the presenter DIRECT queue and full-screen
warp/copies are disturbing KCD2's render queue under Proton/VKD3D.

Run a controlled matrix in the same 1440p scene:

| Test | Warp | HUD isolation | Late latch |
|---|---|---|---|
| B0 | off, reproj disabled | n/a | n/a |
| B1 | identity presenter | off | off |
| B2 | rotation | off | inline P6 |
| B3 | rotation | on | inline P6 |
| B4 | rotation | on | deferred fixed P7 |
| B5 | rotation | on | deferred adaptive P7 |

Test the primary 60 -> 120 case first. Then repeat at the monitor's native/high target refresh.

Record:

- source FPS and source frame-time median/p95/p99;
- maximum game `Present()` block time;
- display FPS;
- display interval mean/p95;
- missed display slots;
- new/repeat ratio;
- dropped anchors and capture-not-ready count;
- effective latch lead;
- temporary GPU warp duration if needed.

Temporary GPU timestamp queries are acceptable for this phase. Do not restore the old telemetry
system.

#### P8 acceptance

Target:

- no repeatable source-FPS loss versus reproj-off caused by CPU blocking;
- source frame-time median and p95 remain close to baseline;
- steady presenter miss rate below roughly 1% of display slots;
- no periodic 57-59 FPS source cadence pattern caused by reprojection;
- 60 -> 120 produces the expected approximately 60 new + 60 repeated warped outputs;
- higher refresh targets scale by adding repeat warps rather than throttling the source.

If B1 is clean and B2 causes the source regression, the warp/presenter GPU path is the problem.
Do not add scheduling logic to hide it.

### P9 - Minimize the warp critical path

Only optimize what P8 measures.

Investigate, in order:

1. shader execution time in `RPD`;
2. `_warpOutput` UAV -> real backbuffer copy cost;
3. HUD composite cost;
4. presenter DIRECT queue scheduling/serialization under VKD3D;
5. unnecessary barriers or allocator waits.

The goal is to shorten the interval between latch release and scanout. Every millisecond removed
from the warp critical path can move the input sample roughly one millisecond later.

#### COMPUTE experiment gate

If, and only if, P8 proves that `_presentQueue` DIRECT work is being serialized against KCD2's
game DIRECT queue badly enough to damage source cadence, create a small experiment that moves
only the warp execution to COMPUTE.

That experiment must keep:

- the same `FrameSlot[3]`;
- the same game DIRECT capture;
- the same latest-completed-anchor policy;
- the same minimal late-latch concept;
- no COPY worker, world fence, UI borrowing, or source pacing.

Do not merge COMPUTE back merely because the parent branch used it. It has to win measured
source cadence and total latency on Proton.

### P10 - Input coverage and rotational prediction

Mouse is the first-class latency target because raw motion can be sampled independently of the
game frame.

After P7-P9 are stable:

- keep the KCD2 camera snapshot as the authoritative rendered-pose baseline;
- keep raw mouse totals for sub-frame rotation correction;
- validate sensitivity tracking across FOV, menus, mounted states, and different sensitivity
  settings;
- add cut/reset rejection so camera teleports never extrapolate;
- investigate controller support separately.

Controller input should not be faked by treating stick values as mouse counts. Without
intercepting the game's input-to-camera mapping, controller timewarp can still extrapolate the
latest observed camera angular velocity, but truly sub-frame controller response may require a
KCD2-specific input hook.

### P11 - Quality hardening

Only after latency and cadence are solved:

- edge/disocclusion handling for larger rotations;
- fast-flick clamp tuning;
- FOV/aspect transitions;
- menus, cutscenes, photo mode, focus loss, and loading transitions;
- unknown-build KCD2 camera fail-closed behavior;
- long-session packet/fence stability;
- DRG or another title as a regression test for generic OptiScaler integration.

Depth/MV/translation work is a separate future project. It is not a prerequisite for excellent
rotation-only latency.

## 6. Acceptance ladder from this point

The old A0-A3 ladder proved the simplified presenter. Do not repeat the whole branch rebuild.

Use this forward ladder:

| Stage | Purpose | Required result |
|---|---|---|
| **L0** | Current P6 baseline | Stable rotation warp and known source/display metrics |
| **L1** | Deferred latch, fixed conservative lead | Same cadence as L0, no deadlocks |
| **L2** | Reduce fixed lead | Find hardware-safe minimum without slot misses |
| **L3** | Adaptive latch lead | Converges near the L2 boundary without oscillation |
| **L4** | HUD + latch + full KCD2 session | Stable gameplay, menus, alt-tab, loads |
| **L5** | High-refresh targets | 120/144/165/240 as hardware allows, source remains independent |

A stage fails if it improves apparent latency by sacrificing source cadence or introducing
frequent display misses. A lower-latency missed frame is still a bad output.

## 7. Metrics to keep

Keep the 1 Hz log small, but make it answer the actual questions.

Recommended steady-state fields:

```text
source=
display=
new=
repeat=
missed=
interval=mean/p95
poseAge=
queue=
dropAnchor=
capWait=
block=
latchLead=
late=applied/samples
maxDeg=
```

For P7/P8 debug builds only, add temporary:

```text
warpGpuMs=
latchToWarpDoneMs=
latchToPresentMs=
```

Do not reintroduce high-frequency per-slot logging in release builds. Logging itself can disturb
the timing being measured.

## 8. Validation workflow

For every runtime change:

1. Build `async-simple` cleanly before stacking another change.
2. Run parser/unit tests that pin the simplified architecture.
3. Test KCD2 in the same 1440p scene with reprojection off first.
4. Run the matching L-stage and capture at least 30-60 seconds of steady metrics.
5. Test stationary view, slow pan, fast flick, and renderer hitch separately.
6. Test enable/disable, alt-tab/minimize, loading screen, save load, and exit.
7. Compare source frame-time distribution, not only average FPS.
8. Only judge latency/feel after cadence passes.

For final latency validation, use an external camera or latency measurement device. Software
timestamps should guide tuning, not be presented as definitive motion-to-photon numbers.

## 9. Cross-cutting checks

Before CI/review:

```bash
# Source pacing must stay gone.
rg 'paceReprojectionSource|sleepForReprojectionSourceMs' OptiScaler

# Old capture/compute machinery must stay gone unless P9's measured COMPUTE experiment exists.
rg '_captureQueue|_captureFence|_captureInputFence|_worldFence|_captureThread|_computeQueue|_computeFence' \
  OptiScaler/framegen/reproj

# After P7, late latch should be a small presenter-only mechanism.
rg '_lateLatchFence|lateLatch' OptiScaler/framegen/reproj

# Removed controllers/telemetry must not creep back accidentally.
rg 'GetTelemetrySnapshot|RecordPipeline|_pipe[A-Z]|_repeatWarpShed|_heldPacketIndex' \
  OptiScaler/framegen/reproj

git diff --check
```

Expected after P7: `_lateLatchFence` hits are limited to presenter creation/destruction,
dispatch/latch release, and safe shutdown/drain handling.

## 10. Decision rules

When a test fails, use these rules instead of adding machinery reflexively.

- **Source FPS drops in identity mode:** investigate virtualization/capture/game-present path.
- **Identity is clean, rotation hurts source FPS:** investigate GPU warp cost or DIRECT queue
  contention.
- **Source is clean, display misses:** investigate presenter clock, `Present(1)`, latch lead,
  and warp critical path.
- **Display is clean but input feels one source frame behind:** investigate input baseline,
  KCD2 camera snapshot matching, and late-latch release timing.
- **HUD smears:** fix KCD2 isolation validity, do not add UI borrowing.
- **Fast flick under-rotates:** inspect raw-input baseline/sensitivity and max-rotation clamp
  before increasing extrapolation complexity.
- **Proton DIRECT queues prove pathological:** run the isolated P9 COMPUTE experiment.
- **A feature requires the game thread to wait:** reject the design.

## 11. Merge criteria

`async-simple` is ready to become the preferred implementation when:

1. 60 -> 120 is stable for normal KCD2 gameplay.
2. Higher display targets behave predictably on capable hardware.
3. Reprojection does not create a repeatable source-cadence regression.
4. Deferred late latch is stable and measurably reduces input age.
5. HUD isolation works without adding a second readiness pipeline.
6. Enable/disable, alt-tab, loading, and shutdown are deadlock-free.
7. The code still looks like the architecture in section 2.

`async-timewarp` remains a reference for proven ideas, not a branch to merge wholesale. Port
only mechanisms that win a specific measurement on `async-simple`.
