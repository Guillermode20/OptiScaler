#include "pch.h"
#include "HybridFsrGenerator.h"

#include <Logger.h>
#include <State.h>
#include <proxies/FfxApi_Proxy.h>

#include <dx12/ffx_api_dx12.h>

#include <algorithm>

FfxApiResourceState HybridFsrGenerator::FfxState(D3D12_RESOURCE_STATES state)
{
    switch (state)
    {
    case D3D12_RESOURCE_STATE_COMMON:
        return FFX_API_RESOURCE_STATE_COMMON;
    case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
        return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
        return FFX_API_RESOURCE_STATE_COMPUTE_READ;
    case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
        return FFX_API_RESOURCE_STATE_PIXEL_READ;
    case (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE):
        return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    case D3D12_RESOURCE_STATE_COPY_SOURCE:
        return FFX_API_RESOURCE_STATE_COPY_SRC;
    case D3D12_RESOURCE_STATE_COPY_DEST:
        return FFX_API_RESOURCE_STATE_COPY_DEST;
    case D3D12_RESOURCE_STATE_GENERIC_READ:
        return FFX_API_RESOURCE_STATE_GENERIC_READ;
    case D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case D3D12_RESOURCE_STATE_RENDER_TARGET:
        return FFX_API_RESOURCE_STATE_RENDER_TARGET;
    default:
        return FFX_API_RESOURCE_STATE_COMMON;
    }
}

bool HybridFsrGenerator::EnsureContext(ID3D12Device* device, const ContentFrame& realFrame)
{
    if (device == nullptr || realFrame.color == nullptr)
        return false;
    const auto desc = realFrame.color->GetDesc();
    if (_context != nullptr &&
        (_device != device || _displayWidth != desc.Width || _displayHeight != desc.Height || _format != desc.Format))
        Shutdown();
    if (_context != nullptr)
        return true;

    if (!FfxApiProxy::IsFGReady())
        FfxApiProxy::InitFfxDx12();
    if (!FfxApiProxy::IsFGReady())
        return false;

    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;

    ffxCreateContextDescFrameGeneration create {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &backend.header;
    create.displaySize = { static_cast<std::uint32_t>(desc.Width), desc.Height };
    create.maxRenderSize = create.displaySize;
    create.backBufferFormat = ffxApiGetSurfaceFormatDX12(desc.Format);
    create.flags = FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    if (realFrame.constants.invertedDepth != 0)
        create.flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (realFrame.constants.mvWidth == realFrame.constants.displayWidth &&
        realFrame.constants.mvHeight == realFrame.constants.displayHeight)
        create.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

    const auto result = FfxApiProxy::D3D12_CreateContext(&_context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK)
    {
        LOG_WARN("HybridTimewarp: FSR frame-generation context creation failed: {:X}", (UINT) result);
        _context = nullptr;
        return false;
    }

    _device = device;
    _displayWidth = static_cast<std::uint32_t>(desc.Width);
    _displayHeight = desc.Height;
    _format = desc.Format;
    LOG_INFO("HybridTimewarp: private FSR content generator created ({}x{}, format {})", _displayWidth, _displayHeight,
             static_cast<int>(_format));
    return true;
}

bool HybridFsrGenerator::EnsureOutput(ID3D12Device* device, ID3D12Resource* source, ContentFrame& output)
{
    if (device == nullptr || source == nullptr)
        return false;
    const auto sourceDesc = source->GetDesc();
    auto outputDesc = sourceDesc;
    switch (outputDesc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        outputDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        outputDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        break;
    default:
        break;
    }
    outputDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (output.color != nullptr)
    {
        const auto old = output.color->GetDesc();
        if (old.Width != outputDesc.Width || old.Height != outputDesc.Height || old.Format != outputDesc.Format)
        {
            output.color->Release();
            output.color = nullptr;
        }
    }
    if (output.color != nullptr)
        return true;

    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto result =
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &outputDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&output.color));
    if (FAILED(result))
    {
        LOG_WARN("HybridTimewarp: generated content texture creation failed: {:X}", (UINT) result);
        return false;
    }
    output.color->SetName(L"HybridTimewarp_GeneratedContent");
    output.colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool HybridFsrGenerator::Generate(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ContentFrame& realFrame,
                                  ContentFrame& generatedFrame, std::uint64_t frameId, bool reset)
{
    if (commandList == nullptr || realFrame.color == nullptr || realFrame.depth == nullptr ||
        realFrame.velocity == nullptr || !EnsureContext(device, realFrame) ||
        !EnsureOutput(device, realFrame.color, generatedFrame))
        return false;

    const bool cut = _cutGeneration != 0 && realFrame.sourceCutGeneration != _cutGeneration;
    _cutGeneration = realFrame.sourceCutGeneration;

    ffxConfigureDescFrameGeneration configure {};
    configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    configure.frameGenerationEnabled = true;
    configure.allowAsyncWorkloads = false;
    configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    configure.frameID = frameId;
    configure.generationRect = { 0, 0, static_cast<int>(_displayWidth), static_cast<int>(_displayHeight) };
    FfxApiProxy::D3D12_Configure(&_context, &configure.header);

    ffxDispatchDescFrameGenerationPrepare prepare {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    ffxCreateBackendDX12Desc backend {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxDispatchDescFrameGenerationPrepareCameraInfo camera {};
    camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    camera.header.pNext = &backend.header;
    std::memcpy(camera.cameraPosition, realFrame.constants.cameraPosition, sizeof(camera.cameraPosition));
    std::memcpy(camera.cameraRight, realFrame.constants.cameraRight, sizeof(camera.cameraRight));
    std::memcpy(camera.cameraUp, realFrame.constants.cameraUp, sizeof(camera.cameraUp));
    std::memcpy(camera.cameraForward, realFrame.constants.cameraForward, sizeof(camera.cameraForward));
    prepare.header.pNext = &camera.header;
    prepare.commandList = commandList;
    prepare.frameID = frameId;
    prepare.renderSize = { realFrame.constants.mvWidth, realFrame.constants.mvHeight };
    prepare.jitterOffset = { realFrame.constants.jitterX, realFrame.constants.jitterY };
    prepare.motionVectorScale = { realFrame.constants.mvScaleX, realFrame.constants.mvScaleY };
    prepare.frameTimeDelta = static_cast<float>(std::clamp(realFrame.sourcePoseInterval, 1.0, 500.0));
    prepare.cameraNear = realFrame.constants.cameraNear;
    prepare.cameraFar = realFrame.constants.cameraFar;
    prepare.cameraFovAngleVertical = realFrame.constants.cameraVFov;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(realFrame.depth, FfxState(realFrame.depthState));
    prepare.motionVectors = ffxApiGetResourceDX12(realFrame.velocity, FfxState(realFrame.velocityState));

    auto result = FfxApiProxy::D3D12_Dispatch(&_context, &prepare.header);
    if (result != FFX_API_RETURN_OK)
    {
        LOG_WARN("HybridTimewarp: FSR prepare failed: {:X}", (UINT) result);
        return false;
    }

    ffxDispatchDescFrameGeneration dispatch {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    dispatch.commandList = commandList;
    dispatch.frameID = frameId;
    dispatch.generationRect = { 0, 0, static_cast<int>(_displayWidth), static_cast<int>(_displayHeight) };
    dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance[0] = 0.0001f;
    dispatch.minMaxLuminance[1] = 1000.0f;
    dispatch.numGeneratedFrames = 1;
    dispatch.outputs[0] = ffxApiGetResourceDX12(generatedFrame.color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.presentColor = ffxApiGetResourceDX12(realFrame.color, FfxState(realFrame.colorState));
    dispatch.reset = reset || cut;
    result = FfxApiProxy::D3D12_Dispatch(&_context, &dispatch.header);
    if (result != FFX_API_RETURN_OK)
    {
        LOG_WARN("HybridTimewarp: FSR generation failed: {:X}", (UINT) result);
        return false;
    }
    generatedFrame.colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return !dispatch.reset;
}

void HybridFsrGenerator::Reset() { _cutGeneration = 0; }

void HybridFsrGenerator::Shutdown()
{
    if (_context != nullptr)
        FfxApiProxy::D3D12_DestroyContext(&_context, nullptr);
    _context = nullptr;
    _device = nullptr;
    _displayWidth = 0;
    _displayHeight = 0;
    _format = DXGI_FORMAT_UNKNOWN;
    _cutGeneration = 0;
}

HybridFsrGenerator::~HybridFsrGenerator() { Shutdown(); }
