# Visible timewarp artifacts — candidates ranked by risk

> Branch: `async-simple`. Rotation-only warp in `RPD.hlsl` (single copy), HUD
> isolation composited unwarped, warps on every display slot. These are the
> artifact classes the current pipeline can still show, and concrete,
> independently testable mitigations. Validate each against the 60→120 cap A/B
> before merging — shader feel changes cannot be judged from logs.
>
> **Implementation status (2026-09-05):** artifact 1 (edge lag band) now has
> the two opt-in candidates from the table below as code, default off:
> `[AsyncTimewarp] EdgeExtensionPx > 0` implements a motion-coherent edge
> extension (the listed alternative to 1a/1b — clamp the warp UV onto the
> source edge and stretch boundary color outward with a small inward blur,
> fading back to the static anchor near the limit), and `[AsyncTimewarp]
> GuardCropPercent` adds a fixed guard-crop reserve window. Artifact 4
> (anchor-refresh discontinuity) is addressed by `[AsyncTimewarp]
> ContinuityLatch` (see `plans/walking_judder_options.md`). None is enabled by
> default and none has been A/B-validated yet.

## Artifact inventory on this branch

1. **Edge smear / lag band during rotation.** `RPD.hlsl` already feathers:
   `coverage = saturate(min(edgePixels.x, edgePixels.y) * 0.5f)` blends the
   warped sample to the *unwarped anchor pixel* over a ~2 px band, so beyond
   the anchor's footprint you see the anchor's own (non-rotated) image instead
   of clamped garbage. On fast pans this reads as a narrow, lagging border
   band. Repeats (60→120) expose more of it because `timeStep` ≈ 1.5–2.5.
2. **Aliasing/shimmer inside the covered area.** The warp samples `LastColor`
   with plain bilinear. When rotation zooms out (sample spacing > 1 texel) it
   minifies without prefiltering, so fine detail shimmers on slow pans.
3. **HUD swimming when isolation is unavailable.** With `ReprojHudIsolation`
   off (or a non-KCD2 title), the HUD is part of the warped color: text and
   the crosshair displace with rotation.
4. **Anchor-refresh discontinuity.** Visible as a small "jump back" while
   rotating after the presenter switches anchors whose warped baseline does not
   continue the previous extrapolation (rotation domain; related to the walking
   judder Option B in `plans/walking_judder_options.md`).
5. **Camera-cut smear.** A cut/reset (`sourceCutGeneration`) with stale pose
   can warp two different scenes across the switch for one slot.

## Candidate mitigations (each is an independent A/B)

| # | Change | Artifact | Risk / note |
|---|---|---|---|
| 1a | Wider + smoothstep edge falloff (e.g. 2 px → 8 px cosine ramp) | 1 | Low. Bigger lag band; verify it does not read as softness at 60→120 |
| 1b | Fold the feather target from the *unwarped anchor* toward a faint motion-blurred edge color | 1 | Medium, perceptual. Requires footage to tune |
| 2a | Switch bilinear to a small bicubic/Kaiser tap set (via a second sampler + weights) | 2 | Medium; adds ALU/bandwidth per warp — measure warp GPU cost (P9 discipline) before committing |
| 3a | Keep HUD isolation on by default; surface the checkbox (done in the menu change) and warn when a composed frame is being warped | 3 | Low. Already partially in place |
| 4a | Per-switch pose smoothing gated on cut generation (see judder Option B) | 4 | Presenter-only, low GPU cost, needs feel A/B |
| 5a | Skip the warp for one slot after a cut/reset (present the new anchor unwarped) instead of extrapolating across it | 5 | Low. Presenter-only one-liner; counts one blit-free unwarped output per cut |

## What not to do (this branch)

- **Do not re-add a swap-smooth prev-color blend.** The removed v40 ghost/double
  artifact came from blending the previous anchor's *image* under the current
  homography; the fix (ARCHITECTURE/AGENTS notes) requires a second per-anchor
  UV bake to do it correctly. Not a cheap win.
- **Do not add blur to the whole warp** to mask artifacts — it reads as focus
  loss during motion and hides the anchor-staleness problem this branch is
  still quantifying.
- **Do not tune feather/rotation ceilings without the 60→120 cap A/B running**;
  every one of these interacts with `timeStep`.

## Suggested order

1. Land the source-cap A/B and HUD-isolation checkbox (already in this change).
2. A/B `1a` (edge falloff) — cheapest visible improvement on fast pans and
   repeats.
3. A/B `5a` (cut smear) — one-slot presenter guard, trivially revertible.
4. Measure warp GPU cost before attempting `2a` (per `plans/async_simple.md`
   P9: shader cost is the first thing to optimize, and sampling changes alter
   it).
