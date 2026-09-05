#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
#include <State.h>
// #include "hooks/D3D11Hooks.h"

#include <algorithm>
#include <atomic>
#include <cmath>

inline uint64_t FrameLimit::get_timestamp()
{
    // Monotonic QPC in nanoseconds - wall clock (GetSystemTimePreciseAsFileTime) drifts under Wine/Proton
    // and NTP jumps cause 1fps lock. QPC is steady and matches Util::MillisecondsNow().
    static LARGE_INTEGER s_freq = []
    {
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
    return static_cast<uint64_t>(static_cast<double>(now.QuadPart) * 1'000'000'000.0 /
                                 static_cast<double>(s_freq.QuadPart));
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
    // Async reprojection owns the display cadence and deliberately leaves the
    // source/game cadence unconstrained. This guard also covers the Vulkan/
    // VKD3D present path, which can reach this shared limiter without going
    // through FGHooks' reprojection-specific bypass.
    if (IsReprojectionOutput(State::Instance().activeFgOutput))
        return;

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
    // Opt-in source cap for the async-timewarp 60->120 A/B test (see header).
    // The grid is absolute, not "minimum interval since last present", so
    // render jitter cannot accumulate into a cadence that drifts down to
    // 57-58 FPS (the pacer-overshoot failure mode of the old interval pacer on
    // Proton). Overshoots advance the grid without sleeping so one slow frame
    // lets the next frame recover cadence instead of being dragged late too.
    struct SourcePacer
    {
        uint64_t nextDeadlineNs = 0;
        float capHz = 0.0f;
    };
    thread_local SourcePacer pacer;

    float requestedCap = 0.0f;
    if (active)
        requestedCap = Config::Instance()->ReprojSourceFramerateLimit.value_or_default();
    const float capHz = std::clamp(std::isfinite(requestedCap) ? requestedCap : 0.0f, 0.0f, 1000.0f);
    if (capHz <= 0.0f)
    {
        // Uncapped (the shipped default) or disabled: clear the grid so a later
        // re-enable starts on a fresh deadline instead of an old one.
        pacer = {};
        return;
    }

    // Small target headroom (0.2%) ensures a 60 FPS cap completes 60 frames
    // per second instead of letting microsecond scheduler jitter pull the
    // measured rate down to 57-58 FPS.
    const double targetHz = static_cast<double>(capHz) * 1.002;
    const uint64_t intervalNs = std::clamp(static_cast<uint64_t>(1'000'000'000.0 / targetHz), 1ULL, 100'000'000'000ULL);
    const uint64_t nowNs = get_timestamp();

    // Only reset the absolute grid on the first frame, a cap change, or a large
    // stall (> 2 intervals). Resetting on minor late frames prevents the pacer
    // from recovering cadence and pulls sustainable 60 FPS down to 55 FPS.
    const bool capChanged = std::abs(pacer.capHz - capHz) > 0.001f;
    const bool stalled = pacer.nextDeadlineNs != 0 && (nowNs > pacer.nextDeadlineNs + 2 * intervalNs);
    if (pacer.nextDeadlineNs == 0 || capChanged || stalled)
    {
        pacer.nextDeadlineNs = nowNs + intervalNs;
        pacer.capHz = capHz;
        return;
    }

    if (nowNs >= pacer.nextDeadlineNs)
    {
        // The frame finished behind schedule. Advance to the next grid slot
        // without sleeping so the subsequent frame can recover cadence.
        while (pacer.nextDeadlineNs <= nowNs)
            pacer.nextDeadlineNs += intervalNs;
        return;
    }

    const uint64_t deadlineNs = pacer.nextDeadlineNs;
    // Keep the larger Proton spin tail: a coarse timer sleep alone overshoots
    // the 16.67 ms grid by 1-3 ms on Wine, which reads as source FPS dips.
    sleepForPrecisePacingMs(static_cast<double>(deadlineNs - nowNs) / 1'000'000.0);
    const uint64_t completedNs = get_timestamp();
    pacer.nextDeadlineNs = deadlineNs + intervalNs;
    // If the sleep overshot by more than a quarter interval, re-anchor the grid
    // to completion so the error does not carry into the next frame.
    if (completedNs > pacer.nextDeadlineNs - intervalNs / 4)
        pacer.nextDeadlineNs = completedNs + intervalNs;
}
