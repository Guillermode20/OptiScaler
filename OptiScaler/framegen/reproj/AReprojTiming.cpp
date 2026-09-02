#include "pch.h"
#include "AReproj_Dx12.h"

#include <algorithm>
#include <cmath>

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/FrameLimit.h>

// Timing policy is deliberately separate from packet capture and GPU work. It
// is shared by the presenter and the source-side capture cadence, but owns no
// resources and does not make presentation decisions.
double AReproj_Dx12::TargetRefreshHz()
{
    std::scoped_lock lock(_refreshMutex);
    auto config = Config::Instance();
    auto target = static_cast<double>(config->ReprojTargetRefresh.value_or_default());
    if (target > 1.0)
        return std::clamp(target, 0.0, 1000.0);

    const auto now = Util::MillisecondsNow();
    if ((now - _lastRefreshQueryMs) >= 1000.0 || _cachedRefreshHz <= 1.0)
    {
        double measured = 0.0;
        if (_swapChain != nullptr)
        {
            DXGI_SWAP_CHAIN_DESC scDesc {};
            if (SUCCEEDED(_swapChain->GetDesc(&scDesc)))
            {
                const auto& rr = scDesc.BufferDesc.RefreshRate;
                if (rr.Denominator > 0)
                    measured = static_cast<double>(rr.Numerator) / static_cast<double>(rr.Denominator);
            }
        }
        if (measured <= 1.0 && _hwnd != NULL)
            measured = static_cast<double>(Util::GetActiveRefreshRate(_hwnd));

        _cachedRefreshHz = measured;
        _lastRefreshQueryMs = now;
    }

    return std::clamp(_cachedRefreshHz, 0.0, 1000.0);
}

uint32_t AReproj_Dx12::WarpCountForPeriod(double realFrameMs, double refreshHz) const
{
    if (refreshHz <= 1.0)
        return 1;

    const auto refreshMs = 1000.0 / refreshHz;
    const auto requested = realFrameMs > refreshMs ? static_cast<int>(std::ceil(realFrameMs / refreshMs)) - 1 : 0;
    constexpr uint32_t maximum = 1;
    return static_cast<uint32_t>(std::clamp(requested, 0, maximum));
}

uint32_t AReproj_Dx12::WarpCountForFrame(double refreshHz) const
{
    return WarpCountForPeriod(State::Instance().lastFGFrameTime, refreshHz);
}

void AReproj_Dx12::WaitUntil(double deadlineMs) const
{
    const auto remaining = deadlineMs - Util::MillisecondsNow();
    if (remaining > 0.1)
        FrameLimit::sleepForMs(remaining);
}

bool AReproj_Dx12::WaitForPresenterDeadline(double deadlineMs)
{
    const double spinWindowMs = State::Instance().isRunningOnLinux ? 1.0 : 0.2;
    while (!_stopPresenter.load())
    {
        const auto remaining = deadlineMs - Util::MillisecondsNow();
        if (remaining <= spinWindowMs)
            break;
        const double chunk = std::min(remaining - spinWindowMs, 5.0);
        if (chunk > spinWindowMs)
        {
            FrameLimit::sleepForPrecisePacingMs(chunk);
            if (_stopPresenter.load())
                return false;
        }
        else
        {
            // Remaining is small but still above spin window - yield briefly to avoid busy spin
            // This path handles Wine timer granularity where sleepForPrecisePacingMs would overshoot
            YieldProcessor();
        }
    }

    while (!_stopPresenter.load() && Util::MillisecondsNow() < deadlineMs)
        YieldProcessor();

    return !_stopPresenter.load();
}

bool AReproj_Dx12::SampleDisplayClock(double nowMs)
{
    if (_swapChain == nullptr)
        return false;

    static const LONGLONG qpfFrequency = []
    {
        LARGE_INTEGER frequency {};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }();
    if (qpfFrequency <= 0)
        return false;
    if (_lastStatsQueryMs > 0.0 && (nowMs - _lastStatsQueryMs) < 50.0)
        return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
    _lastStatsQueryMs = nowMs;

    DXGI_FRAME_STATISTICS stats {};
    if (FAILED(_swapChain->GetFrameStatistics(&stats)) || stats.SyncQPCTime.QuadPart <= 0 ||
        stats.SyncRefreshCount == 0)
        return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;

    LARGE_INTEGER qpcNow {};
    QueryPerformanceCounter(&qpcNow);
    const auto qpcOffsetMs = nowMs - static_cast<double>(qpcNow.QuadPart) * 1000.0 / static_cast<double>(qpfFrequency);
    if (_lastStatsSyncRefreshCount != 0 && stats.SyncRefreshCount > _lastStatsSyncRefreshCount)
    {
        const auto refreshDelta = static_cast<double>(stats.SyncRefreshCount - _lastStatsSyncRefreshCount);
        const auto qpcDeltaMs = static_cast<double>(stats.SyncQPCTime.QuadPart - _lastStatsSyncQpc) * 1000.0 /
                                static_cast<double>(qpfFrequency);
        if (qpcDeltaMs > 0.0)
        {
            const auto measuredPeriodMs = qpcDeltaMs / refreshDelta;
            if (measuredPeriodMs > 2.7 && measuredPeriodMs < 42.0)
                _measuredRefreshPeriodMs = _measuredRefreshPeriodMs > 1.0
                                               ? _measuredRefreshPeriodMs * 0.8 + measuredPeriodMs * 0.2
                                               : measuredPeriodMs;
        }
    }

    _lastStatsSyncRefreshCount = stats.SyncRefreshCount;
    _lastStatsSyncQpc = stats.SyncQPCTime.QuadPart;
    _displayClockAnchorMs =
        static_cast<double>(stats.SyncQPCTime.QuadPart) * 1000.0 / static_cast<double>(qpfFrequency) + qpcOffsetMs;
    return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
}
