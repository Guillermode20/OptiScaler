# Walking judder on rotation-only timewarp — options

> Branch: `async-simple`. This doc proposes options only — no code is committed
> from it yet. Decide, then file the chosen option as its own plan.

## Why walking judders today

The async-simple warp is a pure rotation homography (`RPD.hlsl`): it reprojects
each anchor as if the whole scene were at infinity. Two independent effects
show up as "judder while walking":

1. **Translation parallax on near geometry (inherent).** Walking moves the eye
   forward/sideways ~2–4 cm per 60 Hz source frame. Near objects (grass, hands,
   weapon, doorframes) parallax-correct at `t/Z`; a rotation-only warp cannot
   represent that, so near geometry appears to swim or double between anchors.
   This is a *correctness* limit of the warp, not a timing bug.
2. **Stale anchors + snaps on anchor refresh (timing).** In GPU-bound scenes
   the game DIRECT queue runs deep, so an anchor's inline capture completes 1–3
   frames after its pose was sampled. The presenter re-warps an old anchor
   (high `poseAge` / `capWait`), and when a fresh anchor finally lands, its
   warped pose can differ from where the extrapolation had carried the old
   anchor — that discontinuity reads as a periodic snap while moving.

Effect 2 is amplified when the source cadence is *irregular*: extrapolation
step oscillates between frames. A stable 60 Hz source turns the cadence term
off entirely and leaves only effect 1, which is why the 60→120 A/B (the new
`[AsyncTimewarp] SourceFramerateLimit` control) is the correct first
measurement, not a fix by itself.

## Option A — Measure first with a stable 60 Hz source (recommended starting point)

No new machinery. Run the new opt-in `SourceFramerateLimit=60` on a 120 Hz
display in the same Rattay/forest scene used for the P8 matrix, and capture 60 s
of the 1 Hz log.

- If `poseAge` and `capWait` collapse and the *residual* judder is smooth
  (near-geometry parallax only, no snaps), the fix for most users is cadence —
  and Option B's anchor-switch smoothing becomes a small polish item.
- If `poseAge` stays high with a clean 60 Hz source, the capture is still
  queue-bound behind the game's own DIRECT work and Option D is the real lever.

Do not tune anything else until this baseline exists; without it, every other
option below is being tuned blind.

## Option B — Anchor-switch pose smoothing (small, rotation-domain)

When the presenter switches to a fresh anchor it currently jumps its warp
baseline to the new anchor's pose immediately. A bounded per-switch latch/EMA
on the *displayed* rotational offset suppresses the perception of the snap.

- Keep the invariant that repeated slots re-warp the active anchor; only change
  how the baseline transitions at `newAnchor`.
- Must be disabled across cut/reset (`packet.sourceCutGeneration`) so real
  camera cuts (dialogue, death, fast travel) do not smear.
- Risk: it delays genuine mouse-look by a few ms at the switch; must be A/B'd
  against the P6 baseline with the 60→120 cap for feel, not just FPS.
- Cost: presenter-only, ~a day, no shader/queue changes. This does **not**
  address near-geometry parallax (effect 1).

## Option C — Depth-corrected (parallax) warp revival (large, previously failed)

Per-pixel depth reprojection would fix effect 1 directly. It was prototyped on
`async-timewarp`, fully removed at pre26 after regressing feel (disocclusion
tearing at silhouette edges, halos around first-person geometry, erratic motion
during hills/stairs). AGENTS.md gates any revival on a **footage-tuning
channel** — a repeatable recorded scene + before/after capture to tune against
— which does not exist today.

If the 60→120 measurement shows the remaining judder is dominated by effect 1
and is unacceptable, the revival path is:

1. Re-add depth capture to the single inline game-DIRECT submit (one extra
   `CopyResource`, same fence — capture stays one submit).
2. Unproject per-pixel in `RPD.hlsl`, displace by camera translation scaled by
   depth, and **clip disoccluded regions to the unwarped anchor** (the edge
   feather logic generalizes to interior seams).
3. Tune strictly against recorded footage; ship only if it beats the rotation
   baseline for near-geometry motion without reintroducing tearing/halos.

Cost: multi-week, new shader + capture + footage workflow, and it competes
directly with the branch's architecture freeze ("rotation warp remains the
baseline until latency and cadence are proven"). Do not start before P8
numbers exist.

## Option D — Anchor freshness (capture latency)

If Option A shows `poseAge`/`capWait` still high with a 60 Hz source, the
inline capture on the game DIRECT queue is completing late. Levers, in order:

1. **Confirm with the cap first.** A 60 Hz source leaves GPU queue headroom,
   which often collapses the capture lag by itself (the pre-cap KCD2 runs saw
   `poseAge` 25–46 ms from an uncapped ~100 FPS source saturating the queue).
2. Mid-frame world-fence capture (parent branch's mechanism) — explicitly
   removed in async-simple P2; reviving it re-introduces a second capture
   readiness gate. Only justify if (1) proves staleness is the residual judder
   driver.
3. Preemptible/high-priority copy on a separate COPY queue — also removed in
   P2. Same gate as (2): require measurements first.

## Recommendation ladder

1. **Run Option A now** with `SourceFramerateLimit=60` (cap is in this change;
   menu → Output (Async Timewarp) → Source FPS cap). Collect the log matrix.
2. If residual judder is anchor-switch snaps: **Option B** (small presenter
   change, easy A/B).
3. If residual judder is smooth near-geometry parallax and unacceptable:
   **Option C** — but only after standing up the footage-tuning channel the
   removal notes require, and after P8 latency/cadence proof.
4. Option D only if A shows queue-bound capture even at a clean 60 Hz source.
