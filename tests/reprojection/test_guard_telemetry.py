import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class GuardTelemetryTests(unittest.TestCase):
    def test_telemetry_is_opt_in_and_aggregate_only(self):
        config = (ROOT / "OptiScaler/Config.h").read_text(encoding="utf-8")
        source = (ROOT / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertRegex(config, r"ReprojWarpTelemetry\s*\{\s*false\s*\}")
        self.assertIn("if (Config::Instance()->ReprojWarpTelemetry.value_or_default())", source)
        self.assertIn('LOG_INFO("ReprojWarp:', source)
        self.assertNotIn("Readback", source.split("void MeasureWarpCoverage", 1)[1].split(
            "void PrepareRotationConstants", 1)[0])

    def test_coverage_uses_raw_homography_and_filter_safe_bounds(self):
        source = (ROOT / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        measure = source.split("void MeasureWarpCoverage", 1)[1].split(
            "void PrepareRotationConstants", 1)[0]
        self.assertIn("0.5f / constants.displayWidth", measure)
        self.assertIn("0.5f / constants.displayHeight", measure)
        self.assertIn("DotReprojVec3(xRow, outputPixel) / denominator", measure)
        self.assertIn("DotReprojVec3(yRow, outputPixel) / denominator", measure)
        self.assertNotIn("clamp(", measure)
        self.assertNotIn("saturate", measure)

    def test_frame_telemetry_carries_timestamps_residuals_and_each_edge(self):
        header = (ROOT / "OptiScaler/framegen/reproj/ContentFrame.h").read_text(encoding="utf-8")
        for field in (
            "frameId", "renderCameraTimestamp", "sourceObservedReadyTimestamp", "warpTimestamp",
            "predictedYawDelta", "predictedPitchDelta", "actualYawDelta", "actualPitchDelta",
            "residualYaw", "residualPitch", "requiredPixelsLeft", "requiredPixelsRight",
            "requiredPixelsTop", "requiredPixelsBottom", "maxOobUvLeft", "maxOobUvRight",
            "maxOobUvTop", "maxOobUvBottom", "predictionHorizonMs", "predictorConfidence",
            "guardPixelsLeft", "guardPixelsRight", "guardPixelsTop", "guardPixelsBottom",
            "coverageClamped",
        ):
            self.assertIn(field, header)

    def test_debug_view_is_off_by_default_and_reaches_shader_constants(self):
        config = (ROOT / "OptiScaler/Config.h").read_text(encoding="utf-8")
        source = (ROOT / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        shader = (ROOT / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        self.assertRegex(config, r"ReprojDebugView\s*\{\s*false\s*\}")
        self.assertIn("ReprojDebugView.value_or_default() ? 1u : 0u", source)
        self.assertIn("DebugView == 1 && coverage <= 0.0f", shader)


if __name__ == "__main__":
    unittest.main()
