#include "pch.h"
#include "ReprojInputPredictor.h"
#include "Kcd2Input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace
{
using namespace ReprojInputPredictor;

constexpr size_t GAIN_SAMPLE_COUNT = 128;  // per-axis robust gain window (~2 s at 60 FPS; wider than the old
                                           // 32-sample ring so per-frame gain scatter from aim smoothing no
                                           // longer collapses confidence and drops prediction mid-play)
constexpr size_t MOTION_SAMPLE_COUNT = 16; // recent pose samples
constexpr size_t MIN_GAIN_SAMPLES = 6;     // before any prediction is allowed
constexpr float MIN_GAIN = 1.0e-6f;        // radians per count plausibility window
constexpr float MAX_GAIN = 0.05f;
constexpr double MIN_INTERVAL_MS = 2.0;
constexpr double MAX_INTERVAL_MS = 200.0;
constexpr double FRESH_MS = 2000.0; // estimator expires without new samples
constexpr float MIN_CAMERA_MOTION_RAD = 1.0e-3f;
constexpr float MIN_INPUT_COUNTS = 4.0f;   // quantization floor for a usable sample
constexpr float MOTION_MEMORY_MS = 250.0f; // camera-motion gate decay time
constexpr std::size_t KCD2_PHASE_CANDIDATE_COUNT = 11;
constexpr float KCD2_PHASE_STEP_MS = 4.0f;
constexpr std::size_t KCD2_MIN_PHASE_SAMPLES = 24;
constexpr std::uint32_t KCD2_PHASE_LOCK_STREAK = 12;

struct PoseSample
{
    double timestampMs = 0.0;
    double intervalMs = 0.0;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    float inputX = 0.0f;
    float inputY = 0.0f;
};

struct GainRing
{
    std::array<float, GAIN_SAMPLE_COUNT> samples {};
    size_t count = 0;
    size_t cursor = 0;
    float median = 0.0f;
    float medianAbsoluteDeviation = 0.0f;
    double lastUpdateMs = 0.0; // process clock ms of the newest sample

    void Push(float value, double nowMs)
    {
        samples[cursor] = value;
        cursor = (cursor + 1) % GAIN_SAMPLE_COUNT;
        if (count < GAIN_SAMPLE_COUNT)
            ++count;
        lastUpdateMs = nowMs;
        Recompute();
    }

    // Median + MAD over the retained samples (fixed stack copy, no allocation).
    void Recompute()
    {
        if (count == 0)
        {
            median = 0.0f;
            medianAbsoluteDeviation = 0.0f;
            return;
        }

        std::array<float, GAIN_SAMPLE_COUNT> sorted {};
        std::copy_n(samples.begin(), count, sorted.begin());
        const size_t middle = count / 2;
        std::nth_element(sorted.begin(), sorted.begin() + middle, sorted.begin() + count);
        median = sorted[middle];

        for (size_t i = 0; i < count; ++i)
            sorted[i] = std::abs(samples[i] - median);
        std::nth_element(sorted.begin(), sorted.begin() + middle, sorted.begin() + count);
        medianAbsoluteDeviation = sorted[middle];
    }
};

struct Kcd2PhaseCandidate
{
    struct AxisFit
    {
        double input2 = 0.0;
        double inputCamera = 0.0;
        double camera2 = 0.0;
        std::size_t count = 0;

        void Add(float cameraDelta, float inputDelta)
        {
            if (std::abs(cameraDelta) < 1.0e-4f)
                return;
            input2 += static_cast<double>(inputDelta) * inputDelta;
            inputCamera += static_cast<double>(inputDelta) * cameraDelta;
            camera2 += static_cast<double>(cameraDelta) * cameraDelta;
            ++count;
        }

        float Error() const
        {
            if (count < KCD2_MIN_PHASE_SAMPLES || input2 <= 1.0e-8 || camera2 <= 1.0e-8)
                return INFINITY;
            const double gain = inputCamera / input2;
            if (!(gain > MIN_GAIN && gain < MAX_GAIN))
                return INFINITY;
            const double residual = std::max(0.0, camera2 - 2.0 * gain * inputCamera + gain * gain * input2);
            return static_cast<float>(std::sqrt(residual / camera2));
        }
    };

    GainRing gainX;
    GainRing gainY;
    AxisFit fitX;
    AxisFit fitY;
};

struct PredictorState
{
    std::mutex mutex;
    std::array<PoseSample, MOTION_SAMPLE_COUNT> motion {};
    size_t motionCount = 0;
    size_t motionCursor = 0;
    double lastPoseTimestampMs = 0.0;
    float recentCameraMotion = 0.0f; // decayed max camera motion (radians)
    GainRing gainX;
    GainRing gainY;
    float errorEmaYawDeg = 0.0f;
    float errorEmaPitchDeg = 0.0f;
    uint32_t errorSamples = 0;
    float confidence = 0.0f;
    std::array<Kcd2PhaseCandidate, KCD2_PHASE_CANDIDATE_COUNT> kcd2Phase {};
    std::size_t kcd2BestCandidate = KCD2_PHASE_CANDIDATE_COUNT;
    std::uint32_t kcd2BestStreak = 0;
    bool kcd2PhaseLocked = false;
    Kcd2PhaseModel kcd2Model {};
};

PredictorState& GetPredictorState()
{
    static PredictorState state;
    return state;
}

double ClockMs()
{
    // Low-resolution process clock is sufficient: the rings only need a
    // monotone-ish staleness signal, not telemetry-grade timing.
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

float ComputeConfidence(const GainRing& ring, double nowMs)
{
    if (ring.count < MIN_GAIN_SAMPLES || nowMs - static_cast<double>(ring.lastUpdateMs) > FRESH_MS)
        return 0.0f;

    const float absMedian = std::abs(ring.median);
    if (absMedian < MIN_GAIN || absMedian > MAX_GAIN)
        return 0.0f;

    // Dispersion: tight MAD relative to the median means a linear response
    // (constant sensitivity). High dispersion means aim acceleration, smoothing
    // or a polluted sample stream - fall back to velocity extrapolation.
    // Real games show moderate dispersion from aim smoothing; 0.6 keeps those
    // calibrating while still rejecting nonlinear/polluted streams.
    const float madRatio = ring.medianAbsoluteDeviation / std::max(absMedian, 1.0e-9f);
    float dispersion = 1.0f - madRatio / 0.6f;
    dispersion = std::clamp(dispersion, 0.0f, 1.0f);
    const float recency = 1.0f - std::min(1.0f, static_cast<float>((nowMs - ring.lastUpdateMs) / FRESH_MS)) * 0.5f;
    return dispersion * recency;
}

void PushGainSample(GainRing& ring, float cameraDelta, float inputDelta, double nowMs)
{
    if (std::abs(inputDelta) < MIN_INPUT_COUNTS || std::abs(cameraDelta) < 1.0e-4f)
        return;
    // Sign consistency: the camera must have turned the way the mouse moved.
    if (cameraDelta * inputDelta <= 0.0f)
        return;

    const float gain = cameraDelta / inputDelta;
    if (!std::isfinite(gain) || gain <= 0.0f)
        return;
    ring.Push(gain, nowMs);
}

float PhaseCandidateScore(const Kcd2PhaseCandidate& candidate)
{
    float score = 0.0f;
    std::uint32_t axes = 0;
    const auto addAxis = [&](const Kcd2PhaseCandidate::AxisFit& fit)
    {
        const float error = fit.Error();
        if (!std::isfinite(error))
            return;
        score += error;
        ++axes;
    };
    addAxis(candidate.fitX);
    addAxis(candidate.fitY);
    return axes > 0 ? score / static_cast<float>(axes) : INFINITY;
}

void UpdateMotionState(PredictorState& state, double poseTimestampMs, double intervalMs, float deltaYawRadians,
                       float deltaPitchRadians, float inputDeltaX, float inputDeltaY)
{
    const float sampleMotion = std::max(std::abs(deltaYawRadians), std::abs(deltaPitchRadians));
    const float decay = std::exp(-static_cast<float>(intervalMs) / MOTION_MEMORY_MS);
    state.recentCameraMotion = std::max(sampleMotion, state.recentCameraMotion * decay);

    auto& sample = state.motion[state.motionCursor];
    sample.timestampMs = poseTimestampMs;
    sample.intervalMs = intervalMs;
    sample.cameraYaw = deltaYawRadians;
    sample.cameraPitch = deltaPitchRadians;
    sample.inputX = inputDeltaX;
    sample.inputY = inputDeltaY;
    state.motionCursor = (state.motionCursor + 1) % MOTION_SAMPLE_COUNT;
    if (state.motionCount < MOTION_SAMPLE_COUNT)
        ++state.motionCount;
    state.lastPoseTimestampMs = poseTimestampMs;
}
} // namespace

namespace ReprojInputPredictor
{
void Reset()
{
    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    state.motionCount = 0;
    state.motionCursor = 0;
    state.lastPoseTimestampMs = 0.0;
    state.recentCameraMotion = 0.0f;
    state.gainX = {};
    state.gainY = {};
    state.errorEmaYawDeg = 0.0f;
    state.errorEmaPitchDeg = 0.0f;
    state.errorSamples = 0;
    state.confidence = 0.0f;
    state.kcd2Phase = {};
    state.kcd2BestCandidate = KCD2_PHASE_CANDIDATE_COUNT;
    state.kcd2BestStreak = 0;
    state.kcd2PhaseLocked = false;
    state.kcd2Model = {};
}

void OnPoseSample(double poseTimestampMs, double poseIntervalMs, float deltaYawRadians, float deltaPitchRadians,
                  float inputDeltaX, float inputDeltaY)
{
    if (poseTimestampMs <= 0.0 || !std::isfinite(deltaYawRadians) || !std::isfinite(deltaPitchRadians))
        return;

    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);

    // Duplicate or out-of-order feed: ignore, the caller guarantees newest-first.
    if (poseTimestampMs <= state.lastPoseTimestampMs)
        return;

    const double intervalMs =
        std::clamp(poseIntervalMs > 0.0 ? poseIntervalMs : 16.6, MIN_INTERVAL_MS, MAX_INTERVAL_MS);
    const double nowMs = ClockMs();

    // Closed-loop error statistic: what the CURRENT gain would have predicted
    // for the interval that just completed, versus the actual rotation.
    if (state.gainX.count >= MIN_GAIN_SAMPLES && state.gainY.count >= MIN_GAIN_SAMPLES)
    {
        const float predictedYaw = state.gainX.median * inputDeltaX;
        const float predictedPitch = -state.gainY.median * inputDeltaY;
        const float errorYawDeg = std::abs(deltaYawRadians - predictedYaw) * 57.2957795f;
        const float errorPitchDeg = std::abs(deltaPitchRadians - predictedPitch) * 57.2957795f;
        constexpr float errorBlend = 0.15f;
        state.errorEmaYawDeg = state.errorEmaYawDeg * (1.0f - errorBlend) + errorYawDeg * errorBlend;
        state.errorEmaPitchDeg = state.errorEmaPitchDeg * (1.0f - errorBlend) + errorPitchDeg * errorBlend;
        ++state.errorSamples;
    }

    PushGainSample(state.gainX, deltaYawRadians, inputDeltaX, nowMs);
    // Convention: raw mouse +Y is downward, camera pitch is positive upward,
    // so the usable gain magnitude flips the sign of the camera delta.
    PushGainSample(state.gainY, -deltaPitchRadians, inputDeltaY, nowMs);

    UpdateMotionState(state, poseTimestampMs, intervalMs, deltaYawRadians, deltaPitchRadians, inputDeltaX, inputDeltaY);

    // Both axes must be trustworthy before the warp may use them.
    state.confidence = std::min(ComputeConfidence(state.gainX, nowMs), ComputeConfidence(state.gainY, nowMs));
}

void OnKcd2PoseSample(double poseTimestampMs, double poseIntervalMs, float deltaYawRadians, float deltaPitchRadians)
{
    if (poseTimestampMs <= 0.0 || !std::isfinite(deltaYawRadians) || !std::isfinite(deltaPitchRadians))
        return;

    const double intervalMs =
        std::clamp(poseIntervalMs > 0.0 ? poseIntervalMs : 16.6, MIN_INTERVAL_MS, MAX_INTERVAL_MS);
    std::array<Kcd2Input::MouseInterval, KCD2_PHASE_CANDIDATE_COUNT> intervals {};
    std::array<bool, KCD2_PHASE_CANDIDATE_COUNT> valid {};
    for (std::size_t candidate = 0; candidate < KCD2_PHASE_CANDIDATE_COUNT; ++candidate)
    {
        const double offsetMs = static_cast<double>(candidate) * KCD2_PHASE_STEP_MS;
        valid[candidate] = Kcd2Input::QueryMouseInterval(poseTimestampMs - intervalMs - offsetMs,
                                                         poseTimestampMs - offsetMs, intervals[candidate]) &&
                           intervals[candidate].complete;
    }

    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    if (poseTimestampMs <= state.lastPoseTimestampMs)
        return;

    const double nowMs = ClockMs();
    for (std::size_t candidate = 0; candidate < KCD2_PHASE_CANDIDATE_COUNT; ++candidate)
    {
        if (!valid[candidate])
            continue;
        state.kcd2Phase[candidate].fitX.Add(deltaYawRadians, static_cast<float>(intervals[candidate].yaw));
        state.kcd2Phase[candidate].fitY.Add(-deltaPitchRadians, static_cast<float>(intervals[candidate].pitch));
        PushGainSample(state.kcd2Phase[candidate].gainX, deltaYawRadians, static_cast<float>(intervals[candidate].yaw),
                       nowMs);
        PushGainSample(state.kcd2Phase[candidate].gainY, -deltaPitchRadians,
                       static_cast<float>(intervals[candidate].pitch), nowMs);
    }

    if (!state.kcd2PhaseLocked)
    {
        std::size_t best = KCD2_PHASE_CANDIDATE_COUNT;
        float bestScore = INFINITY;
        for (std::size_t candidate = 0; candidate < KCD2_PHASE_CANDIDATE_COUNT; ++candidate)
        {
            const float score = PhaseCandidateScore(state.kcd2Phase[candidate]);
            if (score < bestScore)
            {
                bestScore = score;
                best = candidate;
            }
        }

        if (best < KCD2_PHASE_CANDIDATE_COUNT && bestScore < 0.45f)
        {
            if (best == state.kcd2BestCandidate)
                ++state.kcd2BestStreak;
            else
            {
                state.kcd2BestCandidate = best;
                state.kcd2BestStreak = 1;
            }

            if (state.kcd2BestStreak >= KCD2_PHASE_LOCK_STREAK)
            {
                const auto& selected = state.kcd2Phase[best];
                state.kcd2Model.phaseOffsetMs = static_cast<float>(best) * KCD2_PHASE_STEP_MS;
                state.kcd2Model.yawCalibrated =
                    selected.fitX.count >= KCD2_MIN_PHASE_SAMPLES && selected.gainX.count >= MIN_GAIN_SAMPLES;
                state.kcd2Model.pitchCalibrated =
                    selected.fitY.count >= KCD2_MIN_PHASE_SAMPLES && selected.gainY.count >= MIN_GAIN_SAMPLES;
                state.kcd2Model.gainX = selected.gainX.median;
                state.kcd2Model.gainY = selected.gainY.median;
                // Do not mix input prediction on one axis with rendered
                // velocity on the other. Engage only after one shared phase
                // has calibrated both axes, then keep that model immutable.
                state.kcd2PhaseLocked = state.kcd2Model.yawCalibrated && state.kcd2Model.pitchCalibrated;
            }
        }
        else
        {
            state.kcd2BestCandidate = KCD2_PHASE_CANDIDATE_COUNT;
            state.kcd2BestStreak = 0;
        }
    }

    const auto& zeroPhase = intervals[0];
    UpdateMotionState(state, poseTimestampMs, intervalMs, deltaYawRadians, deltaPitchRadians,
                      valid[0] ? static_cast<float>(zeroPhase.yaw) : 0.0f,
                      valid[0] ? static_cast<float>(zeroPhase.pitch) : 0.0f);
}

bool GetKcd2PhaseModel(Kcd2PhaseModel* model)
{
    if (model == nullptr)
        return false;
    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    if (!state.kcd2PhaseLocked)
        return false;
    *model = state.kcd2Model;
    return true;
}

bool GetEstimatedGain(float* gainX, float* gainY)
{
    if (gainX == nullptr || gainY == nullptr)
        return false;

    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    if (state.gainX.count < MIN_GAIN_SAMPLES || state.gainY.count < MIN_GAIN_SAMPLES)
        return false;
    if (state.confidence <= 0.0f)
        return false;

    *gainX = state.gainX.median;
    *gainY = state.gainY.median;
    return true;
}

void GetAxisEstimates(AxisEstimate* yaw, AxisEstimate* pitch)
{
    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    const auto nowMs = ClockMs();
    if (yaw != nullptr)
    {
        yaw->gain = state.gainX.median;
        yaw->confidence = ComputeConfidence(state.gainX, nowMs);
        yaw->errorDegrees = state.errorEmaYawDeg;
        yaw->calibrated = state.gainX.count >= MIN_GAIN_SAMPLES && yaw->confidence > 0.0f;
    }
    if (pitch != nullptr)
    {
        pitch->gain = state.gainY.median;
        pitch->confidence = ComputeConfidence(state.gainY, nowMs);
        pitch->errorDegrees = state.errorEmaPitchDeg;
        pitch->calibrated = state.gainY.count >= MIN_GAIN_SAMPLES && pitch->confidence > 0.0f;
    }
}

float GetConfidence()
{
    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    return state.confidence;
}

bool IsInputDriven(float inputDeltaX, float inputDeltaY)
{
    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);

    // No fresh mouse motion over the prediction window: nothing to predict
    // from. Velocity extrapolation handles coasting motion (and degenerates to
    // a stable hold for a stationary camera).
    if (std::abs(inputDeltaX) < 1.0f && std::abs(inputDeltaY) < 1.0f)
        return false;

    // Mouse moving but the camera has not been responding: menu, cutscene,
    // gamepad steering with idle mouse - do not rotate the warp.
    if (state.recentCameraMotion < MIN_CAMERA_MOTION_RAD)
        return false;

    return true;
}

bool PredictRotation(float gainX, float gainY, float inputDeltaX, float inputDeltaY, float responseScale,
                     float maxRotationRad, RotationEstimate* out)
{
    if (out == nullptr || !std::isfinite(gainX) || !std::isfinite(gainY))
        return false;
    if (gainX < MIN_GAIN || gainX > MAX_GAIN || gainY < MIN_GAIN || gainY > MAX_GAIN)
        return false;

    const double response = std::clamp(responseScale, 0.05f, 1.0f);
    double yaw = static_cast<double>(gainX) * static_cast<double>(inputDeltaX) * response;
    double pitch = static_cast<double>(-gainY) * static_cast<double>(inputDeltaY) * response;
    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return false;

    const double rotation = std::hypot(yaw, pitch);
    if (rotation > static_cast<double>(maxRotationRad))
    {
        const double scale = static_cast<double>(maxRotationRad) / rotation;
        yaw *= scale;
        pitch *= scale;
    }

    out->yawRadians = static_cast<float>(yaw);
    out->pitchRadians = static_cast<float>(pitch);
    return true;
}

bool DescribeStats(char* buffer, size_t size)
{
    if (buffer == nullptr || size == 0)
        return false;

    auto& state = GetPredictorState();
    std::scoped_lock lock(state.mutex);
    bool hasKcd2Samples = state.kcd2PhaseLocked;
    for (const auto& candidate : state.kcd2Phase)
        hasKcd2Samples = hasKcd2Samples || candidate.gainX.count > 0 || candidate.gainY.count > 0;
    if (!hasKcd2Samples && state.gainX.count < MIN_GAIN_SAMPLES && state.gainY.count < MIN_GAIN_SAMPLES)
        return false;

    if (state.kcd2PhaseLocked)
    {
        snprintf(buffer, size, "KCD2 phase %.1fms LOCKED gainX %.5f%s gainY %.5f%s", state.kcd2Model.phaseOffsetMs,
                 state.kcd2Model.gainX, state.kcd2Model.yawCalibrated ? "" : "?", state.kcd2Model.gainY,
                 state.kcd2Model.pitchCalibrated ? "" : "?");
    }
    else if (state.kcd2BestCandidate < KCD2_PHASE_CANDIDATE_COUNT)
    {
        snprintf(buffer, size, "KCD2 phase %.1fms acquiring %u/%u",
                 static_cast<float>(state.kcd2BestCandidate) * KCD2_PHASE_STEP_MS, state.kcd2BestStreak,
                 KCD2_PHASE_LOCK_STREAK);
    }
    else
    {
        snprintf(buffer, size, "gainX %.5f gainY %.5f rad/ct conf %.2f errX %.3f deg errY %.3f deg n %u",
                 state.gainX.median, state.gainY.median, state.confidence, state.errorEmaYawDeg, state.errorEmaPitchDeg,
                 state.errorSamples);
    }
    return true;
}
} // namespace ReprojInputPredictor
