#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/reprojection/RP_Common.h>

#define RP_NUM_OF_HEAPS BUFFER_COUNT

// Async reprojection warp pass. Two PSOs sharing one root signature:
//   - MV-only warp (v1): LastColor(t0) + Velocity(t1)
//   - depth-aware warp (v2): + Depth(t2), camera block in the constant buffer
// Root signature: 3 SRVs, 1 UAV, 1 CBV, 1 static bilinear-clamp sampler.
class RP_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[RP_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[RP_NUM_OF_HEAPS] = {};
    UINT8* _constantBufferData[RP_NUM_OF_HEAPS] = {};
    ID3D12PipelineState* _pipelineStateDepth = nullptr;

    static void ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState);

  public:
    // Warps `lastColor` forward to the fake-frame time and writes `output`.
    // `depth` is optional: when null (or constants.mode == 0) the MV-only PSO runs.
    // Inputs are transitioned to NON_PIXEL_SHADER_RESOURCE; `output` is transitioned
    // from COPY_SOURCE to UNORDERED_ACCESS (the caller returns it to COPY_SOURCE).
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* lastColor, D3D12_RESOURCE_STATES lastColorState,
                  ID3D12Resource* velocity, D3D12_RESOURCE_STATES velocityState, ID3D12Resource* depth,
                  D3D12_RESOURCE_STATES depthState, ID3D12Resource* output, RP_Constants& constants,
                  int constantSlot = -1, bool deferConstants = false);

    // Completes a deferred dispatch after the command list has been queued
    // behind a CPU-signaled fence. Each slot is immutable until its SC fence
    // completes; the caller owns that ordering.
    bool WriteConstants(int constantSlot, const RP_Constants& constants);

    RP_Dx12(std::string InName, ID3D12Device* InDevice);

    ~RP_Dx12();
};
