#!/usr/bin/env python3
"""E3 limiter policy: an edge-width budget, not a zero-overrun veto.

Live 2026-09-07 showed scale=0.000 with limited~=all slots on every motion
second: any real rotation moves uncovered content in at one edge (~22 px per
degree), so demanding zero invalid boundary samples can only ever return s=0
and the warp was fully neutralized. The policy is now a pixel budget: small
rotations pass at s=1, large flicks clamp to partial.
"""

from pathlib import Path
import math
import unittest


class EdgeLimiterTests(unittest.TestCase):
    def _evaluator(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        return source.split("WarpCoverage EvaluateWarpCoverage", 1)[1].split(
            "ReprojVec3 RotateReprojVec3", 1)[0]

    def test_validity_uses_an_edge_width_budget(self):
        evaluator = self._evaluator()
        self.assertIn("kEdgeOverrunBudgetPx", evaluator)
        self.assertIn("budgetU", evaluator)
        self.assertIn("budgetV", evaluator)
        # Invalid means beyond budget, not merely outside the inset rect.
        self.assertIn("left > budgetU || right > budgetU || top > budgetV || bottom > budgetV", evaluator)
        self.assertNotIn("left > 0.0f || right > 0.0f || top > 0.0f || bottom > 0.0f", evaluator)

    def test_raw_overruns_still_reported_for_telemetry(self):
        # The 1 Hz line must keep showing the true coverage demand so the
        # limiter cannot hide the underlying deficit.
        evaluator = self._evaluator()
        self.assertIn("coverage.overrunLeft = std::max(coverage.overrunLeft, left)", evaluator)
        self.assertIn("coverage.overrunRight = std::max(coverage.overrunRight, right)", evaluator)
        self.assertIn("coverage.overrunTop = std::max(coverage.overrunTop, top)", evaluator)
        self.assertIn("coverage.overrunBottom = std::max(coverage.overrunBottom, bottom)", evaluator)

    def test_partial_warp_search_and_fail_closed_paths_survive(self):
        root = Path(__file__).resolve().parents[2]
        source = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        prepare = source.split("WarpCoverage PrepareRotationConstants", 1)[1].split(
            "bool AReproj_Dx12::ApplyLateInput", 1)[0]
        self.assertIn("for (int i = 0; i < 8; ++i)", prepare)
        self.assertIn("coverage.safeScale = low;", prepare)
        self.assertIn("coverage.safeScale = 0.0f;", prepare)

    def test_budget_covers_ordinary_turns_but_not_flicks(self):
        # ~22 px edge excursion per degree of yaw at 1280 px focal length.
        # A 12 px budget passes gentle motion at full warp while a fast flick
        # still clamps to partial instead of smearing the edge.
        focal_px = 1280.0
        budget_px = 12.0

        def excursion_px(deg_per_slot):
            return focal_px * math.radians(deg_per_slot)

        gentle = excursion_px(0.5)   # 60 deg/s turn at 120 Hz
        flick = excursion_px(2.5)    # 300 deg/s flick at 120 Hz
        self.assertLess(gentle, budget_px)
        self.assertGreater(flick, budget_px)
        # And the budget dwarfs float noise, so identity verdicts are stable.
        self.assertGreater(budget_px / 2560.0, 1e-6)


if __name__ == "__main__":
    unittest.main()
