#pragma once

#include <cstdint>
#include <cstddef>

// Input-predicted timewarp estimator.
//
// True timewarp needs the camera pose the game WILL have at scanout, not the
// last rendered velocity. This module calibrates the game's mouse-to-camera
// response (radians per raw input count, per axis) from rendered pose samples
// and predicts the rotation for the pose -> display-deadline window from the
// raw input stream. Prediction is anchored to the rendered pose and only
// applies the delta since it, so mispredictions cost at most one frame of
// motion and self-correct at the next anchor (no accumulated drift, no snap).
//
// The 2026-08-23 attempt failed for two reasons this design avoids:
//  1. raw-input rotation was ADDED on top of velocity extrapolation ->
//     double-counting; here prediction REPLACES the extrapolation term.
//  2. the auto-tracked sensitivity fed atan2(|fwd|^2, |fwd|^2) = pi/4 into the
//     gain estimate, converging to a garbage oversensitive value.
//
// Threading: OnPoseSample/GetEstimatedGain/PredictRotation are internally
// synchronized and safe from any thread, but the warp call sites are
// exclusive per mode (sync = game thread, async = presenter thread).
namespace ReprojInputPredictor
{
struct RotationEstimate
{
    float yawRadians;
    float pitchRadians;
};

struct AxisEstimate
{
    float gain = 0.0f;
    float confidence = 0.0f;
    float errorDegrees = 0.0f;
    bool calibrated = false;
};

// Clear all calibration and statistics state (context reset, mode change).
void Reset();

// Feed one rendered camera sample (newest pose first, one per rendered frame).
// deltaYaw/deltaPitch: rotation from the previous rendered pose to this pose
// (radians, pitch positive looking up). inputDeltaX/Y: raw mouse counts over
// the SAME interval. Repeated timestamps are ignored by the caller.
void OnPoseSample(double poseTimestampMs, double poseIntervalMs, float deltaYawRadians, float deltaPitchRadians,
                  float inputDeltaX, float inputDeltaY);

// Calibrated gain magnitudes (radians per count, always positive; the caller
// applies the +X->+yaw / +Y->-pitch conventions). Returns false until enough
// consistent samples exist.
bool GetEstimatedGain(float* gainX, float* gainY);

// Independent axis state. A weak/unmoving axis must not disable a calibrated
// orthogonal axis in the target-pose resolver.
void GetAxisEstimates(AxisEstimate* yaw, AxisEstimate* pitch);

// Confidence of the current gain estimate, 0..1. Hysteresis thresholds are
// applied by the caller (enter ~0.55, exit ~0.35).
float GetConfidence();

// Is the input stream plausibly driving the camera motion right now?
// Returns false when the mouse is idle over the prediction window while the
// camera keeps turning (gamepad, cutscene, scripted motion) and when the
// camera has not been responding recently (menus, photo mode). In both cases
// the caller falls back to velocity extrapolation, which degenerates to a
// stable hold for a stationary camera.
bool IsInputDriven(float inputDeltaX, float inputDeltaY);

// Gain-scaled rotation for the input accumulated over the window that
// produced inputDeltaX/Y. responseScale (0.05..1) deliberately under-rotates
// to follow games with smoothed aim; maxRotationRad clamps the magnitude.
bool PredictRotation(float gainX, float gainY, float inputDeltaX, float inputDeltaY, float responseScale,
                     float maxRotationRad, RotationEstimate* out);

// Diagnostics: calibrated gains, confidence and mean absolute prediction
// error (degrees) since the last Reset. Returns false while nothing has been
// calibrated yet.
bool DescribeStats(char* buffer, size_t size);
} // namespace ReprojInputPredictor
