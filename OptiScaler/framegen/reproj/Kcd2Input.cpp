#include "pch.h"
#include "Kcd2Input.h"

#include "Logger.h"
#include "Util.h"
#include "scanner/scanner.h"
#include <detours/detours.h>

#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>

// KCD2 input-event layout, look-axis ids, and signature strategy are derived
// from tkhquang/KCD2Tools TPVCamera (MIT), retail KCD2 1.5.x. This adapter only
// observes the dispatcher and always forwards the original event unchanged.
namespace Kcd2Input
{
namespace
{
using InputDispatchFn = void(__fastcall*)(uintptr_t controller, uintptr_t inputEvent, char flag);

constexpr ptrdiff_t INPUT_EVENT_STATE_OFFSET = 0x04;
constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;
constexpr ptrdiff_t INPUT_EVENT_VALUE_OFFSET = 0x18;
constexpr int INPUT_STATE_CHANGED = 8;
constexpr int INPUT_MOUSE_YAW = 0x10A;
constexpr int INPUT_MOUSE_PITCH = 0x10B;
constexpr int INPUT_PAD_YAW = 0x21A;
constexpr int INPUT_PAD_PITCH = 0x21B;

InputDispatchFn g_original = nullptr;
// 0 waits for WHGame.dll, 1 installs, 2 installed, 3 permanent failure.
std::atomic<int> g_initState { 0 };
std::atomic<double> g_mouseYawTotal { 0.0 };
std::atomic<double> g_mousePitchTotal { 0.0 };
std::atomic<float> g_gamepadYaw { 0.0f };
std::atomic<float> g_gamepadPitch { 0.0f };
std::atomic<double> g_mouseTimestampMs { 0.0 };
std::atomic<double> g_gamepadTimestampMs { 0.0 };
std::atomic<std::uint64_t> g_mouseEventCount { 0 };
std::atomic<std::uint64_t> g_gamepadEventCount { 0 };

constexpr std::size_t MOUSE_HISTORY_CAPACITY = 2048;
enum class MouseAxis : std::uint8_t
{
    Yaw,
    Pitch,
};
struct MouseEvent
{
    double timestampMs = 0.0;
    float value = 0.0f;
    MouseAxis axis = MouseAxis::Yaw;
};
std::array<MouseEvent, MOUSE_HISTORY_CAPACITY> g_mouseHistory {};
std::size_t g_mouseHistoryCursor = 0;
std::size_t g_mouseHistoryCount = 0;
std::mutex g_mouseHistoryMutex;

void RecordMouseEvent(double timestampMs, float value, MouseAxis axis)
{
    std::scoped_lock lock(g_mouseHistoryMutex);
    g_mouseHistory[g_mouseHistoryCursor] = { timestampMs, value, axis };
    g_mouseHistoryCursor = (g_mouseHistoryCursor + 1) % MOUSE_HISTORY_CAPACITY;
    g_mouseHistoryCount = std::min(g_mouseHistoryCount + 1, MOUSE_HISTORY_CAPACITY);
}

void Capture(uintptr_t inputEvent)
{
    if (inputEvent < 0x10000)
        return;

    __try
    {
        const auto state = *reinterpret_cast<const int32_t*>(inputEvent + INPUT_EVENT_STATE_OFFSET);
        if (state != INPUT_STATE_CHANGED)
            return;

        const auto id = *reinterpret_cast<const int32_t*>(inputEvent + INPUT_EVENT_ID_OFFSET);
        const auto value = *reinterpret_cast<const float*>(inputEvent + INPUT_EVENT_VALUE_OFFSET);
        if (!std::isfinite(value))
            return;

        const auto timestampMs = Util::MillisecondsNow();
        if (id == INPUT_MOUSE_YAW)
        {
            g_mouseYawTotal.fetch_add(static_cast<double>(value), std::memory_order_relaxed);
            RecordMouseEvent(timestampMs, value, MouseAxis::Yaw);
            g_mouseTimestampMs.store(timestampMs, std::memory_order_release);
            g_mouseEventCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (id == INPUT_MOUSE_PITCH)
        {
            g_mousePitchTotal.fetch_add(static_cast<double>(value), std::memory_order_relaxed);
            RecordMouseEvent(timestampMs, value, MouseAxis::Pitch);
            g_mouseTimestampMs.store(timestampMs, std::memory_order_release);
            g_mouseEventCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (id == INPUT_PAD_YAW)
        {
            g_gamepadYaw.store(value, std::memory_order_relaxed);
            g_gamepadTimestampMs.store(timestampMs, std::memory_order_release);
            g_gamepadEventCount.fetch_add(1, std::memory_order_relaxed);
        }
        else if (id == INPUT_PAD_PITCH)
        {
            g_gamepadPitch.store(value, std::memory_order_relaxed);
            g_gamepadTimestampMs.store(timestampMs, std::memory_order_release);
            g_gamepadEventCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void __fastcall Hook(uintptr_t controller, uintptr_t inputEvent, char flag)
{
    Capture(inputEvent);
    if (g_original)
        g_original(controller, inputEvent, flag);
}
} // namespace

bool Initialize()
{
    const auto state = g_initState.load(std::memory_order_acquire);
    if (state == 2)
        return true;
    if (state == 3)
        return false;

    const auto module = GetModuleHandleW(L"WHGame.dll");
    if (!module)
        return false;

    int expected = 0;
    if (!g_initState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 2;

    // All candidates anchor on distinctive mid-body runs so another mod's
    // entry detour does not hide the signature. Offsets walk back to the same
    // generic input-dispatch entry.
    static constexpr const char* patterns[] = {
        "44 38 81 D8 00 00 00 0F 84 ? ? ? ? 83 7A 10 FF 0F 84 ? ? ? ?",
        "83 7A 10 FF 0F 84 ? ? ? ? 48 8B 05 ? ? ? ? 8B 08 85 C9",
        "0F B6 52 28 F3 0F 10 5B 18 48 8B 0D ? ? ? ? 4C 8B 43 08 0F 5A DB",
    };
    static constexpr ptrdiff_t offsets[] = { -0x15, -0x22, -0x50 };

    uintptr_t address = 0;
    for (std::size_t i = 0; i < std::size(patterns) && !address; ++i)
        address = scanner::GetAddress(module, patterns[i], offsets[i]);

    if (!address)
    {
        LOG_WARN("KCD2 input: generic input-dispatch signature not found; late-input adapter disabled");
        g_initState.store(3, std::memory_order_release);
        return false;
    }

    g_original = reinterpret_cast<InputDispatchFn>(address);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_original), Hook);
    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("KCD2 input: input-dispatch detour failed: {}", result);
        g_original = nullptr;
        g_initState.store(3, std::memory_order_release);
        return false;
    }

    g_initState.store(2, std::memory_order_release);
    LOG_INFO("KCD2 input: passive look-event acquisition installed at {:X}", address);
    return true;
}

bool IsAvailable() { return g_initState.load(std::memory_order_acquire) == 2; }

bool ReadSnapshot(Snapshot& snapshot)
{
    if (!IsAvailable())
        return false;

    snapshot.mouseYawTotal = g_mouseYawTotal.load(std::memory_order_relaxed);
    snapshot.mousePitchTotal = g_mousePitchTotal.load(std::memory_order_relaxed);
    snapshot.gamepadYaw = g_gamepadYaw.load(std::memory_order_relaxed);
    snapshot.gamepadPitch = g_gamepadPitch.load(std::memory_order_relaxed);
    snapshot.mouseTimestampMs = g_mouseTimestampMs.load(std::memory_order_acquire);
    snapshot.gamepadTimestampMs = g_gamepadTimestampMs.load(std::memory_order_acquire);
    snapshot.mouseEventCount = g_mouseEventCount.load(std::memory_order_relaxed);
    snapshot.gamepadEventCount = g_gamepadEventCount.load(std::memory_order_relaxed);
    return true;
}

bool QueryMouseInterval(double beginTimestampMs, double endTimestampMs, MouseInterval& interval)
{
    interval = {};
    if (!IsAvailable() || !std::isfinite(beginTimestampMs) || !std::isfinite(endTimestampMs) ||
        endTimestampMs < beginTimestampMs)
        return false;

    std::scoped_lock lock(g_mouseHistoryMutex);
    if (g_mouseHistoryCount == 0)
    {
        interval.complete = true;
        return true;
    }

    const auto oldestIndex =
        (g_mouseHistoryCursor + MOUSE_HISTORY_CAPACITY - g_mouseHistoryCount) % MOUSE_HISTORY_CAPACITY;
    const auto oldestTimestamp = g_mouseHistory[oldestIndex].timestampMs;
    interval.complete = g_mouseHistoryCount < MOUSE_HISTORY_CAPACITY || beginTimestampMs >= oldestTimestamp;

    for (std::size_t offset = 0; offset < g_mouseHistoryCount; ++offset)
    {
        const auto& event = g_mouseHistory[(oldestIndex + offset) % MOUSE_HISTORY_CAPACITY];
        if (event.timestampMs <= beginTimestampMs || event.timestampMs > endTimestampMs)
            continue;
        if (interval.firstTimestampMs <= 0.0)
            interval.firstTimestampMs = event.timestampMs;
        interval.lastTimestampMs = event.timestampMs;
        if (event.axis == MouseAxis::Yaw)
        {
            interval.yaw += event.value;
            ++interval.yawEvents;
        }
        else
        {
            interval.pitch += event.value;
            ++interval.pitchEvents;
        }
    }
    return true;
}

bool DescribeStats(char* buffer, std::size_t size)
{
    if (buffer == nullptr || size == 0)
        return false;

    Snapshot snapshot {};
    if (!ReadSnapshot(snapshot))
        return false;

    const auto nowMs = Util::MillisecondsNow();
    const auto mouseAgeMs = snapshot.mouseTimestampMs > 0.0 ? nowMs - snapshot.mouseTimestampMs : -1.0;
    const auto padAgeMs = snapshot.gamepadTimestampMs > 0.0 ? nowMs - snapshot.gamepadTimestampMs : -1.0;
    snprintf(buffer, size,
             "mouseEvents %llu mouseAge %.2fms totals %.2f/%.2f padEvents %llu padAge %.2fms deflection %.3f/%.3f",
             static_cast<unsigned long long>(snapshot.mouseEventCount), mouseAgeMs, snapshot.mouseYawTotal,
             snapshot.mousePitchTotal, static_cast<unsigned long long>(snapshot.gamepadEventCount), padAgeMs,
             snapshot.gamepadYaw, snapshot.gamepadPitch);
    return true;
}
} // namespace Kcd2Input
