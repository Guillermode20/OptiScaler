#!/usr/bin/env python3
"""Deterministic regression tests for the async display-clock invariants."""

from pathlib import Path
import math
import unittest


class DisplayClock:
    def __init__(self, period_ms):
        self.period = period_ms
        self.deadline = None
        self.active = None
        self.presents = []
        self.missed = 0

    def slot(self, now_ms, ready_packets):
        if self.deadline is None:
            self.deadline = now_ms + self.period
        if now_ms > self.deadline:
            self.missed += 1
            self.deadline = now_ms + self.period
        if ready_packets:
            self.active = max(ready_packets)
        if self.active is not None:
            self.presents.append((self.deadline, self.active))
        self.deadline += self.period


class ReprojectionTests(unittest.TestCase):
    def test_packet_replacement_happens_only_on_slot(self):
        clock = DisplayClock(8.0)
        clock.slot(0.0, [1])
        # Publishing packet 2 does not append a presentation by itself.
        self.assertEqual(len(clock.presents), 1)
        clock.slot(8.0, [2])
        self.assertEqual(clock.presents, [(8.0, 1), (16.0, 2)])

    def test_deadlines_are_monotonic_without_catchup_burst(self):
        clock = DisplayClock(8.0)
        clock.slot(0.0, [1])
        clock.slot(30.0, [])
        clock.slot(38.0, [])
        deadlines = [deadline for deadline, _ in clock.presents]
        self.assertEqual(deadlines, sorted(set(deadlines)))
        self.assertEqual(clock.missed, 1)

    def test_one_successful_present_per_slot(self):
        clock = DisplayClock(1000 / 120)
        for slot in range(120):
            ready = [slot // 4 + 1] if slot % 4 == 0 else []
            clock.slot(slot * 1000 / 120, ready)
        self.assertEqual(len(clock.presents), 120)
        self.assertEqual(len({deadline for deadline, _ in clock.presents}), 120)
        self.assertEqual(len({anchor for _, anchor in clock.presents}), 30)

    def test_runtime_presenter_has_one_vsync_present_site(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1]
        self.assertEqual(presenter.count("PresentCompositorFrame("), 1)
        self.assertIn("PresentCompositorFrame(1, 0, !newContent, false)", presenter)
        self.assertNotIn("DXGI_PRESENT_ALLOW_TEARING", presenter)

    def test_async_hot_path_has_no_per_output_debug_logging(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        dispatch = (root / "OptiScaler/shaders/reprojection/RP_Dx12.cpp").read_text(encoding="utf-8")
        present = reproj.split("HRESULT AReproj_Dx12::PresentCompositorFrame", 1)[1].split(
            "void AReproj_Dx12::PresenterMain", 1)[0]
        warp = dispatch.split("bool RP_Dx12::Dispatch", 1)[1].split("RP_Dx12::RP_Dx12", 1)[0]
        self.assertNotIn("LOG_DEBUG", present)
        self.assertNotIn("LOG_DEBUG", warp)

    def test_warp_phase_extrapolates_from_pose_timestamp(self):
        # Capture-completion timestamps carry the game's pipeline latency and its
        # jitter into the warp phase; the pose-sample time is the valid origin.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1]
        self.assertIn("selectedContent->sourcePoseTimestamp", presenter)
        self.assertIn("warpOriginMs", presenter)
        self.assertNotIn("(targetDisplayMs - packet.renderTimestamp)", presenter)

    def test_source_period_is_ema_smoothed_against_outliers(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "int AReproj_Dx12::AcquirePacket()", 1)[0]
        self.assertIn("_realPeriodEmaMs", capture)
        self.assertIn("packet.frameDelta = _realPeriodEmaMs", capture)

    def test_repeat_warp_shed_and_ui_borrow_are_removed(self):
        # async-simple P3.4: the adaptive repeat-warp shed (EvaluateRepeatWarpShed,
        # its stall/cadence EMAs, and the _repeatWarpShed flag) is deleted — every
        # slot is a full warp when the stage allows, unconditionally (no blit
        # repeats to hand GPU headroom back to the game).
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        header = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertNotIn("EvaluateRepeatWarpShed", presenter)
        self.assertNotIn("ReprojRepeatWarp", presenter)
        self.assertNotIn("_repeatWarpShed", presenter + header)
        self.assertNotIn("_latestGameStallMs", presenter + header + reproj)
        self.assertNotIn("_stallEmaMs", header)
        self.assertNotIn("_cadenceEmaMs", header)
        # shouldWarp is unconditional per slot: stage >= 1 warps every output.
        self.assertIn("const bool shouldWarp = kAsyncSimpleStage >= 1 && packet.warpAllowed && !focusLost", presenter)
        # The 1 Hz log no longer carries shed/stallEma/hold/uiBorrow keys.
        self.assertNotIn("stallEma", reproj)
        self.assertNotIn("shed=", reproj)
        self.assertNotIn("hold=", reproj)
        self.assertNotIn("uiBorrow={}", reproj)

    def test_world_fence_marker_does_not_relock_the_isolation_mutex(self):
        # Regression: TryRedirect holds the isolation g_mutex and calls
        # MarkWorldSnapshotCl, which used to take the same non-recursive lock
        # again. That recursive lock is EDEADLK -> an uncaught
        # std::system_error on the game's render thread, crashing the process
        # at startup once the async presenter armed the world fence (BugSplat
        # 0xE06D7363, msvcp140 std::system_error vtable).
        root = Path(__file__).resolve().parents[2]
        src = (root / "OptiScaler/framegen/reproj/Kcd2HudIsolation.cpp").read_text(encoding="utf-8")
        redirect = src.split("bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,", 1)[1]
        # The marker call sits inside TryRedirect's g_mutex scope.
        self.assertIn("std::scoped_lock lock(g_mutex)", redirect)
        self.assertIn("MarkWorldSnapshotCl(commandList)", redirect)
        # The marker body itself must never take the lock again.
        marker = src.split("UINT64 MarkWorldSnapshotCl(ID3D12GraphicsCommandList* commandList)", 1)[1].split(
            "bool OnWorldSnapshotSubmitted", 1)[0]
        self.assertIn("recursively locked a non-recursive std::mutex", marker)
        self.assertNotIn("std::scoped_lock lock(g_mutex)", marker)
        self.assertNotIn("std::unique_lock lock(g_mutex)", marker)
        # TryRedirect is the only caller - no other site depends on the old
        # self-locking behavior.
        self.assertEqual(redirect.count("MarkWorldSnapshotCl(commandList)"), 1)

    def test_deferred_late_latch_uses_the_presenter_direct_queue(self):
        # P7: the single DIRECT presenter queue waits on one CPU-signaled latch
        # fence while the presenter writes the per-output upload constants. The
        # first implementation uses a fixed conservative lead; adaptive tuning
        # is intentionally deferred until fixed-lead cadence is measured.
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        dispatch = reproj.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork", 1)[0]
        self.assertIn("deferredLateLatch", dispatch)
        self.assertIn("_lateLatchFence", dispatch)
        self.assertIn("_presentQueue->Wait(_lateLatchFence", dispatch)
        self.assertNotIn("_gameCommandQueue->Wait(_lateLatchFence", reproj)
        self.assertIn("SignalLateLatch()", dispatch)
        self.assertIn("WriteConstants", dispatch)
        self.assertIn("lateLeadCfg", dispatch)
        self.assertIn("WaitForPresenterDeadline", dispatch)
        self.assertIn("scanoutDeadlineMs - lateLatchLeadMs", dispatch)
        self.assertNotIn("adaptiveLateSample", dispatch)
        # The 1 Hz log reports the effective fixed latch lead.
        log = reproj.split("void AReproj_Dx12::LogMetricsIfDue", 1)[1]
        self.assertNotIn("sampLead=", log)
        self.assertIn("_lastLateSampleLeadMs.load(std::memory_order_relaxed)", log)
        self.assertIn("latchLead={:.2f}ms", log)
        # The latch constants and state live on the class definition.
        header = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        self.assertIn("LATE_LATCH_DEFAULT_MS", header)
        self.assertIn("LATE_LATCH_MIN_MS", header)
        self.assertIn("_lateLatchFence", header)
        self.assertIn("_lastLateSampleLeadMs", header)
        self.assertIn("SAFE_RELEASE(_lateLatchFence)", reproj)
        self.assertIn("CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_lateLatchFence))", reproj)
        drain = reproj.split("bool AReproj_Dx12::DrainGpuWork", 1)[1].split(
            "void AReproj_Dx12::RecordRealFrame", 1
        )[0]
        release = reproj.split("void AReproj_Dx12::ReleaseObjects", 1)[1].split(
            "void AReproj_Dx12::CreateObjects", 1
        )[0]
        self.assertIn("if (!SignalLateLatch())", drain)
        self.assertLess(drain.index("SignalLateLatch()"), drain.index("for (int i = 0; i < BUFFER_COUNT"))
        self.assertLess(release.index("DrainGpuWork()"), release.index("SAFE_RELEASE(_lateLatchFence)"))
        # INI key stays readable; auto still resolves to the 0 default.
        config_h = (root / "OptiScaler/Config.h").read_text(encoding="utf-8")
        block = config_h.split("CustomOptional<float> ReprojLateSampleLead", 1)[1]
        self.assertIn("0.0f", block)
        config_cpp = (root / "OptiScaler/Config.cpp").read_text(encoding="utf-8")
        self.assertIn('readFloat("AsyncTimewarp", "LateSampleLead")', config_cpp)

    def test_presenter_uses_present_completion_clock(self):
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        self.assertIn("nextDeadlineMs = presentedAt + refreshPeriodMs", presenter)
        self.assertNotIn("SampleDisplayClock(", presenter)

    def test_presenter_uses_high_resolution_wait_for_final_timer_slice(self):
        # Wine/Proton can oversleep condition_variable::wait_for by most of a
        # 120 Hz slot. The presenter must keep the final sub-2ms wait on the
        # high-resolution QPC timer path.
        root = Path(__file__).resolve().parents[2]
        timing = (root / "OptiScaler/framegen/reproj/AReprojTiming.cpp").read_text(encoding="utf-8")
        wait = timing.split("bool AReproj_Dx12::WaitForPresenterDeadline", 1)[1]
        self.assertIn("FrameLimit::sleepForPrecisePacingMs(chunk)", wait)
        self.assertIn("YieldProcessor", wait)
        self.assertNotIn("_presentCv.wait_for", wait)
        # The precise pacing helper owns the spin tail. Reserving it again in
        # WaitForPresenterDeadline creates tiny Wine timer sleeps that overshoot.
        self.assertNotIn("remaining - spinWindowMs", wait)
        # Final spin window is 1.0ms on Proton for timer granularity, 0.2ms on Windows
        self.assertIn("spinWindowMs", wait)
        frame_limit = (root / "OptiScaler/misc/FrameLimit.cpp").read_text(encoding="utf-8")
        # async-simple: the source pacer trio is gone; sleepForPrecisePacingMs
        # is the only remaining precise sleeper and owns the Proton spin tail.
        self.assertNotIn("paceReprojectionSource", frame_limit)
        self.assertIn("void FrameLimit::sleepForPrecisePacingMs", frame_limit)
        self.assertIn("spinNs", frame_limit)
        self.assertIn("200'000", frame_limit)

    def test_completion_clock_cannot_run_away_from_present(self):
        # Wine advances frame statistics per composed output, so the presenter
        # must derive a fresh grid from Present completion rather than phase
        # correcting against those statistics.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1]
        self.assertIn("nextDeadlineMs = presentedAt + refreshPeriodMs", presenter)
        self.assertNotIn("totalEarlyCorrectionMs", presenter)

    def test_occlusion_pauses_gpu_work_and_resets_cadence(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1]
        self.assertIn("IsIconic(_hwnd)", presenter)
        self.assertIn("_swapChain->Present(0, DXGI_PRESENT_TEST)", presenter)
        self.assertIn("resetPresentationClock()", presenter)
        self.assertIn("FrameLimit::sleepForMs(50.0)", presenter)

    def test_presenter_watchdog_downgrades_on_jammed_presents(self):
        # Present(1) is an unbounded blocking call; sustained jams or one
        # multi-second wedge must fail the worker so the game thread's Failed
        # handling downgrades to the synchronous presenter instead of freezing.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1]
        self.assertIn("WATCHDOG_CONSECUTIVE_JAMS", presenter)
        self.assertIn("WATCHDOG_WEDGE_MS", presenter)
        self.assertGreater(presenter.count("_presenterState.store(PresenterState::Failed)"), 1)

    def test_runtime_and_precompiled_shader_sources_match(self):
        root = Path(__file__).resolve().parents[2]
        common = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        pairs = {"RPD_ShaderCode": root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl"}
        for symbol, source_path in pairs.items():
            marker = f'{symbol} = R"(\n'
            embedded = common.split(marker, 1)[1].split('\n)";', 1)[0]
            self.assertEqual(embedded.strip(), source_path.read_text(encoding="utf-8").strip())

    def test_reproj_never_paces_the_game_thread(self):
        # async-simple P1: every source-pacing call site is gone from the reproj
        # path. The game thread publishes anchors and returns without OptiScaler
        # ever sleeping or throttling it; FG_Hooks never applies the half-rate
        # rule to a reprojection output either.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        hooks = (root / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8")
        self.assertNotIn("paceReprojectionSource", source)
        self.assertNotIn("paceReprojectionSource", hooks)
        self.assertNotIn("sleepForReprojectionSourceMs", source)
        publish = source.split("if (captured && submitted && advanced)", 1)[1].split(
            "// Hard publication failures", 1)[0]
        # The published frame still notifies the presenter before returning.
        self.assertIn("_presentCv.notify_one()", publish)
        self.assertIn("return true", publish)

    def test_shared_frame_limiter_bypasses_reprojection(self):
        root = Path(__file__).resolve().parents[2]
        frame_limit = (root / "OptiScaler/misc/FrameLimit.cpp").read_text(encoding="utf-8")
        sleep = frame_limit.split("void FrameLimit::sleep(bool fgActive)", 1)[1].split(
            "void FrameLimit::sleepForMs", 1
        )[0]
        self.assertIn("IsReprojectionOutput(State::Instance().activeFgOutput)", sleep)
        self.assertIn("return;", sleep)

    def test_every_source_frame_is_captured_without_pacing(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        # Every virtualized present publishes an anchor (no sampling skip) and
        # none of them sleep for a source cap afterwards.
        self.assertIn("constexpr bool captureThisPresent = true", source)
        self.assertNotIn("FrameLimit::paceReprojectionSource", source)

    def test_hud_isolation_split_capture_rides_the_single_inline_submit(self):
        # HUD-fix rollover: when Kcd2HudIsolation redirected the HUD into an
        # isolated UI texture this frame, CaptureFramePacket copies the HUD-less
        # world snapshot AND the UI in the same inline submit on the game DIRECT
        # queue (one list, one fence — the UI is as fresh as the color, so no
        # borrow). Falls back to the composed frame otherwise. No
        # AllowComposedWarp gate on this branch.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertNotIn("allowComposed", capture)
        self.assertIn("Kcd2HudIsolation::GetHudlessColor(gameBackBuffer", capture)
        self.assertIn("Kcd2HudIsolation::GetUIColor(gameBackBuffer", capture)
        self.assertIn("packet.hasUi = true", capture)
        self.assertIn("CopyPacketResource(cmdList, ui, kcd2UiState", capture)
        self.assertNotIn("GetResource(FG_ResourceType::HudlessColor", capture)
        self.assertIn("packet.warpAllowed = warpAllowed && packet.hasCamera;", capture)
        self.assertNotIn("CopyPacketResource(cmdList, velocity", capture)
        self.assertIn("packet.constants.mode = 2", capture)
        # The UI alpha mode is baked into the warp constants (premultiplied by
        # default) exactly like the parent branch.
        self.assertIn("FGUIPremultipliedAlpha", capture)
        self.assertIn("hudlessSource", capture)

    def test_async_warp_dispatches_on_the_presenter_direct_queue(self):
        # async-simple P7: DispatchPacketWarp records the warp + copy-to-
        # backbuffer on the presenter's SC (DIRECT) command list and retires on
        # _scFence. The one deferred latch fence only gates this same queue;
        # there is still no COMPUTE queue or second presentation path.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        shader = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        dispatch = source.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork", 1)[0]
        # The warp is a single SC-list dispatch on _presentQueue; the isolated
        # UI (when captured) is composited unwarped by the same dispatch, and a
        # composed capture dispatches ui == nullptr (RPD samples color twice).
        self.assertIn("GetSCCommandList(outputIndex)", dispatch)
        self.assertIn("SubmitSCCommandList(outputIndex)", dispatch)
        self.assertIn("_scAllocatorFenceValues[outputIndex] = ++_scFenceValue;", dispatch)
        self.assertIn("packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];", dispatch)
        self.assertIn("outputIndex, deferredLateLatch, packet.hasUi ? packet.ui : nullptr,", dispatch)
        self.assertIn("packet.hasUi ? packet.uiState : D3D12_RESOURCE_STATE_COMMON", dispatch)
        # No COMPUTE queue or RUI composite on the warp path.
        self.assertNotIn("uiPacket", dispatch)
        self.assertNotIn("composeUi", dispatch)
        self.assertNotIn("_computeQueue", dispatch)
        self.assertNotIn("_computeFence", dispatch)
        self.assertIn("_lateLatchFence", dispatch)
        self.assertIn("deferredLateLatch", dispatch)
        self.assertNotIn("useCompute", dispatch)
        self.assertNotIn("WaitForComputeAllocator", dispatch)
        self.assertNotIn("SubmitComputeCommandList", dispatch)
        self.assertIn("WriteConstants", dispatch)
        self.assertNotIn("_renderUI->Dispatch", dispatch)
        # Constants are written once as a baseline, then replaced while the
        # queue is parked behind the latch fence.
        self.assertIn("PrepareRotationConstants(constants, false);", dispatch)
        self.assertIn("_warp->WriteConstants(outputIndex, constants)", dispatch)
        self.assertIn("PrepareRotationConstants(lateConstants, false);", dispatch)
        # The machinery is deleted from the class definition as well.
        header = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        self.assertNotIn("_computeQueue", header)
        self.assertNotIn("_computeFence", header)
        self.assertIn("_lateLatchFence", header)
        self.assertNotIn("GetComputeCommandList", header)
        self.assertNotIn("SubmitComputeCommandList", header)
        self.assertNotIn("WaitForComputeAllocator", header)
        # RPD carries the isolated-UI texture and composites it unwarped when
        # HudlessSource != 0 (the HUD-fix rollover path).
        self.assertIn("Texture2D<float4> UI : register(t1)", shader)
        self.assertIn("UI.Load(int3(dtid.xy, 0))", shader)
        self.assertIn("if (HudlessSource != 0)", shader)

    def test_kcd2_late_input_uses_camera_callback_baseline(self):
        root = Path(__file__).resolve().parents[2]
        camera = (root / "OptiScaler/framegen/reproj/Kcd2Camera.cpp").read_text(encoding="utf-8")
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        late_input = reproj.split("bool AReproj_Dx12::ApplyLateInput", 1)[1].split(
            "void AReproj_Dx12::UpdateMouseSensitivity", 1)[0]
        self.assertIn("pose.mouseTotalX = mouse.TotalX", camera)
        self.assertIn("current.TotalX - latestCamera.mouseTotalX", late_input)
        self.assertNotIn("GetRawMouseMotionAt(latestCamera.timestampMs)", late_input)

    def test_kcd2_isolation_feeds_packet_capture_on_the_single_submit(self):
        # HUD-fix rollover: CaptureFramePacket consumes the Kcd2HudIsolation
        # world/UI textures when they are valid for this backbuffer. The packet
        # copies own their resources (color+UI in one submit on the game queue),
        # so the isolation generation needs no MarkFrameCaptured fence here —
        # same-queue ordering already protects the copies.
        root = Path(__file__).resolve().parents[2]
        isolation = (root / "OptiScaler/framegen/reproj/Kcd2HudIsolation.cpp").read_text(encoding="utf-8")
        capture = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::CaptureFramePacket", 1)[1].split("bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertIn("completed < slot.captureFenceValue", isolation)
        self.assertIn("Kcd2HudIsolation::GetHudlessColor", capture)
        self.assertIn("Kcd2HudIsolation::GetUIColor", capture)
        self.assertNotIn("MarkFrameCaptured", capture)
        self.assertIn("GetHudlessColor", capture)

    def test_goal_telemetry_survives_without_present_path_info_logs(self):
        # async-simple: the once-per-second goal line is the only INFO logger
        # near the frame path. The per-frame Present() body (sync fallback
        # deleted in P3.5 — non-virtualized presents pass straight through) must
        # never emit LOG_INFO.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("late={}/{} maxDeg={:.2f}", source)
        self.assertNotIn("hud=", source)
        self.assertNotIn("sampLead=", source)
        self.assertNotIn("ReprojPipe", source)
        self.assertNotIn("PresentVirtualFrameSync", source)
        self.assertNotIn("CopyLastFrame", source)
        self.assertNotIn("DispatchWarp", source)
        present = source.split("bool AReproj_Dx12::Present()", 1)[1].split("void AReproj_Dx12::Activate()", 1)[0]
        self.assertNotIn("LOG_INFO(", present)
        self.assertNotIn('LOG_INFO("Reproj diag:', present)
        hooks = (root / "OptiScaler/hooks/FG_Hooks.cpp").read_text(encoding="utf-8")
        self.assertNotIn('LOG_INFO("Reproj diag: FGPresent pre-activation', hooks)

    def test_kcd2_rotation_path_is_rigid_and_bounded(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        camera = (root / "OptiScaler/framegen/reproj/Kcd2Camera.cpp").read_text(encoding="utf-8")
        self.assertIn("ExtrapolateCameraRotation", reproj)
        self.assertIn("angle * constants.timeStep", reproj)
        self.assertIn("Kcd2Camera::IsAvailable() ?", reproj)
        self.assertIn("std::clamp(unclampedStep, 0.0f, maxTimeStep)", presenter)
        self.assertIn("directionReversed ? dR", camera)

    def test_completion_clock_is_the_safe_default_and_lead_is_slot_bounded(self):
        root = Path(__file__).resolve().parents[2]
        config = (root / "OptiScaler/Config.h").read_text(encoding="utf-8")
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        self.assertNotIn("ReprojPresentCompletionClock", config)
        self.assertIn("nextDeadlineMs = presentedAt + refreshPeriodMs", presenter)
        self.assertNotIn("SampleDisplayClock(", presenter)
        self.assertIn("maxUsableLeadMs", presenter)
        # Fixed P7 latch lead (ReprojLateSampleLead override), with the
        # submission wake held at least one millisecond earlier.
        self.assertIn("std::min(std::max(latchLeadMs + 1.0, 4.0), maxUsableLeadMs)", presenter)
        self.assertIn("LATE_LATCH_DEFAULT_MS", presenter)
        self.assertIn("_lastLateSampleLeadMs.store(latchLeadMs", presenter)

    def test_experimental_control_surface_is_removed(self):
        root = Path(__file__).resolve().parents[2]
        config = (root / "OptiScaler/Config.h").read_text(encoding="utf-8")
        for removed in ("ReprojMode", "ReprojUseDepth", "ReprojRotationOnly", "ReprojLateLatch",
                        "ReprojNonBlockingAnchorSampling", "ReprojTelemetry"):
            self.assertNotIn(removed, config)
        # async-simple: the source limit still parses but defaults to 0 (never pace).
        self.assertIn("ReprojSourceFramerateLimit { 0.0f }", config)

    def test_source_pacer_is_fully_removed_from_frame_limit(self):
        # async-simple P1 deleted the whole source-cap machinery:
        # paceReprojectionSource, its sleep helper, and the stats getter no
        # longer exist in FrameLimit. The presenter sleepers survive.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/misc/FrameLimit.cpp").read_text(encoding="utf-8")
        self.assertNotIn("paceReprojectionSource", source)
        self.assertNotIn("sleepForReprojectionSourceMs", source)
        self.assertNotIn("SourcePacingStats", source)
        self.assertNotIn("g_reprojectionSourceCapHz", source)
        self.assertIn("void FrameLimit::sleepForPrecisePacingMs", source)

    def test_kcd2_input_yaw_uses_world_up_before_pitch(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        composition = reproj.split("if (inputLatched)", 1)[1].split(
            "if (inputLatched && constants.mode == 1)", 1)[0]
        self.assertIn("ReprojVec3 { 0.0f, 0.0f, 1.0f }", composition)
        self.assertIn("RotateReprojVec3(right, yawAxis, -lateYaw)", composition)
        self.assertIn("RotateReprojVec3(yawForward, yawRight, latePitch)", composition)
        self.assertLess(composition.index("yawForward"), composition.index("latePitch"))

    def test_packet_exhaustion_does_not_stall_game_thread(self):
        # Any game-thread wait inside Present() eats the source budget
        # directly. Packet exhaustion must drop the anchor (the presenter
        # keeps re-warping its active anchor) and count it, never wait on
        # the presenter condition variable.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        present = source.split("bool AReproj_Dx12::Present()", 1)[1].split(
            "void AReproj_Dx12::Activate()", 1)[0]
        exhaust = present.split("auto packetIndex = AcquirePacket();", 1)[1].split(
            "auto& packet = _packets[packetIndex];", 1)[0]
        self.assertNotIn("wait_for", exhaust)
        self.assertNotIn("WaitForSingleObject", exhaust)
        # One immediate re-scan catches a concurrently retired packet.
        self.assertGreaterEqual(exhaust.count("AcquirePacket()"), 1)
        self.assertIn("++_metricsSkippedAnchorSamples", exhaust)

    def test_capture_is_one_inline_copy_on_the_game_queue(self):
        # async-simple P2: the capture worker and COPY queue are gone. Capture
        # records one composed CopyResource on the packet's UI command list and
        # submits it on the game DIRECT queue; completion tracks on _uiFence.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        self.assertIn("CopyPacketResource(cmdList, color, colorState, &packet.color", capture)
        self.assertIn("SubmitUICommandList((UINT) packetIndex)", capture)
        self.assertIn("packet.completionFence = _uiFence", capture)
        self.assertIn("packet.completionFenceValue = packet.captureFenceValue", capture)
        # No capture queue / worker / fence remains anywhere in the subsystem.
        self.assertNotIn("_captureFence", capture)
        self.assertNotIn("_captureQueue", source)
        self.assertNotIn("SubmitCaptureCommandList", source)
        self.assertNotIn("ProcessCapturePacket", source)
        retire = source.split("void AReproj_Dx12::RetirePackets()", 1)[1].split(
            "uint32_t AReproj_Dx12::PacketQueueDepth()", 1)[0]
        self.assertIn("packet.completionFence", retire)
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        # P3.4: the presenter gates selection on the packet's own captureFenceValue
        # against _uiFence directly — the completionFence alias is gone from the
        # presenter; it survives only in capture/retire bookkeeping.
        self.assertNotIn("completionFence", presenter)
        self.assertNotIn("_captureFence", presenter)

    def test_virtual_buffer_handoff_is_always_fence_free(self):
        # async-simple: the capture copy is inline on the game DIRECT queue, so
        # it is GPU-ordered before any later render into the same virtual
        # buffer. The handoff therefore never carries a fence — publish, skip,
        # and SkipAnchorPublication all pass nullptr/0 to the ring.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        present = source.split("bool AReproj_Dx12::Present()", 1)[1].split(
            "void AReproj_Dx12::Activate", 1)[0]
        skip = source.split("void AReproj_Dx12::SkipAnchorPublication", 1)[1].split(
            "bool AReproj_Dx12::CaptureFramePacket", 1)[0]
        self.assertNotIn("handoffFence", capture)
        self.assertNotIn("handoffFence", present)
        self.assertNotIn("nonBlockingHandoff", source)
        self.assertGreaterEqual(present.count("SubmitReprojectionBuffer(virtualBufferIndex, nullptr, 0)"), 1)
        self.assertIn("SubmitReprojectionBuffer(virtualBufferIndex, nullptr, 0)", skip)

    def test_capture_warp_gate_is_the_single_ui_fence_value(self):
        # async-simple: capture is one inline submit, so the warp gate, the
        # readiness gate, and the recycling gate are all the single _uiFence
        # value recorded on the packet. No color/UI split, no world fence, no
        # worker phases.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        # Color and UI are one submit, so the single _uiFence value gates the
        # whole anchor — hasUi just marks the split-capture variant.
        self.assertIn("packet.hasUi = false", capture)
        self.assertIn("bool hasUi = false", (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8"))
        self.assertIn("packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex]", capture)
        self.assertNotIn("colorFenceValue", capture)
        self.assertNotIn("worldFenceValue", capture)
        self.assertNotIn("ProcessCapturePacket", source)
        # The presenter still gates anchor selection on that capture value.
        self.assertIn("captureFenceValue == 0", presenter)
        self.assertIn("captureComplete", presenter)

    def test_ui_borrow_hold_is_removed_with_the_single_submit_capture(self):
        # HUD-fix rollover: the isolated UI is back, but color and UI are one
        # inline submit, so the UI is always as fresh as the color — the UI
        # borrow hold that shared _heldPacketIndex stays deleted. The previous
        # anchor is retired immediately on a real switch.
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        header = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        dispatch = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split("bool AReproj_Dx12::DrainGpuWork", 1)[0]
        shader = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        common = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        self.assertNotIn("_heldPacketIndex", presenter + header)
        self.assertNotIn("_metricsUiBorrows", presenter + header)
        self.assertNotIn("uiPacketIndex = _heldPacketIndex", presenter)
        # On a real switch the previous anchor is retired immediately.
        self.assertIn("_packets[activePacketIndex].state.store(PacketState::Retired)", presenter)
        # Swap blend stays fully absent: no prev-color dispatch wiring, no
        # blend factor, no PrevColor SRV, no blend in either shader copy.
        self.assertNotIn("prevPacketIndex", presenter)
        self.assertNotIn("prevPacketIndex", dispatch)
        self.assertNotIn("kSwapBlendFactor", dispatch)
        self.assertNotIn("blendSwap", dispatch)
        self.assertNotIn("PrevColor", dispatch)
        self.assertNotIn("PrevColor", shader)
        self.assertNotIn("prevWarped", shader)
        self.assertNotIn("PrevColor", common)
        self.assertNotIn("prevWarped", common)

    def test_midframe_world_fence_is_safe_without_perpass_submission(self):
        # The world-completion signal rides on the CL-submit hook: on per-pass
        # renderers it fires mid-frame; on single-CL-per-frame renderers the
        # signal lands at present-time submission, which is exactly today's
        # ordering. Markers are cleared per frame so a never-submitted CL
        # cannot leave a dangling capture wait, and the signal rides the
        # submitting queue so GPU ordering is guaranteed.
        root = Path(__file__).resolve().parents[2]
        isolation = (root / "OptiScaler/framegen/reproj/Kcd2HudIsolation.cpp").read_text(encoding="utf-8")
        hook = (root / "OptiScaler/resource_tracking/ResTrack_dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("MarkWorldSnapshotCl(commandList)", isolation)
        self.assertIn("OnWorldSnapshotSubmitted", hook)
        self.assertIn("g_pendingWorldSignalCount = 0", isolation)
        self.assertIn("queue->Signal(g_worldFence, g_pendingWorldSignals[j].value)", isolation)
        # async-simple P2: AReproj no longer wires a world fence — the
        # isolation machinery stays compiled but the presenter never feeds it.
        self.assertNotIn("SetWorldSignalContext",
                         (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8"))

    def test_phase_fit_selects_input_window_that_explains_camera_motion(self):
        # Model the C++ through-origin least-squares score. Camera response is
        # delayed by two 4 ms candidates; the aligned candidate must have the
        # smallest normalized residual rather than merely a plausible gain.
        inputs = [0, 3, 8, 12, 5, -4, -9, -2, 7, 11, 4, -6] * 4
        camera = [0.0024 * (inputs[i - 2] if i >= 2 else 0) for i in range(len(inputs))]

        def score(offset):
            pairs = [(inputs[i - offset] if i >= offset else 0, camera[i]) for i in range(len(camera))]
            input2 = sum(value * value for value, _ in pairs)
            input_camera = sum(value * motion for value, motion in pairs)
            camera2 = sum(motion * motion for _, motion in pairs)
            gain = input_camera / input2
            residual = max(
                0.0,
                camera2 - 2.0 * gain * input_camera + gain * gain * input2,
            )
            return math.sqrt(residual / camera2)

        self.assertEqual(min(range(6), key=score), 2)
        self.assertLess(score(2), 1.0e-6)


if __name__ == "__main__":
    unittest.main()
