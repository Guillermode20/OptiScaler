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


class SourcePacer:
    """Model the game-thread absolute-deadline cap without platform sleeping."""

    def __init__(self):
        self.deadline = None
        self.cap = 0.0

    def publish(self, now_ms, cap_hz, active=True):
        cap_hz = cap_hz if active and cap_hz > 0 else 0.0
        if not cap_hz:
            self.deadline = None
            self.cap = 0.0
            return None
        period = 1000.0 / cap_hz
        if self.deadline is None or abs(self.cap - cap_hz) > 0.001 or now_ms >= self.deadline:
            self.deadline = now_ms + period
            self.cap = cap_hz
            return None
        deadline = self.deadline
        self.deadline += period
        return deadline


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
        presenter_sleep = frame_limit.split("void FrameLimit::sleepForPrecisePacingMs", 1)[1].split(
            "void FrameLimit::paceReprojectionSource", 1)[0]
        self.assertIn("200'000", presenter_sleep)
        self.assertIn("spinNs", presenter_sleep)

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

    def test_source_cap_uses_an_absolute_deadline_grid(self):
        pacer = SourcePacer()
        self.assertIsNone(pacer.publish(0.0, 60.0))
        self.assertEqual(pacer.publish(5.0, 60.0), 1000 / 60)
        # The next deadline stays on the original grid, not 5 ms after completion.
        self.assertEqual(pacer.publish(20.0, 60.0), 2000 / 60)

    def test_source_cap_resets_on_late_frame_or_setting_change(self):
        pacer = SourcePacer()
        pacer.publish(0.0, 60.0)
        self.assertIsNone(pacer.publish(20.0, 60.0))  # missed 16.67 ms deadline
        self.assertIsNone(pacer.publish(21.0, 50.0))
        self.assertEqual(pacer.publish(25.0, 50.0), 41.0)

    def test_source_cap_disable_cannot_create_a_catchup_burst(self):
        pacer = SourcePacer()
        pacer.publish(0.0, 60.0)
        self.assertIsNone(pacer.publish(1000.0, 0.0, active=False))
        self.assertIsNone(pacer.publish(1001.0, 60.0))

    def test_runtime_source_pacer_is_async_virtualized_only(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        publish = source.split("if (captured && submitted && advanced)", 1)[1].split(
            "// Hard publication failures", 1)[0]
        self.assertIn("FrameLimit::paceReprojectionSource(true)", publish)
        self.assertLess(publish.index("FrameLimit::paceReprojectionSource(true)"), publish.index("return true"))
        self.assertIn("FrameLimit::paceReprojectionSource(false)", source)

    def test_every_paced_source_frame_is_an_anchor(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("constexpr bool captureThisPresent = true", source)
        self.assertIn("FrameLimit::paceReprojectionSource(true)", source)

    def test_minimal_path_requires_camera_and_separate_hud(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertIn("packet.warpAllowed = warpAllowed && packet.hasCamera && (packet.hasUi || allowComposed);", capture)
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

    def test_kcd2_isolation_generations_are_fenced_until_capture_completes(self):
        root = Path(__file__).resolve().parents[2]
        isolation = (root / "OptiScaler/framegen/reproj/Kcd2HudIsolation.cpp").read_text(encoding="utf-8")
        capture = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::CaptureFramePacket", 1)[1].split("bool AReproj_Dx12::DisplayPacket", 1)[0]
        self.assertIn("completed < slot.captureFenceValue", isolation)
        self.assertIn("return nullptr", isolation)
        self.assertIn("Kcd2HudIsolation::MarkFrameCaptured", capture)

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
        self.assertIn("ReprojSourceFramerateLimit { 60.0f }", config)

    def test_source_cap_does_not_burn_two_ms_of_cpu_every_frame(self):
        # KCD2 can sustain more than 60 FPS uncapped. A full 2 ms busy tail in
        # the cap itself takes roughly 12% of one core at 60 Hz and turns a
        # sustainable source into a sub-60 one under load.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/misc/FrameLimit.cpp").read_text(encoding="utf-8")
        pacer = source.split("void FrameLimit::paceReprojectionSource", 1)[1].split(
            "FrameLimit::SourcePacingStats", 1)[0]
        self.assertIn("sleepForReprojectionSourceMs", pacer)
        source_sleep = source.split("void FrameLimit::sleepForReprojectionSourceMs", 1)[1].split(
            "void FrameLimit::paceReprojectionSource", 1)[0]
        self.assertIn("SOURCE_SPIN_NS = 200'000", source_sleep)
        self.assertNotIn("isRunningOnLinux", source_sleep)
        self.assertNotIn("combined_sleep(static_cast<int64_t>(deadlineNs - nowNs))", pacer)

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
        # The SourceFramerateLimit pacer can only delay frames, never speed
        # them up: any game-thread wait inside Present() eats the source
        # budget directly. Packet exhaustion must drop the anchor (the
        # presenter keeps re-warping its active anchor) and count it,
        # never wait on the presenter condition variable.
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

    def test_capture_copies_can_bypass_game_queue(self):
        # Capture color/UI copies must be able to run on a dedicated queue so
        # they overlap rendering; the game queue then only publishes its frame
        # fence. Retirement/presenter/downgrade must follow the packet
        # completion fence rather than assuming the UI fence.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        self.assertIn("PollCaptureAllocator(packetIndex)", capture)
        self.assertIn("GetCaptureCommandList(packetIndex)", capture)
        self.assertIn("SubmitCaptureCommandList(packetIndex", capture)
        self.assertIn("_captureInputFence", capture)
        self.assertIn("packet.completionFence = _captureFence", capture)
        # DIRECT fallback path is verbatim when compute capture is unavailable.
        self.assertIn("packet.completionFence = _uiFence", capture)
        # The composed-HUD check: only warp with UI unless AllowComposedWarp is enabled.
        self.assertIn("packet.warpAllowed = warpAllowed && packet.hasCamera && (packet.hasUi || allowComposed);", capture)
        retire = source.split("void AReproj_Dx12::RetirePackets()", 1)[1].split(
            "uint32_t AReproj_Dx12::PacketQueueDepth()", 1)[0]
        self.assertIn("packet.completionFence", retire)
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        self.assertIn("completionFence", presenter)
        self.assertIn("_captureFence", presenter)

    def test_isolated_hud_releases_virtual_buffer_before_packet_copy_finishes(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        present = source.split("bool AReproj_Dx12::Present()", 1)[1].split(
            "void AReproj_Dx12::Activate", 1)[0]
        self.assertIn("packet.hasUi && nonBlockingHandoff", capture)
        self.assertIn("packet.handoffFence = nullptr", capture)
        self.assertIn("packet.handoffFence", present)
        self.assertIn("packet.handoffFenceValue", present)
        self.assertIn("SubmitReprojectionBuffer(virtualBufferIndex, handoffFence", present)

    def test_capture_warp_gate_is_color_copy_with_trailing_ui(self):
        # Latency pass: the warp gate is the color copy (colorFenceValue,
        # signaled first, optionally gated on the mid-frame world fence); the
        # UI copy trails on the present-time input fence. Selection requires
        # SOME complete UI (own or the held previous anchor's) so a
        # half-copied UI is never composited.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket(", 1)[0]
        worker = source.split("void AReproj_Dx12::ProcessCapturePacket", 1)[1].split(
            "void AReproj_Dx12::StopCaptureWorker", 1)[0]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        self.assertIn("packet.colorFenceValue = ++_captureFenceValue", capture)
        self.assertIn("packet.worldFenceValue = Kcd2HudIsolation::TakeWorldSignalValue", capture)
        self.assertIn("captureViaWorker ? packet.colorFenceValue : packet.captureFenceValue", capture)
        self.assertIn("PHASE 1 - world (color) copy", worker)
        self.assertIn("PHASE 2 - UI copy", worker)
        self.assertIn("_captureQueue->Wait(_worldFence, packet.worldFenceValue)", worker)
        # The composed-backbuffer fallback must stay on the present gate even
        # when a world fence fired (its final composite is frame-late).
        self.assertIn("packet.captureSrcColor != packet.captureSrcComposed", worker)
        self.assertIn("colorComplete", presenter)
        self.assertIn("ownUiComplete", presenter)
        self.assertIn("_heldPacketIndex", presenter)

    def test_swap_blend_and_ui_borrow_hold_previous_anchor(self):
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        dispatch = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8").split(
            "bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split("bool AReproj_Dx12::DispatchWarp", 1)[0]
        shader = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        common = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        self.assertIn("_heldPacketIndex = activePacketIndex", presenter)
        self.assertIn("prevPacketIndex = _heldPacketIndex", presenter)
        self.assertIn("uiPacketIndex = _heldPacketIndex", presenter)
        self.assertIn("constants.strength = blendSwap ? kSwapBlendFactor : 0.0f", dispatch)
        self.assertIn("Texture2D<float4> PrevColor : register(t3)", shader)
        self.assertIn("world = lerp(world, prevWarped, Strength)", shader)
        self.assertIn("Texture2D<float4> PrevColor : register(t3)", common)
        self.assertIn("_metricsUiBorrows", presenter)
        log = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("uiBorrow={}", log)

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
        self.assertIn("Kcd2HudIsolation::SetWorldSignalContext(_worldFence)",
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
