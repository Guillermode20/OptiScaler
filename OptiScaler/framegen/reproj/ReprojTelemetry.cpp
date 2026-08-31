#include "pch.h"
#include "ReprojTelemetry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <Config.h>
#include <Logger.h>
#include <misc/FrameLimit.h>
#include <Util.h>
#include "TargetPoseResolver.h"

ReprojTelemetry::ReprojTelemetry()
{
    LARGE_INTEGER freq {};
    QueryPerformanceFrequency(&freq);
    _clock.qpcFrequency = freq.QuadPart;
    _windowStartQpc = _clock.NowQpc();
    _lastPublishQpc = _windowStartQpc;
}

void ReprojTelemetry::Initialize(ID3D12CommandQueue* presentQueue)
{
    _presentQueue = presentQueue;
    _recentGpuQueueDelayMs.store(0.0f, std::memory_order_relaxed);
    _recentGpuDurationMs.store(2.0f, std::memory_order_relaxed);
    ResetWindow(_clock.NowQpc());
}

void ReprojTelemetry::Shutdown()
{
    _presentQueue = nullptr;
    _timestampHeap = nullptr;
    _timestampReadback = nullptr;
    _scFence = nullptr;
}

void ReprojTelemetry::SetTimestampResources(ID3D12QueryHeap* heap, ID3D12Resource* readback, ID3D12Fence* scFence,
                                            uint64_t timestampFrequency)
{
    _timestampHeap = heap;
    _timestampReadback = readback;
    _scFence = scFence;
    _timestampFrequency = timestampFrequency;
}

ReprojSlotRecord* ReprojTelemetry::BeginSlot()
{
    const uint64_t seq = _nextSequence.fetch_add(1, std::memory_order_relaxed);
    const uint32_t idx = static_cast<uint32_t>(seq % TRACE_SLOT_COUNT);
    auto& slot = _slots[idx];
    slot = ReprojSlotRecord {};
    slot.sequence = seq;
    slot.occupied = true;
    slot.loopBeginQpc = _clock.NowQpc();
    slot.outcome = ReprojSlotOutcome::Pending;
    if (_windowStartSequence == 0 && seq == 0)
        _windowStartSequence = seq;
    else if (_windowStartSequence == 0)
        _windowStartSequence = seq;
    return &slot;
}

void ReprojTelemetry::FinalizeSlot(ReprojSlotRecord* slot)
{
    if (slot == nullptr)
        return;
    // Compute derived durations before classification
    if (slot->wakeTargetQpc != 0 && slot->wakeCompletedQpc != 0)
        slot->wakeLatenessMs = static_cast<float>(_clock.DeltaMs(slot->wakeTargetQpc, slot->wakeCompletedQpc));
    if (slot->waitableBeginQpc != 0 && slot->waitableEndQpc != 0)
        slot->waitableDurationMs = static_cast<float>(_clock.DeltaMs(slot->waitableBeginQpc, slot->waitableEndQpc));
    if (slot->commandRecordingBeginQpc != 0 && slot->commandRecordingEndQpc != 0)
        slot->commandRecordingMs =
            static_cast<float>(_clock.DeltaMs(slot->commandRecordingBeginQpc, slot->commandRecordingEndQpc));
    if (slot->presentBeginQpc != 0 && slot->presentEndQpc != 0)
        slot->presentBlockMs = static_cast<float>(_clock.DeltaMs(slot->presentBeginQpc, slot->presentEndQpc));
    if (slot->gpuValid && slot->calibratedGpuStartQpc != 0 && slot->queueSubmitQpc != 0)
        slot->gpuQueueDelayMs = static_cast<float>(_clock.DeltaMs(slot->queueSubmitQpc, slot->calibratedGpuStartQpc));
    if (slot->gpuValid && slot->calibratedGpuStartQpc != 0 && slot->lateLatchSignalQpc != 0)
        slot->lateLatchToGpuStartMs =
            static_cast<float>(_clock.DeltaMs(slot->lateLatchSignalQpc, slot->calibratedGpuStartQpc));
    if (slot->targetScanoutTimestampMs > 0.0 && slot->poseSampleTimestampMs > 0.0)
        slot->cameraLatencyEstimateMs =
            static_cast<float>(slot->targetScanoutTimestampMs - slot->poseSampleTimestampMs);
    if (slot->gpuValid && slot->gpuStartTimestamp != 0 && slot->gpuEndTimestamp != 0 && _timestampFrequency != 0)
    {
        const double gpuMs = static_cast<double>(slot->gpuEndTimestamp - slot->gpuStartTimestamp) * 1000.0 /
                             static_cast<double>(_timestampFrequency);
        if (gpuMs >= 0.0 && gpuMs < 500.0)
            slot->gpuDurationMs = static_cast<float>(gpuMs);
    }
    if (slot->gpuValid && slot->calibratedGpuEndQpc != 0 && slot->softwareDeadlineQpc != 0)
        slot->gpuEndLatenessMs =
            static_cast<float>(_clock.DeltaMs(slot->softwareDeadlineQpc, slot->calibratedGpuEndQpc));

    // Validate GPU queue delay sanity
    if (std::isfinite(slot->gpuQueueDelayMs) && slot->gpuQueueDelayMs < -5.0f)
    {
        slot->gpuQueueDelayMs = std::numeric_limits<float>::quiet_NaN();
        slot->gpuValid = false;
    }
}

ReprojSlotRecord* ReprojTelemetry::GetSlot(uint64_t sequence)
{
    const uint32_t idx = static_cast<uint32_t>(sequence % TRACE_SLOT_COUNT);
    auto& slot = _slots[idx];
    if (slot.occupied && slot.sequence == sequence)
        return &slot;
    return nullptr;
}

const ReprojSlotRecord* ReprojTelemetry::GetSlot(uint64_t sequence) const
{
    const uint32_t idx = static_cast<uint32_t>(sequence % TRACE_SLOT_COUNT);
    const auto& slot = _slots[idx];
    if (slot.occupied && slot.sequence == sequence)
        return &slot;
    return nullptr;
}

void ReprojTelemetry::TryCalibrate()
{
    if (_presentQueue == nullptr)
        return;
    const int64_t now = _clock.NowQpc();
    // once per second
    if (_lastCalibrationAttemptQpc != 0 && _clock.DeltaMs(_lastCalibrationAttemptQpc, now) < 1000.0)
        return;
    _lastCalibrationAttemptQpc = now;

    UINT64 gpuTimestamp = 0;
    UINT64 cpuQpc = 0;
    HRESULT hr = _presentQueue->GetClockCalibration(&gpuTimestamp, &cpuQpc);
    if (FAILED(hr) || gpuTimestamp == 0)
    {
        ++_calibrationFailures;
        _calibrationValid = false;
        return;
    }
    UINT64 freq = 0;
    if (FAILED(_presentQueue->GetTimestampFrequency(&freq)) || freq == 0)
    {
        ++_calibrationFailures;
        _calibrationValid = false;
        return;
    }
    _calibratedGpuTimestamp = gpuTimestamp;
    _calibratedCpuQpc = static_cast<int64_t>(cpuQpc);
    _timestampFrequency = freq;
    _calibrationQpc = now;
    _calibrationValid = true;
    ++_calibrationGeneration;
}

bool ReprojTelemetry::CalibrateGpuTimestamp(uint64_t gpuTimestamp, int64_t& outCpuQpc) const
{
    if (!_calibrationValid || _timestampFrequency == 0 || _clock.qpcFrequency == 0)
        return false;
    // Reject stale calibration (>5s)
    const int64_t now = _clock.NowQpc();
    if (_clock.DeltaMs(_calibrationQpc, now) > 5000.0)
        return false;
    const int64_t deltaGpu = static_cast<int64_t>(gpuTimestamp) - static_cast<int64_t>(_calibratedGpuTimestamp);
    const double deltaQpc = static_cast<double>(deltaGpu) * static_cast<double>(_clock.qpcFrequency) /
                            static_cast<double>(_timestampFrequency);
    outCpuQpc = _calibratedCpuQpc + static_cast<int64_t>(deltaQpc);
    // Sanity: converted time should be within plausible window (±10s)
    if (std::llabs(outCpuQpc - now) > _clock.qpcFrequency * 10)
        return false;
    return true;
}

uint32_t ReprojTelemetry::ReserveGpuQueries(uint64_t sequence, uint64_t fenceValue)
{
    const uint32_t traceIndex = static_cast<uint32_t>(sequence % TRACE_SLOT_COUNT);
    auto& gpu = _gpuSlots[traceIndex];
    if (gpu.pending)
    {
        // Prior record using this trace index has not completed — skip GPU telemetry for this slot.
        ++_gpuQuerySkipped;
        return UINT32_MAX;
    }
    gpu.sequence = sequence;
    gpu.fenceValue = fenceValue;
    gpu.queryStart = traceIndex * 2;
    gpu.pending = true;
    return gpu.queryStart;
}

void ReprojTelemetry::OnGpuWorkSubmitted(uint64_t sequence, uint64_t fenceValue, uint32_t queryStart, int64_t submitQpc)
{
    const uint32_t traceIndex = static_cast<uint32_t>(sequence % TRACE_SLOT_COUNT);
    auto& gpu = _gpuSlots[traceIndex];
    gpu.sequence = sequence;
    gpu.fenceValue = fenceValue;
    gpu.queryStart = queryStart;
    gpu.submitQpc = submitQpc;
    gpu.generation = _calibrationGeneration;
    gpu.pending = true;
}

void ReprojTelemetry::PollCompletedGpuWork()
{
    if (_scFence == nullptr || _timestampReadback == nullptr || _timestampFrequency == 0)
        return;

    const uint64_t completed = _scFence->GetCompletedValue();
    for (uint32_t i = 0; i < TRACE_SLOT_COUNT; ++i)
    {
        auto& gpu = _gpuSlots[i];
        if (!gpu.pending || gpu.fenceValue == 0 || completed < gpu.fenceValue)
            continue;

        const uint32_t queryStart = gpu.queryStart;
        if (queryStart == UINT32_MAX || queryStart + 1 >= GPU_QUERY_COUNT)
        {
            gpu.pending = false;
            continue;
        }

        const SIZE_T offset = static_cast<SIZE_T>(queryStart * sizeof(UINT64));
        D3D12_RANGE range { offset, offset + 2 * sizeof(UINT64) };
        void* mapped = nullptr;
        if (FAILED(_timestampReadback->Map(0, &range, &mapped)))
        {
            gpu.pending = false;
            continue;
        }
        const auto* stamps = reinterpret_cast<const UINT64*>(static_cast<const uint8_t*>(mapped) + offset);
        const uint64_t start = stamps[0];
        const uint64_t end = stamps[1];
        D3D12_RANGE written { 0, 0 };
        _timestampReadback->Unmap(0, &written);

        auto* slot = GetSlot(gpu.sequence);
        if (slot != nullptr)
        {
            slot->gpuStartTimestamp = start;
            slot->gpuEndTimestamp = end;
            if (end >= start)
            {
                int64_t startQpc = 0, endQpc = 0;
                const bool okStart = CalibrateGpuTimestamp(start, startQpc);
                const bool okEnd = CalibrateGpuTimestamp(end, endQpc);
                if (okStart && okEnd)
                {
                    slot->calibratedGpuStartQpc = startQpc;
                    slot->calibratedGpuEndQpc = endQpc;
                    slot->gpuValid = true;
                    slot->queueSubmitQpc = gpu.submitQpc;
                    // Recompute derived GPU fields now that we have calibration
                    if (gpu.submitQpc != 0)
                    {
                        slot->gpuQueueDelayMs = static_cast<float>(_clock.DeltaMs(gpu.submitQpc, startQpc));
                        if (std::isfinite(slot->gpuQueueDelayMs) && slot->gpuQueueDelayMs >= 0.0f &&
                            slot->gpuQueueDelayMs < 100.0f)
                            _recentGpuQueueDelayMs.store(slot->gpuQueueDelayMs, std::memory_order_relaxed);
                    }
                    const double gpuMs =
                        static_cast<double>(end - start) * 1000.0 / static_cast<double>(_timestampFrequency);
                    if (gpuMs >= 0.0 && gpuMs < 500.0)
                    {
                        slot->gpuDurationMs = static_cast<float>(gpuMs);
                        const auto previous = _recentGpuDurationMs.load(std::memory_order_relaxed);
                        _recentGpuDurationMs.store(previous * 0.9f + static_cast<float>(gpuMs) * 0.1f,
                                                   std::memory_order_relaxed);
                    }
                    if (slot->softwareDeadlineQpc != 0)
                        slot->gpuEndLatenessMs = static_cast<float>(_clock.DeltaMs(slot->softwareDeadlineQpc, endQpc));
                    slot->gpuFinishedBeforePresentCall = slot->presentBeginQpc == 0 || endQpc < slot->presentBeginQpc;
                }
                else
                {
                    slot->gpuValid = false;
                }
            }
        }
        gpu.pending = false;
    }
}

void ReprojTelemetry::ClassifySlot(ReprojSlotRecord& slot, double refreshPeriodMs)
{
    // Already has outcome? Still classify miss cause for Presented with long interval.
    if (slot.outcome == ReprojSlotOutcome::SoftwareSkipped)
    {
        // A correction is only a clock cause when this slot actually had one
        // applied. The old path left wakeTargetQpc unset for software skips,
        // which made every ordinary late loop look like ClockCorrection.
        if (slot.displayClockCorrectionApplied)
            slot.primaryMissCause = ReprojMissCause::ClockCorrection;
        else if (std::isfinite(slot.wakeLatenessMs) && slot.wakeLatenessMs > 1.0f)
            slot.primaryMissCause = ReprojMissCause::CpuWakeLate;
        else
            slot.primaryMissCause = ReprojMissCause::SoftwareScheduleSkip;
        return;
    }
    if (slot.outcome == ReprojSlotOutcome::WaitableTimeout)
    {
        slot.primaryMissCause = ReprojMissCause::WaitableLate;
        return;
    }
    if (slot.outcome == ReprojSlotOutcome::DispatchFailed || slot.outcome == ReprojSlotOutcome::FenceFailed ||
        slot.outcome == ReprojSlotOutcome::PresentFailed)
    {
        slot.primaryMissCause = ReprojMissCause::Unknown;
        return;
    }
    if (slot.outcome != ReprojSlotOutcome::Presented)
    {
        slot.primaryMissCause = ReprojMissCause::None;
        return;
    }

    // Successful present — check for slipped slot via present interval
    const bool slipped = std::isfinite(slot.presentIntervalMs) && refreshPeriodMs > 1.0 &&
                         slot.presentIntervalMs > static_cast<float>(refreshPeriodMs * 1.5);
    if (!slipped)
    {
        slot.primaryMissCause = ReprojMissCause::None;
        return;
    }

    // Classify slipped present
    uint32_t secondary = Secondary_None;

    if (std::isfinite(slot.wakeLatenessMs) && slot.wakeLatenessMs > 0.5f)
        secondary |= Secondary_CpuWakeLate;
    if (std::isfinite(slot.waitableDurationMs) && slot.waitableDurationMs > 2.0f)
        secondary |= Secondary_WaitableLate;
    if (!slot.captureFenceReadyAtSelection)
        secondary |= Secondary_CaptureNotReady;

    const float queueThreshold = std::max(1.0f, static_cast<float>(refreshPeriodMs * 0.15));
    if (std::isfinite(slot.gpuQueueDelayMs) && slot.gpuQueueDelayMs > queueThreshold)
        secondary |= Secondary_QueueBacklog;

    const float warpThreshold = std::max(2.0f, static_cast<float>(refreshPeriodMs * 0.35));
    if (std::isfinite(slot.gpuDurationMs) && slot.gpuDurationMs > warpThreshold)
        secondary |= Secondary_WarpGpuSlow;

    if (std::isfinite(slot.gpuEndLatenessMs) && std::isfinite(slot.gpuDurationMs) && slot.gpuEndLatenessMs < -1.0f)
        secondary |= Secondary_PresentSlip;

    slot.secondaryCauseFlags = secondary;

    // Primary precedence as per plan
    if ((secondary & Secondary_CpuWakeLate) && slot.gpuQueueDelayMs < queueThreshold)
        slot.primaryMissCause = ReprojMissCause::CpuWakeLate;
    else if (!slot.captureFenceReadyAtSelection)
        slot.primaryMissCause = ReprojMissCause::CaptureNotReady;
    else if (secondary & Secondary_QueueBacklog)
        slot.primaryMissCause = ReprojMissCause::PresentQueueBacklog;
    else if (secondary & Secondary_WarpGpuSlow)
        slot.primaryMissCause = ReprojMissCause::WarpGpuSlow;
    else if (secondary & Secondary_PresentSlip)
        slot.primaryMissCause = ReprojMissCause::PresentSlip;
    else if (secondary & Secondary_WaitableLate)
        slot.primaryMissCause = ReprojMissCause::WaitableLate;
    else
        slot.primaryMissCause = ReprojMissCause::Unknown;
}

bool ReprojTelemetry::ShouldPublish(int64_t nowQpc) const { return _clock.DeltaMs(_lastPublishQpc, nowQpc) >= 1000.0; }

float ReprojTelemetry::Percentile(std::array<float, TRACE_SLOT_COUNT>& values, size_t count, double p)
{
    if (count == 0)
        return std::numeric_limits<float>::quiet_NaN();
    size_t idx = static_cast<size_t>(std::clamp(p * (count - 1), 0.0, static_cast<double>(count - 1)));
    std::nth_element(values.begin(), values.begin() + idx, values.begin() + count);
    return values[idx];
}

float ReprojTelemetry::SafeDeltaMs(const ReprojClock& clock, int64_t a, int64_t b)
{
    if (a == 0 || b == 0)
        return std::numeric_limits<float>::quiet_NaN();
    return static_cast<float>(clock.DeltaMs(a, b));
}

void ReprojTelemetry::ResetWindow(int64_t nowQpc)
{
    _windowStartQpc = nowQpc;
    _windowStartSequence = _nextSequence.load(std::memory_order_relaxed);
    _gpuQuerySkipped = 0;
}

ReprojTelemetrySnapshot ReprojTelemetry::Publish(int64_t nowQpc, uint32_t legacyMissed)
{
    ReprojTelemetrySnapshot snap {};
    const double windowMs = _clock.DeltaMs(_windowStartQpc, nowQpc);
    if (windowMs < 10.0)
        return snap;

    const uint64_t startSeq = _windowStartSequence;
    const uint64_t endSeq = _nextSequence.load(std::memory_order_relaxed);

    // Collect values for percentiles
    std::array<float, TRACE_SLOT_COUNT> presentIntervals {};
    std::array<float, TRACE_SLOT_COUNT> wakeLateness {};
    std::array<float, TRACE_SLOT_COUNT> waitDurations {};
    std::array<float, TRACE_SLOT_COUNT> cmdDurations {};
    std::array<float, TRACE_SLOT_COUNT> queueDelays {};
    std::array<float, TRACE_SLOT_COUNT> gpuDurations {};
    std::array<float, TRACE_SLOT_COUNT> gpuMargins {};
    std::array<float, TRACE_SLOT_COUNT> presentBlocks {};
    std::array<float, TRACE_SLOT_COUNT> rawIntervals {};
    std::array<float, TRACE_SLOT_COUNT> selectedIntervals {};
    std::array<float, TRACE_SLOT_COUNT> ratios {};
    std::array<float, TRACE_SLOT_COUNT> poseIntervals {};
    std::array<float, TRACE_SLOT_COUNT> anchorAges {};
    std::array<float, TRACE_SLOT_COUNT> unclampedSteps {};
    std::array<float, TRACE_SLOT_COUNT> finalSteps {};
    std::array<float, TRACE_SLOT_COUNT> latchToGpu {};
    std::array<float, TRACE_SLOT_COUNT> shadowRaw {};
    std::array<float, TRACE_SLOT_COUNT> shadowSource {};

    size_t nPresent = 0, nWake = 0, nWait = 0, nCmd = 0, nQueue = 0, nGpu = 0, nMargin = 0, nBlock = 0;
    size_t nRaw = 0, nSelected = 0, nRatio = 0, nPoseInterval = 0, nAge = 0, nUnclamped = 0, nFinal = 0, nShadowRaw = 0,
           nShadowSource = 0, nLatchToGpu = 0;

    uint32_t scheduled = 0, presented = 0, missed = 0, skippedRep = 0, newAnchor = 0, repeated = 0, slipped = 0;
    uint32_t lateWakes = 0, clampCount = 0;
    uint32_t modeMv = 0, modeDepth = 0, modeRotation = 0, modeUnwarped = 0;
    uint32_t camAvail = 0, depthAvail = 0, depthConst = 0, hudless = 0;
    uint32_t contentReal = 0, contentGenerated = 0;
    uint32_t causeCpu = 0, causeWait = 0, causeCap = 0, causeQueue = 0, causeGpu = 0, causePresent = 0, causeClock = 0,
             causeSchedule = 0, causeUnknown = 0;

    const auto countMissCause = [&](ReprojMissCause cause)
    {
        switch (cause)
        {
        case ReprojMissCause::CpuWakeLate:
            ++causeCpu;
            break;
        case ReprojMissCause::WaitableLate:
            ++causeWait;
            break;
        case ReprojMissCause::CaptureNotReady:
            ++causeCap;
            break;
        case ReprojMissCause::PresentQueueBacklog:
            ++causeQueue;
            break;
        case ReprojMissCause::WarpGpuSlow:
            ++causeGpu;
            break;
        case ReprojMissCause::PresentSlip:
            ++causePresent;
            break;
        case ReprojMissCause::ClockCorrection:
            ++causeClock;
            break;
        case ReprojMissCause::SoftwareScheduleSkip:
            ++causeSchedule;
            break;
        case ReprojMissCause::Unknown:
            ++causeUnknown;
            break;
        default:
            break;
        }
    };

    double sumRefresh = 0.0, sumConfigured = 0.0, sumMeasured = 0.0;
    uint32_t nRefresh = 0;

    for (uint64_t seq = startSeq; seq < endSeq; ++seq)
    {
        const auto* slot = GetSlot(seq);
        if (slot == nullptr || !slot->occupied)
            continue;
        if (slot->outcome == ReprojSlotOutcome::Pending)
            continue;

        ++scheduled;
        skippedRep += (slot->representedSlots > 1 ? slot->representedSlots - 1 : 0) + slot->skippedSlotsBeforeAttempt;

        if (slot->outcome == ReprojSlotOutcome::Presented)
        {
            ++presented;
            const bool presentSlipped = std::isfinite(slot->presentIntervalMs) && slot->refreshPeriodMs > 1.0f &&
                                        slot->presentIntervalMs > slot->refreshPeriodMs * 1.5f;
            if (presentSlipped)
            {
                ++slipped;
                const auto represented = static_cast<uint32_t>(
                    std::floor(slot->presentIntervalMs / slot->refreshPeriodMs + 0.5f));
                if (represented > 1)
                    skippedRep += represented - 1;
            }
            if (std::isfinite(slot->presentIntervalMs) && nPresent < TRACE_SLOT_COUNT)
                presentIntervals[nPresent++] = slot->presentIntervalMs;
            // Wake
            if (std::isfinite(slot->wakeLatenessMs) && nWake < TRACE_SLOT_COUNT)
            {
                wakeLateness[nWake++] = slot->wakeLatenessMs;
                if (slot->wakeLatenessMs > 0.5f)
                    ++lateWakes;
            }
            if (std::isfinite(slot->waitableDurationMs) && nWait < TRACE_SLOT_COUNT)
                waitDurations[nWait++] = slot->waitableDurationMs;
            if (std::isfinite(slot->commandRecordingMs) && nCmd < TRACE_SLOT_COUNT)
                cmdDurations[nCmd++] = slot->commandRecordingMs;
            if (std::isfinite(slot->gpuQueueDelayMs) && nQueue < TRACE_SLOT_COUNT)
                queueDelays[nQueue++] = slot->gpuQueueDelayMs;
            if (std::isfinite(slot->gpuDurationMs) && nGpu < TRACE_SLOT_COUNT)
                gpuDurations[nGpu++] = slot->gpuDurationMs;
            if (std::isfinite(slot->lateLatchToGpuStartMs) && nLatchToGpu < TRACE_SLOT_COUNT)
                latchToGpu[nLatchToGpu++] = slot->lateLatchToGpuStartMs;
            if (std::isfinite(slot->gpuEndLatenessMs) && nMargin < TRACE_SLOT_COUNT)
                gpuMargins[nMargin++] = slot->gpuEndLatenessMs;
            if (std::isfinite(slot->presentBlockMs) && nBlock < TRACE_SLOT_COUNT)
                presentBlocks[nBlock++] = slot->presentBlockMs;

            // Prediction
            if (std::isfinite(slot->rawCaptureIntervalMs) && nRaw < TRACE_SLOT_COUNT)
                rawIntervals[nRaw++] = slot->rawCaptureIntervalMs;
            if (std::isfinite(slot->selectedFrameIntervalMs) && nSelected < TRACE_SLOT_COUNT)
                selectedIntervals[nSelected++] = slot->selectedFrameIntervalMs;
            if (std::isfinite(slot->rawCaptureIntervalMs) && std::isfinite(slot->selectedFrameIntervalMs) &&
                slot->selectedFrameIntervalMs > 0.0f && nRatio < TRACE_SLOT_COUNT)
                ratios[nRatio++] = slot->rawCaptureIntervalMs / slot->selectedFrameIntervalMs;
            if (std::isfinite(slot->poseIntervalMs) && slot->poseIntervalMs > 0.0f && nPoseInterval < TRACE_SLOT_COUNT)
                poseIntervals[nPoseInterval++] = slot->poseIntervalMs;
            if (std::isfinite(slot->anchorAgeMs) && nAge < TRACE_SLOT_COUNT)
                anchorAges[nAge++] = slot->anchorAgeMs;
            if (std::isfinite(slot->unclampedTimeStep) && nUnclamped < TRACE_SLOT_COUNT)
                unclampedSteps[nUnclamped++] = slot->unclampedTimeStep;
            if (std::isfinite(slot->finalTimeStep) && nFinal < TRACE_SLOT_COUNT)
                finalSteps[nFinal++] = slot->finalTimeStep;
            if (slot->timestepClamped)
                ++clampCount;

            // Shadow delta: current - raw
            if (std::isfinite(slot->finalTimeStep) && std::isfinite(slot->unclampedTimeStep))
            {
                // For now approximate shadow as difference between final and unclamped (placeholder for rawInterval
                // shadow) Real shadow calc needs rawIntervalStep; we store unclamped as alt for now.
            }

            switch (slot->effectiveMode)
            {
            case ReprojEffectiveMode::MotionVector:
                ++modeMv;
                break;
            case ReprojEffectiveMode::DepthCamera:
                ++modeDepth;
                break;
            case ReprojEffectiveMode::RotationOnly:
                ++modeRotation;
                break;
            default:
                ++modeUnwarped;
                break;
            }
            camAvail += slot->cameraBasisAvailable ? 1 : 0;
            depthAvail += slot->depthAvailable ? 1 : 0;
            depthConst += slot->depthConstantsValid ? 1 : 0;
            hudless += slot->hudlessSource ? 1 : 0;
            newAnchor += slot->newAnchor ? 1 : 0;
            repeated += slot->repeatedAnchor ? 1 : 0;
            contentGenerated += slot->contentKind == 1 ? 1 : 0;
            contentReal += slot->contentKind == 0 ? 1 : 0;

        }
        else
        {
            ++missed;
            // Cause totals describe slots that were not presented. A
            // successful but long-interval present is reported separately as
            // slipped and must not inflate the miss budget.
            countMissCause(slot->primaryMissCause);
        }

        if (slot->refreshPeriodMs > 0 && std::isfinite(slot->refreshPeriodMs))
        {
            sumRefresh += slot->refreshPeriodMs;
            ++nRefresh;
        }
        if (slot->configuredPeriodMs > 0)
            sumConfigured += slot->configuredPeriodMs;
        if (slot->measuredPeriodMs > 0)
            sumMeasured += slot->measuredPeriodMs;
    }

    snap.windowSequenceStart = startSeq;
    snap.windowSequenceEnd = endSeq;
    snap.windowDurationMs = windowMs;
    snap.scheduledSlots = scheduled;
    snap.presented = presented;
    snap.classifiedMisses = missed;
    snap.legacyMisses = legacyMissed;
    snap.skippedRepresentedSlots = skippedRep;
    snap.newAnchorOutputs = newAnchor;
    snap.repeatedAnchorOutputs = repeated;
    snap.slippedPresents = slipped;
    snap.displayFps = windowMs > 0.0 ? presented * 1000.0 / windowMs : 0.0;

    snap.presentIntervalP50 = Percentile(presentIntervals, nPresent, 0.50);
    snap.presentIntervalP95 = Percentile(presentIntervals, nPresent, 0.95);
    snap.presentIntervalP99 = Percentile(presentIntervals, nPresent, 0.99);
    snap.presentIntervalMax = Percentile(presentIntervals, nPresent, 1.0);

    snap.wakeP50 = Percentile(wakeLateness, nWake, 0.50);
    snap.wakeP95 = Percentile(wakeLateness, nWake, 0.95);
    snap.wakeP99 = Percentile(wakeLateness, nWake, 0.99);
    snap.wakeMax = Percentile(wakeLateness, nWake, 1.0);
    snap.waitP50 = Percentile(waitDurations, nWait, 0.50);
    snap.waitP95 = Percentile(waitDurations, nWait, 0.95);
    snap.waitP99 = Percentile(waitDurations, nWait, 0.99);
    snap.cmdP50 = Percentile(cmdDurations, nCmd, 0.50);
    snap.cmdP95 = Percentile(cmdDurations, nCmd, 0.95);
    snap.lateWakes = lateWakes;

    snap.queueP50 = Percentile(queueDelays, nQueue, 0.50);
    snap.queueP95 = Percentile(queueDelays, nQueue, 0.95);
    snap.queueP99 = Percentile(queueDelays, nQueue, 0.99);
    snap.queueMax = Percentile(queueDelays, nQueue, 1.0);
    snap.gpuP50 = Percentile(gpuDurations, nGpu, 0.50);
    snap.gpuP95 = Percentile(gpuDurations, nGpu, 0.95);
    snap.gpuP99 = Percentile(gpuDurations, nGpu, 0.99);
    snap.gpuMax = Percentile(gpuDurations, nGpu, 1.0);
    snap.gpuMarginP50 = Percentile(gpuMargins, nMargin, 0.50);
    snap.gpuQuerySkipped = _gpuQuerySkipped;
    snap.calibrationFailures = _calibrationFailures;
    snap.calibrationValid = _calibrationValid;

    snap.presentBlockP50 = Percentile(presentBlocks, nBlock, 0.50);
    snap.presentBlockP95 = Percentile(presentBlocks, nBlock, 0.95);
    snap.presentBlockP99 = Percentile(presentBlocks, nBlock, 0.99);
    snap.presentBlockMax = Percentile(presentBlocks, nBlock, 1.0);
    snap.frameStatisticsPeriodMs = nRefresh > 0 ? sumRefresh / nRefresh : 0.0;
    snap.configuredPeriodMs = scheduled > 0 ? sumConfigured / scheduled : 0.0;
    snap.measuredPeriodMs = scheduled > 0 ? sumMeasured / scheduled : 0.0;

    snap.modeMv = modeMv;
    snap.modeDepth = modeDepth;
    snap.modeRotation = modeRotation;
    snap.modeUnwarped = modeUnwarped;
    snap.sourceRawP50 = Percentile(rawIntervals, nRaw, 0.50);
    snap.sourceRawP95 = Percentile(rawIntervals, nRaw, 0.95);
    snap.sourceSelectedP50 = Percentile(selectedIntervals, nSelected, 0.50);
    snap.sourceSelectedP95 = Percentile(selectedIntervals, nSelected, 0.95);
    snap.sourceRatioP50 = Percentile(ratios, nRatio, 0.50);
    snap.sourceRatioP95 = Percentile(ratios, nRatio, 0.95);
    snap.poseIntervalP50 = Percentile(poseIntervals, nPoseInterval, 0.50);
    snap.poseIntervalP95 = Percentile(poseIntervals, nPoseInterval, 0.95);
    const auto sourcePacing = FrameLimit::reprojectionSourcePacingStats();
    snap.sourceCapHz = sourcePacing.capHz;
    snap.sourceCapTimingErrorMs = sourcePacing.timingErrorMs;
    const auto requestedSourceCap = Config::Instance()->ReprojSourceFramerateLimit.value_or_default();
    snap.sourceCapRequestedHz = std::isfinite(requestedSourceCap) && requestedSourceCap > 0.0f
                                    ? std::clamp(requestedSourceCap, 0.0f, 1000.0f)
                                    : 0.0f;
    snap.sourceCapActive = sourcePacing.capHz > 0.0f;
    snap.anchorAgeP50 = Percentile(anchorAges, nAge, 0.50);
    snap.anchorAgeP95 = Percentile(anchorAges, nAge, 0.95);
    snap.anchorAgeMax = Percentile(anchorAges, nAge, 1.0);
    snap.unclampedP50 = Percentile(unclampedSteps, nUnclamped, 0.50);
    snap.unclampedP95 = Percentile(unclampedSteps, nUnclamped, 0.95);
    snap.unclampedMax = Percentile(unclampedSteps, nUnclamped, 1.0);
    snap.finalP50 = Percentile(finalSteps, nFinal, 0.50);
    snap.finalP95 = Percentile(finalSteps, nFinal, 0.95);
    snap.finalMax = Percentile(finalSteps, nFinal, 1.0);
    snap.clampCount = clampCount;
    snap.lateLatchToGpuStartP95 = Percentile(latchToGpu, nLatchToGpu, 0.95);
    const auto targetStats = TargetPoseResolver::GetShadowStats();
    snap.targetCoverage = targetStats.activeCoverage;
    snap.targetErrorP95Degrees = targetStats.errorSamples > 0 ? targetStats.errorP95Degrees
                                                               : std::numeric_limits<float>::quiet_NaN();
    snap.targetResolverEnabled = Config::Instance()->ReprojTargetPoseResolver.value_or_default();
    snap.targetActiveSamples = targetStats.activeSamples;
    snap.contentReal = contentReal;
    snap.contentGenerated = contentGenerated;
    snap.cameraBasisAvailable = camAvail;
    snap.depthAvailable = depthAvail;
    snap.depthConstantsValid = depthConst;
    snap.hudlessSource = hudless;

    snap.causeCpu = causeCpu;
    snap.causeWaitable = causeWait;
    snap.causeCapture = causeCap;
    snap.causeQueue = causeQueue;
    snap.causeGpu = causeGpu;
    snap.causePresent = causePresent;
    snap.causeClock = causeClock;
    snap.causeSchedule = causeSchedule;
    snap.causeUnknown = causeUnknown;

    snap.valid = scheduled > 0;

    {
        std::scoped_lock lock(_snapshotMutex);
        _snapshot = snap;
    }

    LogSnapshot(snap);

    ResetWindow(nowQpc);
    _lastPublishQpc = nowQpc;
    _calibrationFailures = 0;

    return snap;
}

ReprojTelemetrySnapshot ReprojTelemetry::GetSnapshot() const
{
    std::scoped_lock lock(_snapshotMutex);
    return _snapshot;
}

void ReprojTelemetry::LogSnapshot(const ReprojTelemetrySnapshot& snap)
{
    if (!snap.valid)
        return;

    // Stable machine-parseable format per spec section 12.
    // Keep every key stable for parsing; do not include human prose inside values.
    LOG_INFO("ReprojTelemetry v=1 slots={} presented={} missed={} legacyMissed={} newAnchor={} repeatAnchor={} "
             "skippedRep={} "
             "cause.cpu={} cause.wait={} cause.capture={} cause.queue={} cause.gpu={} cause.present={} cause.clock={} "
             "cause.schedule={} cause.unknown={} "
             "interval.p50={:.2f} interval.p95={:.2f} interval.p99={:.2f} interval.max={:.2f} "
             "wake.p50={:.2f} wake.p95={:.2f} wake.p99={:.2f} wake.max={:.2f} lateWakes={} "
             "wait.p50={:.2f} wait.p95={:.2f} wait.p99={:.2f} "
             "queue.p50={:.2f} queue.p95={:.2f} queue.p99={:.2f} queue.max={:.2f} "
             "gpu.p50={:.2f} gpu.p95={:.2f} gpu.p99={:.2f} gpu.max={:.2f} gpuMargin.p50={:.2f} gpuSkipped={} "
             "calibFail={} calibValid={} "
             "present.p50={:.2f} present.p95={:.2f} present.p99={:.2f} present.max={:.2f} "
             "mode.mv={} mode.depth={} mode.rotation={} mode.unwarped={} "
             "source.raw.p50={:.2f} source.raw.p95={:.2f} source.selected.p50={:.2f} source.selected.p95={:.2f} "
             "ratio.p50={:.2f} ratio.p95={:.2f} source.capHz={:.2f} source.capError={:.2f} "
             "anchorAge.p50={:.2f} anchorAge.p95={:.2f} anchorAge.max={:.2f} "
             "step.raw.p50={:.2f} step.raw.p95={:.2f} step.raw.max={:.2f} step.final.p50={:.2f} step.final.p95={:.2f} "
             "step.final.max={:.2f} step.clamped={} "
             "camera={}/{} depth={}/{} depthConstants={}/{} hudless={}/{} "
             "fps={:.1f} poseInterval.p50={:.2f} poseInterval.p95={:.2f} content.real={} content.generated={} "
             "target.coverage={:.3f} target.errorP95={:.3f} latchGpu.p95={:.2f} slipped={} "
             "source.capRequestedHz={:.2f} source.capActive={} target.enabled={} target.samples={}",
             snap.scheduledSlots, snap.presented, snap.classifiedMisses, snap.legacyMisses, snap.newAnchorOutputs,
             snap.repeatedAnchorOutputs, snap.skippedRepresentedSlots, snap.causeCpu, snap.causeWaitable,
             snap.causeCapture, snap.causeQueue, snap.causeGpu, snap.causePresent, snap.causeClock, snap.causeSchedule,
             snap.causeUnknown,
             snap.presentIntervalP50, snap.presentIntervalP95, snap.presentIntervalP99, snap.presentIntervalMax,
             snap.wakeP50, snap.wakeP95, snap.wakeP99, snap.wakeMax, snap.lateWakes, snap.waitP50, snap.waitP95,
             snap.waitP99, snap.queueP50, snap.queueP95, snap.queueP99, snap.queueMax, snap.gpuP50, snap.gpuP95,
             snap.gpuP99, snap.gpuMax, snap.gpuMarginP50, snap.gpuQuerySkipped, snap.calibrationFailures,
             snap.calibrationValid ? 1 : 0, snap.presentBlockP50, snap.presentBlockP95, snap.presentBlockP99,
             snap.presentBlockMax, snap.modeMv, snap.modeDepth, snap.modeRotation, snap.modeUnwarped, snap.sourceRawP50,
             snap.sourceRawP95, snap.sourceSelectedP50, snap.sourceSelectedP95, snap.sourceRatioP50,
             snap.sourceRatioP95, snap.sourceCapHz, snap.sourceCapTimingErrorMs, snap.anchorAgeP50, snap.anchorAgeP95,
             snap.anchorAgeMax, snap.unclampedP50, snap.unclampedP95, snap.unclampedMax, snap.finalP50, snap.finalP95,
             snap.finalMax, snap.clampCount, snap.cameraBasisAvailable, snap.scheduledSlots, snap.depthAvailable,
             snap.scheduledSlots, snap.depthConstantsValid, snap.scheduledSlots, snap.hudlessSource,
             snap.scheduledSlots, snap.displayFps, snap.poseIntervalP50, snap.poseIntervalP95, snap.contentReal,
             snap.contentGenerated, snap.targetCoverage, snap.targetErrorP95Degrees, snap.lateLatchToGpuStartP95,
             snap.slippedPresents, snap.sourceCapRequestedHz, snap.sourceCapActive ? 1 : 0,
             snap.targetResolverEnabled ? 1 : 0, snap.targetActiveSamples);
}

bool ReprojTelemetry::ShouldDumpMiss(const ReprojTelemetrySnapshot& snap) const
{
    if (!Config::Instance()->ReprojTelemetryMissDump.value_or_default())
        return false;
    if (!snap.valid)
        return false;
    // Severe miss or burst
    if (snap.classifiedMisses + snap.skippedRepresentedSlots >= 6 || snap.presentIntervalP95 > 25.0f)
        return true;
    return false;
}

void ReprojTelemetry::DumpMissWindow(uint64_t triggerSequence)
{
    if (!Config::Instance()->ReprojTelemetry.value_or_default())
        return;
    const int64_t now = _clock.NowQpc();
    if (_lastDumpQpc != 0 && _clock.DeltaMs(_lastDumpQpc, now) < 10000.0)
        return;
    _lastDumpQpc = now;

    const uint64_t start = triggerSequence > 16 ? triggerSequence - 16 : 0;
    const uint64_t end = triggerSequence + 16;

    for (uint64_t seq = start; seq <= end; ++seq)
    {
        const auto* slot = GetSlot(seq);
        if (slot == nullptr || !slot->occupied)
            continue;
        LOG_INFO("ReprojSlot v=1 seq={} outcome={} cause={} secondary={:X} anchor={} new={} repeat={} effMode={} "
                 "wake={:.2f} wait={:.2f} queue={:.2f} gpu={:.2f} present={:.2f} interval={:.2f} age={:.2f} "
                 "step={:.2f}/{:.2f} vel={} depth={} cam={} pred={} pyaw={:.4f} ppitch={:.4f} content={} "
                 "fraction={:.3f} posePath={} residual={:.2f} conf={:.2f}/{:.2f} err={:.3f}/{:.3f} latchGpu={:.2f}",
                 slot->sequence, static_cast<int>(slot->outcome), static_cast<int>(slot->primaryMissCause),
                 slot->secondaryCauseFlags, slot->anchorFrameId, slot->newAnchor ? 1 : 0, slot->repeatedAnchor ? 1 : 0,
                 static_cast<int>(slot->effectiveMode), slot->wakeLatenessMs, slot->waitableDurationMs,
                 slot->gpuQueueDelayMs, slot->gpuDurationMs, slot->presentBlockMs, slot->presentIntervalMs,
                 slot->anchorAgeMs, slot->unclampedTimeStep, slot->finalTimeStep, slot->velocityAvailable ? 1 : 0,
                 slot->depthAvailable ? 1 : 0, slot->cameraBasisAvailable ? 1 : 0, slot->inputPredicted ? 1 : 0,
                 slot->predictedYawRad, slot->predictedPitchRad, slot->contentKind, slot->contentFraction,
                 slot->posePath, slot->residualPredictionIntervalMs, slot->yawConfidence, slot->pitchConfidence,
                 slot->yawErrorDegrees, slot->pitchErrorDegrees, slot->lateLatchToGpuStartMs);
    }
}

void ReprojTelemetry::FillOverlayText(char* buffer, size_t size) const
{
    auto snap = GetSnapshot();
    if (!snap.valid)
    {
        snprintf(buffer, size, "Reproj Telemetry: no data");
        return;
    }
    snprintf(buffer, size,
             "Display %.1f Hz | p95 %.1f ms | missed %u (skipped %u, slipped %u; legacy %u)\n"
             "Misses: queue %u | capture %u | CPU %u | GPU %u | present %u | schedule %u\n"
             "Queue p95 %.1f ms | GPU p95 %.1f ms | Present p95 %.1f ms\n"
             "Effective: MV %u depth %u rot %u | source %.1f/%.1f ms cap %.1f Hz err %.2f ms | step %.2f/%.2f",
             snap.displayFps, snap.presentIntervalP95, snap.classifiedMisses, snap.skippedRepresentedSlots,
             snap.slippedPresents, snap.legacyMisses, snap.causeQueue, snap.causeCapture, snap.causeCpu, snap.causeGpu,
             snap.causePresent, snap.causeSchedule, snap.queueP95, snap.gpuP95,
             snap.presentBlockP95, snap.modeMv, snap.modeDepth, snap.modeRotation, snap.sourceRawP50, snap.sourceRawP95,
             snap.sourceCapHz, snap.sourceCapTimingErrorMs, snap.finalP50, snap.finalP95);
}
