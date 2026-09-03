#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/reprojection/RP_Common.h>

#define RP_NUM_OF_HEAPS BUFFER_COUNT

// Async camera warp pass. The isolated UI is composited in the
// same compute dispatch so the presenter never has to cross back to DIRECT.
// Mode 1 samples an anchor depth texture to correct translation parallax.
// Root signature: 3 SRVs (color, ui, depth), 1 UAV, 1 CBV, 1 static bilinear-clamp sampler.
class RP_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[RP_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[RP_NUM_OF_HEAPS] = {};
    UINT8* _constantBufferData[RP_NUM_OF_HEAPS] = {};

    static void ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState);

  public:
    // Warps `lastColor` forward to the fake-frame time and writes `output`.
    // Inputs are transitioned to NON_PIXEL_SHADER_RESOURCE; `output` is transitioned
    // from COPY_SOURCE to UNORDERED_ACCESS (the caller returns it to COPY_SOURCE).
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* lastColor, D3D12_RESOURCE_STATES lastColorState,
                  ID3D12Resource* output, RP_Constants& constants, int constantSlot = -1, bool deferConstants = false,
                  ID3D12Resource* ui = nullptr, D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON,
                  ID3D12Resource* depth = nullptr, D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON);

    // Completes a deferred dispatch after the command list has been queued
    // behind a CPU-signaled fence. Each slot is immutable until its SC fence
    // completes; the caller owns that ordering.
    bool WriteConstants(int constantSlot, const RP_Constants& constants);

    RP_Dx12(std::string InName, ID3D12Device* InDevice);

    ~RP_Dx12();
};
