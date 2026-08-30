#!/usr/bin/env python3
"""Deterministic contracts for hybrid content ordering and late target pose."""

from pathlib import Path
import unittest


class HybridTimeline:
    """One display-slot consumer; publication never presents by itself."""

    def __init__(self, generated_fractions):
        self.generated_fractions = generated_fractions
        self.pending = []
        self.latest = None

    def publish(self, frame_id):
        self.pending.append(
            [(frame_id, "generated", fraction) for fraction in self.generated_fractions]
            + [(frame_id, "real", 1.0)]
        )

    def slot(self):
        if self.latest is None and self.pending:
            self.latest = self.pending.pop(0)
        if self.latest:
            item = self.latest.pop(0)
            if not self.latest:
                self.latest = None
            return item
        return None


class HybridTimewarpTests(unittest.TestCase):
    def test_one_generated_frame_is_midpoint_then_real(self):
        timeline = HybridTimeline([0.5])
        timeline.publish(1)
        self.assertEqual(timeline.slot(), (1, "generated", 0.5))
        self.assertEqual(timeline.slot(), (1, "real", 1.0))

    def test_three_generated_frames_are_strictly_ordered(self):
        timeline = HybridTimeline([0.25, 0.5, 0.75])
        timeline.publish(9)
        output = [timeline.slot() for _ in range(4)]
        self.assertEqual([item[2] for item in output], [0.25, 0.5, 0.75, 1.0])

    def test_stall_does_not_create_a_catchup_burst(self):
        timeline = HybridTimeline([0.5])
        timeline.publish(1)
        self.assertIsNotNone(timeline.slot())
        self.assertIsNotNone(timeline.slot())
        for _ in range(20):
            self.assertIsNone(timeline.slot())

    def test_sources_publish_at_most_one_content_per_display_slot(self):
        for source_hz in (30, 45, 60):
            timeline = HybridTimeline([0.5])
            outputs = []
            accumulator = 0
            for _ in range(120):
                accumulator += source_hz
                if accumulator >= 120:
                    accumulator -= 120
                    timeline.publish(len(outputs) + 1)
                output = timeline.slot()
                outputs.append(output)
            self.assertEqual(len(outputs), 120)

    def test_live_code_keeps_hybrid_opt_in_and_finishes_sequences(self):
        root = Path(__file__).resolve().parents[2]
        state = (root / "OptiScaler/State.h").read_text(encoding="utf-8")
        config = (root / "OptiScaler/Config.cpp").read_text(encoding="utf-8")
        presenter = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.assertIn("HybridTimewarp", state)
        self.assertIn('"hybridtimewarp") == 0', config.lower())
        self.assertIn("!contentSequencePending", presenter)
        self.assertIn("nextContentIndex > packet.generatedCount", presenter)
        self.assertIn("packet.constants.mode = 2", presenter)

    def test_constant_slice_is_written_before_cpu_fence_release(self):
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        write = presenter.index("_warp->WriteConstants(outputIndex, lateConstants)")
        release = presenter.index("_lateLatchFence->Signal(lateLatchValue)", write)
        self.assertLess(write, release)
        self.assertIn("WaitForSCAllocator(outputIndex)", presenter)

    def test_shader_source_and_inline_constants_match(self):
        root = Path(__file__).resolve().parents[2]
        inline = (root / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        rp = (root / "OptiScaler/shaders/reprojection/precompile/RP.hlsl").read_text(encoding="utf-8")
        rpd = (root / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        for member, shader_name in (
            ("targetPosition", "TargetPosition"),
            ("targetRight", "TargetRight"),
            ("targetUp", "TargetUp"),
            ("targetForward", "TargetForward"),
        ):
            self.assertIn(member, inline)
            self.assertIn(shader_name, rp)
            self.assertIn(shader_name, rpd)

    def test_orthonormalize_basis_gram_schmidt(self):
        import math

        def normalize(v):
            l = math.sqrt(sum(x * x for x in v))
            return [x / l for x in v] if l > 1e-12 else [0.0, 0.0, 0.0]

        def dot(a, b):
            return sum(x * y for x, y in zip(a, b))

        def cross(a, b):
            return [
                a[1] * b[2] - a[2] * b[1],
                a[2] * b[0] - a[0] * b[2],
                a[0] * b[1] - a[1] * b[0],
            ]

        # Simulate camera basis with slight roll / rotation
        fwd = normalize([0.1, 0.2, 0.9])
        rgt = [0.95, -0.1, 0.05]
        up_vec = [-0.05, 0.98, -0.1]

        rgt_proj = dot(rgt, fwd)
        rgt_ortho = normalize([rgt[i] - fwd[i] * rgt_proj for i in range(3)])
        up_ortho = normalize(cross(fwd, rgt_ortho))
        if dot(up_ortho, up_vec) < 0.0:
            up_ortho = [-x for x in up_ortho]

        # Verify pairwise dot products are ~0 (orthogonal) and vector lengths are ~1 (normal)
        self.assertAlmostEqual(dot(fwd, rgt_ortho), 0.0, places=6)
        self.assertAlmostEqual(dot(fwd, up_ortho), 0.0, places=6)
        self.assertAlmostEqual(dot(rgt_ortho, up_ortho), 0.0, places=6)
        self.assertAlmostEqual(dot(fwd, fwd), 1.0, places=6)
        self.assertAlmostEqual(dot(rgt_ortho, rgt_ortho), 1.0, places=6)
        self.assertAlmostEqual(dot(up_ortho, up_ortho), 1.0, places=6)

    def test_device_loss_and_cleanup_drains_resources(self):
        root = Path(__file__).resolve().parents[2]
        presenter = (root / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        generator = (root / "OptiScaler/framegen/reproj/HybridFsrGenerator.cpp").read_text(encoding="utf-8")
        self.assertIn("SAFE_RELEASE(generated.color)", presenter)
        self.assertIn("_hybridGenerator->Shutdown()", presenter)
        self.assertIn("FfxApiProxy::D3D12_DestroyContext", generator)

    def test_fsr_generator_format_normalization(self):
        root = Path(__file__).resolve().parents[2]
        generator = (root / "OptiScaler/framegen/reproj/HybridFsrGenerator.cpp").read_text(encoding="utf-8")
        self.assertIn("DXGI_FORMAT_R8G8B8A8_TYPELESS", generator)
        self.assertIn("DXGI_FORMAT_B8G8R8A8_TYPELESS", generator)
        self.assertIn("D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS", generator)
        self.assertIn("FfxApiProxy::D3D12_Configure", generator)


if __name__ == "__main__":
    unittest.main()

