#include "pch.h"
#include "RP_Dx12.h"

#include "RP_Common.h"

#include <State.h>

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
                       D3D12_RESOURCE_STATES lastColorState, ID3D12Resource* output, RP_Constants& constants,
                       int constantSlot, bool deferConstants, ID3D12Resource* ui, D3D12_RESOURCE_STATES uiState,
                       ID3D12Resource* depth, D3D12_RESOURCE_STATES depthState)
{
    if (!_init || _device == nullptr || cmdList == nullptr || lastColor == nullptr || output == nullptr)
    {
        return false;
    }

    int heapIndex = constantSlot;
    if (heapIndex < 0 || heapIndex >= RP_NUM_OF_HEAPS)
    {
        _counter++;
        _counter = _counter % RP_NUM_OF_HEAPS;
        heapIndex = _counter;
    }
    FrameDescriptorHeap& currentHeap = _frameHeaps[heapIndex];

    // Transition inputs/output to the states the shader needs
    ResourceBarrier(cmdList, lastColor, lastColorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (ui != nullptr)
        ResourceBarrier(cmdList, ui, uiState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (depth != nullptr)
        ResourceBarrier(cmdList, depth, depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ResourceBarrier(cmdList, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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
    if (ui != nullptr)
        CreateShaderResourceView(_device, ui, currentHeap.GetSrvCPU(1));
    else
        CreateShaderResourceView(_device, lastColor, currentHeap.GetSrvCPU(1), lastColorViewFormat);
    if (depth != nullptr)
    {
        // Anchor depth arrives typeless (KCD2: R24G8_TYPELESS); view the depth
        // plane as normalized float, mirroring the sRGB->UNORM handling above.
        DXGI_FORMAT depthViewFormat = DXGI_FORMAT_UNKNOWN;
        if (depth->GetDesc().Format == DXGI_FORMAT_R24G8_TYPELESS)
            depthViewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        CreateShaderResourceView(_device, depth, currentHeap.GetSrvCPU(2), depthViewFormat);
    }
    else
        CreateShaderResourceView(_device, lastColor, currentHeap.GetSrvCPU(2), lastColorViewFormat);

    CreateUnorderedAccessView(_device, output, currentHeap.GetUavCPU(0), 0);

    if (_constantBufferData[heapIndex] == nullptr)
        return false;
    if (!deferConstants)
        memcpy(_constantBufferData[heapIndex], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    cmdList->SetComputeRootSignature(_rootSignature);

    cmdList->SetPipelineState(_pipelineState);

    cmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    auto outDesc = output->GetDesc();
    UINT dispatchWidth = static_cast<UINT>((outDesc.Width + 15) / 16);
    UINT dispatchHeight = (outDesc.Height + 15) / 16;

    cmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    if (ui != nullptr)
        ResourceBarrier(cmdList, ui, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, uiState);
    if (depth != nullptr)
        ResourceBarrier(cmdList, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depthState);

    return true;
}

bool RP_Dx12::WriteConstants(int constantSlot, const RP_Constants& constants)
{
    if (constantSlot < 0 || constantSlot >= RP_NUM_OF_HEAPS || _constantBufferData[constantSlot] == nullptr)
        return false;
    std::memcpy(_constantBufferData[constantSlot], &constants, sizeof(constants));
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

    if (!SetupRootSignature(InDevice, 3, 1, 1, 0, 0, 1, &sampler)))
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

    if (!CreateComputePipeline(InDevice, &_pipelineState, RPD_cso, sizeof(RPD_cso), RPD_ShaderCode.c_str()))
    {
        LOG_ERROR("[{}] Failed to create camera-warp compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RP_NUM_OF_HEAPS);
    if (!_init)
        return;

    // These upload buffers live for the lifetime of the reprojection pass. D3D12
    // explicitly permits persistent mapping, so avoid Map/Unmap and CBV creation
    // in every display slot. Each descriptor heap owns the matching ring-buffer
    // entry and command-allocator fences prevent it from being overwritten early.
    const CD3DX12_RANGE noCpuReads(0, 0);
    for (int i = 0; i < RP_NUM_OF_HEAPS; ++i)
    {
        const auto result = _constantBuffers[i]->Map(0, &noCpuReads, reinterpret_cast<void**>(&_constantBufferData[i]));
        if (FAILED(result))
        {
            LOG_ERROR("[{}] Persistent constant-buffer map failed: {:x}", _name, static_cast<unsigned int>(result));
            for (int mapped = 0; mapped < i; ++mapped)
            {
                _constantBuffers[mapped]->Unmap(0, nullptr);
                _constantBufferData[mapped] = nullptr;
            }
            _init = false;
            return;
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc {};
        cbvDesc.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = static_cast<UINT>(sizeof(RP_Constants));
        InDevice->CreateConstantBufferView(&cbvDesc, _frameHeaps[i].GetCbvCPU(0));
    }
}

RP_Dx12::~RP_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < RP_NUM_OF_HEAPS; i++)
    {
        if (_constantBuffers[i] != nullptr && _constantBufferData[i] != nullptr)
        {
            _constantBuffers[i]->Unmap(0, nullptr);
            _constantBufferData[i] = nullptr;
        }
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}
