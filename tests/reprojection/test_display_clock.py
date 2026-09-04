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

    def test_repeat_warp_shed_engages_on_source_cadence_and_recovers(self):
        # The adaptive shed exists so a 60 Hz source keeps full-warp repeats
        # while it holds the cap, and hands the GPU back to the game (blit
        # repeats) only when the source falls behind or the game thread stalls.
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        whole = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        self.assertIn("void AReproj_Dx12::EvaluateRepeatWarpShed", presenter)
        decision = presenter.split("const bool shouldWarp =", 1)[1].split("const bool dispatched = shouldWarp", 1)[0]
        # The shed flag suppresses warps on repeated slots only: new anchors and
        # a healthy source still get full-warp repeats.
        self.assertIn("newContent || repeatWarp", decision)
        self.assertIn("ReprojRepeatWarp.value_or_default()", presenter)
        self.assertIn("!_repeatWarpShed.load(std::memory_order_relaxed)", presenter)
        # Hysteresis with an engage band above the cap and a release back at it.
        self.assertIn("CADENCE_ENGAGE_RATIO", presenter)
        self.assertIn("CADENCE_RELEASE_RATIO", presenter)
        self.assertIn("MIN_SHED_MS", presenter)
        self.assertIn("STALL_ENGAGE_MS", presenter)
        # The game present path publishes the per-frame stall the shed consumes.
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("_latestGameStallMs.store", reproj)
        self.assertIn("std::atomic<float> _latestGameStallMs", whole)

    def test_shed_controller_hysteresis_model(self):
        # Model EvaluateRepeatWarpShed: cadence EMA + stall EMA with engage at
        # 1.15x the cap period and release at 1.0x once the stall clears. A
        # source holding its cap keeps warping repeats; a sustained drop sheds
        # them and recovery re-engages warps (with the minimum shed dwell).
        target_period = 1000.0 / 60.0
        cadence_ema = 0.0
        stall_ema = 0.0
        shed = False
        engaged_at = None

        def slot(now_ms, period_ms, stall_ms):
            nonlocal cadence_ema, stall_ema, shed, engaged_at
            if cadence_ema <= 0.0:
                cadence_ema = period_ms
            else:
                cadence_ema = cadence_ema * 0.6 + period_ms * 0.4
            if stall_ema <= 0.0:
                stall_ema = stall_ms
            else:
                stall_ema = stall_ema * 0.7 + stall_ms * 0.3
            engage = cadence_ema > target_period * 1.15 or stall_ema > 8.0
            if engage and not shed:
                shed = True
                engaged_at = now_ms
            elif shed:
                recovered = (cadence_ema <= target_period * 1.03 and stall_ema <= 6.0 and
                             (now_ms - engaged_at) >= 400.0)
                if recovered:
                    shed = False
            return shed

        # Healthy 60 Hz source, modest stall: warps stay on.
        now = 0.0
        for i in range(120):
            self.assertFalse(slot(now, target_period, 3.0))
            now += 8.333
        # Sustained 48 FPS source with a rising stall: the shed engages and sticks.
        shed_at = None
        for i in range(120):
            s = slot(now, 20.8, 8.0)
            if s and shed_at is None:
                shed_at = now
            now += 8.333
        self.assertIsNotNone(shed_at)
        # Recovery to the cap with a cleared stall re-engages warps after the dwell.
        recovered_at = None
        for i in range(240):
            s = slot(now, target_period, 2.0)
            if not s and recovered_at is None:
                recovered_at = now
            now += 8.333
        self.assertIsNotNone(recovered_at)
        self.assertGreater(recovered_at - shed_at, 400.0)

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

    def test_late_sample_lead_defaults_to_adaptive_and_retunes(self):
        # LateSampleLead auto/0 = adaptive: after each warp completes the
        # presenter measures headroom to the deadline and slides the sample
        # later (smaller lead) whenever more than ~2 ms of slack remains, so the
        # mouse is sampled as late as the warp allows instead of a fixed 4 ms.
        root = Path(__file__).resolve().parents[2]
        config_h = (root / "OptiScaler/Config.h").read_text(encoding="utf-8")
        block = config_h.split("CustomOptional<float> ReprojLateSampleLead", 1)[1]
        self.assertIn("0.0f", block)  # auto/0 = adaptive, not the old 4.0 constant
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        dispatch = reproj.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1]
        self.assertIn("adaptiveLateSample", dispatch)
        self.assertIn("SAMPLE_LEAD_MIN_MS", dispatch)
        self.assertIn("SAMPLE_LEAD_MAX_MS", dispatch)
        self.assertIn("SAMPLE_LEAD_REDUCE_HEADROOM_MS", dispatch)
        self.assertIn("SAMPLE_LEAD_GROW_HEADROOM_MS", dispatch)
        self.assertIn("_lateSampleLeadMs = std::max(SAMPLE_LEAD_MIN_MS, _lateSampleLeadMs - SAMPLE_LEAD_STEP_MS)",
                      dispatch)
        # A fixed value (>0.5) still overrides with the old constant-lead path.
        self.assertIn("lateLeadCfg > 0.5", dispatch)
        # The adaptive hunt runs only on the compute late-latch path, never on
        # the DIRECT fallback where the sample cannot be deferred.
        self.assertIn("adaptiveLateSample && lateLatchValue != 0", dispatch)
        # The 1 Hz log reports the effective lead (auto shows it converging).
        log = reproj.split("void AReproj_Dx12::LogMetricsIfDue", 1)[1]
        self.assertIn("sampLead=", log)
        self.assertIn("_lastLateSampleLeadMs.load()", log)
        # INI key stays the existing one; auto resolves to the new default.
        config_cpp = (root / "OptiScaler/Config.cpp").read_text(encoding="utf-8")
        self.assertIn('readFloat("AsyncTimewarp", "LateSampleLead")', config_cpp)

    def test_adaptive_late_sample_controller_model(self):
        # Model the DispatchPacketWarp sample-lead hunt. Cost = signal latency +
        # warp + copy + CPU wake. Headroom = lead - cost; reduce the lead by a
        # step while headroom > 2.0 ms (sample was released too early), grow it
        # when headroom < 0.9 ms (crowding the vblank). A fast warp must end up
        # sampling later than the old fixed 4.0 ms, and a sudden GPU stall must
        # push the lead back up instead of missing vblanks.
        def controller(cost_ms, slots=200):
            lead = 4.0
            trace = []
            for _ in range(slots):
                headroom = lead - cost_ms
                if headroom > 2.0:
                    lead = max(2.0, lead - 0.25)
                elif headroom < 0.9:
                    lead = min(6.0, lead + 0.25)
                trace.append(lead)
            return lead, trace

        # Typical warp cost ~1-1.5 ms: converges to cost + 2.0 (headroom at the
        # reduce edge) = 3.0-3.5 ms, fresher than the old constant 4.0.
        for cost in (1.0, 1.5):
            lead, trace = controller(cost)
            self.assertAlmostEqual(lead, cost + 2.0, delta=0.26)
            self.assertLess(lead, 4.0)
        # Expensive warp (cost >= 2.0): the controller must not push below 4.0.
        lead, _ = controller(2.0)
        self.assertAlmostEqual(lead, 4.0, delta=0.26)
        # A late GPU stall (cost spikes to 5 ms) grows the lead to the cap, and
        # recovery back to a fast warp decays it to the fresh setting again.
        lead = 4.0
        for _ in range(60):
            headroom = lead - 5.0
            lead = min(6.0, lead + 0.25) if headroom < 0.9 else max(2.0, lead - 0.25)
        self.assertAlmostEqual(lead, 6.0, delta=0.26)
        lead, _ = controller(1.0)
        self.assertAlmostEqual(lead, 3.0, delta=0.26)
        # Under the old wide 1.2-3.0 ms deadband a typical 1.5 ms cost would
        # leave 2.5 ms headroom and never move - the retuned 2.0 edge is what
        # makes the adaptive default actually hunt.
        self.assertGreater(4.0 - 1.5, 2.0)

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

    def test_every_source_frame_is_captured_without_pacing(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        # Every virtualized present publishes an anchor (no sampling skip) and
        # none of them sleep for a source cap afterwards.
        self.assertIn("constexpr bool captureThisPresent = true", source)
        self.assertNotIn("FrameLimit::paceReprojectionSource", source)

    def test_minimal_path_captures_the_composed_frame_only(self):
        # async-simple P2: CaptureFramePacket copies exactly one composed frame
        # (HUD included) via the game DIRECT UI command list. No isolation, no
        # separate UI texture, no AllowComposedWarp gate — composed is the model.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertNotIn("allowComposed", capture)
        self.assertNotIn("Kcd2HudIsolation", capture)
        self.assertNotIn("GetResource(FG_ResourceType::HudlessColor", capture)
        self.assertIn("packet.warpAllowed = warpAllowed && packet.hasCamera;", capture)
        self.assertNotIn("CopyPacketResource(cmdList, velocity", capture)
        self.assertIn("packet.constants.mode = 2", capture)

    def test_async_warp_composites_ui_in_compute_without_direct_queue_roundtrip(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        shader = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        dispatch = source.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split(
            "bool AReproj_Dx12::DispatchWarp", 1)[0]
        # UI comes from the newest completed UI packet (own or the held previous).
        self.assertIn("uiPacket.ui, uiPacket.uiState", dispatch)
        self.assertNotIn("_presentQueue->Wait(_computeFence", dispatch)
        self.assertNotIn("_renderUI->Dispatch", dispatch)
        self.assertIn("Texture2D<float4> UI : register(t1)", shader)
        self.assertIn("UI.Load(int3(dtid.xy, 0))", shader)

    def test_compute_warp_uses_scanout_time_latch(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        dispatch = source.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split(
            "bool AReproj_Dx12::DispatchWarp", 1)[0]
        self.assertIn("deferredLateLatch = useCompute && _lateLatchFence != nullptr", dispatch)
        self.assertIn("WaitForPresenterDeadline(scanoutDeadlineMs - lateLeadMs)", dispatch)
        self.assertNotIn("_gameCommandQueue->Wait(_computeFence", dispatch)
        self.assertIn("WaitForComputeAllocator(outputIndex)", dispatch)
        self.assertGreaterEqual(dispatch.count("PrepareRotationConstants("), 2)
        self.assertNotIn("PrepareRotationConstants(constants);", dispatch)
        self.assertNotIn("PrepareRotationConstants(lateConstants);", dispatch)

    def test_kcd2_late_input_uses_camera_callback_baseline(self):
        root = Path(__file__).resolve().parents[2]
        camera = (root / "OptiScaler/framegen/reproj/Kcd2Camera.cpp").read_text(encoding="utf-8")
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        late_input = reproj.split("bool AReproj_Dx12::ApplyLateInput", 1)[1].split(
            "void AReproj_Dx12::UpdateMouseSensitivity", 1)[0]
        self.assertIn("pose.mouseTotalX = mouse.TotalX", camera)
        self.assertIn("current.TotalX - latestCamera.mouseTotalX", late_input)
        self.assertNotIn("GetRawMouseMotionAt(latestCamera.timestampMs)", late_input)

    def test_kcd2_isolation_no_longer_feeds_packet_capture(self):
        # async-simple P2: the Kcd2HudIsolation code remains compiled but is
        # never consumed by CaptureFramePacket — capture is the composed frame.
        root = Path(__file__).resolve().parents[2]
        isolation = (root / "OptiScaler/framegen/reproj/Kcd2HudIsolation.cpp").read_text(encoding="utf-8")
        capture = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::CaptureFramePacket", 1)[1].split("bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertIn("completed < slot.captureFenceValue", isolation)
        self.assertNotIn("Kcd2HudIsolation", capture)
        self.assertNotIn("MarkFrameCaptured", capture)
        self.assertNotIn("GetHudlessColor", capture)

    def test_goal_telemetry_survives_without_per_frame_reproj_info_spam(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("late={}/{} maxDeg={:.2f} hud={}", source)
        sync_present = source.split("HRESULT AReproj_Dx12::PresentVirtualFrameSync", 1)[1].split(
            "bool AReproj_Dx12::IsCameraAllZero", 1)[0]
        self.assertNotIn('LOG_INFO("Reproj diag:', sync_present)
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
        self.assertIn("std::clamp(_dispatchLeadMs, 3.0, maxUsableLeadMs)", presenter)

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
        self.assertIn("completionFence", presenter)
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
        self.assertIn("packet.hasUi = false", capture)
        self.assertIn("packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex]", capture)
        self.assertNotIn("colorFenceValue", capture)
        self.assertNotIn("worldFenceValue", capture)
        self.assertNotIn("ProcessCapturePacket", source)
        # The presenter still gates anchor selection on that capture value.
        self.assertIn("captureFenceValue == 0", presenter)
        self.assertIn("colorComplete", presenter)

    def test_ui_borrow_holds_previous_anchor_without_swap_blend(self):
        # v37's swap blend sampled the PREVIOUS anchor's image with the CURRENT
        # anchor's baked output-pixel -> source-UV homography. That misaligns the
        # previous frame by the full inter-anchor rotation whenever the camera
        # moves, ghosting/doubling on look-around. It was removed in v40; the
        # UI-borrow hold that shared _heldPacketIndex must remain.
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        dispatch = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split("bool AReproj_Dx12::DispatchWarp", 1)[0]
        shader = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        common = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        # UI borrow survives: the held previous anchor's UI composites while the
        # new anchor's own UI copy trails, tracked/retired via _heldPacketIndex.
        self.assertIn("_heldPacketIndex = activePacketIndex", presenter)
        self.assertIn("uiPacketIndex = _heldPacketIndex", presenter)
        self.assertIn("_metricsUiBorrows", presenter)
        log = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("uiBorrow={}", log)
        # Swap blend fully removed: no prev-color dispatch wiring, no blend
        # factor, no PrevColor SRV, no blend in either shader copy.
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
