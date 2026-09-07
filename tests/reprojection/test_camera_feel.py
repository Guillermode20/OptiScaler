#!/usr/bin/env python3
"""Camera-feel regression tests: no default smoothing lag, per-content late-latch baselines."""

from pathlib import Path
import math
import unittest


class CameraFeelTests(unittest.TestCase):
    def test_smoothing_defaults_off(self):
        # The EMA on camera angular velocity lags motion onset and creeps after
        # stop (geometric tail), and biases the FSR midpoint via the smoothed
        # prev pose. It stays opt-in; the default must be off.
        root = Path(__file__).resolve().parents[2]
        config = (root / "OptiScaler/Config.h").read_text(encoding="utf-8")
        self.assertRegex(config, r"ReprojSmoothing\s*\{\s*0\.0f\s*\}")

    def test_late_input_warps_selected_content_from_its_own_baseline(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        header = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        content = (root / "OptiScaler/framegen/reproj/ContentFrame.h").read_text(encoding="utf-8")
        # Per-content mouse baseline lives on the base frame so generated
        # midpoints (older than their anchor) carry their own.
        self.assertIn("sourceMouseX", content)
        self.assertIn("sourceMouseY", content)
        self.assertIn("sourceMouseTimestamp", content)
        # The late warp takes the displayed content plus its owning packet.
        self.assertIn("ApplyLateInput(RP_Constants& constants, const ContentFrame& content", header)
        late_input = reproj.split("bool AReproj_Dx12::ApplyLateInput", 1)[1].split(
            "void AReproj_Dx12::UpdateMouseSensitivity", 1)[0]
        # Freshness gate and both baselines key off the displayed content, not
        # always the real anchor.
        self.assertIn("latestCamera.timestampMs > content.sourcePoseTimestamp", late_input)
        self.assertIn("latestCamera.cutGeneration == content.sourceCutGeneration", late_input)
        self.assertIn("current.TotalX - content.sourceMouseX", late_input)
        self.assertIn("current.TotalY - content.sourceMouseY", late_input)
        self.assertNotIn("packet.sourceMouseX", late_input)
        self.assertNotIn("packet.sourcePoseTimestamp", late_input)
        # Both dispatch sites (baseline + late) warp the selected content.
        self.assertEqual(reproj.count("ApplyLateInput(constants, content, packet)"), 1)
        self.assertEqual(reproj.count("ApplyLateInput(lateConstants, content, packet)"), 1)

    def test_midpoint_pose_is_a_halfway_slerp_with_own_mouse_baseline(self):
        root = Path(__file__).resolve().parents[2]
        reproj = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        capture = reproj.split("bool AReproj_Dx12::CaptureFramePacket", 1)[1].split(
            "bool AReproj_Dx12::DisplayPacket", 1)[0]
        # Orientation midpoint: halfway rotation (slerp), never a linear
        # average of basis vectors (which shortens larger rotations).
        self.assertIn("RotationAxisAngle(prevRight, prevUp, prevForward,", capture)
        self.assertIn("midAngle * 0.5f", capture)
        self.assertNotIn("0.5f * (packet.constants.prevCameraForward[axis]", capture)
        self.assertNotIn("0.5f * (packet.constants.prevCameraRight[axis]", capture)
        self.assertNotIn("0.5f * (packet.constants.prevCameraUp[axis]", capture)
        # The midpoint's late-latch baseline comes from the timestamped input
        # history at the midpoint time, not the newer anchor totals.
        self.assertIn("GetRawMouseMotionAt(generated.sourcePoseTimestamp)", capture)
        self.assertIn("generated.sourceMouseX = midMouse.TotalX", capture)

    def test_halfway_slerp_preserves_angle_while_lerp_shortens_it(self):
        # Numeric guard for the bug class: averaging two unit vectors
        # separated by angle a yields cos(a/2) < 1 before renormalization,
        # i.e. the naive midpoint under-rotates once renormalized in a
        # non-orthonormal frame. A 10-degree yaw must midpoint at 5 degrees.
        angle = math.radians(10.0)
        prev = (1.0, 0.0)
        curr = (math.cos(angle), math.sin(angle))
        slerp = (math.cos(angle / 2), math.sin(angle / 2))
        lerp_len = math.hypot((prev[0] + curr[0]) / 2, (prev[1] + curr[1]) / 2)
        self.assertLess(lerp_len, 1.0)
        self.assertAlmostEqual(math.degrees(math.acos(max(-1.0, min(1.0, slerp[0])))), 5.0, places=9)
        # Lerp shortening at 10 deg is small but systematic (~0.2%), and it
        # alternates every other slot against real anchors at 120 Hz.
        self.assertGreater(1.0 - lerp_len, 0.0)


if __name__ == "__main__":
    unittest.main()
