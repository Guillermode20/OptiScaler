#pragma once

#include <cstdint>
#include <cstddef>

struct RP_Constants;

namespace Kcd2Camera
{
// Immutable, allocation-free camera sample published by the retail CView hook.
// All vectors are an orthonormal world-space basis and timestampMs is in the
// same QPC domain as Util::MillisecondsNow(). cameraIdentity is diagnostic only;
// consumers must use cutGeneration to invalidate prediction/history.
struct Snapshot
{
    float position[3] {};
    float right[3] {};
    float up[3] {};
    float forward[3] {};
    float verticalFov = 0.0f;
    float pixelAspect = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    double timestampMs = 0.0;
    std::uintptr_t cameraIdentity = 0;
    std::uint64_t sequence = 0;
    std::uint64_t cutGeneration = 0;
};

// Installs a read-only KCD2 CCamera::UpdateFrustumPlanes hook when WHGame.dll is present.
// Safe no-op in every other game and on an unknown KCD2 build.
bool Initialize();

// Replaces missing API camera constants with the latest gameplay CView pose.
// Returns the QPC-compatible millisecond timestamp of that pose, or 0 when unavailable/stale.
// poseIntervalMs receives the exact interval represented by the current/previous camera pair.
double ApplyToConstants(RP_Constants& constants, float fallbackAspect, double* poseIntervalMs = nullptr);

bool IsAvailable();

// Reads a coherent current/previous pair from the hook's seqlock. The
// snapshots remain valid after the call and never alias mutable hook storage.
bool ReadSnapshots(Snapshot& current, Snapshot& previous);

// Human-readable dump of the raw CCamera projection block (0x30..0x64) from the latest pose.
// Used for live validation of the CryEngine CCamera layout in this retail build. Returns false
// while no pose has been published yet.
bool DescribeProjection(char* buffer, size_t size);
} // namespace Kcd2Camera
