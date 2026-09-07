#!/usr/bin/env python3
"""Async-timewarp knob wiring: SafeWarpBudget, MaxTimeStep, DebugView.

Pins defaults, ini keys, save/load plumbing, runtime use, and menu widgets
so the floatiness A/B (full warp vs budgeted) stays one toggle away.
"""

from pathlib import Path
import unittest


class TimewarpKnobTests(unittest.TestCase):
    def _root(self):
        return Path(__file__).resolve().parents[2]

    def test_config_defaults(self):
        config = (self._root() / "OptiScaler/Config.h").read_text(encoding="utf-8")
        self.assertRegex(config, r"ReprojSafeWarpBudget\s*\{\s*12\.0f\s*\}")
        self.assertRegex(config, r"ReprojMaxTimeStep\s*\{\s*2\.5f\s*\}")
        self.assertRegex(config, r"ReprojHistoryBorderFallback\s*\{\s*false\s*\}")
        self.assertRegex(config, r"ReprojDebugView\s*\{\s*false\s*\}")
        self.assertRegex(config, r"ReprojPredictiveProbe\s*\{\s*false\s*\}")

    def test_config_reload_and_save(self):
        cpp = (self._root() / "OptiScaler/Config.cpp").read_text(encoding="utf-8")
        self.assertIn('"AsyncTimewarp", "SafeWarpBudget"', cpp)
        self.assertIn('"AsyncTimewarp", "MaxTimeStep"', cpp)
        self.assertIn('"AsyncTimewarp", "HistoryBorderFallback"', cpp)
        self.assertIn('"AsyncTimewarp", "DebugView"', cpp)
        self.assertIn('"AsyncTimewarp", "PredictiveProbe"', cpp)
        # Ranges are enforced on load so a stray ini cannot wedge the presenter.
        self.assertIn("0.0f, 32.0f", cpp)
        self.assertIn("1.0f, 4.0f", cpp)

    def test_ini_documents_knobs(self):
        ini = (self._root() / "OptiScaler.ini").read_text(encoding="utf-8")
        self.assertIn("SafeWarpBudget=auto", ini)
        self.assertIn("MaxTimeStep=auto", ini)
        self.assertIn("HistoryBorderFallback=auto", ini)
        # DebugView and PredictiveProbe share the AsyncTimewarp section (distinct from [FrameGen]).
        async_section = ini.split("[AsyncTimewarp]", 1)[1].split("[XeFG]", 1)[0]
        self.assertIn("DebugView=auto", async_section)
        self.assertIn("PredictiveProbe=auto", async_section)

    def test_runtime_uses_config(self):
        reproj = (self._root() / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        presenter = (self._root() / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(
            encoding="utf-8")
        camera = (self._root() / "OptiScaler/framegen/reproj/Kcd2Camera.cpp").read_text(encoding="utf-8")
        self.assertIn("ReprojSafeWarpBudget", reproj)
        self.assertIn("ReprojDebugView", reproj)
        self.assertIn("ReprojPredictiveProbe", camera)
        self.assertIn("ReprojMaxTimeStep", presenter)
        self.assertNotIn("constexpr float kEdgeOverrunBudgetPx", reproj)
        self.assertNotIn("constexpr float maxTimeStep", presenter)

    def test_menu_exposes_knobs_with_guidance(self):
        menu = (self._root() / "OptiScaler/menu/menu_common.cpp").read_text(encoding="utf-8")
        self.assertIn("Safe warp budget##reproj-live", menu)
        self.assertIn("Max warp step##reproj-live", menu)
        self.assertIn("Edge debug view##reproj-live", menu)
        self.assertIn("Predictive render probe##reproj-live", menu)
        self.assertIn("History border fallback##reproj-live", menu)
        self.assertIn("ReprojSafeWarpBudget", menu)
        self.assertIn("ReprojMaxTimeStep", menu)
        self.assertIn("ReprojHistoryBorderFallback", menu)
        self.assertIn("ReprojDebugView", menu)
        self.assertIn("ReprojPredictiveProbe", menu)


if __name__ == "__main__":
    unittest.main()
