#!/usr/bin/env python3
"""Deterministic regression tests for the async display-clock invariants."""

import math
from pathlib import Path
import statistics
import unittest


def mouse_at(history, current, timestamp_ms):
    if timestamp_ms <= 0:
        return current
    eligible = [sample for sample in history if 0 < sample[0] <= timestamp_ms]
    return max(eligible, default=min(history, key=lambda sample: sample[0]), key=lambda sample: sample[0])


def select_lag(frames, lag_bins=range(0, 151, 2)):
    # One-axis specialization of the runtime least-squares lag search.
    best = None
    for lag in lag_bins:
        pairs = [(mouse(t1 - lag) - mouse(t0 - lag), rotation) for t0, t1, rotation, mouse in frames]
        xx = sum(x * x for x, _ in pairs)
        xy = sum(x * y for x, y in pairs)
        yy = sum(y * y for _, y in pairs)
        if xx == 0 or yy == 0:
            continue
        scale = xy / xx
        confidence = max(0.0, 1.0 - sum((y - scale * x) ** 2 for x, y in pairs) / yy)
        if best is None or confidence > best[0]:
            best = confidence, lag, scale
    return best


def robust_rotation(observations):
    accepted = [True] * len(observations)
    yaw = pitch = 0.0
    for _ in range(3):
        aa = ab = bb = ay = by = 0.0
        for keep, (a, b, c, d, du, dv) in zip(accepted, observations):
            if not keep:
                continue
            aa += a * a + c * c
            ab += a * b + c * d
            bb += b * b + d * d
            ay += a * du + c * dv
            by += b * du + d * dv
        determinant = aa * bb - ab * ab
        yaw = (bb * ay - ab * by) / determinant
        pitch = (aa * by - ab * ay) / determinant
        residuals = [math.hypot(du - a * yaw - b * pitch, dv - c * yaw - d * pitch)
                     for a, b, c, d, du, dv in observations]
        threshold = max(0.75, statistics.median(residuals) * 3.0)
        accepted = [residual <= threshold for residual in residuals]
    return yaw, pitch, sum(accepted) / len(accepted)


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
    def test_zero_mouse_timestamp_returns_current(self):
        history = [(10, 2), (20, 4), (30, 7)]
        self.assertEqual(mouse_at(history, (40, 11), 0), (40, 11))
        self.assertEqual(mouse_at(history, (40, 11), 25), (20, 4))

    def test_lag_bin_selection(self):
        samples = {t: 0.03 * t + 8.0 * math.sin(t / 19.0) + 3.0 * math.sin(t / 7.0)
                   for t in range(0, 1000)}
        mouse = lambda t: samples[max(0, min(999, int(t)))]
        frames = [(100 + i * 20, 120 + i * 20,
                   (mouse(120 + i * 20 - 42) - mouse(100 + i * 20 - 42)) * 0.002,
                   mouse) for i in range(30)]
        confidence, lag, _ = select_lag(frames)
        self.assertGreaterEqual(confidence, 0.99)
        self.assertEqual(lag, 42)

    def test_robust_rotation_rejects_scene_outliers(self):
        yaw, pitch = 0.012, -0.007
        observations = []
        for i in range(40):
            a, b, c, d = 800 + i, (i % 5) - 2, (i % 7) - 3, 760 + i
            du, dv = a * yaw + b * pitch, c * yaw + d * pitch
            if i >= 30:
                du += 100 + i
                dv -= 80
            observations.append((a, b, c, d, du, dv))
        fitted_yaw, fitted_pitch, inliers = robust_rotation(observations)
        self.assertAlmostEqual(fitted_yaw, yaw, places=5)
        self.assertAlmostEqual(fitted_pitch, pitch, places=5)
        self.assertGreaterEqual(inliers, 0.70)

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


if __name__ == "__main__":
    unittest.main()
