# Async Timewarp Architecture Reset

## Why the current experiment failed

The presenter reached a high output rate, but four independent problems were being conflated:

1. **Render/presenter serialization.** Every compute warp inserted a fence wait on KCD2's DIRECT queue. Commands the game submitted afterward could not execute until presenter compute completed, so enabling a nominally asynchronous feature reduced source FPS.
2. **Isolation-resource reuse race.** COPY capture read one world/UI pair per virtual backbuffer, but that pair could be overwritten by the next use of the backbuffer before COPY completion.
3. **Input phase estimation.** The camera pose and its raw-mouse baseline were sampled separately and then joined by timestamp history. Scheduler phase error and a continuously changing sensitivity estimate produced visible corrections during pans.
4. **Missing translational content.** A rotation homography can correct view orientation, but it cannot synthesize parallax from walking, head bob, hills, moving objects, or animation. Higher presenter FPS does not change that boundary.

## Stage 1: make rotation-only ATW genuinely asynchronous

This is the minimum trustworthy baseline:

- KCD2 renders only into wrapper-owned virtual backbuffers.
- KCD2's DIRECT queue signals producer completion; COPY captures immutable HUD-less world/UI generations.
- Each isolation generation remains fenced until COPY completion. If the ring is exhausted, fail closed to the composed game frame.
- The presenter selects only already-completed packets and keeps warping its active packet while a newer capture is unfinished.
- Warp/UI composition runs on the normal-priority COMPUTE queue.
- The presenter thread waits for compute completion before `Present`. It never inserts a presenter fence wait on KCD2's DIRECT queue.
- The KCD2 camera callback snapshots camera basis and raw-mouse totals together. Late input is `current totals - pose totals`; automatic sensitivity is fitted over multiple samples and then locked for the run.
- Missing camera or isolated UI/world inputs always produce an unchanged frame.

Stage 1 is successful only if an uncapped source no longer collapses toward the presenter rate, the 120 Hz presenter sustains at least 117 FPS in a stable scene, and panning no longer contains sensitivity/phase jumps.

## Stage 2: solve walking with content generation, not a larger rotation warp

Walking and hills require new scene content between source frames. The intended end state is:

```text
KCD2 source frame N -----> content interpolation -----> display slot
          camera/input --------------------------------> final late rotation
          isolated UI ---------------------------------> unwarped composite
```

The interpolation stage should use the repository's maintained frame-generation backend and its depth/motion-vector contracts. Do not resurrect the removed one-pass source-depth correction in `RPD`: backward sampling a single source depth cannot robustly resolve disocclusions and was already shown to regress image quality.

The final ATW pass remains small and rotation-only. It corrects only the camera motion after the content frame's represented pose. UI is composited afterward. There must be exactly one paced present per display slot, no unwarped anchor insertion, and generated content must fail closed to the newest real content on cuts or invalid resources.

Stage 2 needs its own proof gates: valid motion/depth inputs, one generated midpoint frame before adaptive ratios, cut/reset behavior, artifact review from recorded hill/walk footage, and separate reporting of source FPS, generated-content cadence, final presentation cadence, and camera latency.

## Non-goals

- Claiming that repeated rotation-only anchors are equivalent to 120 Hz scene motion.
- Hiding queue coupling with more buffering, higher queue priority, or longer waits.
- Reintroducing heuristic depth translation without a reproducible footage-based quality test.
- Warping a composed HUD.
