#include "pch.h"
#include "TargetPoseResolver.h"

#include "Kcd2Camera.h"
#include "Kcd2Input.h"
#include "ReprojInputPredictor.h"

#include <Config.h>
#include <Util.h>
#include <menu/input/input_system.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace
{
using namespace TargetPoseResolver;

constexpr std::size_t ERROR_HISTORY_COUNT = 256;
constexpr std::uint32_t MIN_ACTIVE_SAMPLES = 120;
constexpr std::uint32_t MIN_ERROR_SAMPLES = 30;
constexpr float REQUIRED_COVERAGE = 0.95f;
constexpr float REQUIRED_P95_ERROR_DEGREES = 0.25f;

struct ResolverState
{
    std::mutex mutex;
    std::array<float, ERROR_HISTORY_COUNT> errors {};
    std::size_t errorCount = 0;
    std::size_t errorCursor = 0;
    std::uint32_t activeSamples = 0;
    std::uint32_t predictedSamples = 0;
    Pose pendingTarget {};
    bool pendingValidation = false;
    std::uint64_t lastCameraSequence = 0;
    std::uint64_t cutGeneration = 0;
};

ResolverState& GetResolverState()
{
    static ResolverState state;
    return state;
}

struct Vec3
{
    float x;
    float y;
    float z;
};

Vec3 Load(const float* value) { return { value[0], value[1], value[2] }; }
void Store(float* target, Vec3 value)
{
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
}
Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
Vec3 Scale(Vec3 value, float scale) { return { value.x * scale, value.y * scale, value.z * scale }; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
Vec3 Normalize(Vec3 value)
{
    const auto length2 = Dot(value, value);
    if (!(length2 > 1.0e-12f) || !std::isfinite(length2))
        return {};
    return Scale(value, 1.0f / std::sqrt(length2));
}

void Orthonormalize(Pose& pose)
{
    auto forward = Normalize(Load(pose.forward));
    auto right = Normalize(Add(Load(pose.right), Scale(forward, -Dot(Load(pose.right), forward))));
    auto up = Normalize(Cross(forward, right));
    if (Dot(up, Load(pose.up)) < 0.0f)
        up = Scale(up, -1.0f);
    Store(pose.forward, forward);
    Store(pose.right, right);
    Store(pose.up, up);
}

Vec3 RotateAxis(Vec3 value, Vec3 axis, float angle)
{
    axis = Normalize(axis);
    const auto sine = std::sin(angle);
    const auto cosine = std::cos(angle);
    return Add(Add(Scale(value, cosine), Scale(Cross(axis, value), sine)),
               Scale(axis, Dot(axis, value) * (1.0f - cosine)));
}

void ApplyYawPitch(Pose& pose, float yaw, float pitch)
{
    auto right = Load(pose.right);
    auto up = Load(pose.up);
    auto forward = Load(pose.forward);
    right = RotateAxis(right, up, yaw);
    forward = RotateAxis(forward, up, yaw);
    up = RotateAxis(up, right, pitch);
    forward = RotateAxis(forward, right, pitch);
    Store(pose.right, right);
    Store(pose.up, up);
    Store(pose.forward, forward);
    Orthonormalize(pose);
}

Pose FromSnapshot(const Kcd2Camera::Snapshot& snapshot)
{
    Pose pose {};
    std::memcpy(pose.position, snapshot.position, sizeof(pose.position));
    std::memcpy(pose.right, snapshot.right, sizeof(pose.right));
    std::memcpy(pose.up, snapshot.up, sizeof(pose.up));
    std::memcpy(pose.forward, snapshot.forward, sizeof(pose.forward));
    pose.verticalFov = snapshot.verticalFov;
    pose.timestampMs = snapshot.timestampMs;
    pose.cutGeneration = snapshot.cutGeneration;
    return pose;
}

float AngularErrorDegrees(const Pose& predicted, const Kcd2Camera::Snapshot& actual)
{
    const auto dot = std::clamp(Dot(Normalize(Load(predicted.forward)), Normalize(Load(actual.forward))), -1.0f, 1.0f);
    return std::acos(dot) * 57.2957795f;
}

float ErrorP95(const ResolverState& state)
{
    if (state.errorCount == 0)
        return 180.0f;
    auto copy = state.errors;
    const auto index = static_cast<std::size_t>(std::ceil((state.errorCount - 1) * 0.95));
    std::nth_element(copy.begin(), copy.begin() + index, copy.begin() + state.errorCount);
    return copy[index];
}

ShadowStats StatsLocked(const ResolverState& state)
{
    ShadowStats stats {};
    stats.activeSamples = state.activeSamples;
    stats.predictedSamples = state.predictedSamples;
    stats.errorSamples = static_cast<std::uint32_t>(state.errorCount);
    stats.activeCoverage = state.activeSamples > 0
                               ? static_cast<float>(state.predictedSamples) / static_cast<float>(state.activeSamples)
                               : 0.0f;
    stats.errorP95Degrees = ErrorP95(state);
    stats.qualified = state.activeSamples >= MIN_ACTIVE_SAMPLES && state.errorCount >= MIN_ERROR_SAMPLES &&
                      stats.activeCoverage >= REQUIRED_COVERAGE && stats.errorP95Degrees < REQUIRED_P95_ERROR_DEGREES;
    return stats;
}

void ResetLocked(ResolverState& state, std::uint64_t cutGeneration)
{
    state.errorCount = 0;
    state.errorCursor = 0;
    state.activeSamples = 0;
    state.predictedSamples = 0;
    state.pendingValidation = false;
    state.lastCameraSequence = 0;
    state.cutGeneration = cutGeneration;
}
} // namespace

namespace TargetPoseResolver
{
void Reset()
{
    auto& state = GetResolverState();
    std::scoped_lock lock(state.mutex);
    ResetLocked(state, 0);
}

Result Resolve(const Request& request)
{
    Result result {};
    result.target = request.content;
    result.poseSampleMs = request.content.timestampMs;

    Kcd2Camera::Snapshot currentCamera {};
    Kcd2Camera::Snapshot previousCamera {};
    const bool hasCamera = Kcd2Camera::ReadSnapshots(currentCamera, previousCamera);
    auto& state = GetResolverState();
    std::scoped_lock lock(state.mutex);

    if (hasCamera)
    {
        if (state.cutGeneration != 0 && currentCamera.cutGeneration != state.cutGeneration)
            ResetLocked(state, currentCamera.cutGeneration);
        else if (state.cutGeneration == 0)
            state.cutGeneration = currentCamera.cutGeneration;

        if (state.pendingValidation && currentCamera.sequence != state.lastCameraSequence &&
            currentCamera.timestampMs >= state.pendingTarget.timestampMs &&
            currentCamera.cutGeneration == state.pendingTarget.cutGeneration)
        {
            state.errors[state.errorCursor] = AngularErrorDegrees(state.pendingTarget, currentCamera);
            state.errorCursor = (state.errorCursor + 1) % ERROR_HISTORY_COUNT;
            state.errorCount = std::min(state.errorCount + 1, ERROR_HISTORY_COUNT);
            state.pendingValidation = false;
        }
        state.lastCameraSequence = currentCamera.sequence;

        // A newer authoritative CView pose supersedes every estimate covering
        // the same interval. Never add rendered velocity beneath it.
        if (currentCamera.timestampMs > request.content.timestampMs &&
            currentCamera.timestampMs <= request.targetScanoutMs + 2.0 &&
            (request.content.cutGeneration == 0 || request.content.cutGeneration == currentCamera.cutGeneration))
        {
            result.target = FromSnapshot(currentCamera);
            result.poseSampleMs = currentCamera.timestampMs;
            result.source = PoseSource::Kcd2CView;
        }
    }

    const auto nowMs = Util::MillisecondsNow();
    const auto intervalEndMs = std::min(request.targetScanoutMs, nowMs);
    result.residualIntervalMs = std::max(0.0, intervalEndMs - result.poseSampleMs);

    ReprojInputPredictor::AxisEstimate yawAxis {};
    ReprojInputPredictor::AxisEstimate pitchAxis {};
    ReprojInputPredictor::GetAxisEstimates(&yawAxis, &pitchAxis);
    const auto* config = Config::Instance();
    const float manualYaw = config->ReprojMouseSensitivityX.value_or_default();
    const float manualPitch = config->ReprojMouseSensitivityY.value_or_default();
    if (manualYaw > 0.0f)
    {
        yawAxis.gain = manualYaw;
        yawAxis.confidence = 1.0f;
        yawAxis.calibrated = true;
    }
    if (manualPitch > 0.0f)
    {
        pitchAxis.gain = manualPitch;
        pitchAxis.confidence = 1.0f;
        pitchAxis.calibrated = true;
    }
    result.yawConfidence = yawAxis.confidence;
    result.pitchConfidence = pitchAxis.confidence;
    result.yawErrorDegrees = yawAxis.errorDegrees;
    result.pitchErrorDegrees = pitchAxis.errorDegrees;

    float inputX = 0.0f;
    float inputY = 0.0f;
    bool postMapInput = false;
    Kcd2Input::MouseInterval postMap {};
    if (result.residualIntervalMs > 0.0 && Kcd2Input::QueryMouseInterval(result.poseSampleMs, intervalEndMs, postMap) &&
        postMap.complete && (postMap.yawEvents > 0 || postMap.pitchEvents > 0))
    {
        inputX = static_cast<float>(postMap.yaw);
        inputY = static_cast<float>(postMap.pitch);
        postMapInput = true;
    }
    else if (result.residualIntervalMs > 0.0)
    {
        OptiInput::RefreshMouseMotion();
        const auto atPose = OptiInput::GetRawMouseMotionAt(result.poseSampleMs);
        const auto atTarget = OptiInput::GetRawMouseMotionAt(intervalEndMs);
        inputX = static_cast<float>(atTarget.TotalX - atPose.TotalX);
        inputY = static_cast<float>(atTarget.TotalY - atPose.TotalY);
    }

    const bool activeMouse = postMapInput ? std::abs(inputX) >= 1.0e-5f || std::abs(inputY) >= 1.0e-5f
                                          : std::abs(inputX) >= 1.0f || std::abs(inputY) >= 1.0f;
    const float confidenceThreshold = 0.55f;
    const float response = std::clamp(request.responseScale, 0.05f, 1.0f);
    float yaw =
        yawAxis.calibrated && yawAxis.confidence >= confidenceThreshold ? yawAxis.gain * inputX * response : 0.0f;
    float pitch = pitchAxis.calibrated && pitchAxis.confidence >= confidenceThreshold
                      ? -pitchAxis.gain * inputY * response
                      : 0.0f;
    result.yawPredicted = std::abs(yaw) > 0.0f;
    result.pitchPredicted = std::abs(pitch) > 0.0f;

    const auto magnitude = std::hypot(yaw, pitch);
    if (magnitude > request.maxRotationRadians && magnitude > 0.0f)
    {
        const auto scale = request.maxRotationRadians / magnitude;
        yaw *= scale;
        pitch *= scale;
    }

    if (result.yawPredicted || result.pitchPredicted)
    {
        ApplyYawPitch(result.target, yaw, pitch);
        result.target.timestampMs = request.targetScanoutMs;
        result.source = postMapInput ? PoseSource::Kcd2PostMapInput : PoseSource::RawInput;
        result.residualAngularVelocity =
            result.residualIntervalMs > 0.0 ? magnitude / static_cast<float>(result.residualIntervalMs * 0.001) : 0.0f;
    }
    else if (result.source == PoseSource::Hold && request.previous.timestampMs > 0.0 &&
             request.content.timestampMs > request.previous.timestampMs)
    {
        const auto poseInterval = request.content.timestampMs - request.previous.timestampMs;
        const auto scale = static_cast<float>(
            1.0 + std::max(0.0, request.targetScanoutMs - request.content.timestampMs) / poseInterval);
        for (int axis = 0; axis < 3; ++axis)
        {
            result.target.right[axis] =
                request.previous.right[axis] + (request.content.right[axis] - request.previous.right[axis]) * scale;
            result.target.up[axis] =
                request.previous.up[axis] + (request.content.up[axis] - request.previous.up[axis]) * scale;
            result.target.forward[axis] = request.previous.forward[axis] +
                                          (request.content.forward[axis] - request.previous.forward[axis]) * scale;
            result.target.position[axis] = request.previous.position[axis] +
                                           (request.content.position[axis] - request.previous.position[axis]) * scale;
        }
        Orthonormalize(result.target);
        result.target.timestampMs = request.targetScanoutMs;
        result.source = PoseSource::RenderedVelocity;
    }

    if (activeMouse)
    {
        ++state.activeSamples;
        if (result.yawPredicted || result.pitchPredicted || result.source == PoseSource::Kcd2CView)
            ++state.predictedSamples;
    }
    state.pendingTarget = result.target;
    state.pendingTarget.timestampMs = request.targetScanoutMs;
    state.pendingValidation = activeMouse && hasCamera;
    const auto stats = StatsLocked(state);
    result.qualified = stats.qualified;
    return result;
}

ShadowStats GetShadowStats()
{
    auto& state = GetResolverState();
    std::scoped_lock lock(state.mutex);
    return StatsLocked(state);
}
} // namespace TargetPoseResolver
