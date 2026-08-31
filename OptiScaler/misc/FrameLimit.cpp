#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include <State.h>
// #include "hooks/D3D11Hooks.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace
{
std::atomic<float> g_reprojectionSourceCapHz { 0.0f };
std::atomic<float> g_reprojectionSourceTimingErrorMs { 0.0f };
}

inline uint64_t FrameLimit::get_timestamp()
{
    // Monotonic QPC in nanoseconds - wall clock (GetSystemTimePreciseAsFileTime) drifts under Wine/Proton
    // and NTP jumps cause 1fps lock. QPC is steady and matches Util::MillisecondsNow().
    static LARGE_INTEGER s_freq = [] {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    if (s_freq.QuadPart == 0)
    {
        FILETIME ft {};
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return t * 100; // fallback 100ns->ns
    }
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    // QPC * 1e9 / freq = ns (avoid overflow via double - < 10y uptime fits 53b mantissa)
    return static_cast<uint64_t>(static_cast<double>(now.QuadPart) * 1'000'000'000.0 / static_cast<double>(s_freq.QuadPart));
}

// https://learn.microsoft.com/en-us/windows/win32/sync/using-waitable-timer-objects
inline int FrameLimit::timer_sleep(int64_t hundred_ns)
{
    // The presenter and game threads can sleep concurrently. A waitable timer is
    // mutable, so sharing one would let either thread overwrite the other's due time.
    // Intentionally leaked per-thread waitable timer (threads live for process lifetime).
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due_time;

    due_time.QuadPart = -hundred_ns;

    if (!timer)
        return 1;

    if (!SetWaitableTimerEx(timer, &due_time, 0, NULL, NULL, NULL, 0))
        return 2;

    if (WaitForSingleObject(timer, 1000) != WAIT_OBJECT_0)
        return 3;

    return 0;
};

inline int FrameLimit::busywait_sleep(int64_t ns)
{
    auto current_time = get_timestamp();
    auto wait_until = current_time + ns;
    while (current_time < wait_until)
    {
        current_time = get_timestamp();
    }
    return 0;
}

inline int FrameLimit::combined_sleep(int64_t ns, int64_t busywaitThresholdNs)
{
    const auto busywaitThreshold = std::clamp(busywaitThresholdNs, 0LL, ns);
    int status {};
    auto current_time = get_timestamp();
    if (ns <= busywaitThreshold)
        status = busywait_sleep(ns);
    else
    {
        status = timer_sleep((ns - busywaitThreshold) / 100);
        if (status)
        {
            // Wine/Proton may not support CREATE_WAITABLE_TIMER_HIGH_RESOLUTION - fallback to busywait
            status = busywait_sleep(ns);
            return status;
        }
    }

    if (int64_t sleep_deviation = ns - (get_timestamp() - current_time); sleep_deviation > 0 && !status)
        status = busywait_sleep(sleep_deviation);

    return status;
}

void FrameLimit::sleep(bool fgActive)
{
    if (auto fpsCap = Config::Instance()->FramerateLimit.value_or_default(); fpsCap != 0.0f)
    {
        uint64_t min_interval_us = std::clamp((uint64_t) (1'000'000 / fpsCap), 0ULL, 100'000'000ULL);

        if (fgActive)
            min_interval_us *= 2;

        thread_local uint64_t previous_frame_time = 0;
        uint64_t current_time = get_timestamp();
        if (previous_frame_time == 0)
        {
            previous_frame_time = current_time;
            return;
        }
        uint64_t frame_time = current_time >= previous_frame_time ? current_time - previous_frame_time : 0;
        // Large jumps (>1s) mean clock jump or first frame after pause - don't sleep
        if (frame_time > 1'000'000'000ULL)
        {
            previous_frame_time = current_time;
            return;
        }
        if (frame_time < 1000 * min_interval_us)
        {
            if (auto res = combined_sleep(static_cast<int64_t>(min_interval_us * 1000 - frame_time)); res)
                LOG_ERROR("Sleep command failed: {}", res);
            previous_frame_time = get_timestamp();
        }
        else
        {
            previous_frame_time = current_time;
        }
    }
}

void FrameLimit::sleepForMs(double ms)
{
    if (ms <= 0.0)
        return;

    // combined_sleep takes nanoseconds
    if (auto res = combined_sleep((int64_t) (ms * 1'000'000.0)); res)
        LOG_ERROR("Sleep command failed: {}", res);
}

void FrameLimit::sleepForPrecisePacingMs(double ms)
{
    if (ms <= 0.0)
        return;

    // Keep the accuracy benefit of the QPC wait, but reserve spin window so
    // neither the presenter nor the capped game thread monopolizes a CPU core.
    // On Proton the waitable timer granularity can overshoot 0.2ms by 3-10ms,
    // so keep a larger 1.0ms spin window for the time-critical presenter.
    const int64_t spinNs = State::Instance().isRunningOnLinux ? 1'000'000 : 200'000;
    if (auto res = combined_sleep(static_cast<int64_t>(ms * 1'000'000.0), spinNs); res)
        LOG_ERROR("Precise pacing sleep failed: {}", res);
}

void FrameLimit::paceReprojectionSource(bool active)
{
    struct SourcePacer
    {
        uint64_t nextDeadlineNs = 0;
        float capHz = 0.0f;
    };
    thread_local SourcePacer pacer;

    const float requestedCap = active ? Config::Instance()->ReprojSourceFramerateLimit.value_or_default() : 0.0f;
    float capHz = std::isfinite(requestedCap) && requestedCap > 0.0f ? requestedCap : 0.0f;
    // Clamp absurd INI values; 1000 Hz is well above any display/present rate and keeps interval sane.
    capHz = std::clamp(capHz, 0.0f, 1000.0f);
    if (capHz <= 0.0f)
    {
        pacer = {};
        g_reprojectionSourceCapHz.store(0.0f, std::memory_order_relaxed);
        g_reprojectionSourceTimingErrorMs.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const uint64_t intervalNs = std::clamp(static_cast<uint64_t>(1'000'000'000.0 / capHz), 1ULL, 100'000'000'000ULL);
    const uint64_t nowNs = get_timestamp();
    g_reprojectionSourceCapHz.store(capHz, std::memory_order_relaxed);

    // First frame, cap changes, and long stalls all establish a new absolute grid.
    // Resetting on a missed deadline avoids a catch-up burst after resize, alt-tab,
    // or a frame that took longer than its cap interval.
    if (pacer.nextDeadlineNs == 0 || std::abs(pacer.capHz - capHz) > 0.001f || nowNs >= pacer.nextDeadlineNs)
    {
        const float errorMs = pacer.nextDeadlineNs != 0
                                  ? static_cast<float>(static_cast<double>(nowNs - pacer.nextDeadlineNs) / 1'000'000.0)
                                  : 0.0f;
        pacer.nextDeadlineNs = nowNs + intervalNs;
        pacer.capHz = capHz;
        g_reprojectionSourceTimingErrorMs.store(errorMs, std::memory_order_relaxed);
        return;
    }

    const uint64_t deadlineNs = pacer.nextDeadlineNs;
    // The legacy limiter's 2 ms spin tail costs about 12% of a CPU core at
    // 60 Hz. That contention turns an otherwise sustainable 60 FPS KCD2
    // source into 25-30 ms frames. Keep the same absolute deadline grid, but
    // use the 0.2 ms precision tail used by the async presenter.
    sleepForPrecisePacingMs(static_cast<double>(deadlineNs - nowNs) / 1'000'000.0);
    const uint64_t completedNs = get_timestamp();
    g_reprojectionSourceTimingErrorMs.store(
        static_cast<float>(static_cast<double>(completedNs - deadlineNs) / 1'000'000.0), std::memory_order_relaxed);
    pacer.nextDeadlineNs = deadlineNs + intervalNs;
}

FrameLimit::SourcePacingStats FrameLimit::reprojectionSourcePacingStats()
{
    return { g_reprojectionSourceCapHz.load(std::memory_order_relaxed),
             g_reprojectionSourceTimingErrorMs.load(std::memory_order_relaxed) };
}
