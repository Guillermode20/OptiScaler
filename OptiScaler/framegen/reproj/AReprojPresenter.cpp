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

    // async-simple: the presenter owns exactly one queue (_presentQueue,
    // DIRECT) and one retirement fence (_scFence). Warps and unwarped blits
    // are recorded on the SC command lists and submitted here; there is no
    // COMPUTE warp queue and no deferred late-latch fence. Anchor capture runs
    // inline on the game's DIRECT queue via the base-class _uiCommandList
    // (see CaptureFramePacket) — no dedicated capture queue exists either.
    LOG_INFO("Reproj: async presenter created (capture: game DIRECT, warp: DIRECT)");
    return true;
}

void AReproj_Dx12::DestroyAsyncPresenter()
{
    _presentWaitableObject = nullptr;
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
        std::scoped_lock metricsLock(_metricsMutex);
        _lastDisplayPresentMs = 0.0;
        _presentIntervalCount = 0;
        _presentIntervalCursor = 0;
        std::fill(_presentIntervals, _presentIntervals + _countof(_presentIntervals), 0.0);
    };

    while (!_stopPresenter.load())
    {
        const auto refreshHz = TargetRefreshHz();
        auto refreshPeriodMs = refreshHz > 1.0 ? 1000.0 / refreshHz : 8.333;
        // A serial Present(1) loop cannot make a preparation lead longer than
        // one refresh useful: after Present returns, an older grid deadline can
        // already be in the past. Fixed 3 ms dispatch lead (kDispatchLeadMs),
        // kept inside the current slot with a small safety margin.
        const auto maxUsableLeadMs = std::max(3.0, std::min(20.0, refreshPeriodMs * 0.75));
        const auto dispatchLeadMs = std::min(kDispatchLeadMs, maxUsableLeadMs);

        // Handle TargetRefresh change without restart: reset the grid so a
        // 240→120 switch doesn't stay stuck at the old period.
        static double lastSeenRefreshHz = 0.0;
        if (lastSeenRefreshHz > 1.0 && std::abs(refreshHz - lastSeenRefreshHz) > 0.5)
            nextDeadlineMs = 0.0;
        lastSeenRefreshHz = refreshHz;

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
        for (int i = 0; i < kReprojFrameSlots; ++i)
        {
            const auto packetState = _packets[i].state.load();
            if (packetState == PacketState::Ready)
            {
                if (_packets[i].frameId > newestReadyFrame)
                    newestReadyFrame = _packets[i].frameId;
                // Only completed anchors are eligible. The warp gate is the
                // single capture fence: the presenter must never block its warp
                // queue behind an unfinished capture, so an incomplete packet
                // stays READY and is reconsidered next slot (counted as capWait
                // via the newestReadyFrame branch below).
                const auto& cand = _packets[i];
                const bool captureComplete = cand.captureFenceValue == 0 || _uiFence == nullptr ||
                                             _uiFence->GetCompletedValue() >= cand.captureFenceValue;
                if (captureComplete && _packets[i].frameId > newestFrame)
                {
                    newestFrame = _packets[i].frameId;
                    newestPacketIndex = i;
                }
            }
        }

        bool newAnchor = false;
        // A claimed packet is complete by construction above. On a real switch
        // the previous anchor is retired immediately: nothing borrows it on the
        // minimal path (no isolated-UI composite), and recycling waits on its
        // _scFence retirement value before the slot is reused for capture.
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
                if (activePacketIndex >= 0)
                    _packets[activePacketIndex].state.store(PacketState::Retired);
                activePacketIndex = newestPacketIndex;
                activeFrame = newest.frameId;
                newAnchor = true;
                for (int i = 0; i < kReprojFrameSlots; ++i)
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
        // Rotation-only extrapolation step, clamped only by the absolute cap.
        // No hitch hold on the minimal path: a stall simply clamps timeStep
        // (extrapolation is bounded by the 2.5 cap either way).
        const auto timeStep = std::clamp(unclampedStep, 0.0f, maxTimeStep);

        // A0 (kAsyncSimpleStage == 0): never dispatch the warp shader. Every
        // slot identity-blits the newest completed anchor so the source cadence
        // can be measured with zero warp cost (see plans/async_simple.md).
        // >=1: every output is a warp of the newest completed anchor — repeated
        // slots included (RepeatWarp is unconditional on this branch; no shed
        // controller exists to take the blit path).
        const bool shouldWarp = kAsyncSimpleStage >= 1 && packet.warpAllowed && !focusLost;
        const bool dispatched = shouldWarp
                                    ? DispatchPacketWarp(activePacketIndex, timeStep, targetDisplayMs)
                                    : DisplayPacket(activePacketIndex);

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

    if (activePacketIndex >= 0)
        _packets[activePacketIndex].state.store(PacketState::Retired);

    if (_presenterState.load() != PresenterState::Failed)
        _presenterState.store(PresenterState::Stopped);
    _presentCv.notify_all();
}
