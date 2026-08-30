#pragma once

#include <cstdint>

namespace TargetPoseResolver
{
enum class PoseSource : std::uint8_t
{
    Hold = 0,
    RenderedVelocity = 1,
    RawInput = 2,
    Kcd2PostMapInput = 3,
    Kcd2CView = 4,
};

struct Pose
{
    float position[3] {};
    float right[3] {};
    float up[3] {};
    float forward[3] {};
    float verticalFov = 0.0f;
    double timestampMs = 0.0;
    std::uint64_t cutGeneration = 0;
};

struct Request
{
    Pose content {};
    Pose previous {};
    double targetScanoutMs = 0.0;
    float responseScale = 1.0f;
    float maxRotationRadians = 0.35f;
};

struct Result
{
    Pose target {};
    PoseSource source = PoseSource::Hold;
    double poseSampleMs = 0.0;
    double residualIntervalMs = 0.0;
    float yawConfidence = 0.0f;
    float pitchConfidence = 0.0f;
    float yawErrorDegrees = 0.0f;
    float pitchErrorDegrees = 0.0f;
    float residualAngularVelocity = 0.0f;
    bool yawPredicted = false;
    bool pitchPredicted = false;
    bool qualified = false;
};

struct ShadowStats
{
    float activeCoverage = 0.0f;
    float errorP95Degrees = 0.0f;
    std::uint32_t activeSamples = 0;
    std::uint32_t predictedSamples = 0;
    std::uint32_t errorSamples = 0;
    bool qualified = false;
};

void Reset();
Result Resolve(const Request& request);
ShadowStats GetShadowStats();
} // namespace TargetPoseResolver
