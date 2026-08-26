#include "pch.h"
#include "RP_Dx12.h"

#include "RP_Common.h"

#include <Config.h>
#include <State.h>

#include "precompile/RP_Shader.h"
#include "precompile/RPD_Shader.h"

void RP_Dx12::ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                              D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

bool RP_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* lastColor,
                       D3D12_RESOURCE_STATES lastColorState, ID3D12Resource* velocity,
                       D3D12_RESOURCE_STATES velocityState, ID3D12Resource* depth, D3D12_RESOURCE_STATES depthState,
                       ID3D12Resource* output, RP_Constants& constants)
{
    if (!_init || _device == nullptr || cmdList == nullptr || lastColor == nullptr || velocity == nullptr ||
        output == nullptr)
    {
        return false;
    }

    LOG_DEBUG("[{}] Start!", _name);

    _counter++;
    _counter = _counter % RP_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    // Transition inputs/output to the states the shader needs
    ResourceBarrier(cmdList, lastColor, lastColorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResourceBarrier(cmdList, velocity, velocityState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (depth != nullptr)
        ResourceBarrier(cmdList, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ResourceBarrier(cmdList, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Sample the previous frame byte-faithfully: an sRGB backbuffer copy would otherwise
    // gamma-decode on sampling and come out too bright when written to the UAV output.
    DXGI_FORMAT lastColorViewFormat = DXGI_FORMAT_UNKNOWN;
    switch (lastColor->GetDesc().Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        lastColorViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        lastColorViewFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        break;
    default:
        break;
    }

    CreateShaderResourceView(_device, lastColor, currentHeap.GetSrvCPU(0), lastColorViewFormat);
    CreateShaderResourceView(_device, velocity, currentHeap.GetSrvCPU(1));

    if (depth != nullptr)
        CreateShaderResourceView(_device, depth, currentHeap.GetSrvCPU(2));

    CreateUnorderedAccessView(_device, output, currentHeap.GetUavCPU(0), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[_counter], constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetComputeRootSignature(_rootSignature);

    // v2 (depth-aware/camera rotation) uses depth pipeline; mode 0 uses MV-only PSO
    auto pso = (constants.mode == 2 || (depth != nullptr && constants.mode != 0)) ? _pipelineStateDepth : _pipelineState;
    cmdList->SetPipelineState(pso);

    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    auto outDesc = output->GetDesc();
    UINT dispatchWidth = static_cast<UINT>((outDesc.Width + 15) / 16);
    UINT dispatchHeight = (outDesc.Height + 15) / 16;

    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    // Restore the velocity/depth inputs to their pre-dispatch states. The caller's
    // SetResource copy path reuses these resources and expects them unchanged.
    ResourceBarrier(cmdList, velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, velocityState);

    if (depth != nullptr)
        ResourceBarrier(cmdList, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);

    return true;
}

RP_Dx12::RP_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{} start!", _name);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0);
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    if (!SetupRootSignature(InDevice, 3, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RP_Constants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (auto& constantBuffer : _constantBuffers)
    {
        const auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                              IID_PPV_ARGS(&constantBuffer));
        if (result != S_OK)
        {
            LOG_ERROR("[{}] CreateCommittedResource error {:x}", _name, (unsigned int) result);
            return;
        }
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, RP_cso, sizeof(RP_cso), RPMV_ShaderCode.c_str()))
    {
        LOG_ERROR("[{}] Failed to create MV compute pipeline", _name);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineStateDepth, RPD_cso, sizeof(RPD_cso), RPD_ShaderCode.c_str()))
    {
        LOG_ERROR("[{}] Failed to create depth compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RP_NUM_OF_HEAPS);
}

RP_Dx12::~RP_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    SAFE_RELEASE(_pipelineStateDepth);

    for (int i = 0; i < RP_NUM_OF_HEAPS; i++)
    {
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}
