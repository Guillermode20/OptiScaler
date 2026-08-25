#include "pch.h"
#include "AReproj_Dx12.h"
#include "Kcd2Camera.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

#include <State.h>
#include <Config.h>
#include <Util.h>
#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <misc/FrameLimit.h>
#include <nvapi/fakenvapi.h>
#include <wrapped/wrapped_swapchain.h>
#include <menu/input/input_system.h>

#include <magic_enum.hpp>

const char* AReproj_Dx12::Name() { return "Async Timewarp"; }

feature_version AReproj_Dx12::Version() { return feature_version { 1, 0, 0 }; }

HWND AReproj_Dx12::Hwnd() { return _hwnd; }

void* AReproj_Dx12::FrameGenerationContext() { return nullptr; }

void* AReproj_Dx12::SwapchainContext() { return nullptr; }

bool AReproj_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount)
{
    // Reprojection count is selected adaptively from cadence and ReprojMaxWarpFrames.
    return true;
}

void AReproj_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

HRESULT AReproj_Dx12::PresentFrame(UINT SyncInterval, UINT Flags, bool interpolated)
{
    if (_swapChain == nullptr)
        return E_FAIL;

    // Route through the hooked vtable with the skip flag set, so hkFGPresent passes
    // straight through to the original present (no recursion, no double handling).
    FGHooks::SkipPresent(true);
    auto result = _swapChain->Present(SyncInterval, Flags);
    FGHooks::SkipPresent(false);

    if (result == S_OK)
    {
        LOG_DEBUG("Presented frame, SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
        fakenvapi::reportFGPresent(_swapChain, true, interpolated);
    }
    else
        LOG_DEBUG("Present result: {:X}", (UINT) result);

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

    return result;
}

bool AReproj_Dx12::SubmitSCCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || !_scCommandListResetted[fIndex])
        return true;

    auto queue = _presentQueue != nullptr ? _presentQueue : _gameCommandQueue;
    if (queue == nullptr)
    {
        LOG_ERROR("Can't submit SC command list, queue is nullptr");
        return false;
    }

    LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
    auto closeResult = _scCommandList[fIndex]->Close();

    if (closeResult != S_OK)
    {
        LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);
        return false;
    }

    queue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
    _scCommandListResetted[fIndex] = false;

    // Queue the signal after the command list. ID3D12Fence::Signal only changes the
    // CPU-side completed value and would let us reset this allocator while the GPU is
    // still reading it.
    if (_scFence != nullptr)
    {
        const auto result = queue->Signal(_scFence, _scAllocatorFenceValues[fIndex]);
        if (FAILED(result))
        {
            LOG_ERROR("Reproj: SC fence signal failed. slot {}, fence {}, result {:X}", fIndex,
                      _scAllocatorFenceValues[fIndex], (UINT) result);
            return false;
        }
    }

    return true;
}

bool AReproj_Dx12::WaitForSCAllocator(int fIndex)
{
    if (_scFence == nullptr || _scFenceEvent == nullptr)
        return true;

    const auto fenceValue = _scAllocatorFenceValues[fIndex];
    if (fenceValue == 0)
        return true;

    if (_scFence->GetCompletedValue() >= fenceValue)
        return true;

    if (FAILED(_scFence->SetEventOnCompletion(fenceValue, _scFenceEvent)))
    {
        LOG_ERROR("SC allocator fence SetEventOnCompletion failed. slot {}, fence {}", fIndex, fenceValue);
        return false;
    }

    if (WaitForSingleObject(_scFenceEvent, 5000) != WAIT_OBJECT_0)
    {
        LOG_ERROR("SC allocator fence wait failed. slot {}, fence {}, completed {}", fIndex, fenceValue,
                  _scFence->GetCompletedValue());
        return false;
    }

    return true;
}

DXGI_FORMAT AReproj_Dx12::NormalizeReprojFormat(DXGI_FORMAT format)
{
    format = WrappedIDXGISwapChain4::ReprojectionResourceFormat(format);
    if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    return format;
}

bool AReproj_Dx12::CopyLastFrame(int fIndex, ID3D12Resource* source)
{
    if (source == nullptr)
        return false;

    // CreateBufferResource reuses _lastColor when format/size match (leaving it in the
    // state the previous warp put it in), but a (re)created resource starts in COPY_DEST.
    ID3D12Resource* oldLastColor = _lastColor[fIndex];

    if (!CreateBufferResource(_device, source, D3D12_RESOURCE_STATE_COPY_DEST, &_lastColor[fIndex], false, false))
        return false;

    if (_lastColor[fIndex] != oldLastColor)
        _lastColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;

    auto cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
        return false;

    // CreateBufferResource reuses _uiColor when format/size match; a new one starts in COPY_DEST.
    ID3D12Resource* oldUiColor = _uiColor[fIndex];

    // The backbuffer is expected to be in PRESENT state at present time (RUI_Dx12 does the same)
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // _lastColor may be left in NON_PIXEL_SHADER_RESOURCE by the previous warp
    ResourceBarrier(cmdList, _lastColor[fIndex], _lastColorState[fIndex], D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(_lastColor[fIndex], source);

    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    _lastColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;

    // HUD composition (sync path): warp the HUD-less frame instead of the composed
    // backbuffer so the baked-in HUD is not timewarped; UI is composited after the warp.
    _syncHasUi[fIndex] = false;
    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (hudless && ui && IsResourceReady(FG_ResourceType::HudlessColor, fIndex) &&
            IsResourceReady(FG_ResourceType::UIColor, fIndex))
        {
            const auto& hudlessDesc = hudless->GetResource()->GetDesc();
            const auto sourceDesc = source->GetDesc();
            if (hudlessDesc.Width == sourceDesc.Width && hudlessDesc.Height == sourceDesc.Height &&
                NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(sourceDesc.Format))
            {
                ResourceBarrier(cmdList, hudless->GetResource(), hudless->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmdList->CopyResource(_lastColor[fIndex], hudless->GetResource());
                ResourceBarrier(cmdList, hudless->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, hudless->state);

                if (CreateBufferResource(_device, ui->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, &_uiColor[fIndex],
                                         false, false))
                {
                    if (_uiColor[fIndex] != oldUiColor)
                        _uiColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;
                    ResourceBarrier(cmdList, ui->GetResource(), ui->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ResourceBarrier(cmdList, _uiColor[fIndex], _uiColorState[fIndex], D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(_uiColor[fIndex], ui->GetResource());
                    ResourceBarrier(cmdList, ui->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, ui->state);
                    _uiColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;
                    _syncHasUi[fIndex] = true;
                }
            }
        }
    }

    // Submit now: the copy must be queued before the real present so the warp (submitted
    // after the present) runs on the same queue after it.
    if (!SubmitUICommandList((UINT) fIndex))
    {
        LOG_ERROR("Reproj: failed to submit last-frame copy");
        return false;
    }

    return true;
}

bool AReproj_Dx12::CreateWarpOutput(int fIndex, ID3D12Resource* source)
{
    auto inDesc = source->GetDesc();

    // sRGB formats can't be used as UAVs; use the typeless parent instead. Typeless and
    // sRGB are in the same DXGI type group, so CopyResource into the backbuffer still works.
    switch (inDesc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        inDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        inDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        break;
    default:
        break;
    }

    if (_warpOutput[fIndex] != nullptr)
    {
        auto bufDesc = _warpOutput[fIndex]->GetDesc();

        if (bufDesc.Width == inDesc.Width && bufDesc.Height == inDesc.Height && bufDesc.Format == inDesc.Format)
            return true;

        SAFE_RELEASE(_warpOutput[fIndex]);
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = source->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {:X}", (UINT64) hr);
        return false;
    }

    inDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = _device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&_warpOutput[fIndex]));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateWarpOutput result: {:X}", (UINT64) hr);
        return false;
    }

    _warpOutput[fIndex]->SetName(L"Reproj_WarpOutput");

    return true;
}

bool AReproj_Dx12::IsCameraAllZero(int fIndex) const
{
    for (int i = 0; i < 3; i++)
    {
        if (_cameraPosition[fIndex][i] != 0.0f || _cameraUp[fIndex][i] != 0.0f || _cameraRight[fIndex][i] != 0.0f ||
            _cameraForward[fIndex][i] != 0.0f)
        {
            return false;
        }
    }

    return true;
}

bool AReproj_Dx12::IsPoseFresh(double timestamp, float* ageMs) const
{
    const auto age =
        timestamp > 0.0 ? std::max(0.0, Util::MillisecondsNow() - timestamp) : std::numeric_limits<double>::infinity();
    if (ageMs != nullptr)
        *ageMs = static_cast<float>(age);

    const auto maxAge = std::clamp<double>(Config::Instance()->ReprojMaxPoseAgeMs.value_or_default(), 1.0, 1000.0);
    return std::isfinite(age) && age <= maxAge;
}

bool AReproj_Dx12::HasFreshCameraPose(int fIndex, float* ageMs) const
{
    const auto prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    const bool hasCamera = _cameraVFov[fIndex] > 0.0f && _cameraAspectRatio[fIndex] > 0.0f &&
                           !IsCameraAllZero(fIndex) && !IsCameraAllZero(prevIndex);
    return hasCamera && IsPoseFresh(_cameraTimestamp[fIndex], ageMs);
}

namespace
{
float ReprojHalfToFloat(uint16_t value)
{
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 0x1fu;
    uint32_t mantissa = value & 0x3ffu;
    uint32_t bits = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            int shift = 0;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                ++shift;
            }
            mantissa &= 0x3ffu;
            bits = sign | static_cast<uint32_t>(127 - 15 - shift) << 23 | mantissa << 13;
        }
    }
    else if (exponent == 31)
        bits = sign | 0x7f800000u | mantissa << 13;
    else
        bits = sign | (exponent + 112u) << 23 | mantissa << 13;
    return std::bit_cast<float>(bits);
}

struct ReprojVec3
{
    float x;
    float y;
    float z;
};

ReprojVec3 LoadReprojVec3(const float* value) { return { value[0], value[1], value[2] }; }

ReprojVec3 NormalizeReprojVec3(ReprojVec3 value)
{
    const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
    if (lengthSquared <= 1.0e-12f)
        return {};

    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    return { value.x * inverseLength, value.y * inverseLength, value.z * inverseLength };
}

float DotReprojVec3(ReprojVec3 left, ReprojVec3 right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

ReprojVec3 ReprojTransformRow(ReprojVec3 sourceAxis, ReprojVec3 predictedRight, ReprojVec3 predictedUp,
                              ReprojVec3 predictedForward)
{
    return { DotReprojVec3(sourceAxis, predictedRight), DotReprojVec3(sourceAxis, predictedUp),
             DotReprojVec3(sourceAxis, predictedForward) };
}

ReprojVec3 ExtrapolateReprojVec3(ReprojVec3 previous, ReprojVec3 current, float scale)
{
    return NormalizeReprojVec3({ previous.x + (current.x - previous.x) * scale,
                                 previous.y + (current.y - previous.y) * scale,
                                 previous.z + (current.z - previous.z) * scale });
}

void StoreReprojVec3(float* target, ReprojVec3 value)
{
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
}

void PrepareRotationConstants(RP_Constants& constants)
{
    if (constants.mode != 2)
        return;

    const auto right = NormalizeReprojVec3(LoadReprojVec3(constants.cameraRight));
    const auto up = NormalizeReprojVec3(LoadReprojVec3(constants.cameraUp));
    const auto forward = NormalizeReprojVec3(LoadReprojVec3(constants.cameraForward));
    ReprojVec3 predictedRight {};
    ReprojVec3 predictedUp {};
    ReprojVec3 predictedForward {};

    const float scale = 1.0f + constants.timeStep;
    predictedRight = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraRight), right, scale);
    predictedUp = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraUp), up, scale);
    predictedForward = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraForward), forward, scale);

    StoreReprojVec3(constants.prevCameraRight,
                    ReprojTransformRow(right, predictedRight, predictedUp, predictedForward));
    StoreReprojVec3(constants.prevCameraUp, ReprojTransformRow(up, predictedRight, predictedUp, predictedForward));
    StoreReprojVec3(constants.prevCameraForward,
                    ReprojTransformRow(forward, predictedRight, predictedUp, predictedForward));
    constants.cameraVFov = std::tan(constants.cameraVFov * 0.5f);
}
} // namespace

void AReproj_Dx12::FillConstants(int fIndex, RP_Constants& cb)
{
    auto& state = State::Instance();
    auto config = Config::Instance();
    auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);

    cb = {};
    cb.displayWidth = (uint32_t) state.currentSwapchainDesc.BufferDesc.Width;
    cb.displayHeight = (uint32_t) state.currentSwapchainDesc.BufferDesc.Height;
    cb.mvWidth = velocity ? (uint32_t) velocity->width : 0;
    cb.mvHeight = velocity ? (uint32_t) velocity->height : 0;
    cb.strength = config->ReprojStrength.value_or_default();
    cb.mvScaleX = _mvScaleX[fIndex];
    cb.mvScaleY = _mvScaleY[fIndex];
    cb.jitterX = _jitterX[fIndex];
    cb.jitterY = _jitterY[fIndex];
    cb.invertMV = config->ReprojInvertMV.value_or_default() ? 1 : 0;
    cb.jitterCancelled = (config->ReprojUseJitterCancel.value_or_default() && IsJitteredMVs()) ? 1 : 0;
    cb.invertedDepth = IsInvertedDepth() ? 1 : 0;
    cb.mode = config->ReprojMode.value_or_default();
    cb.debugView =
        config->ReprojCenterCropDebug.value_or_default() ? 2 : (config->ReprojDebugView.value_or_default() ? 1 : 0);
    cb.hudlessSource = 0;
    cb.cameraVFov = _cameraVFov[fIndex];
    cb.cameraAspect = _cameraAspectRatio[fIndex];
    cb.cameraNear = _cameraNear[fIndex];
    cb.cameraFar = _cameraFar[fIndex];

    std::memcpy(cb.cameraPosition, _cameraPosition[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraUp, _cameraUp[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraRight, _cameraRight[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraForward, _cameraForward[fIndex], 3 * sizeof(float));

    const auto prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    std::memcpy(cb.prevCameraPosition, _cameraPosition[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraUp, _cameraUp[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraRight, _cameraRight[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraForward, _cameraForward[prevIndex], 3 * sizeof(float));

    const bool hasCamera = _cameraVFov[fIndex] > 0.0f && _cameraAspectRatio[fIndex] > 0.0f &&
                           !IsCameraAllZero(fIndex) && !IsCameraAllZero(prevIndex);
    if (!hasCamera)
        cb.mode = 0;
    else if (config->ReprojRotationOnly.value_or_default() && cb.mode != 0)
        cb.mode = 2;
}

bool AReproj_Dx12::CopyPacketResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                                      D3D12_RESOURCE_STATES sourceState, ID3D12Resource** target,
                                      D3D12_RESOURCE_STATES& targetState, const wchar_t* name)
{
    if (cmdList == nullptr || source == nullptr || target == nullptr)
        return false;

    auto desc = source->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (*target != nullptr)
    {
        const auto oldDesc = (*target)->GetDesc();
        if (oldDesc.Width != desc.Width || oldDesc.Height != desc.Height || oldDesc.Format != desc.Format ||
            oldDesc.MipLevels != desc.MipLevels || oldDesc.SampleDesc.Count != desc.SampleDesc.Count)
        {
            SAFE_RELEASE(*target);
            targetState = D3D12_RESOURCE_STATE_COMMON;
        }
    }

    if (*target == nullptr)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        const auto result = _device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(target));
        if (FAILED(result))
        {
            LOG_WARN("Reproj: packet resource creation failed: {:X}", (UINT) result);
            return false;
        }
        (*target)->SetName(name);
        targetState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    ResourceBarrier(cmdList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, *target, targetState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(*target, source);
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    targetState = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

int AReproj_Dx12::AcquirePacket()
{
    RetirePackets();
    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        auto expected = PacketState::Free;
        if (_packets[i].state.compare_exchange_strong(expected, PacketState::Capturing))
            return i;
    }
    return -1;
}

void AReproj_Dx12::RetirePackets()
{
    for (auto& packet : _packets)
    {
        if (packet.state.load() != PacketState::Retired)
            continue;

        const bool captureDone = packet.captureFenceValue == 0 || _uiFence == nullptr ||
                                 _uiFence->GetCompletedValue() >= packet.captureFenceValue;
        const bool presentDone = packet.retirementFenceValue == 0 || _scFence == nullptr ||
                                 _scFence->GetCompletedValue() >= packet.retirementFenceValue;
        if (captureDone && presentDone)
        {
            packet.state.store(PacketState::Free);
        }
    }
}

uint32_t AReproj_Dx12::PacketQueueDepth() const
{
    uint32_t depth = 0;
    for (const auto& packet : _packets)
    {
        const auto state = packet.state.load();
        depth += state == PacketState::Ready || state == PacketState::Presenting;
    }
    return depth;
}

bool AReproj_Dx12::CaptureFramePacket(int sourceIndex, int packetIndex, ID3D12Resource* gameBackBuffer,
                                      UINT virtualBufferIndex, bool warpAllowed)
{
    (void) virtualBufferIndex;
    auto& packet = _packets[packetIndex];
    auto velocity = GetResource(FG_ResourceType::Velocity, sourceIndex);
    if (gameBackBuffer == nullptr)
        return false;

    auto depth = GetResource(FG_ResourceType::Depth, sourceIndex);
    auto hudless = GetResource(FG_ResourceType::HudlessColor, sourceIndex);
    auto ui = GetResource(FG_ResourceType::UIColor, sourceIndex);

    ID3D12Resource* color = gameBackBuffer;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_PRESENT;
    packet.hasUi = false;

    if (Config::Instance()->FGDrawUIOverFG.value_or_default() && hudless && ui &&
        IsResourceReady(FG_ResourceType::HudlessColor, sourceIndex) &&
        IsResourceReady(FG_ResourceType::UIColor, sourceIndex))
    {
        const auto hudlessDesc = hudless->GetResource()->GetDesc();
        const auto backBufferDesc = gameBackBuffer->GetDesc();
        if (hudlessDesc.Width == backBufferDesc.Width && hudlessDesc.Height == backBufferDesc.Height &&
            NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(backBufferDesc.Format))
        {
            color = hudless->GetResource();
            colorState = hudless->state;
            packet.hasUi = true;
        }
    }

    auto cmdList = GetUICommandList(packetIndex);
    bool ok = cmdList != nullptr &&
              CopyPacketResource(cmdList, color, colorState, &packet.color, packet.colorState, L"Reproj_PacketColor");
    if (ok && velocity)
        ok = CopyPacketResource(cmdList, velocity->GetResource(), velocity->state, &packet.velocity,
                                packet.velocityState, L"Reproj_PacketVelocity");

    packet.hasDepth = ok && warpAllowed && Config::Instance()->ReprojUseDepth.value_or_default() && depth;
    if (packet.hasDepth)
        packet.hasDepth = CopyPacketResource(cmdList, depth->GetResource(), depth->state, &packet.depth,
                                             packet.depthState, L"Reproj_PacketDepth");

    if (ok && packet.hasUi)
    {
        packet.hasUi =
            CopyPacketResource(cmdList, ui->GetResource(), ui->state, &packet.ui, packet.uiState, L"Reproj_PacketUI");
        if (!packet.hasUi)
        {
            LOG_WARN("Reproj: UI capture failed; using the composed game frame for this packet");
            ok = CopyPacketResource(cmdList, gameBackBuffer, D3D12_RESOURCE_STATE_PRESENT, &packet.color,
                                    packet.colorState, L"Reproj_PacketColor");
        }
    }

    const bool submitted = cmdList != nullptr && SubmitUICommandList((UINT) packetIndex);
    packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex];
    if (!ok || !submitted)
        return false;

    const auto now = Util::MillisecondsNow();
    const auto rawFrameDelta =
        _lastRealFrameTimestamp > 0.0 ? now - _lastRealFrameTimestamp : State::Instance().lastFGFrameTime;
    const auto saneFrameDelta = std::clamp(rawFrameDelta, 1.0, 500.0);
    // EMA-smooth the source period and reject pacing outliers: one stalled frame
    // must not skew the warp extrapolation rate of every display of this anchor.
    if (_realPeriodEmaMs <= 0.0)
        _realPeriodEmaMs = saneFrameDelta;
    else if (saneFrameDelta < _realPeriodEmaMs * 4.0 && saneFrameDelta > _realPeriodEmaMs * 0.25)
        _realPeriodEmaMs = _realPeriodEmaMs * 0.8 + saneFrameDelta * 0.2;
    packet.frameDelta = _realPeriodEmaMs;
    packet.rawFrameDelta = saneFrameDelta;
    _lastRealFrameTimestamp = now;
    packet.renderTimestamp = now;
    FillConstants(sourceIndex, packet.constants);
    packet.constants.hudlessSource = packet.hasUi ? 1u : 0u;
    const auto colorDesc = packet.color->GetDesc();
    const float fallbackAspect = colorDesc.Height > 0 ? static_cast<float>(colorDesc.Width) / colorDesc.Height : 0.0f;
    const auto kcd2CameraTimestamp = Kcd2Camera::ApplyToConstants(packet.constants, fallbackAspect);
    const auto cameraTimestamp = kcd2CameraTimestamp > 0.0 ? kcd2CameraTimestamp : _cameraTimestamp[sourceIndex];
    // Anchor pose age is measured from the camera timestamp; without one, fall
    // back to the frame delta so MaxPoseAgeMs still rejects stale anchors.
    const double sourceTimestamp =
        cameraTimestamp > 0.0 ? cameraTimestamp : now - std::clamp(packet.frameDelta, 0.0, 150.0);
    packet.hasCamera = packet.constants.mode != 0;
    if (!packet.hasDepth && !packet.hasCamera)
        packet.constants.mode = 0;
    packet.warpAllowed = warpAllowed && velocity;
    packet.retirementFenceValue = 0;
    packet.frameId = ++_publishedFrameId;
    packet.sourcePoseTimestamp = sourceTimestamp;
    packet.syncInterval = FGHooks::LastPresentSyncInterval();
    packet.presentFlags = FGHooks::LastPresentFlags();
    return true;
}

bool AReproj_Dx12::DisplayPacket(int packetIndex, bool composeUi, uint32_t telemetryQueryStart)
{
    auto& packet = _packets[packetIndex];
    if (_swapChain == nullptr || packet.color == nullptr)
        return false;

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();
    if (!WaitForSCAllocator(outputIndex))
        return false;

    auto cmdList = GetSCCommandList(outputIndex);
    if (cmdList == nullptr)
        return false;

    _scAllocatorFenceValues[outputIndex] = ++_scFenceValue;

    // Telemetry: use sequence-indexed query if provided, otherwise fallback to outputIndex
    uint32_t queryStart = telemetryQueryStart;
    bool useTelemetryQuery = false;
    if (queryStart == UINT32_MAX && _currentTelemetrySlot && _currentTelemetrySlot->gpuQueryIndex != UINT32_MAX)
    {
        queryStart = _currentTelemetrySlot->gpuQueryIndex;
        useTelemetryQuery = true;
    }
    else if (queryStart != UINT32_MAX)
        useTelemetryQuery = true;

    if (useTelemetryQuery && _warpTimestampHeap != nullptr && queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart);

    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(outputIndex, IID_PPV_ARGS(&backBuffer))))
    {
        if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr && queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
        {
            cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1);
            cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2, _warpTimestampReadback, queryStart * sizeof(UINT64));
        }
        SubmitSCCommandList(outputIndex);
        packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
        if (_currentTelemetrySlot && useTelemetryQuery)
            _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
        return false;
    }

    ResourceBarrier(cmdList, packet.color, packet.colorState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(backBuffer, packet.color);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, packet.color, D3D12_RESOURCE_STATE_COPY_SOURCE, packet.colorState);
    backBuffer->Release();

    if (composeUi && packet.hasUi && _renderUI != nullptr && _renderUI->IsInit())
        _renderUI->Dispatch(realSwapChain, cmdList, packet.ui, packet.uiState);

    if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr && queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
    {
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1);
        cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2, _warpTimestampReadback, queryStart * sizeof(UINT64));
    }

    if (!SubmitSCCommandList(outputIndex))
        return false;

    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    return true;
}

bool AReproj_Dx12::DispatchPacketWarp(int packetIndex, float timeStep, double scanoutDeadlineMs, uint32_t telemetryQueryStart)
{
    auto& packet = _packets[packetIndex];
    if (_swapChain == nullptr || _warp == nullptr || !_warp->IsInit() || packet.velocity == nullptr ||
        !packet.warpAllowed)
        return false;

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(outputIndex, IID_PPV_ARGS(&backBuffer))))
        return false;

    if (!CreateWarpOutput(outputIndex, backBuffer) || !WaitForSCAllocator(outputIndex))
    {
        backBuffer->Release();
        return false;
    }

    auto cmdList = GetSCCommandList(outputIndex);
    if (cmdList == nullptr)
    {
        backBuffer->Release();
        return false;
    }

    UpdateWarpGpuDuration(outputIndex);
    _scAllocatorFenceValues[outputIndex] = ++_scFenceValue;

    uint32_t queryStart = telemetryQueryStart;
    bool useTelemetryQuery = false;
    if (queryStart == UINT32_MAX && _currentTelemetrySlot && _currentTelemetrySlot->gpuQueryIndex != UINT32_MAX)
    {
        queryStart = _currentTelemetrySlot->gpuQueryIndex;
        useTelemetryQuery = true;
    }
    else if (queryStart != UINT32_MAX)
        useTelemetryQuery = true;

    // Legacy fallback uses outputIndex*2, telemetry uses sequence-indexed
    const UINT timestampStart = useTelemetryQuery ? queryStart : static_cast<UINT>(outputIndex * 2);
    if (_warpTimestampHeap != nullptr && timestampStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart);
    auto constants = packet.constants;
    constants.timeStep = timeStep;
    PrepareRotationConstants(constants);
    const bool useDepth = packet.hasDepth;
    const bool ok =
        _warp->Dispatch(cmdList, packet.color, packet.colorState, packet.velocity, packet.velocityState,
                        useDepth ? packet.depth : nullptr, packet.depthState, _warpOutput[outputIndex], constants);
    if (!ok)
    {
        backBuffer->Release();
        SubmitSCCommandList(outputIndex);
        packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
        return false;
    }

    packet.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ResourceBarrier(cmdList, _warpOutput[outputIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(backBuffer, _warpOutput[outputIndex]);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, _warpOutput[outputIndex], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    backBuffer->Release();

    if (constants.debugView != 2 && packet.hasUi && _renderUI != nullptr && _renderUI->IsInit())
        _renderUI->Dispatch(realSwapChain, cmdList, packet.ui, packet.uiState);

    if (_warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr && timestampStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
    {
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart + 1);
        cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart, 2,
                                  _warpTimestampReadback, timestampStart * sizeof(UINT64));
    }

    if (!SubmitSCCommandList(outputIndex))
        return false;

    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    return true;
}

void AReproj_Dx12::UpdateWarpGpuDuration(int outputIndex)
{
    if (_warpTimestampReadback == nullptr || _presentTimestampFrequency == 0 || outputIndex < 0 ||
        outputIndex >= BUFFER_COUNT || _scAllocatorFenceValues[outputIndex] == 0 || _scFence == nullptr ||
        _scFence->GetCompletedValue() < _scAllocatorFenceValues[outputIndex])
        return;

    const SIZE_T offset = static_cast<SIZE_T>(outputIndex * 2 * sizeof(UINT64));
    D3D12_RANGE range { offset, offset + 2 * sizeof(UINT64) };
    void* mapped = nullptr;
    if (FAILED(_warpTimestampReadback->Map(0, &range, &mapped)))
        return;
    const auto* timestamps = reinterpret_cast<const UINT64*>(static_cast<const uint8_t*>(mapped) + offset);
    if (timestamps[1] >= timestamps[0])
    {
        const double durationMs = static_cast<double>(timestamps[1] - timestamps[0]) * 1000.0 /
                                  static_cast<double>(_presentTimestampFrequency);
        if (durationMs > 0.0 && durationMs < 50.0)
        {
            std::scoped_lock metricsLock(_metricsMutex);
            _warpDurationEmaMs = _warpDurationEmaMs * 0.9 + durationMs * 0.1;
            // Full-screen GPU work is not the whole submission path: command
            // recording, queue handoff and DXGI present also need headroom.
            // One millisecond repeatedly missed 120 Hz slots on Proton.
            // Clamp widened 3-5->3-8 to match PresenterMain adaptive range; UpdateWarp must not clobber growth to 8.
            _dispatchLeadMs = std::clamp(_warpDurationEmaMs + 1.0, 3.0, 8.0);
        }
    }
    D3D12_RANGE written { 0, 0 };
    _warpTimestampReadback->Unmap(0, &written);
}

bool AReproj_Dx12::DispatchWarp(int fIndex, float timeStep)
{
    if (_warp == nullptr || !_warp->IsInit())
        return false;

    auto config = Config::Instance();

    auto depth = GetResource(FG_ResourceType::Depth, fIndex);
    auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
    if (!velocity)
    {
        LOG_WARN("Reproj: no motion vectors for frame {}, skipping fake frame", _frameCount);
        return false;
    }

    IDXGISwapChain3* sc = (IDXGISwapChain3*) _swapChain;
    auto bbIndex = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))))
        return false;

    // Warp into a private UAV buffer (backbuffers don't expose UAV), then copy it into the backbuffer
    if (!CreateWarpOutput(fIndex, bb))
    {
        bb->Release();
        return false;
    }

    // Ensure the previous warp on this slot has finished before reusing its allocator
    if (!WaitForSCAllocator(fIndex))
    {
        bb->Release();
        return false;
    }

    auto cmdList = GetSCCommandList(fIndex);
    if (cmdList == nullptr)
    {
        bb->Release();
        return false;
    }

    // Assign the fence value SubmitSCCommandList will signal for this slot
    _scAllocatorFenceValues[fIndex] = ++_scFenceValue;

    RP_Constants cb {};
    FillConstants(fIndex, cb);
    cb.timeStep = timeStep;
    cb.hudlessSource = _syncHasUi[fIndex] ? 1u : 0u;

    // v2 needs depth + a valid camera pair; otherwise fall back to the MV warp
    bool hasDepth = config->ReprojUseDepth.value_or_default() && depth;
    if (!hasDepth)
        cb.mode = 0;
    PrepareRotationConstants(cb);

    bool ok = _warp->Dispatch(cmdList, _lastColor[fIndex], _lastColorState[fIndex], velocity->GetResource(),
                              velocity->state, hasDepth ? depth->GetResource() : nullptr,
                              hasDepth ? depth->state : D3D12_RESOURCE_STATE_COMMON, _warpOutput[fIndex], cb);

    if (!ok)
    {
        bb->Release();
        return false;
    }

    _lastColorState[fIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Copy the warp result into the current backbuffer, then submit everything now
    ResourceBarrier(cmdList, _warpOutput[fIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(bb, _warpOutput[fIndex]);
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, _warpOutput[fIndex], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    // Composite the captured UI unwarped on top of the warped HUD-less frame
    if (cb.debugView != 2 && _syncHasUi[fIndex] && _uiColor[fIndex] != nullptr && _renderUI != nullptr &&
        _renderUI->IsInit())
        _renderUI->Dispatch(sc, cmdList, _uiColor[fIndex], _uiColorState[fIndex]);

    bb->Release();

    if (!SubmitSCCommandList(fIndex))
    {
        LOG_ERROR("Reproj: failed to submit warp command list");
        return false;
    }

    return true;
}

double AReproj_Dx12::TargetRefreshHz()
{
    std::scoped_lock lock(_refreshMutex);
    auto config = Config::Instance();
    auto target = static_cast<double>(config->ReprojTargetRefresh.value_or_default());
    if (target <= 1.0)
        target = static_cast<double>(config->FramerateLimit.value_or_default());
    if (target > 1.0)
        return std::clamp(target, 0.0, 1000.0);

    const auto now = Util::MillisecondsNow();
    if ((now - _lastRefreshQueryMs) >= 1000.0 || _cachedRefreshHz <= 1.0)
    {
        // Prefer the swapchain's actual mode (fractional rates like 59.94/119.88,
        // VRR current rate) and fall back to the desktop display mode.
        double measured = 0.0;
        if (_swapChain != nullptr)
        {
            DXGI_SWAP_CHAIN_DESC scDesc {};
            if (SUCCEEDED(_swapChain->GetDesc(&scDesc)))
            {
                const auto& rr = scDesc.BufferDesc.RefreshRate;
                if (rr.Denominator > 0)
                    measured = static_cast<double>(rr.Numerator) / static_cast<double>(rr.Denominator);
            }
        }
        if (measured <= 1.0 && _hwnd != NULL)
            measured = static_cast<double>(Util::GetActiveRefreshRate(_hwnd));

        _cachedRefreshHz = measured;
        _lastRefreshQueryMs = now;
    }

    return std::clamp(_cachedRefreshHz, 0.0, 1000.0);
}

uint32_t AReproj_Dx12::WarpCountForPeriod(double realFrameMs, double refreshHz) const
{
    if (refreshHz <= 1.0)
        return 1;

    const auto refreshMs = 1000.0 / refreshHz;
    const auto requested = realFrameMs > refreshMs ? static_cast<int>(std::ceil(realFrameMs / refreshMs)) - 1 : 0;
    const auto maximum = std::clamp(Config::Instance()->ReprojMaxWarpFrames.value_or_default(), 1, 8);
    return static_cast<uint32_t>(std::clamp(requested, 0, maximum));
}

uint32_t AReproj_Dx12::WarpCountForFrame(double refreshHz) const
{
    return WarpCountForPeriod(State::Instance().lastFGFrameTime, refreshHz);
}

void AReproj_Dx12::WaitUntil(double deadlineMs) const
{
    const auto remaining = deadlineMs - Util::MillisecondsNow();
    if (remaining > 0.1)
        FrameLimit::sleepForMs(remaining);
}

bool AReproj_Dx12::WaitForPresenterDeadline(double deadlineMs)
{
    // High-res wait that remains stop-preemptible. condition_variable::wait_for on Wine/Proton
    // overshoots 3-10ms for 5ms sleeps (wake.p50 7-11ms in telemetry). Use FrameLimit's
    // waitable-timer (QPC-based ns) for the bulk, then spin the final 0.2ms.
    while (!_stopPresenter.load())
    {
        const auto remaining = deadlineMs - Util::MillisecondsNow();
        if (remaining <= 0.2)
            break;
        // Sleep in 5ms chunks so _stopPresenter can preempt within 5ms
        const double chunk = std::min(remaining - 0.2, 5.0);
        if (chunk > 2.0)
        {
            // Use high-res timer (QPC ns) - matches presenter display clock domain
            FrameLimit::sleepForMs(chunk);
            if (_stopPresenter.load())
                return false;
        }
        else
        {
            std::unique_lock lock(_presentMutex);
            _presentCv.wait_for(lock, std::chrono::duration<double, std::milli>(chunk),
                                [&] { return _stopPresenter.load(); });
            if (_stopPresenter.load())
                return false;
        }
    }

    while (!_stopPresenter.load() && Util::MillisecondsNow() < deadlineMs)
        YieldProcessor();

    return !_stopPresenter.load();
}

bool AReproj_Dx12::CreateAsyncPresenter()
{
    if (_presentQueue != nullptr)
        return true;
    if (_device == nullptr || _swapChain == nullptr || _asyncDowngraded)
        return false;

    auto& state = State::Instance();
    if (state.currentWrappedSwapchain == nullptr || state.currentWrappedSwapchain != state.currentFGSwapchain)
        return false;
    auto* wrapped = static_cast<WrappedIDXGISwapChain4*>(state.currentWrappedSwapchain);
    if (!wrapped->IsReprojectionVirtualized() || wrapped->RealSwapChain3() != _swapChain)
        return false;
    _wrappedSwapChain = wrapped;

    IDXGISwapChain2* realSwapChain2 = nullptr;
    if (FAILED(_swapChain->QueryInterface(IID_PPV_ARGS(&realSwapChain2))))
        return false;
    _presentWaitableObject = realSwapChain2->GetFrameLatencyWaitableObject();
    if (_presentWaitableObject != nullptr)
    {
        // VKD3D/Proton can hold a latency-1 signal until the queued frame reaches
        // scanout, leaving no time for a just-in-time warp before that refresh.
        // The software display clock prevents overproduction, so permit one
        // queued frame while the presenter prepares the next one on Proton.
        const UINT maximumFrameLatency = state.isRunningOnLinux ? 2u : 1u;
        if (FAILED(realSwapChain2->SetMaximumFrameLatency(maximumFrameLatency)))
            LOG_WARN("Reproj: failed to set maximum frame latency {}", maximumFrameLatency);
    }
    realSwapChain2->Release();
    if (_presentWaitableObject == nullptr)
        return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    auto result = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_presentQueue));
    if (FAILED(result))
    {
        LOG_WARN("Reproj: async present queue creation failed: {:X}", (UINT) result);
        return false;
    }
    _presentQueue->SetName(L"Reproj_PresentQueue");
    _presentQueue->GetTimestampFrequency(&_presentTimestampFrequency);
    _telemetry.Initialize(_presentQueue);
    _telemetry.SetTimestampResources(nullptr, nullptr, nullptr, _presentTimestampFrequency);
    D3D12_QUERY_HEAP_DESC queryDesc {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = ReprojTelemetry::TRACE_SLOT_COUNT * 2;
    if (SUCCEEDED(_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&_warpTimestampHeap))))
    {
        const auto readbackDesc = CD3DX12_RESOURCE_DESC::Buffer(ReprojTelemetry::TRACE_SLOT_COUNT * 2 * sizeof(UINT64));
        const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        if (FAILED(_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&_warpTimestampReadback))))
        {
            SAFE_RELEASE(_warpTimestampHeap);
        }
        else
        {
            _telemetry.SetTimestampResources(_warpTimestampHeap, _warpTimestampReadback, _scFence, _presentTimestampFrequency);
        }
    }
    LOG_INFO("Reproj: main-swapchain async presenter created");
    return true;
}

void AReproj_Dx12::DestroyAsyncPresenter()
{
    _telemetry.Shutdown();
    _presentWaitableObject = nullptr;
    SAFE_RELEASE(_warpTimestampReadback);
    SAFE_RELEASE(_warpTimestampHeap);
    _presentTimestampFrequency = 0;
    SAFE_RELEASE(_presentQueue);
}

bool AReproj_Dx12::StartAsyncPresenter()
{
    if (!Config::Instance()->ReprojAsync.value_or_default())
        return false;
    if (_presentThread.joinable())
        return true;

    _presenterState.store(PresenterState::Starting);
    if (!CreateAsyncPresenter())
    {
        LOG_WARN("Reproj: async presenter unavailable; using safe synchronous presenter");
        DestroyAsyncPresenter();
        _presenterState.store(PresenterState::Stopped);
        return false;
    }

    if (Config::Instance()->FGDrawUIOverFG.value_or_default() && _renderUI == nullptr)
        _renderUI = std::make_unique<RUI_Dx12>("ReprojUI", _device,
                                               Config::Instance()->FGUIPremultipliedAlpha.value_or_default());

    _stopPresenter.store(false);
    {
        std::scoped_lock metricsLock(_metricsMutex);
        _lastDisplayPresentMs = 0.0;
        _presentIntervalCount = 0;
        _presentIntervalCursor = 0;
        std::fill(_presentIntervals, _presentIntervals + _countof(_presentIntervals), 0.0);
    }
    _presenterState.store(PresenterState::Running);
    _presentThread = std::thread(&AReproj_Dx12::PresenterMain, this);

    // The presenter must win CPU scheduling against the game's worker threads; a late
    // wake directly converts into a skipped vblank slot, which reads as judder.
#if defined(_WIN32)
    SetThreadPriority(_presentThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    return true;
}

void AReproj_Dx12::StopAsyncPresenter()
{
    if (!_presentThread.joinable())
        return;

    _presenterState.store(PresenterState::Stopping);
    _stopPresenter.store(true);
    _presentCv.notify_all();
    _presentThread.join();
    _presenterState.store(PresenterState::Stopped);
}

HRESULT AReproj_Dx12::WaitForPresentSlot()
{
    if (_presentWaitableObject == nullptr)
        return E_FAIL;

    const auto refreshHz = TargetRefreshHz();
    const DWORD timeout = refreshHz > 10.0 ? static_cast<DWORD>(1000.0 / refreshHz + 5.0) : 25;
    const auto waitResult = WaitForSingleObject(_presentWaitableObject, timeout);
    if (waitResult == WAIT_OBJECT_0)
        return S_OK;
    return waitResult == WAIT_TIMEOUT ? DXGI_ERROR_WAS_STILL_DRAWING : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT AReproj_Dx12::PresentCompositorFrame(UINT syncInterval, UINT flags, bool interpolated, bool waitForSlot)
{
    if (_swapChain == nullptr || _presentWaitableObject == nullptr)
        return E_FAIL;

    // The caller may consume the waitable object before late-latching and dispatch
    // so input is sampled as close to scanout as possible. Never wait both before
    // dispatch and again here: that adds up to a full refresh of latency.
    if (waitForSlot)
    {
        const auto slotResult = WaitForPresentSlot();
        if (slotResult != S_OK)
            return slotResult;
    }

    if (_presenterState.load() == PresenterState::Running)
    {
        syncInterval = 1;
        flags = 0;
    }
    UINT presentFlags = flags;
    if (syncInterval == 0)
        presentFlags |= DXGI_PRESENT_DO_NOT_WAIT;
    const auto result = _swapChain->Present(syncInterval, presentFlags);
    if (result == S_OK)
        fakenvapi::reportFGPresent(_swapChain, true, interpolated);
    else if (result == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
        Util::GetDeviceRemovedReason(_device);

    // An occluded window is a transient success state (minimize/alt-tab), not a
    // presenter failure. Treat it as an invisible-but-successful present.
    if (result == DXGI_STATUS_OCCLUDED)
        return S_OK;

    return result;
}

bool AReproj_Dx12::SampleDisplayClock(double nowMs)
{
    if (_swapChain == nullptr)
        return false;

    static const LONGLONG qpfFrequency = [] {
        LARGE_INTEGER frequency {};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart;
    }();
    if (qpfFrequency <= 0)
        return false;

    // The query is cheap, but there is no reason to run it on every ~8 ms slot.
    if (_lastStatsQueryMs > 0.0 && (nowMs - _lastStatsQueryMs) < 50.0)
        return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
    _lastStatsQueryMs = nowMs;

    DXGI_FRAME_STATISTICS stats {};
    // Wine/Proton may not implement this; keep any previously locked anchor.
    if (FAILED(_swapChain->GetFrameStatistics(&stats)) || stats.SyncQPCTime.QuadPart <= 0 ||
        stats.SyncRefreshCount == 0)
        return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;

    LARGE_INTEGER qpcNow {};
    QueryPerformanceCounter(&qpcNow);
    const auto qpcOffsetMs =
        nowMs - static_cast<double>(qpcNow.QuadPart) * 1000.0 / static_cast<double>(qpfFrequency);

    if (_lastStatsSyncRefreshCount != 0 && stats.SyncRefreshCount > _lastStatsSyncRefreshCount)
    {
        const auto refreshDelta = static_cast<double>(stats.SyncRefreshCount - _lastStatsSyncRefreshCount);
        const auto qpcDeltaMs = static_cast<double>(stats.SyncQPCTime.QuadPart - _lastStatsSyncQpc) * 1000.0 /
                                static_cast<double>(qpfFrequency);
        if (qpcDeltaMs > 0.0)
        {
            // Sanity band covers ~24-360 Hz scanout; reject compositor garbage.
            const auto measuredPeriodMs = qpcDeltaMs / refreshDelta;
            if (measuredPeriodMs > 2.7 && measuredPeriodMs < 42.0)
            {
                _measuredRefreshPeriodMs = _measuredRefreshPeriodMs > 1.0
                                               ? _measuredRefreshPeriodMs * 0.8 + measuredPeriodMs * 0.2
                                               : measuredPeriodMs;
            }
        }
    }

    _lastStatsSyncRefreshCount = stats.SyncRefreshCount;
    _lastStatsSyncQpc = stats.SyncQPCTime.QuadPart;
    _displayClockAnchorMs =
        static_cast<double>(stats.SyncQPCTime.QuadPart) * 1000.0 / static_cast<double>(qpfFrequency) + qpcOffsetMs;

    return _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
}

void AReproj_Dx12::PresenterMain()
{
    int activePacketIndex = -1;
    UINT64 activeFrame = 0;
    double nextDeadlineMs = 0.0;
    constexpr double VBLANK_SAFETY_LEAD_MS = 2.0;
    constexpr double MAX_TOTAL_EARLY_CORRECTION_MS = 16.0; // 4->16: 8ms drift needs >4ms to recover
    double totalEarlyCorrectionMs = 0.0;
    uint32_t consecutiveJammedPresents = 0;

    // Telemetry: ensure clock initialized (already in ReprojTelemetry ctor)
    // Poll calibration once at thread start if enabled.
    if (Config::Instance()->ReprojTelemetry.value_or_default())
        _telemetry.TryCalibrate();

    while (!_stopPresenter.load())
    {
        const auto refreshHz = TargetRefreshHz();
        auto refreshPeriodMs = refreshHz > 1.0 ? 1000.0 / refreshHz : 8.333;
        const auto dispatchLeadMs = std::clamp(_dispatchLeadMs, 3.0, 8.0);

        // Handle TargetRefresh change without restart: reset EMA and grid
        // so 240→120 doesn't stay stuck at 4ms period with early correction drift.
        static double lastSeenRefreshHz = 0.0;
        if (lastSeenRefreshHz > 1.0 && std::abs(refreshHz - lastSeenRefreshHz) > 0.5)
        {
            _measuredRefreshPeriodMs = refreshPeriodMs;
            _displayClockAnchorMs = 0.0;
            totalEarlyCorrectionMs = 0.0;
            _dispatchLeadMs = 3.0;
            nextDeadlineMs = 0.0;
            _lastStatsSyncRefreshCount = 0;
            _lastStatsSyncQpc = 0;
        }
        lastSeenRefreshHz = refreshHz;

        // Telemetry per-slot record (only if enabled, otherwise dummy to avoid overhead)
        ReprojSlotRecord* tSlot = nullptr;
        const bool telemetryEnabled = Config::Instance()->ReprojTelemetry.value_or_default();
        if (telemetryEnabled)
        {
            tSlot = _telemetry.BeginSlot();
            tSlot->loopBeginQpc = _telemetry.NowQpc();
            tSlot->targetRefreshHz = refreshHz;
            tSlot->configuredPeriodMs = refreshPeriodMs;
            tSlot->measuredPeriodMs = _measuredRefreshPeriodMs;
            tSlot->dispatchLeadMs = dispatchLeadMs;
            tSlot->displayClockAnchorMs = _displayClockAnchorMs;
            tSlot->totalEarlyCorrectionMs = totalEarlyCorrectionMs;
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
            tSlot->softwareDeadlineQpc = 0; // filled below when known
            // Periodically try calibration and poll completed GPU work
            _telemetry.TryCalibrate();
            _telemetry.PollCompletedGpuWork();
        }

        if (SampleDisplayClock(Util::MillisecondsNow()) && nextDeadlineMs > 0.0)
        {
            refreshPeriodMs = std::min(_measuredRefreshPeriodMs, refreshPeriodMs);
            const auto nearestVblankMs =
                _displayClockAnchorMs +
                std::round((nextDeadlineMs - _displayClockAnchorMs) / refreshPeriodMs) * refreshPeriodMs;
            const auto correctedMs = nearestVblankMs - VBLANK_SAFETY_LEAD_MS;
            if (correctedMs < nextDeadlineMs)
            {
                const auto remainingBudgetMs = MAX_TOTAL_EARLY_CORRECTION_MS + totalEarlyCorrectionMs;
                const auto appliedDeltaMs =
                    std::max(std::max(correctedMs - nextDeadlineMs, -refreshPeriodMs * 0.5), -remainingBudgetMs);
                if (appliedDeltaMs < 0.0)
                {
                    nextDeadlineMs += appliedDeltaMs;
                    totalEarlyCorrectionMs += appliedDeltaMs;
                }
            }
        }

        if (tSlot)
        {
            tSlot->measuredPeriodMs = _measuredRefreshPeriodMs;
            tSlot->dxgiStatsValid = _displayClockAnchorMs > 0.0 && _measuredRefreshPeriodMs > 1.0;
            tSlot->displayClockAnchorMs = _displayClockAnchorMs;
            tSlot->totalEarlyCorrectionMs = totalEarlyCorrectionMs;
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
        }

        if (nextDeadlineMs > 0.0)
        {
            const auto now = Util::MillisecondsNow();
            const auto lateness = now - nextDeadlineMs;
            if (lateness >= refreshPeriodMs)
            {
                const auto skipped = static_cast<uint32_t>(std::floor(lateness / refreshPeriodMs));
                nextDeadlineMs += skipped * refreshPeriodMs;
                std::scoped_lock metricsLock(_metricsMutex);
                _metricsMissedDisplaySlots += skipped;
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::SoftwareSkipped;
                    tSlot->representedSlots = skipped;
                    tSlot->skippedSlotsBeforeAttempt = skipped;
                    tSlot->softwareDeadlineQpc = tSlot->loopBeginQpc; // approx
                    tSlot->wakeTargetQpc = 0;
                    tSlot->wakeCompletedQpc = _telemetry.NowQpc();
                    tSlot->targetRefreshHz = refreshHz;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    // Publish window if due
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                    // Reset tSlot to avoid double-finalize below (create new slot next iteration)
                    tSlot = nullptr;
                    // We still need to sleep until next deadline's lead, but skip this slot's present.
                    // Continue to next iteration after deadline adjustment.
                    // Fall through to WaitForPresenterDeadline handling below with skipped accounting.
                }
            }

            constexpr double LEAD_GROW_MS = 0.5;
            constexpr double LEAD_DECAY_MS = 0.1;
            if (lateness > 0.5)
                _dispatchLeadMs = std::min(_dispatchLeadMs + LEAD_GROW_MS, 8.0);
            else if (lateness < -1.0)
                _dispatchLeadMs = std::max(_dispatchLeadMs - LEAD_DECAY_MS, 3.0);

            if (tSlot)
            {
                const double deadlineMs = nextDeadlineMs;
                const double wakeTargetMs = deadlineMs - dispatchLeadMs;
                // Convert ms to QPC using telemetry clock
                // Simpler: store ms-based deadline in QPC via Now + delta
                // We'll store softwareDeadline as QPC corresponding to nextDeadlineMs
                // Approximate: softwareDeadlineQpc = NowQpc + (deadlineMs - now) * freq/1000
                LARGE_INTEGER freq {}; QueryPerformanceFrequency(&freq);
                const int64_t deltaQpc = static_cast<int64_t>((deadlineMs - Util::MillisecondsNow()) * freq.QuadPart / 1000.0);
                tSlot->softwareDeadlineQpc = _telemetry.NowQpc() + deltaQpc;
                tSlot->wakeTargetQpc = tSlot->softwareDeadlineQpc - static_cast<int64_t>(dispatchLeadMs * freq.QuadPart / 1000.0);
            }

            const bool deadlineOk = WaitForPresenterDeadline(nextDeadlineMs - dispatchLeadMs);
            if (tSlot)
                tSlot->wakeCompletedQpc = _telemetry.NowQpc();
            if (!deadlineOk)
            {
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::PresenterStopped;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                }
                break;
            }
        }
        else if (tSlot)
        {
            tSlot->softwareDeadlineQpc = 0;
            tSlot->wakeTargetQpc = 0;
            tSlot->wakeCompletedQpc = _telemetry.NowQpc();
        }

        if (tSlot)
            tSlot->waitableBeginQpc = _telemetry.NowQpc();
        const auto slotResult = WaitForPresentSlot();
        if (tSlot)
        {
            tSlot->waitableEndQpc = _telemetry.NowQpc();
            tSlot->waitableResult = slotResult;
            if (FAILED(slotResult) && slotResult != DXGI_ERROR_WAS_STILL_DRAWING)
                tSlot->outcome = ReprojSlotOutcome::WaitableTimeout; // will be refined below
        }
        if (slotResult != S_OK)
        {
            if (slotResult != DXGI_ERROR_WAS_STILL_DRAWING)
            {
                _presenterState.store(PresenterState::Failed);
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::WaitableTimeout;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                }
            }
            else
            {
                std::scoped_lock metricsLock(_metricsMutex);
                ++_metricsMissedDisplaySlots;
                if (tSlot)
                {
                    tSlot->outcome = ReprojSlotOutcome::WaitableTimeout;
                    tSlot->primaryMissCause = ReprojMissCause::WaitableLate;
                    _telemetry.FinalizeSlot(tSlot);
                    _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                    if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                    {
                        auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                        if (_telemetry.ShouldDumpMiss(snap))
                            _telemetry.DumpMissWindow(tSlot->sequence);
                    }
                }
            }
            if (nextDeadlineMs > 0.0)
                nextDeadlineMs += refreshPeriodMs;
            continue;
        }
        if (_stopPresenter.load())
        {
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::PresenterStopped;
                _telemetry.FinalizeSlot(tSlot);
            }
            break;
        }

        if (tSlot)
            tSlot->packetSelectionQpc = _telemetry.NowQpc();

        int newestPacketIndex = -1;
        UINT64 newestFrame = activeFrame;
        uint32_t readyCount = 0;
        for (int i = 0; i < BUFFER_COUNT; ++i)
        {
            if (_packets[i].state.load() == PacketState::Ready)
            {
                ++readyCount;
                if (_packets[i].frameId > newestFrame)
                {
                    newestFrame = _packets[i].frameId;
                    newestPacketIndex = i;
                }
            }
        }

        bool newAnchor = false;
        bool captureReady = true;
        if (newestPacketIndex >= 0)
        {
            // Check capture fence ready at selection
            auto& cand = _packets[newestPacketIndex];
            if (cand.captureFenceValue != 0 && _uiFence != nullptr)
                captureReady = _uiFence->GetCompletedValue() >= cand.captureFenceValue;
            auto expected = PacketState::Ready;
            if (_packets[newestPacketIndex].state.compare_exchange_strong(expected, PacketState::Presenting))
            {
                auto& newest = _packets[newestPacketIndex];
                if (FAILED(_presentQueue->Wait(_uiFence, newest.captureFenceValue)))
                {
                    newest.state.store(PacketState::Retired);
                    _presenterState.store(PresenterState::Failed);
                    if (tSlot)
                    {
                        tSlot->outcome = ReprojSlotOutcome::FenceFailed;
                        _telemetry.FinalizeSlot(tSlot);
                    }
                    break;
                }
                if (activePacketIndex >= 0)
                    _packets[activePacketIndex].state.store(PacketState::Retired);
                activePacketIndex = newestPacketIndex;
                activeFrame = newest.frameId;
                newAnchor = true;
                for (int i = 0; i < BUFFER_COUNT; ++i)
                    if (i != activePacketIndex && _packets[i].state.load() == PacketState::Ready &&
                        _packets[i].frameId < activeFrame)
                        _packets[i].state.store(PacketState::Retired);
                _presentCv.notify_all();
            }
        }

        if (tSlot)
        {
            tSlot->anchorFrameId = activeFrame;
            tSlot->captureFenceReadyAtSelection = captureReady;
            tSlot->newAnchor = newAnchor;
            tSlot->repeatedAnchor = !newAnchor && activePacketIndex >= 0;
        }

        if (activePacketIndex < 0)
        {
            nextDeadlineMs = 0.0;
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::NoAnchor;
                _telemetry.FinalizeSlot(tSlot);
                _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
                if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
                {
                    auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                    if (_telemetry.ShouldDumpMiss(snap))
                        _telemetry.DumpMissWindow(tSlot->sequence);
                }
            }
            std::unique_lock lock(_presentMutex);
            _presentCv.wait_for(lock, std::chrono::duration<double, std::milli>(refreshPeriodMs),
                                [&] { return _stopPresenter.load() || _readyFrameId.load() > activeFrame; });
            continue;
        }

        auto& packet = _packets[activePacketIndex];
        const bool focusLost = !OptiInput::IsFocused() && !State::Instance().isRunningOnLinux;
        const auto targetDisplayMs = nextDeadlineMs > 0.0 ? nextDeadlineMs : Util::MillisecondsNow();
        const auto rawPeriod = packet.rawFrameDelta > 1.0 ? packet.rawFrameDelta : packet.frameDelta;
        const auto realPeriodMs = std::max(rawPeriod, refreshPeriodMs);
        const bool poseOriginValid = packet.hasCamera && packet.sourcePoseTimestamp > 0.0 &&
                                     packet.sourcePoseTimestamp <= targetDisplayMs;
        const auto warpOriginMs = poseOriginValid ? packet.sourcePoseTimestamp : packet.renderTimestamp;
        const auto anchorAgeMs = std::max(0.0, targetDisplayMs - warpOriginMs);
        auto maxTimeStep = std::max(0.25f, Config::Instance()->ReprojMaxTimeStep.value_or_default());
        // KCD2's rendered camera path is accurate, but source stalls can leave an anchor 2-3 frames
        // old. A hard displacement cap freezes the image mid-turn (every slot re-renders the same
        // maximum warp) and then snaps forward on anchor arrival. For KCD2 the cap instead bounds the
        // warp VELOCITY: each slot may advance at most maxTimeStep frame-units per source frame, so
        // motion continues smoothly during stalls and catches up gradually. The absolute cap keeps the
        // generic value; only growth is limited, because KCD2's natural per-slot step (age/period with
        // 16-32ms alternating frames and 27-45ms anchor ages) legitimately exceeds 1.5 in normal play.
        const bool rateLimitedWarp = Kcd2Camera::IsAvailable();
        const auto unclampedStep = static_cast<float>((anchorAgeMs / realPeriodMs) *
                                                      Config::Instance()->ReprojTimeStep.value_or_default() * 2.0f);
        auto timeStep = std::clamp(unclampedStep, 0.0f, maxTimeStep);
        if (rateLimitedWarp)
        {
            if (_warpRateFrameId != packet.frameId)
            {
                // New anchor: start from its natural age, still within the absolute cap.
                _warpRateFrameId = packet.frameId;
                _lastWarpTimeStep = timeStep;
            }
            else
            {
                // Same anchor: allow the step to grow by at most maxTimeStep frame-units per source
                // period scaled to the elapsed slot time, so rotation speed stays bounded but nonzero.
                const double slotDeltaMs = _lastDisplayPresentMs > 0.0
                                               ? std::clamp(targetDisplayMs - _lastDisplayPresentMs, 1.0,
                                                            refreshPeriodMs * 4.0)
                                               : refreshPeriodMs;
                const float growthAllowance = maxTimeStep * static_cast<float>(slotDeltaMs / realPeriodMs);
                timeStep = std::min(unclampedStep, _lastWarpTimeStep + growthAllowance);
                _lastWarpTimeStep = timeStep;
            }
        }

        if (tSlot)
        {
            tSlot->packetRenderTimestampMs = packet.renderTimestamp;
            tSlot->sourcePoseTimestampMs = packet.sourcePoseTimestamp;
            tSlot->rawCaptureIntervalMs = static_cast<float>(packet.rawFrameDelta);
            tSlot->selectedFrameIntervalMs = static_cast<float>(packet.frameDelta);
            tSlot->sourceProvidedFrameIntervalMs = static_cast<float>(State::Instance().lastFGFrameTime);
            tSlot->refreshPeriodMs = static_cast<float>(refreshPeriodMs);
            tSlot->anchorAgeMs = static_cast<float>(anchorAgeMs);
            tSlot->unclampedTimeStep = static_cast<float>((anchorAgeMs / realPeriodMs) * Config::Instance()->ReprojTimeStep.value_or_default() * 2.0f);
            tSlot->finalTimeStep = timeStep;
            tSlot->maxTimeStep = maxTimeStep;
            tSlot->timestepClamped = tSlot->unclampedTimeStep > maxTimeStep || tSlot->unclampedTimeStep < 0;
            tSlot->mvScaleX = packet.constants.mvScaleX;
            tSlot->mvScaleY = packet.constants.mvScaleY;
            tSlot->jitterX = packet.constants.jitterX;
            tSlot->jitterY = packet.constants.jitterY;
            tSlot->cameraVFov = packet.constants.cameraVFov;
            tSlot->cameraAspect = packet.constants.cameraAspect;
            tSlot->cameraNear = packet.constants.cameraNear;
            tSlot->cameraFar = packet.constants.cameraFar;
            tSlot->requestedMode = static_cast<ReprojEffectiveMode>(Config::Instance()->ReprojMode.value_or_default());
            // Record the actual constants submitted to the shader.  Packet resource
            // availability alone cannot distinguish the stable rotation-only path
            // from full depth/camera reprojection.
            if (!packet.warpAllowed)
                tSlot->effectiveMode = ReprojEffectiveMode::Unwarped;
            else if (packet.constants.mode == 2)
                tSlot->effectiveMode = ReprojEffectiveMode::RotationOnly;
            else if (packet.constants.mode == 1 && packet.hasDepth && packet.hasCamera)
                tSlot->effectiveMode = ReprojEffectiveMode::DepthCamera;
            else
                tSlot->effectiveMode = ReprojEffectiveMode::MotionVector;
            tSlot->velocityAvailable = packet.velocity != nullptr;
            tSlot->depthAvailable = packet.hasDepth;
            tSlot->cameraBasisAvailable = packet.hasCamera;
            tSlot->depthConstantsValid = packet.hasDepth && packet.constants.cameraNear > 0 && packet.constants.cameraFar > packet.constants.cameraNear && packet.constants.cameraVFov > 0;
            tSlot->cameraProjectionValid = packet.constants.cameraVFov > 0.01f && packet.constants.cameraAspect > 0.01f;
            tSlot->hudlessSource = packet.hasUi;
            tSlot->poseIntervalMs = static_cast<float>(realPeriodMs);
            // Timestamp origin
            if (packet.sourcePoseTimestamp > 0 && packet.hasCamera)
                tSlot->timestampOrigin = ReprojTimestampOrigin::CameraCallback;
            else if (packet.sourcePoseTimestamp > 0)
                tSlot->timestampOrigin = ReprojTimestampOrigin::PacketCapture;
            else
                tSlot->timestampOrigin = ReprojTimestampOrigin::FrameIntervalFallback;
            // Shadow calculations
            const double rawStep = anchorAgeMs / std::max(1.0, packet.frameDelta);
            const double emaStep = anchorAgeMs / std::max(1.0, _realPeriodEmaMs);
            // Store differences via unused fields for analysis (reusing shadow arrays in publish)
            (void)rawStep; (void)emaStep;
        }

        // GPU query reservation (sequence-indexed, non-blocking)
        uint32_t queryStart = UINT32_MAX;
        uint64_t scFenceBefore = _scFenceValue + 1;
        if (tSlot && telemetryEnabled)
        {
            queryStart = _telemetry.ReserveGpuQueries(tSlot->sequence, scFenceBefore);
            tSlot->gpuQueryIndex = queryStart;
            tSlot->scFenceValue = (queryStart != UINT32_MAX) ? scFenceBefore : 0;
            tSlot->commandRecordingBeginQpc = _telemetry.NowQpc();
            _currentTelemetrySlot = tSlot;
        }
        else
        {
            _currentTelemetrySlot = nullptr;
        }

        const bool dispatched = packet.warpAllowed && !focusLost
                                    ? DispatchPacketWarp(activePacketIndex, timeStep, targetDisplayMs, queryStart)
                                    : DisplayPacket(activePacketIndex, true, queryStart);

        if (tSlot)
        {
            tSlot->commandRecordingEndQpc = _telemetry.NowQpc();
            tSlot->queueSubmitQpc = _telemetry.NowQpc();
            if (queryStart != UINT32_MAX)
                _telemetry.OnGpuWorkSubmitted(tSlot->sequence, scFenceBefore, queryStart, tSlot->queueSubmitQpc);
            _currentTelemetrySlot = nullptr;
        }
        else
        {
            _currentTelemetrySlot = nullptr;
        }

        if (!dispatched)
        {
            LOG_ERROR("Reproj: scheduled output failed for anchor {}", packet.frameId);
            _presenterState.store(PresenterState::Failed);
            if (tSlot)
            {
                tSlot->outcome = ReprojSlotOutcome::DispatchFailed;
                _telemetry.FinalizeSlot(tSlot);
                _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
            }
            break;
        }

        if (tSlot)
            tSlot->presentBeginQpc = _telemetry.NowQpc();

        const auto presentCallStartMs = Util::MillisecondsNow();
        const auto result = PresentCompositorFrame(1, 0, !newAnchor, false);
        const auto presentedAt = Util::MillisecondsNow();
        const auto presentDurationMs = presentedAt - presentCallStartMs;
        const auto poseAge = static_cast<float>(std::max(0.0, targetDisplayMs - packet.sourcePoseTimestamp));
        RecordWarpFrame(result == S_OK, result != S_OK, poseAge);

        if (tSlot)
        {
            tSlot->presentEndQpc = _telemetry.NowQpc();
            tSlot->presentResult = result;
            tSlot->presentBlockMs = static_cast<float>(presentDurationMs);
            tSlot->presentIntervalMs = static_cast<float>(refreshPeriodMs);
            if (_lastDisplayPresentMs > 0.0)
            {
                const double interval = presentedAt - _lastDisplayPresentMs;
                tSlot->presentIntervalMs = static_cast<float>(interval);
            }
            tSlot->outcome = (result == S_OK) ? ReprojSlotOutcome::Presented : ReprojSlotOutcome::PresentFailed;
            _telemetry.FinalizeSlot(tSlot);
            _telemetry.ClassifySlot(*tSlot, refreshPeriodMs);
            if (_telemetry.ShouldPublish(_telemetry.NowQpc()))
            {
                auto snap = _telemetry.Publish(_telemetry.NowQpc(), _metricsMissedDisplaySlots);
                if (_telemetry.ShouldDumpMiss(snap))
                    _telemetry.DumpMissWindow(tSlot->sequence);
            }
        }

        if (result != S_OK)
        {
            LOG_ERROR("Reproj: display-clock present failed: {:X}", (UINT) result);
            _presenterState.store(PresenterState::Failed);
            break;
        }

        constexpr uint32_t WATCHDOG_CONSECUTIVE_JAMS = 10;
        constexpr double WATCHDOG_WEDGE_MS = 2000.0;
        const bool jammedPresent = presentDurationMs > refreshPeriodMs * 1.5;
        consecutiveJammedPresents = jammedPresent ? consecutiveJammedPresents + 1 : 0;
        if (presentDurationMs > WATCHDOG_WEDGE_MS || consecutiveJammedPresents >= WATCHDOG_CONSECUTIVE_JAMS)
        {
            LOG_ERROR("Reproj: presenter watchdog tripped: present blocked {:.2f} ms ({} consecutive jams); "
                      "downgrading to the synchronous presenter",
                      presentDurationMs, consecutiveJammedPresents);
            _presenterState.store(PresenterState::Failed);
            break;
        }

        {
            std::scoped_lock metricsLock(_metricsMutex);
            _metricsNewAnchorDisplays += newAnchor;
            _metricsRepeatedAnchorDisplays += !newAnchor;
            if (_lastDisplayPresentMs > 0.0)
            {
                const auto intervalMs = presentedAt - _lastDisplayPresentMs;
                _presentIntervals[_presentIntervalCursor] = intervalMs;
                _presentIntervalCursor = (_presentIntervalCursor + 1) % _countof(_presentIntervals);
                _presentIntervalCount = std::min<uint32_t>(_presentIntervalCount + 1, _countof(_presentIntervals));

                const auto representedSlots = static_cast<uint32_t>(std::floor(intervalMs / refreshPeriodMs + 0.5));
                if (representedSlots > 1)
                    _metricsMissedDisplaySlots += representedSlots - 1;
            }
            _lastDisplayPresentMs = presentedAt;
        }

        if (nextDeadlineMs <= 0.0)
            nextDeadlineMs = presentedAt + refreshPeriodMs;
        else
            nextDeadlineMs += refreshPeriodMs;
    }

    if (activePacketIndex >= 0)
        _packets[activePacketIndex].state.store(PacketState::Retired);

    if (_presenterState.load() != PresenterState::Failed)
        _presenterState.store(PresenterState::Stopped);
    _presentCv.notify_all();
}

bool AReproj_Dx12::DrainGpuWork()
{
    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        if (_uiCommandListResetted[i] && !SubmitUICommandList(i))
            return false;
        if (_scCommandListResetted[i] && !SubmitSCCommandList(i))
            return false;
    }

    const auto waitForFence = [](ID3D12Fence* fence, HANDLE event, UINT64 value, const char* name, int slot)
    {
        if (fence == nullptr || event == nullptr || value == 0 || fence->GetCompletedValue() >= value)
            return true;

        if (FAILED(fence->SetEventOnCompletion(value, event)) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        {
            LOG_ERROR("Reproj: timed out draining {} fence. slot {}, fence {}, completed {}", name, slot, value,
                      fence->GetCompletedValue());
            return false;
        }
        return true;
    };

    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        if (!waitForFence(_uiFence, _uiFenceEvent, _uiAllocatorFenceValues[i], "UI", i) ||
            !waitForFence(_scFence, _scFenceEvent, _scAllocatorFenceValues[i], "SC", i))
            return false;
    }

    return true;
}

void AReproj_Dx12::RecordRealFrame()
{
    std::scoped_lock lock(_metricsMutex);
    ++_metricsRealFrames;
    LogMetricsIfDue();
}

void AReproj_Dx12::RecordWarpFrame(bool warpPresented, bool dropped, float poseAgeMs)
{
    std::scoped_lock lock(_metricsMutex);
    _metricsWarpFrames += warpPresented;
    _metricsDroppedWarps += dropped;
    if (warpPresented)
    {
        _metricsPoseAgeTotalMs += poseAgeMs;
        ++_metricsPoseSamples;
    }

    LogMetricsIfDue();
}

void AReproj_Dx12::LogMetricsIfDue()
{
    const auto now = Util::MillisecondsNow();
    if (_metricsTimestamp == 0.0)
    {
        _metricsTimestamp = now;
        return;
    }

    const auto elapsed = now - _metricsTimestamp;
    if (elapsed < 1000.0)
        return;

    const auto scale = 1000.0 / elapsed;
    const auto poseAge = _metricsPoseSamples > 0 ? _metricsPoseAgeTotalMs / _metricsPoseSamples : 0.0;
    _runtimeMetrics.realFps = static_cast<float>(_metricsRealFrames * scale);
    _runtimeMetrics.warpFps = static_cast<float>(_metricsWarpFrames * scale);
    _runtimeMetrics.displayFps = _runtimeMetrics.warpFps;
    _runtimeMetrics.poseAgeMs = static_cast<float>(poseAge);
    _runtimeMetrics.targetRefreshHz = static_cast<float>(TargetRefreshHz());
    _runtimeMetrics.warpsPerReal = _metricsMaxWarpsPerReal;
    _runtimeMetrics.droppedWarps = _metricsDroppedWarps;
    _runtimeMetrics.queueDepth = PacketQueueDepth();
    _runtimeMetrics.asyncPresenter = _presenterState.load() == PresenterState::Running &&
                                     _wrappedSwapChain != nullptr && _wrappedSwapChain->IsReprojectionVirtualized();
    _runtimeMetrics.newAnchorDisplays = _metricsNewAnchorDisplays;
    _runtimeMetrics.repeatedAnchorDisplays = _metricsRepeatedAnchorDisplays;
    _runtimeMetrics.missedDisplaySlots = _metricsMissedDisplaySlots;
    if (_presentIntervalCount > 0)
    {
        std::vector<double> intervals(_presentIntervals, _presentIntervals + _presentIntervalCount);
        _runtimeMetrics.meanPresentIntervalMs =
            static_cast<float>(std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size());
        const auto p95 = intervals.begin() + static_cast<size_t>((intervals.size() - 1) * 0.95);
        std::nth_element(intervals.begin(), p95, intervals.end());
        _runtimeMetrics.p95PresentIntervalMs = static_cast<float>(*p95);
    }
    const char* presenter = _runtimeMetrics.asyncPresenter ? "async virtual swapchain" : "safe sync";
    LOG_INFO("Reproj: source={:.1f} FPS display={:.1f} FPS (new={} repeat={}) missed={} interval={:.2f}/{:.2f}ms "
             "lead={:.2f}ms poseAge={:.1f}ms queue={} pose=rendered ({}, block={:.2f}ms)",
             _metricsRealFrames * scale, _metricsWarpFrames * scale, _metricsNewAnchorDisplays,
             _metricsRepeatedAnchorDisplays, _metricsMissedDisplaySlots, _runtimeMetrics.meanPresentIntervalMs,
             _runtimeMetrics.p95PresentIntervalMs, _dispatchLeadMs, poseAge, _runtimeMetrics.queueDepth,
             presenter, _runtimeMetrics.gamePresentBlockMs);
    _metricsTimestamp = now;
    _metricsRealFrames = 0;
    _metricsWarpFrames = 0;
    _metricsDroppedWarps = 0;
    _metricsMaxWarpsPerReal = 0;
    _metricsPoseAgeTotalMs = 0.0;
    _metricsPoseSamples = 0;
    _metricsNewAnchorDisplays = 0;
    _metricsRepeatedAnchorDisplays = 0;
    _metricsMissedDisplaySlots = 0;
}

AReproj_Dx12::RuntimeMetrics AReproj_Dx12::GetRuntimeMetrics() const
{
    std::scoped_lock lock(_metricsMutex);
    return _runtimeMetrics;
}

bool AReproj_Dx12::VirtualAnchorReady() const
{
    if (_device == nullptr || _gameCommandQueue == nullptr || _uiFence == nullptr || _uiFenceEvent == nullptr ||
        _scFence == nullptr || _scFenceEvent == nullptr)
        return false;

    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        if (_uiCommandAllocator[i] == nullptr || _uiCommandList[i] == nullptr || _scCommandAllocator[i] == nullptr ||
            _scCommandList[i] == nullptr)
            return false;
    }
    return true;
}

HRESULT AReproj_Dx12::PresentVirtualFrameSync(int fIndex, ID3D12Resource* source, UINT virtualBufferIndex,
                                              UINT syncInterval, UINT flags, bool allowWarps)
{
    (void) allowWarps;
    LOG_INFO("Reproj diag: PresentVirtualFrameSync fIdx={} vb={} interval={} flags={:X}", fIndex, virtualBufferIndex,
             syncInterval, flags);
    if (_wrappedSwapChain == nullptr || source == nullptr || _presentThread.joinable() ||
        _presenterState.load() == PresenterState::Running)
        return DXGI_ERROR_INVALID_CALL;

    if (!CopyLastFrame(fIndex, source))
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync CopyLastFrame failed");
        return E_FAIL;
    }

    auto* realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto realIndex = realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* realBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(realIndex, IID_PPV_ARGS(&realBuffer))))
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync real GetBuffer({}) failed", realIndex);
        return E_FAIL;
    }

    auto* cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync no UI command list");
        realBuffer->Release();
        return E_FAIL;
    }
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(realBuffer, source);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    realBuffer->Release();

    if (!SubmitUICommandList(static_cast<UINT>(fIndex)))
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync submit failed");
        return E_FAIL;
    }
    const auto captureValue = _uiAllocatorFenceValues[fIndex];
    if (FAILED(_wrappedSwapChain->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence, captureValue)))
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync SubmitReprojectionBuffer failed");
        return E_FAIL;
    }
    const auto advanceResult = _wrappedSwapChain->AdvanceReprojectionBuffer();
    if (FAILED(advanceResult))
    {
        LOG_INFO("Reproj diag: PresentVirtualFrameSync Advance failed {:X}", (UINT) advanceResult);
        return advanceResult;
    }
    LOG_INFO("Reproj diag: PresentVirtualFrameSync presenting real frame");
    return PresentFrame(syncInterval, flags);
}

bool AReproj_Dx12::Present()
{
    LOG_FUNC();
    LOG_INFO("Reproj diag: Present() entry");
    if (_swapChain == nullptr)
        return false;

    const auto presentStart = Util::MillisecondsNow();
    const UINT syncInterval = FGHooks::LastPresentSyncInterval();
    const UINT presentFlags = FGHooks::LastPresentFlags();
    auto& state = State::Instance();
    auto* wrapped =
        state.currentWrappedSwapchain != nullptr && state.currentWrappedSwapchain == state.currentFGSwapchain
            ? static_cast<WrappedIDXGISwapChain4*>(state.currentWrappedSwapchain)
            : nullptr;
    const bool virtualized =
        wrapped != nullptr && wrapped->IsReprojectionVirtualized() && wrapped->RealSwapChain3() == _swapChain;
    if (virtualized)
        _wrappedSwapChain = wrapped;

    // A disable, context transition, or loss of virtual ownership must discard
    // the game-thread pacing grid. The next eligible publication starts fresh.
    if (!virtualized || !IsActive() || IsPaused() || !Config::Instance()->ReprojAsync.value_or_default())
        FrameLimit::paceReprojectionSource(false);

    UINT virtualBufferIndex = 0;
    ID3D12Resource* gameBackBuffer = nullptr;
    if (virtualized)
    {
        virtualBufferIndex = wrapped->GetCurrentBackBufferIndex();
        if (FAILED(wrapped->GetReprojectionBuffer(virtualBufferIndex, IID_PPV_ARGS(&gameBackBuffer))))
            return false;
    }

    if (!IsActive() || IsPaused())
    {
        HRESULT result = S_OK;
        if (virtualized)
        {
            StopAsyncPresenter();
            DrainGpuWork();
            DestroyAsyncPresenter();
            result = PresentVirtualFrameSync(GetIndexWillBeDispatched(), gameBackBuffer, virtualBufferIndex,
                                             syncInterval, presentFlags, false);
        }
        else
            result = PresentFrame(syncInterval, presentFlags);
        SAFE_RELEASE(gameBackBuffer);
        return SUCCEEDED(result);
    }

    if (_presenterState.load() == PresenterState::Failed)
    {
        FrameLimit::paceReprojectionSource(false);
        StopAsyncPresenter();
        DrainGpuWork();
        DestroyAsyncPresenter();
        _asyncDowngraded = true;
        _presenterState.store(PresenterState::Stopped);
        LOG_WARN("Reproj: async presenter failed; continuing with synchronous fallback");
    }

    // Match the input paths that publish resources ahead of the game present
    // (Streamline/FSR3/FFX API) as well as the current upscaler slot.
    const auto fIndex = GetIndexWillBeDispatched();
    _lastDispatchedFrame = _frameCount;
    RecordRealFrame();
    float poseAge = 0.0f;
    const bool useDepth = Config::Instance()->ReprojUseDepth.value_or_default() &&
                          _resourceReady[fIndex].contains(FG_ResourceType::Depth);
    const bool needsCameraPose = Config::Instance()->ReprojMode.value_or_default() != 0 && useDepth;
    const int prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    // Distinguish "camera data never arrived" (fall back to the MV warp, which both
    // dispatch paths already handle by clamping cb.mode to 0) from "camera data went
    // stale" (pause on the last safe anchor instead of warping with old pose).
    const bool cameraAvailable = needsCameraPose && _cameraVFov[fIndex] > 0.0f && _cameraAspectRatio[fIndex] > 0.0f &&
                                 !IsCameraAllZero(fIndex) && !IsCameraAllZero(prevIndex);
    const bool poseFresh = IsPoseFresh(_cameraTimestamp[fIndex], &poseAge);
    const bool focused = OptiInput::IsFocused();
    {
        std::scoped_lock lock(_metricsMutex);
        _runtimeMetrics.depthReady = useDepth;
        _runtimeMetrics.anchorStale = cameraAvailable && !poseFresh;
        _runtimeMetrics.focusLost = !focused;
        _runtimeMetrics.rotationOnly = Config::Instance()->ReprojRotationOnly.value_or_default() ||
                                       Config::Instance()->ReprojMode.value_or_default() == 2;
        _runtimeMetrics.hudWarped = !Config::Instance()->FGDrawUIOverFG.value_or_default();
    }

    // 1. Flush any pending deferred command lists (resource copies from SetResource etc.)
    if (_uiCommandListResetted[fIndex] && !SubmitUICommandList((UINT) fIndex))
        LOG_ERROR("Failed to submit pending UI command list for slot {}", fIndex);

    if (_presenterState.load() != PresenterState::Running && _scCommandListResetted[fIndex])
        SubmitSCCommandList(fIndex);

    // 2. Stall guard: no new frame data for a while, pause instead of presenting garbage
    const bool stalled = (_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData;
    if (stalled)
    {
        LOG_DEBUG("Pausing reproj (no new frame data)");
        _waitingNewFrameData = true;
    }

    _fgFramePresentId++;

    // Present the newest real frame as an unwarped anchor for reset, missing
    // velocity, or focus loss (the focus check is unreliable on Proton). Pose
    // staleness deliberately does NOT disable warping: warping an old anchor is
    // exactly ATW over a stalled renderer - the point of timewarp is that aim
    // feel is independent of the render rate.
    const bool focusLost = !focused && !State::Instance().isRunningOnLinux;
    const bool hasVelocity = _resourceReady[fIndex].contains(FG_ResourceType::Velocity);
    const bool warpAllowed = !stalled && !_reset[fIndex] && hasVelocity && !focusLost;
    if (!warpAllowed)
        LOG_DEBUG("Reproj: publishing unwarped anchor (reset:{} velocity:{} focused:{} poseAge:{:.1f}ms)",
                  _reset[fIndex], hasVelocity, focused, poseAge);

    if (virtualized && _presenterState.load() == PresenterState::Running)
    {
        auto packetIndex = AcquirePacket();
        if (packetIndex < 0)
        {
            // No free packet slot: retire completed packets and wait for one.
            RetirePackets();
            std::unique_lock lock(_presentMutex);
            _presentCv.wait_for(lock, std::chrono::milliseconds(2),
                                [&]
                                {
                                    if (_presenterState.load() == PresenterState::Failed)
                                        return true;
                                    for (const auto& candidate : _packets)
                                        if (candidate.state.load() == PacketState::Free)
                                            return true;
                                    return false;
                                });
            lock.unlock();
            packetIndex = AcquirePacket();
        }
        if (packetIndex < 0)
        {
            // Drop publication but still retire and advance the logical game buffer.
            const auto fenceValue = ++_uiFenceValue;
            _uiAllocatorFenceValues[fIndex] = fenceValue;
            if (_gameCommandQueue == nullptr || _uiFence == nullptr ||
                FAILED(_gameCommandQueue->Signal(_uiFence, fenceValue)) ||
                FAILED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence, fenceValue)) ||
                FAILED(wrapped->AdvanceReprojectionBuffer()))
                _presenterState.store(PresenterState::Failed);
            RecordWarpFrame(false, true, 0.0f);
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            _runtimeMetrics.gamePresentBlockMs = static_cast<float>(Util::MillisecondsNow() - presentStart);
            return true;
        }

        auto& packet = _packets[packetIndex];
        const bool captured = CaptureFramePacket(fIndex, packetIndex, gameBackBuffer, virtualBufferIndex, warpAllowed);
        const bool submitted = captured && SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence,
                                                                                       packet.captureFenceValue));
        const bool advanced = submitted && SUCCEEDED(wrapped->AdvanceReprojectionBuffer());
        if (captured && submitted && advanced)
        {
            packet.state.store(PacketState::Ready);
            _readyFrameId.store(packet.frameId);
            _presentCv.notify_one();
            // Pace only the virtualized game thread after its anchor is published.
            // The presenter remains display-clocked and must never be source-paced.
            FrameLimit::paceReprojectionSource(true);
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            _runtimeMetrics.gamePresentBlockMs = static_cast<float>(Util::MillisecondsNow() - presentStart);
            return true;
        }

        // Hard publication failures permanently hand the real swapchain back to the
        // game queue before displaying this same virtual frame synchronously.
        packet.state.store(PacketState::Retired);
        _presenterState.store(PresenterState::Failed);
        StopAsyncPresenter();
        DrainGpuWork();
        DestroyAsyncPresenter();
        _asyncDowngraded = true;
        _presenterState.store(PresenterState::Stopped);
        HRESULT fallbackResult = E_FAIL;
        bool ringAdvanced = advanced;
        if (submitted && !ringAdvanced)
            ringAdvanced = SUCCEEDED(wrapped->AdvanceReprojectionBuffer());
        if (captured && submitted && ringAdvanced && _gameCommandQueue != nullptr &&
            SUCCEEDED(_gameCommandQueue->Wait(_uiFence, packet.captureFenceValue)) &&
            DisplayPacket(packetIndex, true) &&
            SUCCEEDED(_gameCommandQueue->Wait(_scFence, packet.retirementFenceValue)))
            fallbackResult = PresentFrame(syncInterval, presentFlags);
        else if (!submitted)
            fallbackResult =
                PresentVirtualFrameSync(fIndex, gameBackBuffer, virtualBufferIndex, syncInterval, presentFlags, false);
        LOG_WARN("Reproj: async publication failed; synchronous downgrade result {:X}", (UINT) fallbackResult);
        SAFE_RELEASE(gameBackBuffer);
        return SUCCEEDED(fallbackResult);
    }

    // Synchronous path.  With virtualization the game source is copied to the real
    // anchor while the worker is stopped; otherwise retain the legacy raw-buffer path.
    FrameLimit::paceReprojectionSource(false);
    HRESULT realResult = E_FAIL;
    if (virtualized)
    {
        StopAsyncPresenter();
        DrainGpuWork();
        DestroyAsyncPresenter();
        realResult = PresentVirtualFrameSync(fIndex, gameBackBuffer, virtualBufferIndex, syncInterval, presentFlags,
                                             warpAllowed);
    }
    else
    {
        auto* realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
        ID3D12Resource* source = nullptr;
        if (SUCCEEDED(realSwapChain->GetBuffer(realSwapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&source))))
        {
            if (CopyLastFrame(fIndex, source))
                realResult = PresentFrame(syncInterval, presentFlags);
            source->Release();
        }
    }
    SAFE_RELEASE(gameBackBuffer);

    // 6. Only proceed to the fake frame if the real present actually displayed (S_OK).
    //    Occluded/device-removed/errors all mean we should not spin or fake.
    if (realResult != S_OK || !warpAllowed)
    {
        RecordWarpFrame(false, !warpAllowed, poseAge);
        return true;
    }

    // 7. This is deliberately synchronous. DXGI gives the next buffer back to the
    // game immediately after the real present, so a worker cannot own it safely.
    // Multiple warps are therefore bounded and emitted before returning to the game.
    const auto refreshHz = TargetRefreshHz();
    const auto warpCount = WarpCountForFrame(refreshHz);
    {
        std::scoped_lock lock(_metricsMutex);
        _metricsMaxWarpsPerReal = std::max(_metricsMaxWarpsPerReal, warpCount);
    }
    const auto sourceTimestamp = Util::MillisecondsNow();
    const auto refreshPeriodMs = refreshHz > 1.0 ? 1000.0 / refreshHz : State::Instance().lastFGFrameTime * 0.5;
    const auto realPeriodMs = std::max(State::Instance().lastFGFrameTime, refreshPeriodMs * 2.0);

    for (uint32_t warp = 1; warp <= warpCount; ++warp)
    {
        WaitUntil(sourceTimestamp + refreshPeriodMs * warp);

        // A warp is placed at its display deadline relative to the real frame. This
        // gives 1/3 and 2/3 exposure for a 40 -> 120 FPS cadence instead of repeatedly
        // using the old fixed midpoint.
        const auto configuredStep = Config::Instance()->ReprojTimeStep.value_or_default();
        const auto timeStep =
            std::clamp(static_cast<float>((refreshPeriodMs * warp) / realPeriodMs) * configuredStep * 2.0f, 0.0f, 1.0f);
        if (!DispatchWarp(fIndex, timeStep))
        {
            LOG_WARN("Reproj: failed to dispatch warp {}/{}, dropping it", warp, warpCount);
            RecordWarpFrame(false, true, 0.0f);
            continue;
        }

        UINT fakeFlags = DXGI_PRESENT_ALLOW_TEARING;
        UINT fakeInterval = 0;
        if (!State::Instance().SCAllowTearing || State::Instance().realExclusiveFullscreen)
        {
            fakeFlags = 0;
            fakeInterval = 1;
        }

        const auto fakeResult = PresentFrame(fakeInterval, fakeFlags, true);
        const auto poseTimestamp = _cameraTimestamp[fIndex] > 0.0 ? _cameraTimestamp[fIndex] : sourceTimestamp;
        const auto poseAge = static_cast<float>(std::max(0.0, Util::MillisecondsNow() - poseTimestamp));
        RecordWarpFrame(fakeResult == S_OK, fakeResult != S_OK, poseAge);
        if (fakeResult != S_OK)
            break;
    }

    return true;
}

void AReproj_Dx12::Activate()
{
    if (_isActive)
        return;

    _isActive = true;
    _lastDispatchedFrame = 0;
    {
        std::scoped_lock lock(_metricsMutex);
        _metricsTimestamp = 0.0;
        _metricsRealFrames = 0;
        _metricsWarpFrames = 0;
        _metricsDroppedWarps = 0;
        _metricsMaxWarpsPerReal = 0;
        _metricsPoseAgeTotalMs = 0.0;
        _metricsPoseSamples = 0;
        _runtimeMetrics = {};
    }
    _cachedRefreshHz = 0.0;
    _lastRefreshQueryMs = 0.0;
    _lastRealFrameTimestamp = 0.0;
    if (Config::Instance()->FGDrawUIOverFG.value_or_default() && _renderUI == nullptr)
        _renderUI = std::make_unique<RUI_Dx12>("ReprojUI", _device,
                                               Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
    const bool async = StartAsyncPresenter();
    LOG_INFO("Reproj: activated ({})", async ? "async virtual swapchain" : "synchronous");
}

void AReproj_Dx12::Deactivate()
{
    if (!_isActive)
        return;

    StopAsyncPresenter();
    if (_presentQueue != nullptr)
    {
        DrainGpuWork();
        DestroyAsyncPresenter();
    }

    // Fix freeze on disable/re-enable: clear stale async packets that were
    // left in Ready/Presenting/Retired. Their fences refer to the old
    // presenter run and will never become Free via RetirePackets, so the
    // next Activate would see queueDepth==4 and AcquirePacket would always
    // fail, dropping every new anchor and leaving PresenterMain stuck in
    // WaitableTimeout (display 0, missed 76) as observed in KCD2 21:17:43.
    // Force-free all packets and reset the publish/ready counters so the
    // new presenter starts from a clean epoch.
    for (auto& pkt : _packets)
    {
        auto st = pkt.state.load();
        if (st != PacketState::Free)
        {
            pkt.state.store(PacketState::Free);
            pkt.captureFenceValue = 0;
            pkt.retirementFenceValue = 0;
            pkt.frameId = 0;
            pkt.hasDepth = false;
            pkt.hasCamera = false;
            pkt.hasUi = false;
            pkt.warpAllowed = false;
        }
    }
    _publishedFrameId.store(0);
    _readyFrameId.store(0);
    _presenterState.store(PresenterState::Stopped);
    _currentTelemetrySlot = nullptr;

    auto fIndex = GetIndex();

    if (_uiCommandListResetted[fIndex] && _gameCommandQueue != nullptr && _uiFence != nullptr)
    {
        LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
        auto closeResult = _uiCommandList[fIndex]->Close();

        if (closeResult == S_OK)
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
        else
            LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

        _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
        _uiCommandListResetted[fIndex] = false;
    }

    _isActive = false;
    LOG_INFO("Reproj: deactivated");
}

void AReproj_Dx12::DestroyFGContext()
{
    LOG_DEBUG("");

    ResetCounters();
    _frameCount = 1;
    _lastDispatchedFrame = 0;

    Deactivate();

    // Virtual buffers belong to the swapchain, not the FG context. Keep them alive
    // across an FG context reset; the game can retain references to them until the
    // swapchain is resized or released.
    ReleaseObjects();
}

bool AReproj_Dx12::Shutdown()
{
    Deactivate();

    if (_swapChain != nullptr)
        ReleaseSwapchain(_hwnd);

    ReleaseObjects();

    return true;
}

bool AReproj_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                   IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("Reproj swapchain already created for the same output window!");

            auto bufferCount =
                Config::Instance()->ReprojAsync.value_or_default() && desc->BufferCount < 3 ? 3 : desc->BufferCount;
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              bufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height, desc->BufferDesc.Format,
                              desc->Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating a new swapchain without releasing the old one
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            auto* oldWrappedSwapChain = State::Instance().currentWrappedSwapchain;
            ReleaseSwapchain(_hwnd);

            if (oldWrappedSwapChain != nullptr)
            {
                // FGHooks records this object as oldSwapChain and absorbs the game's
                // eventual stale Release. Do not tear the real object out from under
                // that wrapper here.
                State::Instance().currentWrappedSwapchain = nullptr;
            }
            else if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
            State::Instance().currentRealSwapchain = nullptr;
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // Async presentation needs a free game buffer while the worker owns the real
    // swapchain. The synchronous presenter blocks the game thread, so preserve the
    // game's buffer count (some engines require its original count during startup).
    const bool asyncRequested = Config::Instance()->ReprojAsync.value_or_default();
    const auto originalBufferCount = desc->BufferCount;
    const auto originalFlags = desc->Flags;
    const auto originalSwapEffect = desc->SwapEffect;

    if (asyncRequested && desc->BufferCount < 3)
        desc->BufferCount = 3;
    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (asyncRequested)
        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // FGHooks already coerces these, belt and braces
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    else if (desc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* rawSwapChain = nullptr;
    HRESULT result;
    {
        // This is the private presenter swapchain. Do not let the factory hook wrap it;
        // async mode adds exactly one wrapper below for the game-visible identity.
        ScopedSkipParentWrapping skipParentWrapping {};
        result = realFactory->CreateSwapChain(realQueue, desc, &rawSwapChain);

        if (FAILED(result) && asyncRequested)
        {
            LOG_WARN("Reproj: waitable main swapchain unavailable ({:X}); using safe synchronous presenter",
                     (UINT) result);
            desc->Flags = originalFlags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            desc->BufferCount = originalBufferCount; // keep the engine's expected buffer count
            result = realFactory->CreateSwapChain(realQueue, desc, &rawSwapChain);
        }
    }

    if (result != S_OK)
    {
        desc->BufferCount = originalBufferCount;
        desc->Flags = originalFlags;
        desc->SwapEffect = originalSwapEffect;
    }

    if (result == S_OK)
    {
        _asyncDowngraded = false;
        _gameCommandQueue = realQueue;
        _hwnd = desc->OutputWindow;
        _bufferCount = desc->BufferCount;
        if (!asyncRequested)
        {
            // The synchronous presenter owns the real swapchain directly. Wrapping it
            // makes ResizeBuffers re-enter FGHooks through the detoured real vtable.
            _swapChain = rawSwapChain;
            *swapChain = rawSwapChain;
            _wrappedSwapChain = nullptr;
            State::Instance().currentWrappedSwapchain = nullptr;
        }
        else
        {
            WrappedIDXGISwapChain4* wrapped = nullptr;
            const bool alreadyWrapped =
                SUCCEEDED(rawSwapChain->QueryInterface(__uuidof(WrappedIDXGISwapChain4), (void**) &wrapped));
            if (!alreadyWrapped)
                wrapped = new WrappedIDXGISwapChain4(rawSwapChain, realQueue, _hwnd, desc->Flags, false,
                                                     _gameBufferCount != 0 ? _gameBufferCount : originalBufferCount);

            _swapChain = wrapped->RealSwapChain3();
            _swapChain->AddRef();
            *swapChain = wrapped;
            _wrappedSwapChain = wrapped;
            State::Instance().currentWrappedSwapchain = wrapped;
            State::Instance().currentRealSwapchain = rawSwapChain;

            if (alreadyWrapped)
                wrapped->Release();

            ID3D12Device* device = nullptr;
            if (SUCCEEDED(realQueue->GetDevice(IID_PPV_ARGS(&device))))
            {
                CreateObjects(device);
                device->Release();
            }
            if (!VirtualAnchorReady() || !wrapped->InitializeReprojectionVirtualization())
                LOG_WARN("Reproj: main swapchain virtualization unavailable; using the native swapchain");
        }

        // We force ALLOW_TEARING on our swapchain, so tearing fake presents are always allowed
        State::Instance().SCAllowTearing = true;

        LOG_INFO("Reproj swapchain created: {} buffers, {}x{}", _bufferCount, desc->BufferDesc.Width,
                 desc->BufferDesc.Height);
        return true;
    }

    LOG_ERROR("Reproj swapchain creation failed: {:X}", (UINT) result);
    return false;
}

bool AReproj_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                    DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                    IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("Reproj swapchain already created for the same output window!");

            auto bufferCount =
                Config::Instance()->ReprojAsync.value_or_default() && desc->BufferCount < 3 ? 3 : desc->BufferCount;
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              bufferCount, desc->Width, desc->Height, desc->Format,
                              desc->Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating a new swapchain without releasing the old one
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            auto* oldWrappedSwapChain = State::Instance().currentWrappedSwapchain;
            ReleaseSwapchain(_hwnd);

            if (oldWrappedSwapChain != nullptr)
            {
                // FGHooks records this object as oldSwapChain and absorbs the game's
                // eventual stale Release. Do not tear the real object out from under
                // that wrapper here.
                State::Instance().currentWrappedSwapchain = nullptr;
            }
            else if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
            State::Instance().currentRealSwapchain = nullptr;
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // Async presentation needs a free game buffer while the worker owns the real
    // swapchain. The synchronous presenter blocks the game thread, so preserve the
    // game's buffer count (some engines require its original count during startup).
    const bool asyncRequested = Config::Instance()->ReprojAsync.value_or_default();
    const auto originalBufferCount = desc->BufferCount;
    const auto originalFlags = desc->Flags;
    const auto originalSwapEffect = desc->SwapEffect;

    if (asyncRequested && desc->BufferCount < 3)
        desc->BufferCount = 3;
    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    if (asyncRequested)
        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // FGHooks already coerces these, belt and braces
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    else if (desc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGIFactory2* factory2 = nullptr;
    auto result = realFactory->QueryInterface(IID_PPV_ARGS(&factory2));

    if (result != S_OK || factory2 == nullptr)
    {
        desc->BufferCount = originalBufferCount;
        desc->Flags = originalFlags;
        desc->SwapEffect = originalSwapEffect;

        LOG_ERROR("Reproj swapchain creation failed, factory does not support CreateSwapChainForHwnd: {:X}",
                  (UINT) result);
        return false;
    }

    IDXGISwapChain1* rawSwapChain = nullptr;
    {
        // Keep the private real swapchain unwrapped; the async wrapper below is the
        // only object the game should see.
        ScopedSkipParentWrapping skipParentWrapping {};
        result = factory2->CreateSwapChainForHwnd(realQueue, hwnd, desc, pFullscreenDesc, nullptr, &rawSwapChain);
        if (FAILED(result) && asyncRequested)
        {
            LOG_WARN("Reproj: waitable main swapchain unavailable ({:X}); using safe synchronous presenter",
                     (UINT) result);
            desc->Flags = originalFlags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            desc->BufferCount = originalBufferCount; // keep the engine's expected buffer count
            result = factory2->CreateSwapChainForHwnd(realQueue, hwnd, desc, pFullscreenDesc, nullptr, &rawSwapChain);
        }
    }
    factory2->Release();

    if (result != S_OK)
    {
        desc->BufferCount = originalBufferCount;
        desc->Flags = originalFlags;
        desc->SwapEffect = originalSwapEffect;
    }

    if (result == S_OK)
    {
        _asyncDowngraded = false;
        _gameCommandQueue = realQueue;
        _hwnd = hwnd;
        _bufferCount = desc->BufferCount;
        if (!asyncRequested)
        {
            // The synchronous presenter owns the real swapchain directly. Wrapping it
            // makes ResizeBuffers re-enter FGHooks through the detoured real vtable.
            _swapChain = rawSwapChain;
            *swapChain = rawSwapChain;
            _wrappedSwapChain = nullptr;
            State::Instance().currentWrappedSwapchain = nullptr;
        }
        else
        {
            WrappedIDXGISwapChain4* wrapped = nullptr;
            const bool alreadyWrapped =
                SUCCEEDED(rawSwapChain->QueryInterface(__uuidof(WrappedIDXGISwapChain4), (void**) &wrapped));
            if (!alreadyWrapped)
                wrapped = new WrappedIDXGISwapChain4(rawSwapChain, realQueue, hwnd, desc->Flags, false,
                                                     _gameBufferCount != 0 ? _gameBufferCount : originalBufferCount);

            _swapChain = wrapped->RealSwapChain3();
            _swapChain->AddRef();
            *swapChain = static_cast<IDXGISwapChain1*>(wrapped);
            _wrappedSwapChain = wrapped;
            State::Instance().currentWrappedSwapchain = wrapped;
            State::Instance().currentRealSwapchain = rawSwapChain;

            if (alreadyWrapped)
                wrapped->Release();

            ID3D12Device* device = nullptr;
            if (SUCCEEDED(realQueue->GetDevice(IID_PPV_ARGS(&device))))
            {
                CreateObjects(device);
                device->Release();
            }
            if (!VirtualAnchorReady() || !wrapped->InitializeReprojectionVirtualization())
                LOG_WARN("Reproj: main swapchain virtualization unavailable; using the native swapchain");
        }

        // We force ALLOW_TEARING on our swapchain, so tearing fake presents are always allowed
        State::Instance().SCAllowTearing = true;

        LOG_INFO("Reproj swapchain created: {} buffers, {}x{}", _bufferCount, desc->Width, desc->Height);
        return true;
    }

    LOG_ERROR("Reproj swapchain creation failed: {:X}", (UINT) result);
    return false;
}

bool AReproj_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Acquired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (!State::Instance().isShuttingDown)
        State::Instance().currentFGSwapchain = nullptr;

    if (_wrappedSwapChain != nullptr)
        _wrappedSwapChain->ShutdownReprojectionVirtualization();
    ReleaseObjects();

    if (_swapChain != nullptr)
    {
        // currentFGSwapchain is cleared above, so hkFGRelease passes through instead of recursing
        SAFE_RELEASE(_swapChain);
    }
    State::Instance().currentRealSwapchain = nullptr;
    _wrappedSwapChain = nullptr;

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

void AReproj_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    CreateObjects(device);

    if (_warp == nullptr)
        _warp = std::make_unique<RP_Dx12>("ReprojWarp", device);

    _constants = fgConstants;
}

template <auto Flag> void ReprojCheckAndUpdateFlag(auto currentFlags, std::string_view flagName)
{
    static std::optional<bool> lastState;
    bool currentState = static_cast<bool>(currentFlags & Flag);

    if (lastState.has_value() && *lastState != currentState)
    {
        LOG_DEBUG("{} changed: {}", flagName, currentState);

        State::Instance().fgChanged = true;
        State::Instance().scChanged = true;
    }

    lastState = currentState;
}

void AReproj_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    _constants = fgConstants;

    // FG swapchain is not created yet
    if (State::Instance().currentFGSwapchain == nullptr)
        return;

    if (State::Instance().isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    // Track flag changes; they require recreating our internal resources
    ReprojCheckAndUpdateFlag<FG_Flags::Hdr>(fgConstants.flags, "HDR");
    ReprojCheckAndUpdateFlag<FG_Flags::InvertedDepth>(fgConstants.flags, "Inverted Depth");
    ReprojCheckAndUpdateFlag<FG_Flags::JitteredMVs>(fgConstants.flags, "Jittered MVs");
    ReprojCheckAndUpdateFlag<FG_Flags::DisplayResolutionMVs>(fgConstants.flags, "Display Resolution MVs");

    static std::optional<bool> lastAsyncRequest;
    const bool asyncRequest = Config::Instance()->ReprojAsync.value_or_default();
    if (lastAsyncRequest.has_value() && *lastAsyncRequest != asyncRequest)
    {
        State::Instance().fgChanged = true;
        State::Instance().scChanged = true;
        LOG_WARN("Reproj: Async changed to {}; the game must recreate its swapchain for the setting to take effect",
                 asyncRequest);
    }
    lastAsyncRequest = asyncRequest;

    // The raw Dx12Upscaler config code cannot gate this path: "auto" parses to
    // Upscaler::Reset and many games resolve the actual backend only after their
    // first CreateContext call. Gate on the resolved feature when available and
    // fall back to the configured code only before the backend has materialized.
    const auto configuredBackend = Config::Instance()->Dx12Upscaler.value_or_default();
    const auto activeBackend = State::Instance().currentFeature != nullptr
                                   ? State::Instance().currentFeature->GetUpscalerType()
                                   : configuredBackend;
    const bool supportedTimewarpInput = State::Instance().activeFgInput == FGInput::Upscaler && IsFsr(activeBackend);
    // FGOutput=Reproj is the opt-in; ReprojEnabled only disables it explicitly.
    const bool reprojEnabled = Config::Instance()->ReprojEnabled.value_or_default();

    // Warn on transitions only, so an unsupported game leaves one actionable line
    // instead of (a) spam or (b) silence.
    static bool lastSupportedTimewarpInput = true;
    if (!supportedTimewarpInput && lastSupportedTimewarpInput)
    {
        LOG_WARN("Async Timewarp cannot activate: requires FGInput=upscaler and a resolved DX12 FSR/FFX upscaler "
                 "(FGInput={}, backend={} [{}])",
                 magic_enum::enum_name(State::Instance().activeFgInput), magic_enum::enum_name(activeBackend),
                 State::Instance().currentFeature != nullptr ? State::Instance().currentFeature->ShortName()
                                                             : "not resolved");
    }
    lastSupportedTimewarpInput = supportedTimewarpInput;

    if (Config::Instance()->FGEnabled.value_or_default() && reprojEnabled && supportedTimewarpInput)
    {
        if (_uiCommandAllocator[0] == nullptr || _warp == nullptr)
        {
            CreateContext(device, fgConstants);
            UpdateTarget();
        }
        else if (State::Instance().fgChanged)
        {
            Deactivate();

            // Pause for a few frames
            UpdateTarget();

            if (State::Instance().scChanged)
                DestroyFGContext();
        }

        if (_warp != nullptr && !IsPaused() && !IsActive())
            Activate();
    }
    else if (IsActive())
    {
        LOG_WARN("Async Timewarp deactivated (FGEnabled:{}, ReprojEnabled:{}, FGInput:{}, backend={} [{}])",
                 Config::Instance()->FGEnabled.value_or_default(), reprojEnabled,
                 magic_enum::enum_name(State::Instance().activeFgInput), magic_enum::enum_name(activeBackend),
                 State::Instance().currentFeature != nullptr ? State::Instance().currentFeature->ShortName()
                                                             : "not resolved");
        Deactivate();
    }

    if (State::Instance().fgChanged)
    {
        LOG_DEBUG("FG changed");

        State::Instance().fgChanged = false;

        // Pause for a few frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    State::Instance().scChanged = false;
}

bool AReproj_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources we use the provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (_frameResources[fIndex].contains(type) &&
        _frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow)
    {
        return false;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor && Config::Instance()->FGDisableUI.value_or_default())
        return false;

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    _frameResources[fIndex][type] = {};
    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    // Resource flipping fixes per-game MV/depth orientation (same as FSR-FG)
    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (fResource->type == FG_ResourceType::Velocity || fResource->type == FG_ResourceType::Depth);

    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    // Ensure the resource stays valid until Present() consumes it
    if (fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
        fResource->validity = FG_ResourceValidity::ValidNow;

    fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                              ? FG_ResourceValidity::UntilPresent
                              : FG_ResourceValidity::ValidNow;

    // Copy ValidNow resources so they survive until Present() consumes them
    if (fResource->validity == FG_ResourceValidity::ValidNow)
    {
        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex].at(type);

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        copyOutput->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());

        _resourceCopy[fIndex][type] = copyOutput;
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        LOG_TRACE("Made a copy: {:X} of input: {:X}", (size_t) fResource->copy, (size_t) fResource->resource);
    }

    SetResourceReady(type, fIndex);

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());
    return true;
}

void AReproj_Dx12::ReleaseObjects()
{
    StopAsyncPresenter();

    // A resize, scene reset, or shutdown may arrive immediately after a warp
    // submission. Command allocators, copied color, and descriptor-backed output
    // resources remain in use until their queue fence completes.
    if (!DrainGpuWork())
        LOG_WARN("Reproj: releasing objects after an incomplete GPU drain");

    DestroyAsyncPresenter();
    _warp.reset();

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        SAFE_RELEASE(_lastColor[i]);
        SAFE_RELEASE(_uiColor[i]);
        SAFE_RELEASE(_warpOutput[i]);
        SAFE_RELEASE(_packets[i].color);
        SAFE_RELEASE(_packets[i].depth);
        SAFE_RELEASE(_packets[i].velocity);
        SAFE_RELEASE(_packets[i].ui);

        _lastColorState[i] = D3D12_RESOURCE_STATE_COMMON;
        _uiColorState[i] = D3D12_RESOURCE_STATE_COMMON;
        _syncHasUi[i] = false;
        _packets[i].colorState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].depthState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].velocityState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].uiState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].captureFenceValue = 0;
        _packets[i].retirementFenceValue = 0;
        _packets[i].syncInterval = 0;
        _packets[i].presentFlags = 0;
        _packets[i].warpAllowed = false;
        _packets[i].state.store(PacketState::Free);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    SAFE_RELEASE(_uiFence);
    SAFE_RELEASE(_scFence);
    if (_uiFenceEvent != nullptr)
    {
        CloseHandle(_uiFenceEvent);
        _uiFenceEvent = nullptr;
    }
    if (_scFenceEvent != nullptr)
    {
        CloseHandle(_scFenceEvent);
        _scFenceEvent = nullptr;
    }

    _uiFenceValue = 0;
    _scFenceValue = 0;
    _publishedFrameId.store(0);
    _readyFrameId.store(0);
    _presenterState.store(PresenterState::Stopped);
}

void AReproj_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;

        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _uiCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _scCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

AReproj_Dx12::~AReproj_Dx12() { Shutdown(); }
