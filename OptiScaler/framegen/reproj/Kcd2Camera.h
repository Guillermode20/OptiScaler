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
} // namespace Kcd2Camera
