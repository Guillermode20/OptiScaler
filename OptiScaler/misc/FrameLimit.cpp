#include "pch.h"
#include "FrameLimit.h"

#include "Config.h"
// #include "hooks/D3D11Hooks.h"

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
    static HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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

inline int FrameLimit::combined_sleep(int64_t ns)
{
    constexpr int64_t busywait_threshold = 2'000'000; // 2ms
    int status {};
    auto current_time = get_timestamp();
    if (ns <= busywait_threshold)
        status = busywait_sleep(ns);
    else
    {
        status = timer_sleep((ns - busywait_threshold) / 100);
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
