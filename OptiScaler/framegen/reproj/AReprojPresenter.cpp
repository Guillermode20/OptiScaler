#include "pch.h"
#include "AReproj_Dx12.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <State.h>
#include <Config.h>
#include <Util.h>
#include <misc/FrameLimit.h>
#include <nvapi/fakenvapi.h>
#include <wrapped/wrapped_swapchain.h>
#include <menu/input/input_system.h>

bool AReproj_Dx12::CreateAsyncPresenter()
{
    if (_presentQueue != nullptr)
        return true;
    if (_device == nullptr || _swapChain == nullptr || _asyncDowngraded)
        return false;

    auto& state = State::Instance();
    if (state.currentWrappedSwapchain == nullptr || state.currentWrappedSwapchain != state.currentFGSwapchain)
        return false;
    auto* wrapped = static_cast<WrappedIDXGISwapChain4*>(state.currentWrappedSwapchain);
    if (!wrapped->IsReprojectionVirtualized() || wrapped->RealSwapChain3() != _swapChain)
        return false;
    _wrappedSwapChain = wrapped;

    IDXGISwapChain2* realSwapChain2 = nullptr;
    if (FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&realSwapChain2))))
        return false;
    _presentWaitableObject = realSwapChain2->GetFrameLatencyWaitableObject();
    if (_presentWaitableObject != nullptr)
    {
        // The presenter owns cadence and pre-submits behind its late-latch
        // fence, so a one-frame DXGI queue no longer steals the preparation
        // window. Keep maximum latency at one wherever the runtime supports it.
        const UINT maximumFrameLatency = 1u;
        if (FAILED(realSwapChain2->SetMaximumFrameLatency(maximumFrameLatency)))
            LOG_WARN("Reproj: failed to set maximum frame latency {}", maximumFrameLatency);
    }
    else
    {
        LOG_INFO("Reproj: present waitable unavailable, using software display clock only");
    }
    realSwapChain2->Release();

    // DIRECT queue for SC command list submission (sync path and backward compat)
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    // Wine/Proton serializes DIRECT queues and high priority can starve the game's
    // render queue (source drops 60→35 FPS, queue 26ms, wake 21ms late, 60% miss).
    // Use the normal-priority presenter queue on Linux; keep HIGH as opt-in via
    // ReprojHighPriorityQueue=true (Windows native).
    const bool wantHighPriority =
        !State::Instance().isRunningOnLinux && Config::Instance()->ReprojHighPriorityQueue.value_or_default();
    queueDesc.Priority = wantHighPriority ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    auto result = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_presentQueue));
    if (FAILED(result) && wantHighPriority)
    {
        LOG_INFO("Reproj: high-priority present queue unavailable ({:X}), retrying normal priority", (UINT) result);
        SAFE_RELEASE(_presentQueue);
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        result = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_presentQueue));
    }
    if (FAILED(result))
    {
        LOG_WARN("Reproj: async present queue creation failed: {:X}", (UINT) result);
        return false;
    }
    _presentQueue->SetName(L"Reproj_PresentQueue");
    _presentQueue->GetTimestampFrequency(&_presentTimestampFrequency);

    // COMPUTE queue for async warp — not serialized with the game's DIRECT queue on
    // VKD3D, eliminating the 10-17ms scheduling delay that breaks per-slot steering.
    // Any partial failure tears the whole compute subsystem down so the DIRECT queue
    // (via _presentQueue) is the seamless fallback; useCompute must never see a
    // compute queue without its fence/allocators/lists.
    auto teardownCompute = [this]()
    {
        SAFE_RELEASE(_computeFence);
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            SAFE_RELEASE(_computeCommandList[i]);
            SAFE_RELEASE(_computeAllocator[i]);
            _computeCommandListResetted[i] = false;
            _computeAllocatorFenceValues[i] = 0;
        }
        SAFE_RELEASE(_computeQueue);
        _computeFenceValue = 0;
    };

    D3D12_COMMAND_QUEUE_DESC computeDesc {};
    computeDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    computeDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    if (!Config::Instance()->ReprojUseComputeQueue.value_or_default())
    {
        LOG_INFO("Reproj: compute queue disabled by config; async warp uses DIRECT queue");
    }
    else
    {
        result = _device->CreateCommandQueue(&computeDesc, IID_PPV_ARGS(&_computeQueue));
        if (FAILED(result))
        {
            LOG_WARN("Reproj: compute queue unavailable ({:X}); async warp will use DIRECT queue", (UINT) result);
            // Fall back: async warp uses _presentQueue (DIRECT). VKD3D serialization
            // will still apply, but at least async mode functions.
        }
        else
        {
            _computeQueue->SetName(L"Reproj_ComputeQueue");

            // Telemetry converts warp GPU timestamps to CPU QPC using a per-queue
            // timestamp frequency. If the compute queue runs on a different GPU
            // clock domain, reject it so the telemetry numbers stay truthful.
            UINT64 computeFrequency = 0;
            if (FAILED(_computeQueue->GetTimestampFrequency(&computeFrequency)) ||
                computeFrequency != _presentTimestampFrequency)
            {
                LOG_WARN("Reproj: compute queue timestamp frequency ({}) differs from present queue ({}); "
                         "keeping DIRECT warp so telemetry stays correct",
                         computeFrequency, _presentTimestampFrequency);
                teardownCompute();
            }
            else
            {
                // Create COMPUTE command allocators and command lists for async warp
                bool computeReady = true;
                for (size_t i = 0; i < BUFFER_COUNT; i++)
                {
                    result = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                             IID_PPV_ARGS(&_computeAllocator[i]));
                    if (FAILED(result))
                    {
                        LOG_ERROR("Reproj: compute allocator[{}] creation failed: {:X}", i, (UINT) result);
                        computeReady = false;
                        break;
                    }
                    _computeAllocator[i]->SetName(std::format(L"Reproj_ComputeAllocator[{}]", i).c_str());

                    result = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, _computeAllocator[i],
                                                        nullptr, IID_PPV_ARGS(&_computeCommandList[i]));
                    if (FAILED(result))
                    {
                        LOG_ERROR("Reproj: compute command list[{}] creation failed: {:X}", i, (UINT) result);
                        computeReady = false;
                        break;
                    }
                    _computeCommandList[i]->SetName(std::format(L"Reproj_ComputeCmdList[{}]", i).c_str());
                    _computeCommandList[i]->Close();
                }
                if (!computeReady)
                {
                    // Tear down the partial compute resources so the caller falls back
                    // to the DIRECT present queue for async warps.
                    teardownCompute();
                }
                else if (_computeQueue != nullptr)
                {
                    result = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_computeFence));
                    if (FAILED(result))
                    {
                        LOG_WARN("Reproj: compute fence creation failed: {:X}; falling back to DIRECT warp",
                                 (UINT) result);
                        teardownCompute();
                    }
                    else
                        _computeFence->SetName(L"Reproj_ComputeFence");
                }
            }
        }
    }

    // Telemetry uses the DIRECT queue for timestamp calibration (both queue types
    // support timestamp queries; DIRECT is the existing baseline for comparison).
    _telemetry.Initialize(_presentQueue);
    _telemetry.SetTimestampResources(nullptr, nullptr, nullptr, _presentTimestampFrequency);
    D3D12_QUERY_HEAP_DESC queryDesc {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = ReprojTelemetry::TRACE_SLOT_COUNT * 2;
    if (SUCCEEDED(_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&_warpTimestampHeap))))
    {
        const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(ReprojTelemetry::TRACE_SLOT_COUNT * 2 * sizeof(UINT64));
        const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&_warpTimestampReadback))))
        {
            SAFE_RELEASE(_warpTimestampHeap);
        }
        else
        {
            _telemetry.SetTimestampResources(_warpTimestampHeap, _warpTimestampReadback, _scFence,
                                             _presentTimestampFrequency);
        }
    }
    LOG_INFO("Reproj: async presenter created (SC: DIRECT, warp: {})",
             _computeQueue != nullptr ? "COMPUTE" : "DIRECT (fallback)");
    return true;
}

void AReproj_Dx12::DestroyAsyncPresenter()
{
    _telemetry.Shutdown();
    _presentWaitableObject = nullptr;
    SAFE_RELEASE(_warpTimestampReadback);
    SAFE_RELEASE(_warpTimestampHeap);
    _presentTimestampFrequency = 0;
    SAFE_RELEASE(_computeFence);
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_computeCommandList[i]);
        SAFE_RELEASE(_computeAllocator[i]);
        _computeCommandListResetted[i] = false;
        _computeAllocatorFenceValues[i] = 0;
    }
    SAFE_RELEASE(_computeQueue);
    SAFE_RELEASE(_presentQueue);
}

bool AReproj_Dx12::StartAsyncPresenter()
{
    if (!Config::Instance()->ReprojAsync.value_or_default())
        return false;
    if (_presentThread.joinable())
        return true;

    _presenterState.store(PresenterState::Starting);
    if (!CreateAsyncPresenter())
    {
        LOG_WARN("Reproj: async presenter unavailable; using safe synchronous presenter");
        DestroyAsyncPresenter();
        _presenterState.store(PresenterState::Stopped);
        return false;
    }

    if (Config::Instance()->FGDrawUIOverFG.value_or_default() && _renderUI == nullptr)
        _renderUI = std::make_unique<RUI_Dx12>("ReprojUI", _device,
                                               Config::Instance()->FGUIPremultipliedAlpha.value_or_default());

    _stopPresenter.store(false);
    {
        std::scoped_lock metricsLock(_metricsMutex);
        _lastDisplayPresentMs = 0.0;
        _presentIntervalCount = 0;
        _presentIntervalCursor = 0;
        std::fill(_presentIntervals, _presentIntervals + _countof(_presentIntervals), 0.0);
    }
    _presenterState.store(PresenterState::Running);
    _presentThread = std::thread(&AReproj_Dx12::PresenterMain, this);

    // The presenter must win CPU scheduling against the game's worker threads; a late
    // wake directly converts into a skipped vblank slot, which reads as judder.
#if defined(_WIN32)
    SetThreadPriority(_presentThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
#endif

    // Cursor-locked games (KCD2) consume raw input inside their own frame loop,
    // so the game-facing input paths only see motion at game cadence and the
    // late latch would read stale totals. The dedicated pump (passive
    // WH_MOUSE_LL low-level hook on its own thread — an observer, never a
    // second RegisterRawInputDevices, which on Wine steals the game's raw
    // delivery) keeps the timestamped totals fresh at the mouse report rate
    // while the async presenter is live, independent of game cadence.
    if (Config::Instance()->ReprojLateLatch.value_or_default() &&
        Config::Instance()->ReprojRawInputPump.value_or_default())
    {
        if (!OptiInput::StartRawInputPump())
            LOG_WARN("Reproj: raw input pump unavailable; late latch may read stale motion");
    }
    return true;
}

void AReproj_Dx12::StopAsyncPresenter()
{
    OptiInput::StopRawInputPump();

    if (!_presentThread.joinable())
        return;

    _presenterState.store(PresenterState::Stopping);
    _stopPresenter.store(true);
    _presentCv.notify_all();
    _presentThread.join();
    _presenterState.store(PresenterState::Stopped);
}

HRESULT AReproj_Dx12::WaitForPresentSlot()
{
    if (_presentWaitableObject == nullptr)
        return S_OK;

    // Wine's waitable for KCD2 is known to be permanently unsignaled (d3d12core AV
    // path and VKD3D queue accounting mismatch). Software display clock is
    // authoritative — don't let a starved waitable stall the presenter.
    if (State::Instance().isRunningOnLinux)
    {
        // Poll without blocking: if the queue has capacity we consume the signal,
        // otherwise proceed via software pacing. Never block the presenter on
        // a starved waitable (KCD2: wait=77, display 0 FPS before this fix).
        const auto waitResult = WaitForSingleObject(_presentWaitableObject, 0);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
            return S_OK;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const auto refreshHz = TargetRefreshHz();
    const DWORD timeout = refreshHz > 10.0 ? static_cast<DWORD>(1000.0 / refreshHz + 5.0) : 25;
    const auto waitResult = WaitForSingleObject(_presentWaitableObject, timeout);
    if (waitResult == WAIT_OBJECT_0)
        return S_OK;
    return waitResult == WAIT_TIMEOUT ? DXGI_ERROR_WAS_STILL_DRAWING : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT AReproj_Dx12::PresentCompositorFrame(UINT syncInterval, UINT flags, bool interpolated, bool waitForSlot)
{
    if (_swapChain == nullptr)
        return E_FAIL;
    if (waitForSlot && _presentWaitableObject == nullptr)
        waitForSlot = false;

    // The caller may consume the waitable object before late-latching and dispatch
    // so input is sampled as close to scanout as possible. Never wait both before
    // dispatch and again here: that adds up to a full refresh of latency.
    if (waitForSlot)
    {
        const auto slotResult = WaitForPresentSlot();
        if (slotResult != S_OK)
            return slotResult;
    }

    if (_presenterState.load() == PresenterState::Running)
    {
        syncInterval = 1;
        flags = 0;
    }
    UINT presentFlags = flags;
    if (syncInterval == 0)
        presentFlags |= DXGI_PRESENT_DO_NOT_WAIT;
    const auto result = _swapChain->Present(syncInterval, presentFlags);
    if (result == S_OK)
        fakenvapi::reportFGPresent(_swapChain, true, interpolated);
    else if (result == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
        Util::GetDeviceRemovedReason(_device);

    // An occluded window is a transient success state (minimize/alt-tab), not a
    // presenter failure. Treat it as an invisible-but-successful present.
    if (result == DXGI_STATUS_OCCLUDED)
        return S_OK;

    return result;
}

void AReproj_Dx12::PresenterMain()
{
    int activePacketIndex = -1;
    UINT64 activeFrame = 0;
    double nextDeadlineMs = 0.0;
    constexpr double VBLANK_SAFETY_LEAD_MS = 2.0;
    constexpr double MAX_TOTAL_EARLY_CORRECTION_MS = 16.0; // 4->16: 8ms drift needs >4ms to recover
    double totalEarlyCorrectionMs = 0.0;
    uint32_t consecutiveJammedPresents = 0;
    LARGE_INTEGER qpcFrequency {};
    QueryPerformanceFrequency(&qpcFrequency);
    const auto* config = Config::Instance();

    // Telemetry: ensure clock initialized (already in ReprojTelemetry ctor)
    // Poll calibration once at thread start if enabled.
    if (config->ReprojTelemetry.value_or_default())
        _telemetry.TryCalibrate();

    while (!_stopPresenter.load())
    {
        const auto refreshHz = TargetRefreshHz();
        auto refreshPeriodMs = refreshHz > 1.0 ? 1000.0 / refreshHz : 8.333;
        // A serial Present(1) loop cannot make a preparation lead longer than
        // one refresh useful: after Present returns, an older grid deadline can
        // already be in the past. Keep adaptive and manual leads inside the
        // current slot while retaining a small safety margin.
        const auto maxUsableLeadMs = std::max(3.0, std::min(20.0, refreshPeriodMs * 0.75));
        const auto dispatchLeadMs = std::clamp(_dispatchLeadMs, 3.0, maxUsableLeadMs);
        const bool completionClock = config->ReprojPresentCompletionClock.value_or_default();

        // Handle TargetRefresh change without restart: reset EMA and grid
        // so 240→120 doesn't stay stuck at 4ms period with early correction drift.
        static double lastSeenRefreshHz = 0.0;
        if (lastSeenRefreshHz > 1.0 && std::abs(refreshHz - lastSeenRefreshHz) > 0.5)
        {
            _measuredRefreshPeriodMs = refreshPeriodMs;
            _displayClockAnchorMs = 0.0;
            totalEarlyCorrectionMs = 0.0;
            _dispatchLeadMs = 3.0;
            nextDeadlineMs = 0.0;
            _lastStatsSyncRefreshCount = 0;
            _lastStatsSyncQpc = 0;
        }
        lastSeenRefreshHz = refreshHz;

        // Telemetry per-slot record (only if enabled, otherwise dummy to avoid overhead)
        ReprojSlotRecord* tSlot = nullptr;
        const bool telemetryEnabled = config->ReprojTelemetry.value_or_default();
        if (telemetryEnabled)
        {
            tSlot = _telemetry.BeginSlot();
            tSlot->loopBeginQpc = _telemetry.NowQpc();
            tSlot->targetRefreshHz = refreshHz;
            tSlot->configuredPeriodMs = refreshPeriodMs;
            tSlot->measuredPeriodMs = _measuredRefreshPeriodMs;
            tSlot->dispatchLeadMs = dispatchLeadMs;
            tSlot->displayClockAnchorMs = _displayClockAnchorMs;
            tSlot->totalEarlyCorrectionMs = totalEarlyCorrectionMs;
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
            tSlot->softwareDeadlineQpc = 0; // filled below when known
            // Periodically try calibration and poll completed GPU work
            _telemetry.TryCalibrate();
            _telemetry.PollCompletedGpuWork();
        }

        // Wine can advance DXGI frame statistics once per composed output rather than per
        // physical vblank.  The completion-clock mode deliberately treats Present(1)'s
        // return timestamp as the phase source and leaves this unstable correction out.
        if (!completionClock && SampleDisplayClock(Util::MillisecondsNow()) && nextDeadlineMs > 0.0)
        {
            const auto nearestVblankMs =
                _displayClockAnchorMs +
                std::round((nextDeadlineMs - _displayClockAnchorMs) / refreshPeriodMs) * refreshPeriodMs;
            const auto correctedMs = nearestVblankMs - VBLANK_SAFETY_LEAD_MS;
            if (correctedMs < nextDeadlineMs)
            {
                const auto remainingBudgetMs = MAX_TOTAL_EARLY_CORRECTION_MS + totalEarlyCorrectionMs;
                const auto appliedDeltaMs =
                    std::max(std::max(correctedMs - nextDeadlineMs, -refreshPeriodMs * 0.5), -remainingBudgetMs);
                if (appliedDeltaMs < 0.0)
                {
                    nextDeadlineMs += appliedDeltaMs;
                    totalEarlyCorrectionMs += appliedDeltaMs;
                    if (tSlot)
                        tSlot->displayClockCorrectionApplied = true;
                }
            }
        }

        if (tSlot)
        {
            tSlot->measuredPeriodMs = _measuredRefreshPeriodMs;
            tSlot->dxgiStatsValid = _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
            tSlot->displayClockAnchorMs = _displayClockAnchorMs;
            tSlot->totalEarlyCorrectionMs = totalEarlyCorrectionMs;
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
        }

        if (nextDeadlineMs > 0.0)
        {
            const auto now = Util::MillisecondsNow();
            const auto lateness = now - nextDeadlineMs;
            if (lateness >= refreshPeriodMs)
            {
                const auto missedDeadlineMs = nextDeadlineMs;
                const auto skipped = static_cast<uint32_t>(std::floor(lateness / refreshPeriodMs));
                nextDeadlineMs += skipped * refreshPeriodMs;
                std::scoped_lock metricsLock(_metricsMutex);
                _metricsMissedDisplaySlots += skipped;
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::SoftwareSkipped;
                    // representedSlots describes the output produced by this
                    // record. These are absent slots, so keep it at one and
                    // account for the skipped vblanks exactly once below.
                    tSlot->representedSlots = 1;
                    tSlot->skippedSlotsBeforeAttempt = skipped;
                    const auto nowQpc = _telemetry.NowQpc();
                    const auto deadlineQpc =
                        nowQpc + static_cast<int64_t>((missedDeadlineMs - now) * qpcFrequency.QuadPart / 1000.0);
                    tSlot->softwareDeadlineQpc = deadlineQpc;
                    tSlot->wakeTargetQpc =
                        deadlineQpc - static_cast<int64_t>(dispatchLeadMs * qpcFrequency.QuadPart / 1000.0);
                    tSlot->wakeCompletedQpc = nowQpc;
                    tSlot->targetRefreshHz = refreshHz;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    // Publish window if due
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                    // Reset tSlot to avoid double-finalize below (create new slot next iteration)
                    tSlot = nullptr;
                    // We still need to sleep until next deadline's lead, but skip this slot's present.
                    // Continue to next iteration after deadline adjustment.
                    // Fall through to WaitForPresenterDeadline handling below with skipped accounting.
                }
            }

            if (tSlot)
            {
                const double deadlineMs = nextDeadlineMs;
                const double wakeTargetMs = deadlineMs - dispatchLeadMs;
                // Convert ms to QPC using telemetry clock
                // Simpler: store ms-based deadline in QPC via Now + delta
                // We'll store softwareDeadline as QPC corresponding to nextDeadlineMs
                // Approximate: softwareDeadlineQpc = NowQpc + (deadlineMs - now) * freq/1000
                const int64_t deltaQpc =
                    static_cast<int64_t>((deadlineMs - Util::MillisecondsNow()) * qpcFrequency.QuadPart / 1000.0);
                tSlot->softwareDeadlineQpc = _telemetry.NowQpc() + deltaQpc;
                tSlot->wakeTargetQpc =
                    tSlot->softwareDeadlineQpc - static_cast<int64_t>(dispatchLeadMs * qpcFrequency.QuadPart / 1000.0);
            }

            const bool deadlineOk = WaitForPresenterDeadline(nextDeadlineMs - dispatchLeadMs);
            const auto wakeCompletedMs = Util::MillisecondsNow();
            if (tSlot)
                tSlot->wakeCompletedQpc = _telemetry.NowQpc();

            // Adapt from usable headroom after the actual wake, not from loop-top lateness. Present(1)
            // normally blocks for most of a refresh, so loop-top is early even when Wine's timer then
            // overshoots the requested wake by 3-10 ms. An older GPU-duration path reset this to 3 ms
            // every slot, making the old adaptation ineffective (telemetry always reported lead=3.00).
            const double LEAD_GROW_MS = State::Instance().isRunningOnLinux ? 1.0 : 0.5;
            constexpr double LEAD_DECAY_MS = 0.05;
            const auto wakeHeadroomMs = nextDeadlineMs - wakeCompletedMs;
            const double growCap = std::min(8.0, maxUsableLeadMs);
            // On Proton the timer can overshoot even with 1ms spin window; grow faster so we
            // recover from a burst of late wakes in fewer slots.
            if (wakeHeadroomMs < (State::Instance().isRunningOnLinux ? 2.5 : 2.0))
                _dispatchLeadMs = std::min(_dispatchLeadMs + LEAD_GROW_MS, growCap);
            else if (wakeHeadroomMs > 4.0)
                _dispatchLeadMs = std::max(_dispatchLeadMs - LEAD_DECAY_MS, 3.0);

            if (!deadlineOk)
            {
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::PresenterStopped;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                }
                break;
            }
        }
        else if (tSlot)
        {
            tSlot->softwareDeadlineQpc = 0;
            tSlot->wakeTargetQpc = 0;
            tSlot->wakeCompletedQpc = _telemetry.NowQpc();
        }

        if (tSlot)
            tSlot->waitableBeginQpc = _telemetry.NowQpc();
        const auto slotResult = WaitForPresentSlot();
        if (tSlot)
        {
            tSlot->waitableEndQpc = _telemetry.NowQpc();
            tSlot->waitableResult = slotResult;
            if (FAILED(slotResult) && slotResult != DXGI_ERROR_WAS_STILL_DRAWING)
                tSlot->outcome = ReprojSlotOutcome::WaitableTimeout; // will be refined below
        }
        if (slotResult != S_OK)
        {
            if (slotResult != DXGI_ERROR_WAS_STILL_DRAWING)
            {
                _presenterState.store(PresenterState::Failed);
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::WaitableTimeout;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                }
            }
            else
            {
                std::scoped_lock metricsLock(_metricsMutex);
                ++_metricsMissedDisplaySlots;
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::WaitableTimeout;
                    tSlot->primaryMissCause = ReprojMissCause::WaitableLate;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                }
            }
            if (nextDeadlineMs > 0.0)
                nextDeadlineMs += refreshPeriodMs;
            continue;
        }
        if (_stopPresenter.load())
        {
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::PresenterStopped;
                _telemetry.FinalizeSlot(tSlot);
            }
            break;
        }

        if (tSlot)
            tSlot->packetSelectionQpc = _telemetry.NowQpc();

        int newestPacketIndex = -1;
        UINT64 newestFrame = activeFrame;
        uint32_t readyCount = 0;
        for (int i = 0; i < BUFFER_COUNT; ++i)
        {
            if (_packets[i].state.load() == PacketState::Ready)
            {
                ++readyCount;
                // New anchors remain queued until the next display slot rather
                // than replacing the current real content with a burst.
                if (_packets[i].frameId > newestFrame)
                {
                    newestFrame = _packets[i].frameId;
                    newestPacketIndex = i;
                }
            }
        }

        bool newAnchor = false;
        bool captureReady = true;
        if (newestPacketIndex >= 0)
        {
            // Check capture fence ready at selection
            auto& cand = _packets[newestPacketIndex];
            if (cand.captureFenceValue != 0 && _uiFence != nullptr)
                captureReady = _uiFence->GetCompletedValue() >= cand.captureFenceValue;
            auto expected = PacketState::Ready;
            if (_packets[newestPacketIndex].state.compare_exchange_strong(expected, PacketState::Presenting))
            {
                auto& newest = _packets[newestPacketIndex];
                // Gate the warp on the same queue that will execute it so the
                // captured frame data is ready before the compute/DIRECT warp runs.
                auto* warpQueue = _computeQueue != nullptr ? _computeQueue : _presentQueue;
                if (FAILED(warpQueue->Wait(_uiFence, newest.captureFenceValue)))
                {
                    newest.state.store(PacketState::Retired);
                    _presenterState.store(PresenterState::Failed);
                    if (tSlot)
                    {
                        tSlot->outcome = ReprojSlotOutcome::FenceFailed;
                        _telemetry.FinalizeSlot(tSlot);
                    }
                    break;
                }
                if (activePacketIndex >= 0)
                    _packets[activePacketIndex].state.store(PacketState::Retired);
                activePacketIndex = newestPacketIndex;
                activeFrame = newest.frameId;
                newAnchor = true;
                for (int i = 0; i < BUFFER_COUNT; ++i)
                    if (i != activePacketIndex && _packets[i].state.load() == PacketState::Ready &&
                        _packets[i].frameId < activeFrame)
                        _packets[i].state.store(PacketState::Retired);
                _presentCv.notify_all();
            }
        }

        if (tSlot)
        {
            tSlot->anchorFrameId = activeFrame;
            tSlot->captureFenceReadyAtSelection = captureReady;
            tSlot->newAnchor = newAnchor;
            tSlot->repeatedAnchor = !newAnchor && activePacketIndex >= 0;
        }

        if (activePacketIndex < 0)
        {
            nextDeadlineMs = 0.0;
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::NoAnchor;
                _telemetry.FinalizeSlot(tSlot);
                _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                {
                    auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                    if (_telemetry.ShouldDumpMiss(snap))
                        _telemetry.DumpMissWindow(tSlot->sequence);
                }
            }
            std::unique_lock lock(_presentMutex);
            _presentCv.wait_for(lock, std::chrono::duration<double, std::milli>(refreshPeriodMs),
                                [&] { return _stopPresenter.load() || _readyFrameId.load() > activeFrame; });
            continue;
        }

        auto& packet = _packets[activePacketIndex];
        ContentFrame* selectedContent = &packet;
        const bool newContent = newAnchor;
        if (tSlot)
        {
            tSlot->newAnchor = newContent;
            tSlot->repeatedAnchor = !newContent;
        }
        const bool focusLost = !OptiInput::IsFocused() && !State::Instance().isRunningOnLinux;
        const auto targetDisplayMs = nextDeadlineMs > 0.0 ? nextDeadlineMs : Util::MillisecondsNow();
        const auto rawPeriod = packet.rawFrameDelta > 1.0 ? packet.rawFrameDelta : packet.frameDelta;
        // Rotation extrapolates the exact previous/current camera pair. Its own interval is the only
        // correct denominator; source-present pacing can differ substantially from the camera callback
        // cadence, especially when a cap is enabled. Falling back preserves non-KCD2 behavior.
        const auto representedPeriod =
            selectedContent->sourcePoseInterval > 1.0 ? selectedContent->sourcePoseInterval : rawPeriod;
        const auto realPeriodMs = std::max(representedPeriod, refreshPeriodMs);
        const bool poseOriginValid = packet.hasCamera && selectedContent->sourcePoseTimestamp > 0.0 &&
                                     selectedContent->sourcePoseTimestamp <= targetDisplayMs;
        const auto warpOriginMs =
            poseOriginValid ? selectedContent->sourcePoseTimestamp : selectedContent->renderTimestamp;
        const auto anchorAgeMs = std::max(0.0, targetDisplayMs - warpOriginMs);
        auto maxTimeStep = std::max(0.25f, config->ReprojMaxTimeStep.value_or_default());
        // Bare-bones warp step: anchor age / represented period, clamped only by
        // the absolute extrapolation cap. No velocity limiting.
        const auto unclampedStep =
            static_cast<float>((anchorAgeMs / realPeriodMs) * config->ReprojTimeStep.value_or_default());
        auto timeStep = std::clamp(unclampedStep, 0.0f, maxTimeStep);

        if (tSlot)
        {
            tSlot->packetRenderTimestampMs = selectedContent->renderTimestamp;
            tSlot->sourcePoseTimestampMs = selectedContent->sourcePoseTimestamp;
            tSlot->rawCaptureIntervalMs = static_cast<float>(packet.rawFrameDelta);
            tSlot->selectedFrameIntervalMs = static_cast<float>(packet.frameDelta);
            tSlot->sourceProvidedFrameIntervalMs = static_cast<float>(State::Instance().lastFGFrameTime);
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
            tSlot->anchorAgeMs = static_cast<float>(anchorAgeMs);
            tSlot->unclampedTimeStep =
                static_cast<float>((anchorAgeMs / realPeriodMs) * config->ReprojTimeStep.value_or_default());
            tSlot->finalTimeStep = timeStep;
            tSlot->maxTimeStep = maxTimeStep;
            tSlot->timestepClamped = tSlot->unclampedTimeStep > maxTimeStep || tSlot->unclampedTimeStep < 0;
            tSlot->mvScaleX = selectedContent->constants.mvScaleX;
            tSlot->mvScaleY = selectedContent->constants.mvScaleY;
            tSlot->jitterX = selectedContent->constants.jitterX;
            tSlot->jitterY = selectedContent->constants.jitterY;
            tSlot->cameraVFov = selectedContent->constants.cameraVFov;
            tSlot->cameraAspect = selectedContent->constants.cameraAspect;
            tSlot->cameraNear = selectedContent->constants.cameraNear;
            tSlot->cameraFar = selectedContent->constants.cameraFar;
            tSlot->requestedMode = static_cast<ReprojEffectiveMode>(config->ReprojMode.value_or_default());
            // Record the actual constants submitted to the shader.  Packet resource
            // availability alone cannot distinguish the stable rotation-only path
            // from full depth/camera reprojection.
            if (!packet.warpAllowed)
                tSlot->effectiveMode = ReprojEffectiveMode::Unwarped;
            else if (selectedContent->constants.mode == 2)
                tSlot->effectiveMode = ReprojEffectiveMode::RotationOnly;
            else if (selectedContent->constants.mode == 1 && packet.hasDepth && packet.hasCamera)
                tSlot->effectiveMode = ReprojEffectiveMode::DepthCamera;
            else
                tSlot->effectiveMode = ReprojEffectiveMode::MotionVector;
            tSlot->velocityAvailable = packet.velocity != nullptr;
            tSlot->depthAvailable = packet.hasDepth;
            tSlot->cameraBasisAvailable = packet.hasCamera;
            tSlot->depthConstantsValid = packet.hasDepth && selectedContent->constants.cameraNear > 0 &&
                                         selectedContent->constants.cameraFar > selectedContent->constants.cameraNear &&
                                         selectedContent->constants.cameraVFov > 0;
            tSlot->cameraProjectionValid =
                selectedContent->constants.cameraVFov > 0.01f && selectedContent->constants.cameraAspect > 0.01f;
            tSlot->hudlessSource = packet.hasUi;
            tSlot->poseIntervalMs = static_cast<float>(realPeriodMs);
            // Timestamp origin
            if (selectedContent->sourcePoseTimestamp > 0 && packet.hasCamera)
                tSlot->timestampOrigin = ReprojTimestampOrigin::CameraCallback;
            else if (selectedContent->sourcePoseTimestamp > 0)
                tSlot->timestampOrigin = ReprojTimestampOrigin::PacketCapture;
            else
                tSlot->timestampOrigin = ReprojTimestampOrigin::FrameIntervalFallback;
            // Shadow calculations
            const double rawStep = anchorAgeMs / std::max(1.0, packet.frameDelta);
            const double emaStep = anchorAgeMs / std::max(1.0, _realPeriodEmaMs);
            // Store differences via unused fields for analysis (reusing shadow arrays in publish)
            (void) rawStep;
            (void) emaStep;
        }

        // GPU query reservation (sequence-indexed, non-blocking)
        uint32_t queryStart = UINT32_MAX;
        uint64_t scFenceBefore = _scFenceValue + 1;
        if (tSlot && telemetryEnabled)
        {
            queryStart = _telemetry.ReserveGpuQueries(tSlot->sequence, scFenceBefore);
            tSlot->gpuQueryIndex = queryStart;
            tSlot->scFenceValue = (queryStart != UINT32_MAX) ? scFenceBefore : 0;
            tSlot->commandRecordingBeginQpc = _telemetry.NowQpc();
            _currentTelemetrySlot = tSlot;
        }
        else
        {
            _currentTelemetrySlot = nullptr;
        }

        if (tSlot)
        {
            tSlot->contentKind = static_cast<uint8_t>(selectedContent->kind);
            tSlot->contentFraction = selectedContent->interpolationFraction;
            tSlot->contentAgeMs =
                static_cast<float>(std::max(0.0, targetDisplayMs - selectedContent->virtualContentTimestamp));
            tSlot->fgDurationMs = static_cast<float>(selectedContent->fgDurationMs);
            tSlot->targetScanoutTimestampMs = targetDisplayMs + refreshPeriodMs * 0.5;
        }

        const bool dispatched = packet.warpAllowed && !focusLost
                                    ? DispatchPacketWarp(activePacketIndex, timeStep, targetDisplayMs, queryStart)
                                    : DisplayPacket(activePacketIndex, true, queryStart);

        if (tSlot)
        {
            tSlot->commandRecordingEndQpc = _telemetry.NowQpc();
            tSlot->queueSubmitQpc = _telemetry.NowQpc();
            if (queryStart != UINT32_MAX)
                _telemetry.OnGpuWorkSubmitted(tSlot->sequence, scFenceBefore, queryStart, tSlot->queueSubmitQpc);
            _currentTelemetrySlot = nullptr;
        }
        else
        {
            _currentTelemetrySlot = nullptr;
        }

        if (!dispatched)
        {
            LOG_ERROR("Reproj: scheduled output failed for anchor {}", packet.frameId);
            _presenterState.store(PresenterState::Failed);
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::DispatchFailed;
                _telemetry.FinalizeSlot(tSlot);
                _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
            }
            break;
        }

        if (tSlot)
            tSlot->presentBeginQpc = _telemetry.NowQpc();

        const auto presentCallStartMs = Util::MillisecondsNow();
        const auto result = PresentCompositorFrame(1, 0, !newContent, false);
        const auto presentedAt = Util::MillisecondsNow();
        const auto presentDurationMs = presentedAt - presentCallStartMs;
        const auto poseAge = static_cast<float>(std::max(0.0, targetDisplayMs - selectedContent->sourcePoseTimestamp));
        RecordWarpFrame(result == S_OK, result != S_OK, poseAge);

        if (tSlot)
        {
            tSlot->presentEndQpc = _telemetry.NowQpc();
            tSlot->presentResult = result;
            tSlot->presentBlockMs = static_cast<float>(presentDurationMs);
            tSlot->presentIntervalMs = static_cast<float>(refreshPeriodMs);
            if (_lastDisplayPresentMs > 0.0)
            {
                const double interval = presentedAt - _lastDisplayPresentMs;
                tSlot->presentIntervalMs = static_cast<float>(interval);
            }
            tSlot->outcome = (result == S_OK) ? ReprojSlotOutcome::Presented : ReprojSlotOutcome::PresentFailed;
            _telemetry.FinalizeSlot(tSlot);
            _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
            if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
            {
                auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                if (_telemetry.ShouldDumpMiss(snap))
                    _telemetry.DumpMissWindow(tSlot->sequence);
            }
        }

        if (result != S_OK)
        {
            LOG_ERROR("Reproj: display-clock present failed: {:X}", (UINT) result);
            _presenterState.store(PresenterState::Failed);
            break;
        }

        constexpr uint32_t WATCHDOG_CONSECUTIVE_JAMS = 10;
        constexpr double WATCHDOG_WEDGE_MS = 2000.0;
        const bool jammedPresent = presentDurationMs > std::max(50.0, refreshPeriodMs * 3.0);
        consecutiveJammedPresents = jammedPresent ? consecutiveJammedPresents + 1 : 0;
        if (presentDurationMs > WATCHDOG_WEDGE_MS || consecutiveJammedPresents >= WATCHDOG_CONSECUTIVE_JAMS)
        {
            LOG_ERROR("Reproj: presenter watchdog tripped: present blocked {:.2f} ms ({} consecutive jams); "
                      "downgrading to the synchronous presenter",
                      presentDurationMs, consecutiveJammedPresents);
            _presenterState.store(PresenterState::Failed);
            break;
        }

        {
            std::scoped_lock metricsLock(_metricsMutex);
            _metricsNewAnchorDisplays += newContent;
            _metricsRepeatedAnchorDisplays += !newContent;
            if (_lastDisplayPresentMs > 0.0)
            {
                const auto intervalMs = presentedAt - _lastDisplayPresentMs;
                _presentIntervals[_presentIntervalCursor] = intervalMs;
                _presentIntervalCursor = (_presentIntervalCursor + 1) % _countof(_presentIntervals);
                _presentIntervalCount = std::min<uint32_t>(_presentIntervalCount + 1, _countof(_presentIntervals));

                const auto representedSlots = static_cast<uint32_t>(std::floor(intervalMs / refreshPeriodMs + 0.5));
                if (representedSlots > 1)
                    _metricsMissedDisplaySlots += representedSlots - 1;
            }
            _lastDisplayPresentMs = presentedAt;
        }

        if (completionClock || nextDeadlineMs <= 0.0)
            nextDeadlineMs = presentedAt + refreshPeriodMs;
        else
            nextDeadlineMs += refreshPeriodMs;
    }

    if (activePacketIndex >= 0)
        _packets[activePacketIndex].state.store(PacketState::Retired);

    if (_presenterState.load() != PresenterState::Failed)
        _presenterState.store(PresenterState::Stopped);
    _presentCv.notify_all();
}
