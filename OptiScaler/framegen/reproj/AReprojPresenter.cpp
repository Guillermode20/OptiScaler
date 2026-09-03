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
#include <framegen/reproj/Kcd2HudIsolation.h>

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
    constexpr bool wantHighPriority = false;
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
    if (!Config::Instance()->ReprojAsyncComputeWarp.value_or_default())
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

    // Dedicated COPY queue for anchor capture so color/UI copies overlap
    // rendering instead of extending the game's frame. COPY is a DMA engine:
    // no shader cores, no contention with game/compute work. Falls back to
    // a COMPUTE queue and finally to game DIRECT when COPY is unavailable.
    {
        D3D12_COMMAND_QUEUE_DESC capDesc {};
        capDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        capDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        auto capRes = _device->CreateCommandQueue(&capDesc, IID_PPV_ARGS(&_captureQueue));
        if (FAILED(capRes) || _captureQueue == nullptr)
        {
            capDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            capRes = _device->CreateCommandQueue(&capDesc, IID_PPV_ARGS(&_captureQueue));
        }
        if (SUCCEEDED(capRes) && _captureQueue != nullptr)
        {
            _captureQueue->SetName(L"Reproj_CaptureQueue");
            bool capReady = true;
            for (size_t i = 0; i < BUFFER_COUNT; ++i)
            {
                auto ctype = capDesc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_COMMAND_LIST_TYPE_COPY
                                                                          : D3D12_COMMAND_LIST_TYPE_COMPUTE;
                auto ar = _device->CreateCommandAllocator(ctype, IID_PPV_ARGS(&_captureAllocator[i]));
                if (FAILED(ar))
                {
                    capReady = false;
                    break;
                }
                _captureAllocator[i]->SetName(std::format(L"Reproj_CaptureAllocator[{}]", i).c_str());
                auto lr = _device->CreateCommandList(0, ctype, _captureAllocator[i], nullptr,
                                                     IID_PPV_ARGS(&_captureCommandList[i]));
                if (FAILED(lr))
                {
                    capReady = false;
                    break;
                }
                _captureCommandList[i]->SetName(std::format(L"Reproj_CaptureList[{}]", i).c_str());
                _captureCommandList[i]->Close();
            }
            if (!capReady)
            {
                for (size_t i = 0; i < BUFFER_COUNT; ++i)
                {
                    SAFE_RELEASE(_captureCommandList[i]);
                    SAFE_RELEASE(_captureAllocator[i]);
                }
                SAFE_RELEASE(_captureQueue);
                LOG_WARN("Reproj: capture queue allocators failed, fallback to game DIRECT");
            }
            else
            {
                auto fr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_captureFence));
                auto inputFr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_captureInputFence));
                auto worldFr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_worldFence));
                if (FAILED(fr) || FAILED(inputFr) || FAILED(worldFr))
                {
                    SAFE_RELEASE(_worldFence);
                    SAFE_RELEASE(_captureFence);
                    SAFE_RELEASE(_captureInputFence);
                    for (size_t i = 0; i < BUFFER_COUNT; ++i)
                    {
                        SAFE_RELEASE(_captureCommandList[i]);
                        SAFE_RELEASE(_captureAllocator[i]);
                    }
                    SAFE_RELEASE(_captureQueue);
                    LOG_WARN("Reproj: capture fence failed, fallback to game DIRECT");
                }
                else
                {
                    _captureInputFence->SetName(L"Reproj_CaptureInputFence");
                    _captureFence->SetName(L"Reproj_CaptureFence");
                    _worldFence->SetName(L"Reproj_WorldFence");
                    _captureFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                    // The ResTrack command-list hook signals this fence when the
                    // CL containing the KCD2 world snapshot is submitted, so the
                    // capture worker's color copy starts while the game still
                    // finishes its frame.
                    Kcd2HudIsolation::SetWorldSignalContext(_worldFence);
                    LOG_INFO("Reproj: capture queue created ({})",
                             capDesc.Type == D3D12_COMMAND_LIST_TYPE_COPY ? "COPY" : "COMPUTE");
                }
            }
        }
        else
        {
            LOG_INFO("Reproj: capture COPY queue unavailable, fallback to game DIRECT");
        }
    }

    // Anchor capture now prefers the COPY queue (overlap), presenter polls capture fence.

    // Telemetry uses the DIRECT queue for timestamp calibration (both queue types
    // support timestamp queries; DIRECT is the existing baseline for comparison).
    _telemetry.Initialize(_presentQueue);
    _telemetry.SetTimestampResources(nullptr, nullptr, nullptr, _presentTimestampFrequency);
#if 0 // Per-slot GPU telemetry is intentionally absent from the minimal path.
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
#endif
    LOG_INFO("Reproj: async presenter created (capture: {}, warp: {})",
             _captureQueue != nullptr
                 ? (_captureQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY ? "COPY" : "COMPUTE")
                 : "game DIRECT",
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
    // Safety net: DestroyAsyncPresenter can be reached without StopAsyncPresenter
    // (CreateAsyncPresenter failure paths); never release the COPY queue objects
    // under a still-running capture worker.
    if (_captureThread.joinable())
    {
        {
            std::scoped_lock lock(_captureWorkMutex);
            _captureWorkStop = true;
            _captureWorkCv.notify_all();
        }
        _captureThread.join();
    }
    if (_captureFenceEvent)
    {
        CloseHandle(_captureFenceEvent);
        _captureFenceEvent = nullptr;
    }
    Kcd2HudIsolation::SetWorldSignalContext(nullptr);
    SAFE_RELEASE(_worldFence);
    SAFE_RELEASE(_captureInputFence);
    _captureInputFenceValue = 0;
    SAFE_RELEASE(_captureFence);
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_captureCommandList[i]);
        SAFE_RELEASE(_captureAllocator[i]);
        _captureCommandListResetted[i] = false;
        _captureAllocatorFenceValues[i] = 0;
    }
    SAFE_RELEASE(_captureQueue);
    _captureFenceValue = 0;
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

    if (_renderUI == nullptr)
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
    // Capture worker: performs the COPY-queue Wait/Execute/Signal + allocator
    // resets off the game's present thread (measured 4-12 ms of block= per frame
    // on Wine/VKD3D otherwise). Drains pending packets and exits on stop.
    if (_captureQueue != nullptr)
    {
        {
            std::scoped_lock lock(_captureWorkMutex);
            _captureWorkStop = false;
            _captureWorkCount = 0;
        }
        _captureThread = std::thread(&AReproj_Dx12::CaptureWorkerMain, this);
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
    if (true)
    {
        if (!OptiInput::StartRawInputPump())
            LOG_WARN("Reproj: raw input pump unavailable; late latch may read stale motion");
    }
    return true;
}

void AReproj_Dx12::StopAsyncPresenter()
{
    OptiInput::StopRawInputPump();
    // Join the capture worker BEFORE draining/releasing GPU objects: it may be
    // mid-submit on the COPY queue, and pending packets must be processed (or
    // at least fence-completed) so no handoff wait is left dangling.
    StopCaptureWorker();

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
    if (result == S_OK || result == DXGI_STATUS_OCCLUDED)
        fakenvapi::reportFGPresent(_swapChain, true, interpolated);
    else if (result == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
        Util::GetDeviceRemovedReason(_device);

    // Propagate occlusion: the presenter backs off instead of spinning the full
    // cadence against an invisible window (alt-tab/minimize) and resets its
    // deadline grid on return so no missed-slot burst is counted.
    return result;
}

// Adaptive repeat-warp shed: the presenter normally warps every repeated
// display slot for a full 120 Hz image cadence, but that warp compute shares
// the GPU with the game. When the source cannot sustain its frame-rate cap
// (cadence EMA over target) or the game thread is stalling behind the GPU
// (block EMA), repeated slots take the cheap blit path instead so the headroom
// goes back to the game; full warps resume once both signals recover.
void AReproj_Dx12::EvaluateRepeatWarpShed(double nowMs, double sourcePeriodMs)
{
    const auto sourceCap = Config::Instance()->ReprojSourceFramerateLimit.value_or_default();
    // Shedding only makes sense behind a source frame-rate cap: it hands GPU
    // time back to the game so the cap holds. An uncapped source runs flat-out
    // (its own pacing is already "as fast as the GPU allows"), so warps on
    // repeated slots are pure display smoothness with no source to protect.
    if (!(sourceCap > 1.0))
    {
        _repeatWarpShed.store(false, std::memory_order_relaxed);
        return;
    }
    const auto targetPeriodMs = 1000.0 / sourceCap;

    // Stall samples arrive at the game's present cadence (every source frame,
    // regardless of the display slot rate). Feeding the EMA only when a newer
    // sample exists keeps one slot from double-counting the same stall value.
    const float stallMs = _latestGameStallMs.load(std::memory_order_relaxed);
    const bool haveFreshSample = stallMs >= 0.0f && stallMs < 500.0f;
    const double stallAgeMs = _lastStallSampleMs > 0.0 ? nowMs - _lastStallSampleMs : 0.0;
    double stallEma = _stallEmaMs.load(std::memory_order_relaxed);
    if (haveFreshSample && (stallMs != _lastStallSampleValue || stallAgeMs > 120.0))
    {
        stallEma = stallEma > 0.0 ? stallEma * 0.7 + stallMs * 0.3 : stallMs;
        _lastStallSampleMs = nowMs;
        _lastStallSampleValue = stallMs;
    }
    else
    {
        // No fresh stall sample (game thread paused or stalled). Decay the EMA
        // so a stale stall spike does not keep the shed engaged forever.
        const double elapsedMs = _lastShedEvaluateMs > 0.0 ? std::max(0.0, nowMs - _lastShedEvaluateMs) : 8.0;
        const double decay = std::exp(-elapsedMs / 400.0);
        stallEma *= decay;
    }
    _stallEmaMs.store(stallEma, std::memory_order_relaxed);

    // Smooth the source period (the game's own EMA already rejects pacing
    // outliers, so a light filter here is enough to drive the shed decision).
    if (sourcePeriodMs > 0.0 && sourcePeriodMs < 500.0)
        _cadenceEmaMs = _cadenceEmaMs > 0.0 ? _cadenceEmaMs * 0.6 + sourcePeriodMs * 0.4 : sourcePeriodMs;

    // Hysteresis: engage at 1.15x the target period (a 60 Hz cap sheds when
    // the source cannot hold ~52 FPS), release back at the cap. The cadence EMA
    // converges to the cap period from above when the source recovers, so the
    // release band sits slightly above 1.0x or the shed would never lift. A
    // sustained game-thread stall (block, pacing sleep excluded) engages too
    // and also demands cadence recovery before warps return.
    constexpr double CADENCE_ENGAGE_RATIO = 1.15;
    constexpr double CADENCE_RELEASE_RATIO = 1.03;
    constexpr double STALL_ENGAGE_MS = 8.0;
    constexpr double STALL_RELEASE_MS = 6.0; // block decays back to its ~1-6 ms floor once warps shed
    constexpr double MIN_SHED_MS = 400.0; // prevent warp/shed oscillation around the boundary

    const bool cadenceEngage = _cadenceEmaMs > targetPeriodMs * CADENCE_ENGAGE_RATIO;
    const bool stallEngage = _stallEmaMs.load(std::memory_order_relaxed) > STALL_ENGAGE_MS;
    const bool engage = cadenceEngage || stallEngage;

    if (engage && !_repeatWarpShed.load(std::memory_order_relaxed))
    {
        _repeatWarpShed.store(true, std::memory_order_relaxed);
        _shedEngagedAtMs = nowMs;
    }
    else if (_repeatWarpShed.load(std::memory_order_relaxed))
    {
        const bool heldMinimum = (nowMs - _shedEngagedAtMs) >= MIN_SHED_MS;
        const bool cadenceRecovered = _cadenceEmaMs <= targetPeriodMs * CADENCE_RELEASE_RATIO;
        const bool stallCleared = _stallEmaMs.load(std::memory_order_relaxed) <= STALL_RELEASE_MS;
        if (heldMinimum && cadenceRecovered && stallCleared)
            _repeatWarpShed.store(false, std::memory_order_relaxed);
    }
    _lastShedEvaluateMs = nowMs;
}

void AReproj_Dx12::PresenterMain()
{
    int activePacketIndex = -1;
    UINT64 activeFrame = 0;
    double nextDeadlineMs = 0.0;
    uint32_t consecutiveJammedPresents = 0;
    bool presenterOccluded = false;
    uint32_t occlusionProbeCount = 0;

    const auto resetPresentationClock = [&]
    {
        nextDeadlineMs = 0.0;
        _dispatchLeadMs = 3.0;
        std::scoped_lock metricsLock(_metricsMutex);
        _lastDisplayPresentMs = 0.0;
        _presentIntervalCount = 0;
        _presentIntervalCursor = 0;
        std::fill(_presentIntervals, _presentIntervals + _countof(_presentIntervals), 0.0);
    };

    // Reset the adaptive shed controller on each presenter start so a shed
    // carried over from a previous session cannot suppress warps after a restart.
    _repeatWarpShed.store(false, std::memory_order_relaxed);
    _cadenceEmaMs = 0.0;
    _stallEmaMs.store(0.0, std::memory_order_relaxed);
    _lastStallSampleMs = 0.0;
    _lastStallSampleValue = -1.0f;
    _lastShedEvaluateMs = 0.0;
    _shedEngagedAtMs = 0.0;
    _heldPacketIndex = -1;
    _metricsUiBorrows = 0;
    _lateSampleLeadMs = 4.0;
    _lastLateSampleLeadMs.store(4.0);

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

        // Handle TargetRefresh change without restart: reset the grid so a
        // 240→120 switch doesn't stay stuck at the old period.
        static double lastSeenRefreshHz = 0.0;
        if (lastSeenRefreshHz > 1.0 && std::abs(refreshHz - lastSeenRefreshHz) > 0.5)
        {
            _dispatchLeadMs = 3.0;
            nextDeadlineMs = 0.0;
        }
        lastSeenRefreshHz = refreshHz;

        // Per-slot telemetry is compiled out of the minimal path (see AGENTS.md):
        // the once-per-second Reproj: line is the only instrument. No per-slot
        // allocation, QPC sampling, or GPU timestamp queries here.
        _currentTelemetrySlot = nullptr;

        // Once DXGI reports occlusion, stop recording/dispatching GPU work and
        // use Present(TEST) as the visibility probe recommended by DXGI. Also
        // enter this state proactively for a minimized window because Proton
        // does not consistently return DXGI_STATUS_OCCLUDED on the first slot.
        const bool minimized = _hwnd != NULL && IsIconic(_hwnd);
        if (presenterOccluded || minimized)
        {
            if (!presenterOccluded)
            {
                presenterOccluded = true;
                occlusionProbeCount = 0;
                consecutiveJammedPresents = 0;
                resetPresentationClock();
                LOG_INFO("Reproj: window minimized, pausing presenter GPU work");
            }

            const auto visibilityResult = minimized ? DXGI_STATUS_OCCLUDED : _swapChain->Present(0, DXGI_PRESENT_TEST);
            if (visibilityResult == DXGI_STATUS_OCCLUDED)
            {
                ++occlusionProbeCount;
                FrameLimit::sleepForMs(50.0);
                continue;
            }
            if (FAILED(visibilityResult))
            {
                LOG_ERROR("Reproj: occlusion visibility probe failed: {:X}", (UINT) visibilityResult);
                _presenterState.store(PresenterState::Failed);
                break;
            }

            LOG_INFO("Reproj: window visible again after {} occlusion probes", occlusionProbeCount);
            presenterOccluded = false;
            resetPresentationClock();
        }

        if (nextDeadlineMs > 0.0)
        {
            const auto now = Util::MillisecondsNow();
            const auto lateness = now - nextDeadlineMs;
            if (lateness >= refreshPeriodMs)
            {
                const auto skipped = static_cast<uint32_t>(std::floor(lateness / refreshPeriodMs));
                nextDeadlineMs += skipped * refreshPeriodMs;
            }

            const bool deadlineOk = WaitForPresenterDeadline(nextDeadlineMs - dispatchLeadMs);
            const auto wakeCompletedMs = Util::MillisecondsNow();

            // Symmetric proportional lead control: grow when the wake lands too
            // close to the deadline, relax when headroom is generous. The old
            // +1.0/-0.05 ratchet stuck at the 8 ms cap and added latency.
            const auto wakeHeadroomMs = nextDeadlineMs - wakeCompletedMs;
            const double growCap = std::min(8.0, maxUsableLeadMs);
            if (wakeHeadroomMs < 2.0)
                _dispatchLeadMs = std::min(_dispatchLeadMs + 0.25, growCap);
            else if (wakeHeadroomMs > 4.0)
                _dispatchLeadMs = std::max(_dispatchLeadMs - 0.1, 3.0);

            if (!deadlineOk)
                break;
        }

        const auto slotResult = WaitForPresentSlot();
        if (slotResult != S_OK)
        {
            if (slotResult != DXGI_ERROR_WAS_STILL_DRAWING)
                _presenterState.store(PresenterState::Failed);
            // A later successful-present interval accounts for a timed-out
            // slot; counting here too would double the reported miss rate.
            if (nextDeadlineMs > 0.0)
                nextDeadlineMs += refreshPeriodMs;
            continue;
        }
        if (_stopPresenter.load())
            break;

        int newestPacketIndex = -1;
        UINT64 newestFrame = activeFrame;
        // Highest READY frameId regardless of readiness: if it is newer than
        // the active anchor but no newer completed packet was claimed below,
        // this slot deliberately reuses the active anchor (counted as capWait).
        UINT64 newestReadyFrame = activeFrame;
        // Freshest publish time over all READY packets, complete or not. A
        // fresh publish with an incomplete capture is capture latency (keep
        // extrapolating); no fresh publish at all is a source hitch (hold).
        double newestReadyRenderMs = 0.0;
        uint32_t readyCount = 0;
        for (int i = 0; i < BUFFER_COUNT; ++i)
        {
            if (_packets[i].state.load() == PacketState::Ready)
            {
                ++readyCount;
                // New anchors remain queued until the next display slot rather
                // than replacing the current real content with a burst.
                newestReadyRenderMs = std::max(newestReadyRenderMs, _packets[i].renderTimestamp);
                if (_packets[i].frameId > newestReadyFrame)
                    newestReadyFrame = _packets[i].frameId;
                // Only completed anchors are eligible. The presenter must never
                // block its warp queue behind an unfinished capture: an
                // incomplete packet stays READY and is reconsidered next slot.
                // Latency pass: the warp gate is the color copy (signaled
                // first); the UI copy trails. Selection requires SOME complete
                // UI - the packet's own or the current anchor's (borrowed for
                // the first slot) - so a half-copied UI is never composited.
                auto& cand = _packets[i];
                auto* captureFence = cand.completionFence != nullptr ? cand.completionFence : _uiFence;
                const auto captureValue =
                    cand.completionFence != nullptr ? cand.completionFenceValue : cand.captureFenceValue;
                const bool colorComplete =
                    captureValue == 0 || captureFence == nullptr || captureFence->GetCompletedValue() >= captureValue;
                const bool ownUiComplete =
                    cand.captureFenceValue == 0 || captureFence == nullptr ||
                    captureFence->GetCompletedValue() >= cand.captureFenceValue;
                bool uiComplete = ownUiComplete;
                if (!uiComplete && activePacketIndex >= 0)
                {
                    const auto& prev = _packets[activePacketIndex];
                    auto* prevFence = prev.completionFence != nullptr ? prev.completionFence : _uiFence;
                    uiComplete = prev.captureFenceValue == 0 || prevFence == nullptr ||
                                 prevFence->GetCompletedValue() >= prev.captureFenceValue;
                }
                if (colorComplete && uiComplete && _packets[i].frameId > newestFrame)
                {
                    newestFrame = _packets[i].frameId;
                    newestPacketIndex = i;
                }
            }
        }

        // The previous anchor stays Presenting until the NEXT new-anchor
        // switch (or loop exit): its UI (composited while the new anchor's own
        // UI copy trails) and its color (swap-blend source) are needed by the
        // first slot of the new anchor. Tracked in _heldPacketIndex so every
        // early exit path (occlusion, failure, stop) still retires it.
        bool newAnchor = false;
        // A claimed packet is complete by construction above.
        if (newestPacketIndex >= 0)
        {
            auto expected = PacketState::Ready;
            if (_packets[newestPacketIndex].state.compare_exchange_strong(expected, PacketState::Presenting))
            {
                // No presenter-queue wait for capture here: the CPU check above
                // already confirmed completion, and packet resources are
                // immutable until retirement, so the warp queue needs no
                // additional ordering against the game DIRECT queue.
                auto& newest = _packets[newestPacketIndex];
                if (_heldPacketIndex >= 0)
                {
                    _packets[_heldPacketIndex].state.store(PacketState::Retired);
                    _heldPacketIndex = -1;
                }
                if (activePacketIndex >= 0)
                    _heldPacketIndex = activePacketIndex; // held for the borrow/blend
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
        else if (newestReadyFrame > activeFrame)
        {
            // The newest anchor's DIRECT capture copies had not finished when
            // selection ran, so this slot re-warped the active anchor instead
            // of stalling the warp queue behind unfinished work.
            std::scoped_lock metricsLock(_metricsMutex);
            ++_metricsCaptureNotReady;
        }

        if (activePacketIndex < 0)
        {
            nextDeadlineMs = 0.0;
            std::unique_lock lock(_presentMutex);
            _presentCv.wait_for(lock, std::chrono::duration<double, std::milli>(refreshPeriodMs),
                                [&] { return _stopPresenter.load() || _readyFrameId.load() > activeFrame; });
            continue;
        }

        auto& packet = _packets[activePacketIndex];
        ContentFrame* selectedContent = &packet;
        const bool newContent = newAnchor;
        const bool focusLost = !OptiInput::IsFocused() && !State::Instance().isRunningOnLinux;
        const auto targetDisplayMs = nextDeadlineMs > 0.0 ? nextDeadlineMs : Util::MillisecondsNow();
        const auto rawPeriod = packet.rawFrameDelta > 1.0 ? packet.rawFrameDelta : packet.frameDelta;
        // Rotation extrapolates the exact previous/current camera pair. Its own interval is the only
        // correct denominator; source-present pacing can differ substantially from the camera callback
        // cadence, especially when a cap is enabled. Falling back preserves non-KCD2 behavior.
        const auto representedPeriod =
            selectedContent->sourcePoseInterval > 1.0 ? selectedContent->sourcePoseInterval : rawPeriod;
        const auto realPeriodMs = std::max(representedPeriod, refreshPeriodMs);
        // Measure anchor age from the moment the anchor was published (renderTimestamp)
        // rather than simulation pose sampling time, which includes the engine's internal
        // render latency and would artificially bias timeStep by 1.5 - 2.0 frames.
        const auto warpOriginMs = selectedContent->renderTimestamp;
        const auto anchorAgeMs = std::max(0.0, targetDisplayMs - warpOriginMs);
        constexpr float maxTimeStep = 2.5f;
        // Bare-bones warp step: anchor age / represented period, clamped only by
        // the absolute extrapolation cap. No velocity limiting.
        const auto unclampedStep = static_cast<float>(anchorAgeMs / realPeriodMs);
        // Hitch hold: the game stopped publishing (streaming stall), so velocity
        // extrapolation would dead-reckon far past the last known pose and snap
        // back when publishing resumes. Hold the anchor's own pose instead.
        // The late-latch mouse paths ignore timeStep, so aiming stays live
        // through the hold. Gated on PUBLISH freshness, not anchor age: fresh
        // publishes with lagging captures keep normal extrapolation.
        const double publishAgeMs = std::max(0.0, targetDisplayMs - newestReadyRenderMs);
        constexpr float HITCH_HOLD_PERIODS = 2.5f;
        const bool hitchHold = newestReadyRenderMs > 0.0 && publishAgeMs > HITCH_HOLD_PERIODS * realPeriodMs;
        auto timeStep = hitchHold ? 0.0f : std::clamp(unclampedStep, 0.0f, maxTimeStep);
        if (hitchHold)
        {
            std::scoped_lock metricsLock(_metricsMutex);
            ++_metricsHitchHolds;
        }

        // No per-slot telemetry: timeStep inputs stay local, the 1 Hz log line aggregates.
        constexpr uint32_t queryStart = UINT32_MAX;

        // Adaptive repeat-warp shed: repeated display slots are normally warped
        // for a full display-cadence image. When the source cannot sustain its
        // frame-rate cap or the game thread stalls behind the GPU, the shed
        // takes the cheap blit path so warp compute stops stealing the source's
        // frame budget. It re-engages warps as soon as the source is healthy
        // again, so full 120 Hz feel returns in easy scenes.
        EvaluateRepeatWarpShed(targetDisplayMs, packet.frameDelta);
        const bool repeatWarp =
            Config::Instance()->ReprojRepeatWarp.value_or_default() &&
            !_repeatWarpShed.load(std::memory_order_relaxed);
        const bool shouldWarp =
            packet.warpAllowed && !focusLost && (newContent || repeatWarp);
        // Latency pass: composite the newest completed UI. The new anchor's own
        // UI copy trails its color copy by a few ms, so the first display of a
        // new anchor borrows the held previous anchor's UI (16 ms stale is
        // invisible on HUD elements). Swap-smooth blend piggybacks on the same
        // hold: the first warp of a new anchor lerps the previous anchor's
        // warped color so the 60 Hz content swap does not snap.
        int uiPacketIndex = activePacketIndex;
        int prevPacketIndex = -1;
        if (newContent)
        {
            auto* uiFence = packet.completionFence != nullptr ? packet.completionFence : _uiFence;
            const bool ownUiReady = packet.captureFenceValue == 0 || uiFence == nullptr ||
                                    uiFence->GetCompletedValue() >= packet.captureFenceValue;
            if (!ownUiReady && _heldPacketIndex >= 0)
                uiPacketIndex = _heldPacketIndex;
            if (_heldPacketIndex >= 0 && _heldPacketIndex != activePacketIndex)
                prevPacketIndex = _heldPacketIndex;
        }
        const bool dispatched = shouldWarp
                                    ? DispatchPacketWarp(activePacketIndex, uiPacketIndex, prevPacketIndex, timeStep,
                                                         targetDisplayMs, queryStart)
                                    : DisplayPacket(activePacketIndex, true, uiPacketIndex, queryStart);

        if (!dispatched)
        {
            LOG_ERROR("Reproj: scheduled output failed for anchor {}", packet.frameId);
            _presenterState.store(PresenterState::Failed);
            break;
        }

        const auto presentCallStartMs = Util::MillisecondsNow();
        const auto result = PresentCompositorFrame(1, 0, !newContent, false);
        const auto presentedAt = Util::MillisecondsNow();
        const auto presentDurationMs = presentedAt - presentCallStartMs;
        const auto poseAge = static_cast<float>(std::max(0.0, targetDisplayMs - selectedContent->sourcePoseTimestamp));
        const bool occluded = result == DXGI_STATUS_OCCLUDED;
        // Occlusion is invisible-but-successful: do not count it as a displayed
        // output. Enter the visibility-probe state and restart the deadline grid
        // so returning does not burst-count missed slots or jump timeStep.
        if (occluded)
        {
            presenterOccluded = true;
            occlusionProbeCount = 0;
            consecutiveJammedPresents = 0;
            resetPresentationClock();
            LOG_INFO("Reproj: window occluded, pausing presenter GPU work");
            FrameLimit::sleepForMs(50.0);
            continue;
        }

        if (FAILED(result))
        {
            RecordWarpFrame(false, true, poseAge);
            LOG_ERROR("Reproj: display-clock present failed: {:X}", (UINT) result);
            _presenterState.store(PresenterState::Failed);
            break;
        }
        RecordWarpFrame(true, false, poseAge);

        {
            std::scoped_lock metricsLock(_metricsMutex);
            if (uiPacketIndex >= 0 && uiPacketIndex != activePacketIndex)
                ++_metricsUiBorrows;
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

        if (nextDeadlineMs <= 0.0)
            nextDeadlineMs = presentedAt + refreshPeriodMs;
        else
            nextDeadlineMs += refreshPeriodMs;
    }

    if (_heldPacketIndex >= 0)
    {
        _packets[_heldPacketIndex].state.store(PacketState::Retired);
        _heldPacketIndex = -1;
    }
    if (activePacketIndex >= 0)
        _packets[activePacketIndex].state.store(PacketState::Retired);

    if (_presenterState.load() != PresenterState::Failed)
        _presenterState.store(PresenterState::Stopped);
    _presentCv.notify_all();
}
