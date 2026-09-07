import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class HistoryBorderTests(unittest.TestCase):
    def setUp(self):
        self.header = (ROOT / "OptiScaler/framegen/reproj/AReproj_Dx12.h").read_text(encoding="utf-8")
        self.source = (ROOT / "OptiScaler/framegen/reproj/AReproj_Dx12.cpp").read_text(encoding="utf-8")
        self.presenter = (ROOT / "OptiScaler/framegen/reproj/AReprojPresenter.cpp").read_text(encoding="utf-8")
        self.shader = (ROOT / "OptiScaler/shaders/reprojection/precompile/RPD.hlsl").read_text(encoding="utf-8")
        self.runtime_shader = (ROOT / "OptiScaler/shaders/reprojection/RP_Common.h").read_text(encoding="utf-8")
        self.dispatch = (ROOT / "OptiScaler/shaders/reprojection/RP_Dx12.cpp").read_text(encoding="utf-8")

    def test_history_is_presenter_owned_not_extra_packet_slots(self):
        self.assertIn("static constexpr int kReprojFrameSlots = 3", self.header)
        self.assertIn("static constexpr int kHistoryAnchorCount = 2", self.header)
        self.assertIn("HistoryAnchor _historyAnchors[kHistoryAnchorCount]", self.header)
        self.assertNotIn("HistoryCopyPending", self.header)
        self.assertIn("ReleaseHistoryResources();\n    SAFE_RELEASE(_presentQueue)", self.presenter)

    def test_real_anchor_snapshot_is_ordered_on_presenter_queue(self):
        dispatch_call = self.presenter.split("const bool dispatched = shouldWarp", 1)[1].split(
            "if (!dispatched)", 1
        )[0]
        self.assertIn("contentPhase == 2", dispatch_call)
        snapshot = self.source.split("bool AReproj_Dx12::SnapshotHistoryAnchor", 1)[1].split(
            "uint32_t AReproj_Dx12::SelectHistoryAnchors", 1
        )[0]
        self.assertIn("!packet.hasUi", snapshot)
        self.assertIn("!packet.hasCamera", snapshot)
        self.assertIn("cmdList->CopyResource(history.color, packet.color)", snapshot)
        self.assertIn("D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE", snapshot)
        warp = self.source.split("bool AReproj_Dx12::DispatchPacketWarp", 1)[1].split(
            "bool AReproj_Dx12::DrainGpuWork", 1
        )[0]
        self.assertLess(warp.index("_warp->Dispatch"), warp.index("SnapshotHistoryAnchor"))
        self.assertLess(warp.index("SnapshotHistoryAnchor"), warp.index("UINT64 lateLatchValue"))

    def test_history_rejects_incompatible_or_stale_anchors(self):
        select = self.source.split("uint32_t AReproj_Dx12::SelectHistoryAnchors", 1)[1].split(
            "void AReproj_Dx12::PopulateHistoryConstants", 1
        )[0]
        for predicate in (
            "history.frameId >= packet.frameId",
            "history.sourceCutGeneration != content.sourceCutGeneration",
            "history.hdr != content.hdr",
            "historyDesc.Width == currentDesc.Width",
            "historyDesc.Height == currentDesc.Height",
            "historyDesc.Format == currentDesc.Format",
            "fovDelta > (0.25f",
            "aspectDelta > 0.005f",
            "ageMs > 75.0",
        ):
            self.assertIn(predicate, select)

    def test_history_maps_to_same_safe_limited_target(self):
        self.assertIn("StoreReprojVec3(constants.targetCameraRight, predictedRight)", self.source)
        self.assertIn("StoreReprojVec3(constants.targetCameraUp, predictedUp)", self.source)
        self.assertIn("StoreReprojVec3(constants.targetCameraForward, predictedForward)", self.source)
        populate = self.source.split("void AReproj_Dx12::PopulateHistoryConstants", 1)[1].split(
            "void AReproj_Dx12::RecordHistoryCoverage", 1
        )[0]
        self.assertIn("constants.targetCameraRight", populate)
        self.assertIn("BuildRotationRows(sourceConstants", populate)

    def test_shader_uses_history_only_for_invalid_current_pixels(self):
        for shader in (self.shader, self.runtime_shader):
            self.assertIn("Texture2D<float4> History0 : register(t2)", shader)
            self.assertIn("Texture2D<float4> History1 : register(t3)", shader)
            current_branch = shader.index("if (covered)")
            history0_branch = shader.index("if (history0Covered)")
            history1_branch = shader.index("if (history1Covered)")
            fallback = shader.index("world = LastColor.Load(int3(dtid.xy, 0)).rgb", history1_branch)
            self.assertLess(current_branch, history0_branch)
            self.assertLess(history0_branch, history1_branch)
            self.assertLess(history1_branch, fallback)
            self.assertIn("!covered && !historyCovered", shader)
            self.assertLess(shader.index("if (HudlessSource != 0)"), shader.index("Output[dtid.xy]"))

    def test_root_signature_and_descriptors_match_four_srvs(self):
        self.assertIn("SetupRootSignature(InDevice, 4, 1, 1", self.dispatch)
        self.assertIn("currentHeap.GetSrvCPU(2)", self.dispatch)
        self.assertIn("currentHeap.GetSrvCPU(3)", self.dispatch)
        self.assertIn("sizeof(RP_Constants) == 512", self.runtime_shader)

    def test_generated_midpoint_inherits_hdr_compatibility_metadata(self):
        self.assertIn("generated.hdr = packet.hdr", self.source)

    def test_history_telemetry_is_aggregate_only(self):
        self.assertIn("hist={}/{}/{}/{} age={:.1f}ms copyDrop={}", self.source)
        self.assertNotIn("LOG_INFO", self.source.split("void AReproj_Dx12::RecordHistoryCoverage", 1)[1].split(
            "void AReproj_Dx12::ReleaseHistoryResources", 1
        )[0])


if __name__ == "__main__":
    unittest.main()
