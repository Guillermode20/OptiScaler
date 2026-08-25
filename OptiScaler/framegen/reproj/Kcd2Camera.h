#pragma once

#include <cstdint>

struct RP_Constants;

namespace Kcd2Camera
{
// Installs a read-only KCD2 CCamera::UpdateFrustumPlanes hook when WHGame.dll is present.
// Safe no-op in every other game and on an unknown KCD2 build.
bool Initialize();

// Replaces missing API camera constants with the latest gameplay CView pose.
// Returns the QPC-compatible millisecond timestamp of that pose, or 0 when unavailable/stale.
// poseIntervalMs receives the exact interval represented by the current/previous camera pair.
double ApplyToConstants(RP_Constants& constants, float fallbackAspect, double* poseIntervalMs = nullptr);

bool IsAvailable();

// Human-readable dump of the raw CCamera projection block (0x30..0x64) from the latest pose.
// Used for live validation of the CryEngine CCamera layout in this retail build. Returns false
// while no pose has been published yet.
bool DescribeProjection(char* buffer, size_t size);
} // namespace Kcd2Camera
