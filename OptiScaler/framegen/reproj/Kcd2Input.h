#pragma once

#include <cstddef>
#include <cstdint>

namespace Kcd2Input
{
// Passive snapshot of KCD2's post-input-map look events. Mouse values are
// cumulative event units; gamepad values are the latest right-stick
// deflections. All timestamps use Util::MillisecondsNow's QPC domain.
struct Snapshot
{
    double mouseYawTotal = 0.0;
    double mousePitchTotal = 0.0;
    float gamepadYaw = 0.0f;
    float gamepadPitch = 0.0f;
    double mouseTimestampMs = 0.0;
    double gamepadTimestampMs = 0.0;
    std::uint64_t mouseEventCount = 0;
    std::uint64_t gamepadEventCount = 0;
};

// Lazily installs the read-only WHGame input-dispatch hook. Unknown builds
// fail closed and leave KCD2's input path untouched.
bool Initialize();
bool IsAvailable();
bool ReadSnapshot(Snapshot& snapshot);

// Rate-limited caller diagnostic. Returns false until the hook is installed.
bool DescribeStats(char* buffer, std::size_t size);
} // namespace Kcd2Input
