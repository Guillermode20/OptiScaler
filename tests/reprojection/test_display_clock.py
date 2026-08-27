#!/usr/bin/env python3
"""Deterministic regression tests for the async display-clock invariants."""

from pathlib import Path
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
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork()", 1)[0]
        self.assertEqual(presenter.count("PresentCompositorFrame("), 1)
        self.assertIn("PresentCompositorFrame(1, 0, !newAnchor, false)", presenter)
        self.assertNotIn("DXGI_PRESENT_ALLOW_TEARING", presenter)

    def test_async_hot_path_has_no_per_output_debug_logging(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        dispatch = (root / "OptiScaler/shaders/reprojection/RP_Dx12.cpp").read_text(encoding="utf-8")
        present = reproj.split("HRESULT AReproj_Dx12::PresentCompositorFrame", 1)[1].split(
            "bool AReproj_Dx12::SampleDisplayClock", 1)[0]
        warp = dispatch.split("bool RP_Dx12::Dispatch", 1)[1].split("RP_Dx12::RP_Dx12", 1)[0]
        self.assertNotIn("LOG_DEBUG", present)
        self.assertNotIn("LOG_DEBUG", warp)

    def test_warp_phase_extrapolates_from_pose_timestamp(self):
        # Capture-completion timestamps carry the game's pipeline latency and its
        # jitter into the warp phase; the pose-sample time is the valid origin.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork()", 1)[0]
        self.assertIn("packet.sourcePoseTimestamp", presenter)
        self.assertIn("warpOriginMs", presenter)
        self.assertNotIn("(targetDisplayMs - packet.renderTimestamp)", presenter)

    def test_source_period_is_ema_smoothed_against_outliers(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = source.split("bool AReproj_Dx12::CaptureFramePacket(", 1)[1].split(
            "int AReproj_Dx12::AcquirePacket()", 1)[0]
        self.assertIn("_realPeriodEmaMs", capture)
        self.assertIn("packet.frameDelta = _realPeriodEmaMs", capture)

    def test_presenter_locks_to_measured_vblank_grid(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork()", 1)[0]
        self.assertIn("GetFrameStatistics", source.split("void AReproj_Dx12::PresenterMain()", 1)[0])
        self.assertIn("_displayClockAnchorMs", presenter)
        self.assertIn("_measuredRefreshPeriodMs", presenter)

    def test_vblank_lock_cannot_run_away_earlier(self):
        # Per-slot bounds do not stop a sustained backwards walk when the grid
        # phase estimate is biased early; only a cumulative budget does. Without
        # it the deadline drifts far ahead of scanout and latency-1 presents
        # block progressively until the pipeline wedges.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork()", 1)[0]
        self.assertIn("MAX_TOTAL_EARLY_CORRECTION_MS", presenter)
        self.assertIn("totalEarlyCorrectionMs += appliedDeltaMs", presenter)

    def test_presenter_watchdog_downgrades_on_jammed_presents(self):
        # Present(1) is an unbounded blocking call; sustained jams or one
        # multi-second wedge must fail the worker so the game thread's Failed
        # handling downgrades to the synchronous presenter instead of freezing.
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = source.split("void AReproj_Dx12::PresenterMain()", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork()", 1)[0]
        self.assertIn("WATCHDOG_CONSECUTIVE_JAMS", presenter)
        self.assertIn("WATCHDOG_WEDGE_MS", presenter)
        self.assertGreater(presenter.count("_presenterState.store(PresenterState::Failed)"), 1)

    def test_runtime_and_precompiled_shader_sources_match(self):
        root = Path(__file__).resolve().parents[2]
        common = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        pairs = {
            "RPMV_ShaderCode": root / "OptiScaler/shaders/reprojection/precompile/RP.hlsl",
            "RPD_ShaderCode": root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl",
        }
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


if __name__ == "__main__":
    unittest.main()
