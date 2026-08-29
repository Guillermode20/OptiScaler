# KCD2 real asynchronous timewarp

Date: 2026-08-29
Base: `async-timewarp` at `29714920`

## Verdict on the current branch

The presentation half is already asynchronous timewarp infrastructure. When virtualization succeeds, KCD2 renders
into private game-visible buffers, the presenter thread exclusively owns the real DXGI swapchain, and the newest
completed anchor is warped once per display slot. Scaleform HUD isolation keeps the UI out of the warped world image.

The target-pose half is not yet authoritative late pose. Each packet freezes the newest KCD2 camera pose available at
capture. Later display slots either extrapolate the last two rendered poses or convert Windows raw-mouse counts into
an estimated yaw/pitch delta. That is async presentation with predicted reprojection.

Three concrete gaps matter:

1. `TryInputPredictedRotation` samples input at command-record time, even though `PresenterMain` already knows the
   intended display deadline. `DispatchPacketWarp` receives `scanoutDeadlineMs` but does not use it. The final dispatch
   lead therefore remains outside the target pose.
2. Generic raw-input history advances when KCD2 consumes or dispatches Windows input. It is not proof that the
   presenter sees every device event independently of the renderer. Gamepad look is not covered by this path.
3. Calibration requires both mouse axes to collect enough clean samples before either axis can engage. Mostly
   horizontal play can leave a sound yaw estimate disabled because pitch is uncalibrated.

## Definition of done

For this project, "real KCD2 ATW" means:

- the game thread can continue producing frames independently of the display presenter;
- the presenter samples the newest KCD2-authoritative orientation immediately before each warp;
- the warp maps the anchor's rendered pose to a target pose for that display slot;
- only the residual interval after the newest authoritative pose is predicted to the estimated scanout time;
- scene cuts, camera changes, menus, stale data, and unknown game builds fail to a hold or the existing velocity path;
- telemetry states which pose source drove every slot and measures the prediction against the next authoritative pose.

The warp cannot update simulation, animation, translation, recoil geometry, or newly revealed pixels. Guard-band
rendering remains a separate quality milestone for disoccluded screen edges.

## KCD2-specific pose stack

Use the following priority immediately before warp dispatch:

1. **Late CView basis.** Read the newest validated gameplay `CView` pose published by the frustum hook after the
   packet's source pose. This includes the camera effects already applied by KCD2. Reject stale samples, FOV changes,
   implausible angular jumps, and samples from before the anchor.
2. **Player look-controller pose.** Resolve KCD2's validated local-player chain and read its scalar yaw/pitch plus
   derived aim quaternion. This is the clean aim orientation used by the cameras and covers mouse and controller
   input. It intentionally lacks head bob, weapon sway, and view shake, so it is a residual/aim source rather than a
   replacement for the rendered CView basis.
3. **KCD2 input-event residual.** The engine's generic input dispatcher exposes post-input-map mouse look deltas and
   right-stick deflection. Integrate only from the newest authoritative pose to the display deadline. Calibrate mouse
   event units per axis and gamepad radians-per-second independently against later CView samples.
4. **Generic fallback.** Retain the current Windows raw-mouse predictor, then rendered-pose velocity, then hold.

Never add two estimates for the same interval. The final target is:

```
target pose = newest authoritative pose + residual(newest pose timestamp -> scanout deadline)
```

The packet's rendered pose always remains the reconstruction source.

## Implementation sequence

### M0: passive KCD2 look-input acquisition

Implemented on `kcd2-real-atw-agent` as `Kcd2Input.{h,cpp}`. It signature-finds KCD2's generic input dispatcher,
passively records mouse yaw/pitch event totals and gamepad right-stick deflection with QPC-domain timestamps, and
always forwards the untouched event. Unknown builds fail closed. Layout, ids, and signature strategy derive from
the MIT-licensed `tkhquang/KCD2Tools` TPVCamera project.

No warp behavior changes in M0. With reprojection telemetry enabled, the log emits:

```
KCD2 input: passive look-event acquisition installed at ...
KCD2 late input: mouseEvents ... mouseAge ... padEvents ... padAge ...
```

### M1: authoritative late-CView selection

- Expose a read-only current-pose snapshot from `Kcd2Camera` to the presenter.
- Store the packet source basis and timestamp unchanged.
- At each slot, accept a newer CView pose only when it passes camera identity, orthonormality, age, FOV, and cut gates.
- Add `posePath = kcd2-late-cview` plus source age, late-pose age, and angular delta to telemetry.
- Initially use the full late basis in rotation-only mode. Do not reduce it to yaw/pitch, because that discards roll.

### M2: look-controller and KCD2 input residual

- Resolve the local-player/look-controller chain with RTTI gates and patch-safe signatures.
- Timestamp coherent yaw, pitch, and quaternion snapshots.
- Add a fixed, allocation-free history to `Kcd2Input` so pose calibration uses matching time windows.
- Calibrate yaw, pitch, and gamepad rates independently. One missing axis must not disable another valid axis.
- Predict only the bounded residual to `targetDisplayMs`. Reject stale input, cursor UI, cutscenes, and inconsistent
  controller/CView motion.

### M3: prediction scoring before activation

- Save each proposed target with its target timestamp without applying it.
- When a later authoritative CView sample arrives, compare predicted and actual angular deltas.
- Publish p50/p95 error, bias, clamp rate, source coverage, and lead time.
- Enable the new target path only after KCD2 footage and logs show lower error than rendered-pose extrapolation.

### M4: scanout scheduling and quality

- Sample the late pose after the software wait and as close as possible to command recording.
- Predict to the scheduled display deadline, not `MillisecondsNow()`.
- Measure pose-sample-to-submit, submit-to-present, and estimated present-to-scanout separately.
- Add a KCD2 guard band only after target-pose correctness and cadence are stable.

## Monday validation matrix

Use a fresh unsigned build. Keep the new adapter observational for the first run.

| Test | Expected evidence |
| --- | --- |
| Normal mouse look | Mouse event count rises continuously; age remains low while moving |
| Very slow horizontal turn | Yaw events remain visible without requiring pitch motion |
| Vertical-only look | Pitch events rise independently |
| Controller right stick | Pad event count rises; deflection returns near zero on release |
| Inventory/map/dialogue | Input may arrive, but later target gates must refuse camera motion while UI owns look |
| Horseback/combat/head bob | CView and clean aim are compared, not assumed identical |
| Alt-tab and reload | No input mutation, crash, stuck stick, or failed shutdown |

After the observational run, analyze the normal reprojection telemetry and preserve the `KCD2 late input` lines. Do
not call M1 or M2 validated until KCD2 is visibly tested. DRG remains the swapchain/presenter regression target.

## Safety invariants

- The input hook never edits, blocks, or synthesizes an event.
- The presenter remains the only owner of real swapchain presents while virtualization is active.
- A target pose never replaces the packet's source/reconstruction pose.
- Camera cuts and FOV changes clear pending predictions.
- No heap allocation, file logging, GPU readback, or unbounded wait is allowed in a display slot.
- Signatures and pointer chains are build-gated and RTTI-validated. Unknown KCD2 builds fail closed.

## Source attribution

- `tkhquang/KCD2Tools`, TPVCamera, MIT license: KCD2 input dispatcher signatures, event layout/ids, gameplay camera
  layout, and look-controller offsets.
- OptiScaler's existing `Kcd2Camera` and Scaleform adapters remain the authoritative source for the branch's current
  KCD2 integration behavior.
