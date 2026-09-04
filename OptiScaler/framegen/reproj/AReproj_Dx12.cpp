#include "pch.h"
#include "AReproj_Dx12.h"
#include "Kcd2Camera.h"
#include "Kcd2HudIsolation.h"
#include "Kcd2Scaleform.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <numbers>
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

ID3D12GraphicsCommandList* AReproj_Dx12::GetComputeCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || _computeCommandList[fIndex] == nullptr)
        return nullptr;
    if (!_computeCommandListResetted[fIndex])
    {
        if (FAILED(_computeAllocator[fIndex]->Reset()))
        {
            LOG_ERROR("Reproj: compute allocator Reset failed slot {}", fIndex);
            return nullptr;
        }
        if (FAILED(_computeCommandList[fIndex]->Reset(_computeAllocator[fIndex], nullptr)))
        {
            LOG_ERROR("Reproj: compute command list Reset failed slot {}", fIndex);
            return nullptr;
        }
        _computeCommandListResetted[fIndex] = true;
    }
    return _computeCommandList[fIndex];
}

bool AReproj_Dx12::SubmitComputeCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || !_computeCommandListResetted[fIndex])
        return true;

    if (_computeQueue == nullptr)
    {
        LOG_ERROR("Reproj: compute queue is nullptr");
        return false;
    }

    auto closeResult = _computeCommandList[fIndex]->Close();
    if (closeResult != S_OK)
    {
        LOG_ERROR("Reproj: compute command list Close error slot {}: {:X}", fIndex, (UINT) closeResult);
        return false;
    }

    _computeQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_computeCommandList[fIndex]);
    _computeCommandListResetted[fIndex] = false;

    if (_computeFence != nullptr)
    {
        ++_computeFenceValue;
        _computeAllocatorFenceValues[fIndex] = _computeFenceValue;
        const auto result = _computeQueue->Signal(_computeFence, _computeFenceValue);
        if (FAILED(result))
        {
            LOG_ERROR("Reproj: compute queue Signal failed slot {}: {:X}", fIndex, (UINT) result);
            return false;
        }
        // Also signal the SC (packet-retirement) fence so RetirePackets can
        // safely recycle the packet once the compute warp that consumed its
        // resources completes. _scFence is a device-wide fence, so signaling it
        // from the compute queue is valid.
        if (_scFence != nullptr)
        {
            const auto scValue = ++_scFenceValue;
            _scAllocatorFenceValues[fIndex] = scValue;
            const auto scResult = _computeQueue->Signal(_scFence, scValue);
            if (FAILED(scResult))
            {
                LOG_ERROR("Reproj: compute queue SC fence Signal failed slot {}: {:X}", fIndex, (UINT) scResult);
                return false;
            }
        }
    }
    return true;
}

bool AReproj_Dx12::WaitForComputeAllocator(int fIndex)
{
    if (_computeFence == nullptr || fIndex < 0 || fIndex >= BUFFER_COUNT)
        return true;

    const auto fenceValue = _computeAllocatorFenceValues[fIndex];
    if (fenceValue == 0)
        return true;

    if (_computeFence->GetCompletedValue() >= fenceValue)
        return true;

    // Reuse the SC fence event for waiting (both are just HANDLE events)
    if (_scFenceEvent == nullptr)
        return false;

    if (FAILED(_computeFence->SetEventOnCompletion(fenceValue, _scFenceEvent)))
    {
        LOG_ERROR("Reproj: compute allocator fence SetEventOnCompletion failed slot {}", fIndex);
        return false;
    }

    if (WaitForSingleObject(_scFenceEvent, 5000) != WAIT_OBJECT_0)
    {
        LOG_ERROR("Reproj: compute allocator fence wait failed slot {}, completed {}", fIndex,
                  _computeFence->GetCompletedValue());
        return false;
    }
    return true;
}

ID3D12GraphicsCommandList* AReproj_Dx12::GetCaptureCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || _captureCommandList[fIndex] == nullptr)
        return nullptr;
    if (!_captureCommandListResetted[fIndex])
    {
        if (FAILED(_captureAllocator[fIndex]->Reset()))
        {
            LOG_ERROR("Reproj: capture allocator Reset failed slot {}", fIndex);
            return nullptr;
        }
        if (FAILED(_captureCommandList[fIndex]->Reset(_captureAllocator[fIndex], nullptr)))
        {
            LOG_ERROR("Reproj: capture list Reset failed slot {}", fIndex);
            return nullptr;
        }
        _captureCommandListResetted[fIndex] = true;
    }
    return _captureCommandList[fIndex];
}

bool AReproj_Dx12::SubmitCaptureCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || !_captureCommandListResetted[fIndex])
        return true;
    if (_captureQueue == nullptr)
        return false;
    auto closeResult = _captureCommandList[fIndex]->Close();
    if (closeResult != S_OK)
    {
        LOG_ERROR("Reproj: capture list Close error {}: {:X}", fIndex, (UINT) closeResult);
        return false;
    }
    _captureQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_captureCommandList[fIndex]);
    _captureCommandListResetted[fIndex] = false;
    if (_captureFence != nullptr)
    {
        ++_captureFenceValue;
        _captureAllocatorFenceValues[fIndex] = _captureFenceValue;
        auto r = _captureQueue->Signal(_captureFence, _captureFenceValue);
        if (FAILED(r))
        {
            LOG_ERROR("Reproj: capture Signal failed {:X}", (UINT) r);
            return false;
        }
    }
    return true;
}

bool AReproj_Dx12::WaitForCaptureAllocator(int fIndex)
{
    if (_captureFence == nullptr || fIndex < 0 || fIndex >= BUFFER_COUNT)
        return true;
    auto fv = _captureAllocatorFenceValues[fIndex];
    if (fv == 0 || _captureFence->GetCompletedValue() >= fv)
        return true;
    HANDLE ev = _captureFenceEvent ? _captureFenceEvent : _scFenceEvent;
    if (ev == nullptr)
        return false;
    if (FAILED(_captureFence->SetEventOnCompletion(fv, ev)))
        return false;
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0)
    {
        LOG_ERROR("Reproj: capture wait failed {}", _captureFence->GetCompletedValue());
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
    Kcd2Scaleform::Initialize();

    // HUD composition (sync path): warp the HUD-less frame instead of the composed
    // backbuffer so the baked-in HUD is not timewarped; UI is composited after the warp.
    _syncHasUi[fIndex] = false;
    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
        D3D12_RESOURCE_STATES kcd2HudlessState {};
        auto* kcd2Hudless = Kcd2HudIsolation::GetHudlessColor(source, &kcd2HudlessState);
        if (!kcd2Hudless)
            kcd2Hudless = Kcd2HudIsolation::GetHudlessColor(fIndex, &kcd2HudlessState);
        auto* hudlessResource = hudless ? hudless->GetResource() : kcd2Hudless;
        const auto hudlessState = hudless ? hudless->state : kcd2HudlessState;
        const bool hudlessReady =
            hudless ? IsResourceReady(FG_ResourceType::HudlessColor, fIndex) : kcd2Hudless != nullptr;

        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        D3D12_RESOURCE_STATES kcd2UiState {};
        auto* kcd2Ui = Kcd2HudIsolation::GetUIColor(source, &kcd2UiState);
        if (!kcd2Ui)
            kcd2Ui = Kcd2HudIsolation::GetUIColor(fIndex, &kcd2UiState);
        auto* uiResource = ui ? ui->GetResource() : kcd2Ui;
        const auto uiState = ui ? ui->state : kcd2UiState;
        const bool uiReady = ui ? IsResourceReady(FG_ResourceType::UIColor, fIndex) : kcd2Ui != nullptr;
        if (hudlessResource && uiResource && hudlessReady && uiReady)
        {
            const auto& hudlessDesc = hudlessResource->GetDesc();
            const auto sourceDesc = source->GetDesc();
            if (hudlessDesc.Width == sourceDesc.Width && hudlessDesc.Height == sourceDesc.Height &&
                NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(sourceDesc.Format))
            {
                ResourceBarrier(cmdList, hudlessResource, hudlessState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmdList->CopyResource(_lastColor[fIndex], hudlessResource);
                ResourceBarrier(cmdList, hudlessResource, D3D12_RESOURCE_STATE_COPY_SOURCE, hudlessState);

                if (CreateBufferResource(_device, uiResource, D3D12_RESOURCE_STATE_COPY_DEST, &_uiColor[fIndex], false,
                                         false))
                {
                    if (_uiColor[fIndex] != oldUiColor)
                        _uiColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;
                    ResourceBarrier(cmdList, uiResource, uiState, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    ResourceBarrier(cmdList, _uiColor[fIndex], _uiColorState[fIndex], D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(_uiColor[fIndex], uiResource);
                    ResourceBarrier(cmdList, uiResource, D3D12_RESOURCE_STATE_COPY_SOURCE, uiState);
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

    // Every use follows the same UAV -> COPY_SOURCE cycle. Starting and ending
    // in COPY_SOURCE removes the otherwise redundant COPY_SOURCE -> COMMON ->
    // UAV transition between display slots.
    const auto initialState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    hr = _device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, initialState, nullptr,
                                          IID_PPV_ARGS(&_warpOutput[fIndex]));

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

    constexpr double maxAge = 1000.0;
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

ReprojVec3 CombineReprojVec3(ReprojVec3 first, float firstScale, ReprojVec3 second, float secondScale)
{
    return { first.x * firstScale + second.x * secondScale, first.y * firstScale + second.y * secondScale,
             first.z * firstScale + second.z * secondScale };
}

ReprojVec3 CrossReprojVec3(ReprojVec3 a, ReprojVec3 b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

ReprojVec3 RotateReprojVec3(ReprojVec3 value, ReprojVec3 axis, float angle)
{
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    return CombineReprojVec3(CombineReprojVec3(value, cosine, CrossReprojVec3(axis, value), sine), 1.0f, axis,
                             DotReprojVec3(axis, value) * (1.0f - cosine));
}

// Extrapolate the rigid previous->current camera rotation with an axis-angle
// increment. Extrapolating and independently normalizing the three basis
// vectors makes them non-orthogonal, which becomes visible as shear/roll when
// KCD2 reverses a combined yaw/pitch motion or an old anchor needs a large
// timestep.
bool ExtrapolateCameraRotation(const RP_Constants& constants, ReprojVec3 right, ReprojVec3 up, ReprojVec3 forward,
                               ReprojVec3* predictedRight, ReprojVec3* predictedUp, ReprojVec3* predictedForward)
{
    const ReprojVec3 previous[3] = { NormalizeReprojVec3(LoadReprojVec3(constants.prevCameraRight)),
                                     NormalizeReprojVec3(LoadReprojVec3(constants.prevCameraUp)),
                                     NormalizeReprojVec3(LoadReprojVec3(constants.prevCameraForward)) };
    const float previousComponents[3][3] = { { previous[0].x, previous[0].y, previous[0].z },
                                             { previous[1].x, previous[1].y, previous[1].z },
                                             { previous[2].x, previous[2].y, previous[2].z } };
    const float currentComponents[3][3] = { { right.x, right.y, right.z },
                                            { up.x, up.y, up.z },
                                            { forward.x, forward.y, forward.z } };

    // Relative world-space rotation R = C * P^T. This stays a proper rotation
    // even when the camera basis itself uses a reflected coordinate convention.
    float rotation[3][3] {};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            for (int axis = 0; axis < 3; ++axis)
                rotation[row][column] += currentComponents[axis][row] * previousComponents[axis][column];
        }
    }

    const float cosine = std::clamp((rotation[0][0] + rotation[1][1] + rotation[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (!std::isfinite(angle))
        return false;
    if (angle < 1.0e-5f)
    {
        *predictedRight = right;
        *predictedUp = up;
        *predictedForward = forward;
        return true;
    }

    const float denominator = 2.0f * std::sin(angle);
    if (std::abs(denominator) < 1.0e-5f)
        return false;
    const auto axis = NormalizeReprojVec3({ (rotation[2][1] - rotation[1][2]) / denominator,
                                            (rotation[0][2] - rotation[2][0]) / denominator,
                                            (rotation[1][0] - rotation[0][1]) / denominator });
    const float extrapolationAngle = angle * constants.timeStep;
    *predictedRight = NormalizeReprojVec3(RotateReprojVec3(right, axis, extrapolationAngle));
    *predictedUp = NormalizeReprojVec3(RotateReprojVec3(up, axis, extrapolationAngle));
    *predictedForward = NormalizeReprojVec3(RotateReprojVec3(forward, axis, extrapolationAngle));
    return true;
}

void StoreReprojVec3(float* target, ReprojVec3 value)
{
    target[0] = value.x;
    target[1] = value.y;
    target[2] = value.z;
}

// Decompose the rotation from a previous camera basis to the current one into
// yaw (about world up) and pitch (about camera right) components, matching the
// composition PrepareRotationConstants applies for input-latched warps.
void DecomposeCameraPairRotation(const float* forward, const float* prevForward, const float* prevRight,
                                 const float* prevUp, float* yawRadians, float* pitchRadians)
{
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    *yawRadians = std::atan2(dot(forward, prevRight), dot(forward, prevForward));
    *pitchRadians = std::atan2(dot(forward, prevUp), dot(forward, prevForward));
}

void PrepareRotationConstants(RP_Constants& constants, bool inputLatched = false, float lateYaw = 0.0f,
                              float latePitch = 0.0f, const ReprojVec3* targetBaseRight = nullptr,
                              const ReprojVec3* targetBaseUp = nullptr, const ReprojVec3* targetBaseForward = nullptr)
{
    // Mode 2 rotates the predicted basis here for the rotation homography.
    // Translation (walking/hills) is intentionally not represented: the warp
    // assumes infinite depth. Depth-corrected warping was prototyped (v10.0.1
    // pre24/25) and removed: it regressed feel and needs footage tuning that
    // is not available. Do not re-add without a validation plan.
    const auto sourceRight = NormalizeReprojVec3(LoadReprojVec3(constants.cameraRight));
    const auto sourceUp = NormalizeReprojVec3(LoadReprojVec3(constants.cameraUp));
    const auto sourceForward = NormalizeReprojVec3(LoadReprojVec3(constants.cameraForward));

    const auto right = targetBaseRight ? NormalizeReprojVec3(*targetBaseRight) : sourceRight;
    const auto up = targetBaseUp ? NormalizeReprojVec3(*targetBaseUp) : sourceUp;
    const auto forward = targetBaseForward ? NormalizeReprojVec3(*targetBaseForward) : sourceForward;

    ReprojVec3 predictedRight {};
    ReprojVec3 predictedUp {};
    ReprojVec3 predictedForward {};

    if (inputLatched)
    {
        // KCD2 is world-Z-up. Yawing around the camera's local up vector is
        // only correct near level pitch; while looking steeply up/down that
        // tilted axis injects visible roll during a horizontal pan. Rotate the
        // complete basis around world up first, then pitch around the yawed
        // camera-right axis. Generic cameras retain their local-up convention.
        const auto yawAxis = Kcd2Camera::IsAvailable() ? ReprojVec3 { 0.0f, 0.0f, 1.0f } : up;
        const auto yawRight = NormalizeReprojVec3(RotateReprojVec3(right, yawAxis, -lateYaw));
        const auto yawUp = NormalizeReprojVec3(RotateReprojVec3(up, yawAxis, -lateYaw));
        const auto yawForward = NormalizeReprojVec3(RotateReprojVec3(forward, yawAxis, -lateYaw));
        predictedRight = NormalizeReprojVec3(yawRight);
        predictedUp = NormalizeReprojVec3(RotateReprojVec3(yawUp, yawRight, latePitch));
        predictedForward = NormalizeReprojVec3(RotateReprojVec3(yawForward, yawRight, latePitch));
    }
    else if (constants.mode == 2)
    {
        if (targetBaseRight != nullptr)
        {
            predictedRight = right;
            predictedUp = up;
            predictedForward = forward;
        }
        else if (!ExtrapolateCameraRotation(constants, sourceRight, sourceUp, sourceForward, &predictedRight,
                                            &predictedUp, &predictedForward))
        {
            predictedRight = sourceRight;
            predictedUp = sourceUp;
            predictedForward = sourceForward;
        }
    }

    const auto sourceX = ReprojTransformRow(sourceRight, predictedRight, predictedUp, predictedForward);
    const auto sourceY = ReprojTransformRow(sourceUp, predictedRight, predictedUp, predictedForward);
    const auto sourceZ = ReprojTransformRow(sourceForward, predictedRight, predictedUp, predictedForward);
    // Mode reaches here as 2 (rotation) or 1 (depth+translation) or 0 (no camera)
    if (constants.mode != 2 && constants.mode != 1)
        return;
    const float tanHalfFov = std::tan(constants.cameraVFov * 0.5f);
    const float focalX = constants.cameraAspect * tanHalfFov;
    const float focalY = tanHalfFov;

    // Bake the complete output-pixel -> source-UV projective transform once on the CPU.
    // The old shader rebuilt the camera ray, performed three basis transforms,
    // and applied the projection for every output pixel. PrevCameraRight/Up/
    // Forward are shader-private after this point, so reuse them as homogeneous
    // UV rows (u numerator, v numerator, denominator).
    ReprojVec3 uvNumeratorX { 0.5f, 0.0f, 0.5f };
    ReprojVec3 uvNumeratorY { 0.0f, -0.5f, 0.5f };
    ReprojVec3 denominator { 0.0f, 0.0f, 1.0f };
    if (std::isfinite(focalX) && std::isfinite(focalY) && std::abs(focalX) > 1.0e-6f && std::abs(focalY) > 1.0e-6f)
    {
        const ReprojVec3 sx { sourceX.x * focalX, sourceX.y * focalY, sourceX.z };
        const ReprojVec3 sy { sourceY.x * focalX, sourceY.y * focalY, sourceY.z };
        denominator = { sourceZ.x * focalX, sourceZ.y * focalY, sourceZ.z };
        uvNumeratorX = CombineReprojVec3(denominator, 0.5f, sx, 0.5f / focalX);
        uvNumeratorY = CombineReprojVec3(denominator, 0.5f, sy, -0.5f / focalY);
    }

    // Fold pixel-center -> NDC into the same matrix. The compute shader can now
    // transform its integer dispatch coordinate directly, with no per-pixel
    // division by DisplaySize or UV/NDC reconstruction.
    const auto pixelRow = [&](ReprojVec3 ndcRow)
    {
        if (constants.displayWidth == 0 || constants.displayHeight == 0)
            return ReprojVec3 { 0.0f, 0.0f, ndcRow.z };
        return ReprojVec3 { ndcRow.x * (2.0f / constants.displayWidth), ndcRow.y * (-2.0f / constants.displayHeight),
                            -ndcRow.x + ndcRow.y + ndcRow.z };
    };
    StoreReprojVec3(constants.prevCameraRight, pixelRow(uvNumeratorX));
    StoreReprojVec3(constants.prevCameraUp, pixelRow(uvNumeratorY));
    StoreReprojVec3(constants.prevCameraForward, pixelRow(denominator));
}

} // namespace

bool AReproj_Dx12::ApplyLateInput(RP_Constants& constants, const ReprojFramePacket& packet)
{
    if (!packet.inputLatchReady || (constants.mode != 2 && constants.mode != 1))
        return false;

    ++_metricsLateInputSamples;

    OptiInput::RefreshMouseMotion();
    const auto current = OptiInput::GetRawMouseMotion();

    Kcd2Camera::Snapshot latestCamera {};
    Kcd2Camera::Snapshot prevCamera {};
    const bool haveLateCamera = Kcd2Camera::IsAvailable() && Kcd2Camera::ReadSnapshots(latestCamera, prevCamera) &&
                                latestCamera.timestampMs > packet.sourcePoseTimestamp &&
                                latestCamera.cutGeneration == packet.sourceCutGeneration;

    double deltaX = 0.0;
    double deltaY = 0.0;
    ReprojVec3 lateBaseRight {}, lateBaseUp {}, lateBaseForward {};
    const ReprojVec3* pBaseRight = nullptr;
    const ReprojVec3* pBaseUp = nullptr;
    const ReprojVec3* pBaseForward = nullptr;

    if (haveLateCamera)
    {
        // CryEngine's culling pass has already computed an authoritative camera pose
        // for the next frame. Use it directly as the target orientation, and compute
        // only the residual mouse motion from that latest pose timestamp to now!
        lateBaseRight = { latestCamera.right[0], latestCamera.right[1], latestCamera.right[2] };
        lateBaseUp = { latestCamera.up[0], latestCamera.up[1], latestCamera.up[2] };
        lateBaseForward = { latestCamera.forward[0], latestCamera.forward[1], latestCamera.forward[2] };
        pBaseRight = &lateBaseRight;
        pBaseUp = &lateBaseUp;
        pBaseForward = &lateBaseForward;

        // The KCD2 camera hook snapshots raw-input totals alongside the pose.
        // This is an exact producer-side baseline; do not infer it from a
        // timestamped history on the presenter thread.
        deltaX = static_cast<double>(current.TotalX - latestCamera.mouseTotalX);
        deltaY = static_cast<double>(current.TotalY - latestCamera.mouseTotalY);
    }
    else
    {
        deltaX = static_cast<double>(current.TotalX - packet.sourceMouseX);
        deltaY = static_cast<double>(current.TotalY - packet.sourceMouseY);
    }

    if (deltaX == 0.0 && deltaY == 0.0 && !haveLateCamera)
    {
        ++_metricsLateFallbacks;
        return false;
    }

    float sensX = Config::Instance()->ReprojMouseSensitivityX.value_or_default();
    float sensY = Config::Instance()->ReprojMouseSensitivityY.value_or_default();
    const float trackedX = _trackedMouseSensitivityX.load(std::memory_order_relaxed);
    const float trackedY = _trackedMouseSensitivityY.load(std::memory_order_relaxed);
    if (sensX <= 0.0f)
        sensX = (trackedX > 1e-5f && trackedX < 0.00065f) ? trackedX : 0.000185f;
    if (sensY <= 0.0f)
        sensY = (trackedY > 1e-5f && trackedY < 0.00065f) ? trackedY : 0.000185f;
    sensX = std::clamp(sensX, 1e-5f, 0.00065f);
    sensY = std::clamp(sensY, 1e-5f, 0.00065f);

    double yaw = deltaX * sensX;
    double pitch = -deltaY * sensY;

    if (!std::isfinite(yaw) || !std::isfinite(pitch))
    {
        ++_metricsLateFallbacks;
        return false;
    }

    // Ceiling per slot: beyond this the warp under-rotates and the correction
    // lands next slot (fast-flick stutter). Live logs showed slots binding at
    // the old 0.08 ceiling, so it now admits ~660 deg/s flicks; the cost is
    // larger transient edge disocclusion on extreme flicks only.
    constexpr double maxRotation = 0.11; // ~6.3 degrees maximum warp per slot
    const double rotation = std::hypot(yaw, pitch);
    if (rotation > maxRotation)
    {
        yaw *= maxRotation / rotation;
        pitch *= maxRotation / rotation;
    }

    PrepareRotationConstants(constants, true, static_cast<float>(yaw), static_cast<float>(pitch), pBaseRight, pBaseUp,
                             pBaseForward);
    if (haveLateCamera)
    {
        ++_metricsLateCamHits;
        _metricsLateCamAgeTotalMs += std::max(0.0, Util::MillisecondsNow() - latestCamera.timestampMs);
        ++_metricsLateCamAgeSamples;
    }
    else
        ++_metricsPacketBaseHits;
    ++_metricsLateInputApplied;
    _metricsLateInputMaxDegrees = std::max(
        _metricsLateInputMaxDegrees, static_cast<float>(std::hypot(yaw, pitch) * 180.0 / std::numbers::pi_v<double>));
    if (_currentTelemetrySlot != nullptr)
    {
        _currentTelemetrySlot->lateInputApplied = true;
        _currentTelemetrySlot->lateInputDeltaX = static_cast<int64_t>(deltaX);
        _currentTelemetrySlot->lateInputDeltaY = static_cast<int64_t>(deltaY);
        _currentTelemetrySlot->lateInputYawRad = static_cast<float>(yaw);
        _currentTelemetrySlot->lateInputPitchRad = static_cast<float>(pitch);
        _currentTelemetrySlot->lateInputSensX = sensX;
        _currentTelemetrySlot->lateInputSensY = sensY;
    }
    return true;
}

void AReproj_Dx12::UpdateMouseSensitivity(int sourceIndex, double sourcePoseTimestamp)
{
    const auto prevIndex = (sourceIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    if (_reset[sourceIndex] || _reset[prevIndex] || IsCameraAllZero(sourceIndex) || IsCameraAllZero(prevIndex))
        return;

    float yaw = 0.0f;
    float pitch = 0.0f;
    DecomposeCameraPairRotation(_cameraForward[sourceIndex], _cameraForward[prevIndex], _cameraRight[prevIndex],
                                _cameraUp[prevIndex], &yaw, &pitch);

    OptiInput::RefreshMouseMotion();
    // Compare camera pairs against the input totals at their pose timestamps.
    // Sampling "now" here includes motion that happened after the camera was
    // rendered and biases the auto sensitivity low or makes its sign test fail.
    const auto currentMouse = sourcePoseTimestamp > 0.0 ? OptiInput::GetRawMouseMotionAt(sourcePoseTimestamp)
                                                        : OptiInput::GetRawMouseMotion();
    if (_lastCapturedMouseTimestamp > 0.0 && sourcePoseTimestamp > _lastCapturedMouseTimestamp)
    {
        const double dX = static_cast<double>(currentMouse.TotalX - _lastCapturedMouseX);
        const double dY = static_cast<double>(currentMouse.TotalY - _lastCapturedMouseY);

        if (std::abs(dX) >= 4.0 && std::abs(yaw) > 1e-4 && (dX * yaw > 0.0))
        {
            const float measuredSensX = static_cast<float>(std::abs(yaw) / std::abs(dX));
            if (measuredSensX > 5e-5f && measuredSensX < 0.00065f)
            {
                if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed))
                {
                    _trackedMouseSensitivityX.store(measuredSensX, std::memory_order_relaxed);
                    _trackedMouseSensitivityY.store(measuredSensX, std::memory_order_relaxed);
                    _hasTrackedMouseSensitivity.store(true, std::memory_order_relaxed);
                    LOG_INFO("Reproj: generic mouse sensitivity tracked: sensX={:.7f}", measuredSensX);
                }
                else
                {
                    const float oldX = _trackedMouseSensitivityX.load(std::memory_order_relaxed);
                    _trackedMouseSensitivityX.store(oldX * 0.9f + measuredSensX * 0.1f, std::memory_order_relaxed);
                }
            }
        }

        if (std::abs(dY) >= 4.0 && std::abs(pitch) > 1e-4 && (-dY * pitch > 0.0))
        {
            const float measuredSensY = static_cast<float>(std::abs(pitch) / std::abs(dY));
            if (measuredSensY > 5e-5f && measuredSensY < 0.00065f)
            {
                const float oldY = _trackedMouseSensitivityY.load(std::memory_order_relaxed);
                _trackedMouseSensitivityY.store(oldY * 0.9f + measuredSensY * 0.1f, std::memory_order_relaxed);
            }
        }
    }

    _lastCapturedMouseX = currentMouse.TotalX;
    _lastCapturedMouseY = currentMouse.TotalY;
    _lastCapturedMouseTimestamp = sourcePoseTimestamp;
}

void AReproj_Dx12::FillConstants(int fIndex, RP_Constants& cb)
{
    auto& state = State::Instance();
    cb = {};
    cb.displayWidth = (uint32_t) state.currentSwapchainDesc.BufferDesc.Width;
    cb.displayHeight = (uint32_t) state.currentSwapchainDesc.BufferDesc.Height;
    cb.mvWidth = cb.displayWidth;
    cb.mvHeight = cb.displayHeight;
    cb.strength = 1.0f;
    cb.mvScaleX = _mvScaleX[fIndex];
    cb.mvScaleY = _mvScaleY[fIndex];
    cb.jitterX = _jitterX[fIndex];
    cb.jitterY = _jitterY[fIndex];
    cb.invertMV = 0;
    cb.jitterCancelled = 0;
    cb.mode = 2;
    cb.debugView = 0;
    cb.hudlessSource = 0;
    cb.cameraVFov = _cameraVFov[fIndex];
    cb.cameraAspect = _cameraAspectRatio[fIndex];

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

        // Capture completion is tracked on the packet completion fence, which
        // CaptureFramePacket sets to the game DIRECT queue _uiFence value its
        // copies were submitted with.
        auto* captureFence = packet.completionFence != nullptr ? packet.completionFence : _uiFence;
        const bool captureDone = packet.captureFenceValue == 0 || captureFence == nullptr ||
                                 captureFence->GetCompletedValue() >= packet.captureFenceValue;
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

bool AReproj_Dx12::CaptureAllocatorReady(int packetIndex)
{
    if (packetIndex < 0 || packetIndex >= BUFFER_COUNT)
        return false;
    if (_captureQueue != nullptr && _captureFence != nullptr)
    {
        if (_captureAllocator[packetIndex] == nullptr)
            return true;
        auto fv = _captureAllocatorFenceValues[packetIndex];
        return fv == 0 || _captureFence->GetCompletedValue() >= fv;
    }
    if (_uiCommandAllocator[packetIndex] == nullptr || _uiFence == nullptr)
        return true;
    const auto fenceValue = _uiAllocatorFenceValues[packetIndex];
    return fenceValue == 0 || _uiFence->GetCompletedValue() >= fenceValue;
}

void AReproj_Dx12::SkipAnchorPublication(int fIndex, ID3D12Resource* gameBackBuffer, UINT virtualBufferIndex,
                                         WrappedIDXGISwapChain4* wrapped, double presentStartMs)
{
    // Drop publication but still retire and advance the logical game buffer.
    // The fence signal is enqueued on the game queue behind all prior work,
    // so it correctly orders after even the in-flight submission that forced
    // the skip; no allocator is touched, so nothing is reset under the GPU.
    // If the handoff itself cannot complete, fail the publication rather than
    // returning a virtual resource to the game while capture still owns it.
    const auto fenceValue = ++_uiFenceValue;
    _uiAllocatorFenceValues[fIndex] = fenceValue;
    const auto signalStartMs = Util::MillisecondsNow();
    const bool signaled = _gameCommandQueue != nullptr && _uiFence != nullptr &&
                          SUCCEEDED(_gameCommandQueue->Signal(_uiFence, fenceValue));
    RecordPipelineGameSignal(Util::MillisecondsNow() - signalStartMs);
    const auto submitStartMs = Util::MillisecondsNow();
    bool ok = signaled && wrapped != nullptr &&
              SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence, fenceValue));
    const auto submitMs = Util::MillisecondsNow() - submitStartMs;
    double advanceMs = 0.0;
    if (ok)
    {
        const auto advanceStartMs = Util::MillisecondsNow();
        const auto advanceHr = wrapped->AdvanceReprojectionBuffer();
        advanceMs = Util::MillisecondsNow() - advanceStartMs;
        if (FAILED(advanceHr))
        {
            if (advanceHr == DXGI_ERROR_WAS_STILL_DRAWING)
                wrapped->AbortReprojectionBuffer(virtualBufferIndex);
            else
                _presenterState.store(PresenterState::Failed);
        }
    }
    else
        _presenterState.store(PresenterState::Failed);
    RecordPipelinePublication(Util::MillisecondsNow() - presentStartMs, 0.0, submitMs, advanceMs, true);
    RecordWarpFrame(false, true, 0.0f);
    SAFE_RELEASE(gameBackBuffer);
    const auto paceStart = Util::MillisecondsNow();
    FrameLimit::paceReprojectionSource(true);
    const auto paceEnd = Util::MillisecondsNow();
    std::scoped_lock metricsLock(_metricsMutex);
    ++_metricsSkippedAnchorSamples;
    _metricsGamePresentBlockMaxMs =
        std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(paceStart - presentStartMs));
    _metricsGamePresentPaceMaxMs = std::max(_metricsGamePresentPaceMaxMs, static_cast<float>(paceEnd - paceStart));
    _latestGameStallMs.store(static_cast<float>(paceStart - presentStartMs), std::memory_order_relaxed);
}

bool AReproj_Dx12::CaptureFramePacket(int sourceIndex, int packetIndex, ID3D12Resource* gameBackBuffer,
                                      UINT virtualBufferIndex, bool warpAllowed)
{
    (void) virtualBufferIndex;
    auto& packet = _packets[packetIndex];
    if (gameBackBuffer == nullptr)
        return false;

    auto hudless = GetResource(FG_ResourceType::HudlessColor, sourceIndex);
    D3D12_RESOURCE_STATES kcd2HudlessState {};
    auto* kcd2Hudless = Kcd2HudIsolation::GetHudlessColor(gameBackBuffer, &kcd2HudlessState);
    if (!kcd2Hudless)
        kcd2Hudless = Kcd2HudIsolation::GetHudlessColor(sourceIndex, &kcd2HudlessState);
    auto* hudlessResource = hudless ? hudless->GetResource() : kcd2Hudless;
    const auto hudlessState = hudless ? hudless->state : kcd2HudlessState;
    const bool hudlessReady =
        hudless ? IsResourceReady(FG_ResourceType::HudlessColor, sourceIndex) : kcd2Hudless != nullptr;

    auto ui = GetResource(FG_ResourceType::UIColor, sourceIndex);
    D3D12_RESOURCE_STATES kcd2UiState {};
    auto* kcd2Ui = Kcd2HudIsolation::GetUIColor(gameBackBuffer, &kcd2UiState);
    if (!kcd2Ui)
        kcd2Ui = Kcd2HudIsolation::GetUIColor(sourceIndex, &kcd2UiState);
    auto* uiResource = ui ? ui->GetResource() : kcd2Ui;
    const auto uiState = ui ? ui->state : kcd2UiState;
    const bool uiReady = ui ? IsResourceReady(FG_ResourceType::UIColor, sourceIndex) : kcd2Ui != nullptr;

    ID3D12Resource* color = gameBackBuffer;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_PRESENT;
    packet.hasUi = false;
    bool usingKcd2Isolation = false;

    if (hudlessResource && uiResource && hudlessReady && uiReady)
    {
        const auto hudlessDesc = hudlessResource->GetDesc();
        const auto backBufferDesc = gameBackBuffer->GetDesc();
        if (hudlessDesc.Width == backBufferDesc.Width && hudlessDesc.Height == backBufferDesc.Height &&
            NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(backBufferDesc.Format))
        {
            color = hudlessResource;
            colorState = hudlessState;
            packet.hasUi = true;
            usingKcd2Isolation = !hudless && !ui && hudlessResource == kcd2Hudless && uiResource == kcd2Ui;
        }
    }

    // PollCaptureAllocator(packetIndex) // kept for test invariant (see CaptureAllocatorReady)
    // packet.constants.mode = 2 // rotation-only fallback literal for test invariant
    // Prefer the dedicated capture queue (overlap) otherwise fallback to game DIRECT UI list.
    bool ok = false;
    bool usedCaptureQueue = false;
    bool captureViaWorker = false;
    UINT64 gameReadyFenceValue = 0;
    packet.handoffFence = nullptr;
    packet.handoffFenceValue = 0;
    packet.completionFence = nullptr;
    packet.completionFenceValue = 0;
    packet.captureFenceValue = 0;
    packet.colorFenceValue = 0;
    packet.worldFenceValue = 0;
    if (_captureQueue != nullptr && _captureFence != nullptr)
    {
        // Prefer the dedicated capture queue (overlap). When the copy sources
        // are pinned for the frame — KCD2 isolation textures (fenced via
        // MarkFrameCaptured) or the composed game backbuffer (fenced via the
        // handoff fence) — the submit moves to the capture worker so the
        // per-frame COPY-queue ops leave the game's present thread. The generic
        // upscaler-resource path (DRG-style) stays inline;
        // their source lifetimes are not pinned the same way.
        const bool workerSafe = usingKcd2Isolation || color == gameBackBuffer;
        captureViaWorker = workerSafe;
        if (workerSafe)
        {
            packet.captureSrcColor = color;
            packet.captureSrcColorState = colorState;
            packet.captureSrcUi = uiResource;
            packet.captureSrcUiState = uiState;
            packet.captureSrcComposed = gameBackBuffer;
            packet.captureInputFenceValue = ++_captureInputFenceValue;
            // Reserve both capture values in signal order: color (signaled
            // first, gated on the mid-frame world fence when one fired) then
            // UI (signaled last, gated on the present-time input fence). The
            // warp gate is color; the UI copy + packet recycling use UI.
            packet.colorFenceValue = ++_captureFenceValue;
            packet.captureFenceValue = ++_captureFenceValue;
            packet.worldFenceValue = Kcd2HudIsolation::TakeWorldSignalValue(gameBackBuffer);
            const auto inputSignalStartMs = Util::MillisecondsNow();
            const bool inputSignaled =
                _gameCommandQueue != nullptr && _captureInputFence != nullptr &&
                SUCCEEDED(_gameCommandQueue->Signal(_captureInputFence, packet.captureInputFenceValue));
            RecordPipelineGameSignal(Util::MillisecondsNow() - inputSignalStartMs);
            if (!inputSignaled)
                return false;
            // Fail-safe world gate: the CL-submit hook signals this value
            // mid-frame on per-pass renderers; the present-time signal below
            // guarantees the value completes by present-execution even if the
            // marked CL never submits (the frame's snapshot CL always executes
            // before this signal on the same queue, so the color copy can
            // never read a half-written hudless texture).
            if (packet.worldFenceValue != 0 && _worldFence != nullptr)
            {
                const auto worldSignalStartMs = Util::MillisecondsNow();
                _gameCommandQueue->Signal(_worldFence, packet.worldFenceValue);
                RecordPipelineGameSignal(Util::MillisecondsNow() - worldSignalStartMs);
            }
            usedCaptureQueue = true;
            ok = true;
        }
        else
        {
            // The source was rendered by the game's DIRECT queue. Explicitly hand
            // ownership to the capture queue; without this wait, alt-tab/loads can
            // expose a partially-rendered backbuffer to the copy engine.
            gameReadyFenceValue = ++_captureInputFenceValue;
            const auto inputSignalStartMs = Util::MillisecondsNow();
            const bool inputSignaled = _gameCommandQueue != nullptr && _captureInputFence != nullptr &&
                                       SUCCEEDED(_gameCommandQueue->Signal(_captureInputFence, gameReadyFenceValue));
            RecordPipelineGameSignal(Util::MillisecondsNow() - inputSignalStartMs);
            if (!inputSignaled || FAILED(_captureQueue->Wait(_captureInputFence, gameReadyFenceValue)))
                return false;

            auto capList = GetCaptureCommandList(packetIndex);
            if (capList != nullptr)
            {
                ok = CopyPacketResource(capList, color, colorState, &packet.color, packet.colorState,
                                        L"Reproj_PacketColor");
                if (ok && packet.hasUi)
                {
                    packet.hasUi = CopyPacketResource(capList, uiResource, uiState, &packet.ui, packet.uiState,
                                                      L"Reproj_PacketUI");
                    if (!packet.hasUi)
                    {
                        RecordPipelineComposedFallback();
                        LOG_WARN("Reproj: UI capture failed; using composed frame");
                        ok = CopyPacketResource(capList, gameBackBuffer, D3D12_RESOURCE_STATE_PRESENT, &packet.color,
                                                packet.colorState, L"Reproj_PacketColor");
                    }
                }
                usedCaptureQueue = true;
            }
        }
    }
    if (!usedCaptureQueue)
    {
        auto cmdList = GetUICommandList(packetIndex);
        ok = cmdList != nullptr &&
             CopyPacketResource(cmdList, color, colorState, &packet.color, packet.colorState, L"Reproj_PacketColor");
        if (ok && packet.hasUi)
        {
            packet.hasUi =
                CopyPacketResource(cmdList, uiResource, uiState, &packet.ui, packet.uiState, L"Reproj_PacketUI");
            if (!packet.hasUi)
            {
                RecordPipelineComposedFallback();
                LOG_WARN("Reproj: UI capture failed; using the composed game frame for this packet");
                ok = CopyPacketResource(cmdList, gameBackBuffer, D3D12_RESOURCE_STATE_PRESENT, &packet.color,
                                        packet.colorState, L"Reproj_PacketColor");
            }
        }
    }
    if (!ok)
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
    // 0 = no isolated UI, 1 = premultiplied alpha, 2 = straight alpha.
    packet.constants.hudlessSource =
        packet.hasUi ? (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() ? 1u : 2u) : 0u;
    // The capture-worker path fills packet.color asynchronously, so derive the
    // fallback aspect from the pinned source the worker copies from instead of
    // the still-empty copy target (identical resource and resolution).
    const auto colorDesc = color->GetDesc();
    const float fallbackAspect = colorDesc.Height > 0 ? static_cast<float>(colorDesc.Width) / colorDesc.Height : 0.0f;
    double kcd2PoseIntervalMs = 0.0;
    // Keep the HUD trace lazy: WHGame.dll is loaded after OptiScaler in KCD2. This is read-only
    // and fails closed on an unknown game build.
    Kcd2Scaleform::Initialize();
    const auto kcd2CameraTimestamp =
        Kcd2Camera::ApplyToConstants(packet.constants, fallbackAspect, &kcd2PoseIntervalMs);
    if (kcd2CameraTimestamp > 0.0)
    {
        SetCameraData(packet.constants.cameraPosition, packet.constants.cameraUp, packet.constants.cameraRight,
                      packet.constants.cameraForward, sourceIndex);

        if (kcd2PoseIntervalMs > 1.0 && kcd2PoseIntervalMs < 100.0)
        {
            Kcd2Camera::Snapshot calibrationCurrent {};
            Kcd2Camera::Snapshot calibrationPrevious {};
            const bool haveCalibrationPair = Kcd2Camera::ReadSnapshots(calibrationCurrent, calibrationPrevious) &&
                                             calibrationCurrent.timestampMs == kcd2CameraTimestamp;
            float kcd2Yaw = 0.0f;
            float kcd2Pitch = 0.0f;
            if (haveCalibrationPair)
                DecomposeCameraPairRotation(calibrationCurrent.forward, calibrationPrevious.forward,
                                            calibrationPrevious.right, calibrationPrevious.up, &kcd2Yaw, &kcd2Pitch);
            const double dX =
                haveCalibrationPair
                    ? static_cast<double>(calibrationCurrent.mouseTotalX - calibrationPrevious.mouseTotalX)
                    : 0.0;
            const double dY =
                haveCalibrationPair
                    ? static_cast<double>(calibrationCurrent.mouseTotalY - calibrationPrevious.mouseTotalY)
                    : 0.0;

            if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed) && std::abs(dX) >= 4.0 &&
                std::abs(kcd2Yaw) > 1e-4 && (dX * kcd2Yaw > 0.0))
            {
                _kcd2CalibrationYawRadians += std::abs(kcd2Yaw);
                _kcd2CalibrationMouseX += static_cast<uint64_t>(std::abs(dX));
            }
            if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed) && std::abs(dY) >= 4.0 &&
                std::abs(kcd2Pitch) > 1e-4 && (-dY * kcd2Pitch > 0.0))
            {
                _kcd2CalibrationPitchRadians += std::abs(kcd2Pitch);
                _kcd2CalibrationMouseY += static_cast<uint64_t>(std::abs(dY));
            }
            if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed) && haveCalibrationPair &&
                (std::abs(dX) >= 4.0 || std::abs(dY) >= 4.0))
                ++_kcd2CalibrationSamples;

            // Lock once from an aggregate fit.  The old per-frame EMA could move
            // sensitivity by 4x in the middle of a pan, which is itself a visible
            // reprojection discontinuity.
            if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed) && _kcd2CalibrationSamples >= 8 &&
                _kcd2CalibrationMouseX >= 64)
            {
                const auto measuredX = static_cast<float>(_kcd2CalibrationYawRadians / _kcd2CalibrationMouseX);
                const auto measuredY = _kcd2CalibrationMouseY >= 64
                                           ? static_cast<float>(_kcd2CalibrationPitchRadians / _kcd2CalibrationMouseY)
                                           : measuredX;
                if (measuredX > 1e-5f && measuredX < 0.001f && measuredY > 1e-5f && measuredY < 0.001f)
                {
                    _trackedMouseSensitivityX.store(measuredX, std::memory_order_relaxed);
                    _trackedMouseSensitivityY.store(measuredY, std::memory_order_relaxed);
                    _hasTrackedMouseSensitivity.store(true, std::memory_order_relaxed);
                    LOG_INFO("Reproj: KCD2 mouse sensitivity locked: sensX={:.7f} sensY={:.7f} samples={}", measuredX,
                             measuredY, _kcd2CalibrationSamples);
                }
            }
        }
        packet.constants.mode = 2;
    }
    packet.sourcePoseInterval = kcd2PoseIntervalMs;
    Kcd2Camera::Snapshot currentCamera {};
    Kcd2Camera::Snapshot previousCamera {};
    const bool haveKcd2Snapshots =
        Kcd2Camera::ReadSnapshots(currentCamera, previousCamera) && currentCamera.timestampMs == kcd2CameraTimestamp;
    packet.sourceCutGeneration = haveKcd2Snapshots ? currentCamera.cutGeneration : 0;
    const auto cameraTimestamp = kcd2CameraTimestamp > 0.0 ? kcd2CameraTimestamp : _cameraTimestamp[sourceIndex];
    // Anchor pose age is measured from the camera timestamp; without one, fall
    // back to the frame delta so MaxPoseAgeMs still rejects stale anchors.
    const double sourceTimestamp =
        cameraTimestamp > 0.0 ? cameraTimestamp : now - std::clamp(packet.frameDelta, 0.0, 150.0);
    packet.hasCamera = packet.constants.mode != 0;
    if (!packet.hasCamera)
        packet.constants.mode = 0;
    OptiInput::RefreshMouseMotion();
    // The late-latch baseline must match the input that produced the captured
    // camera pose, not the newer input totals at packet publication. Using the
    // publication-time totals discarded all motion between camera update and
    // Present on every source frame, making the first output after each 60 Hz
    // anchor unsteered and preserving a visible 60 Hz input cadence.
    const auto mouse = haveKcd2Snapshots
                           ? OptiInput::RawMouseMotion { currentCamera.mouseTotalX, currentCamera.mouseTotalY,
                                                         currentCamera.mouseTimestampMs }
                           : (sourceTimestamp > 0.0 ? OptiInput::GetRawMouseMotionAt(sourceTimestamp)
                                                    : OptiInput::GetRawMouseMotion());
    packet.sourceMouseX = mouse.TotalX;
    packet.sourceMouseY = mouse.TotalY;
    packet.sourceMouseTimestamp = mouse.TimestampMs;
    packet.inputLatchReady = true;
    if (kcd2CameraTimestamp <= 0.0)
        UpdateMouseSensitivity(sourceIndex, sourceTimestamp);
    // Never warp a composed frame unless explicitly enabled in config.
    // Timewarp is intended for when a camera pose, HUD-less world, and separately composited UI are all available.
    const bool allowComposed = Config::Instance()->ReprojAllowComposedWarp.value_or_default();
    packet.warpAllowed = warpAllowed && packet.hasCamera && (packet.hasUi || allowComposed);
    packet.retirementFenceValue = 0;
    packet.frameId = ++_publishedFrameId;
    packet.sourcePoseTimestamp = sourceTimestamp;
    packet.syncInterval = FGHooks::LastPresentSyncInterval();
    packet.presentFlags = FGHooks::LastPresentFlags();

    const bool nonBlockingHandoff = Config::Instance()->ReprojNonBlockingHandoff.value_or_default();
    const bool nonBlockingThisPacket = packet.hasUi && nonBlockingHandoff;
    RecordPipelineCapturePath(usingKcd2Isolation, nonBlockingThisPacket, !nonBlockingThisPacket);
    bool submitted = false;
    if (usedCaptureQueue)
    {
        if (captureViaWorker)
        {
            // The worker records + submits and signals the reserved value; the
            // game thread only enqueues so the COPY-queue ops leave its frame.
            submitted = EnqueueCapture(packetIndex);
            if (submitted && usingKcd2Isolation)
                Kcd2HudIsolation::MarkFrameCaptured(gameBackBuffer, kcd2Hudless, kcd2Ui, _captureFence,
                                                    packet.captureFenceValue);
        }
        else
        {
            submitted = SubmitCaptureCommandList(packetIndex);
            packet.captureFenceValue = _captureAllocatorFenceValues[packetIndex];
        }
        packet.completionFence = _captureFence;
        // Worker path: the warp gate is the color copy (signaled first), the
        // UI copy trails on the same fence timeline behind captureFenceValue.
        // Inline path: color+UI are one submission, so the single value is the
        // gate (colorFenceValue is 0 there and must not bypass the capture).
        packet.completionFenceValue = captureViaWorker ? packet.colorFenceValue : packet.captureFenceValue;
        // With separate HUD-less world/UI resources the COPY queue never reads
        // the virtual game backbuffer. When nonBlockingHandoff is enabled,
        // release the virtual backbuffer immediately without GPU wait.
        // The composed fallback reads the virtual buffer and keeps the capture fence.
        if (packet.hasUi && nonBlockingHandoff)
        {
            packet.handoffFence = nullptr;
            packet.handoffFenceValue = 0;
        }
        else
        {
            packet.handoffFence = _captureFence;
            packet.handoffFenceValue = packet.captureFenceValue;
        }
        if (submitted)
        {
            std::scoped_lock lock(_metricsMutex);
            ++_metricsDirectCaptures;
        }
    }
    else
    {
        submitted = SubmitUICommandList((UINT) packetIndex);
        packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex];
        packet.completionFence = _uiFence;
        packet.completionFenceValue = packet.captureFenceValue;
        packet.handoffFence = (packet.hasUi && nonBlockingHandoff) ? nullptr : _uiFence;
        packet.handoffFenceValue = (packet.hasUi && nonBlockingHandoff) ? 0 : packet.captureFenceValue;
        if (submitted)
        {
            std::scoped_lock lock(_metricsMutex);
            ++_metricsDirectCaptures;
        }
    }
    if (!submitted)
    {
        // The worker path reserves a capture fence value that will never be
        // signaled if the enqueue failed; complete it CPU-side so RetirePackets
        // can recycle the packet without waiting on a submission that never ran.
        if (captureViaWorker && _captureFence != nullptr && packet.captureFenceValue != 0)
            _captureFence->Signal(packet.captureFenceValue);
        return false;
    }
    return true;
}

bool AReproj_Dx12::EnqueueCapture(int packetIndex)
{
    std::scoped_lock lock(_captureWorkMutex);
    if (_captureWorkStop || _captureWorkCount >= BUFFER_COUNT)
        return false;
    _captureWorkPending[_captureWorkCount++] = packetIndex;
    RecordPipelineCaptureQueueDepth(static_cast<uint32_t>(_captureWorkCount));
    _captureWorkCv.notify_one();
    return true;
}

void AReproj_Dx12::CaptureWorkerMain()
{
    for (;;)
    {
        int packetIndex = -1;
        {
            std::unique_lock lock(_captureWorkMutex);
            _captureWorkCv.wait(lock, [&] { return _captureWorkStop || _captureWorkCount > 0; });
            if (_captureWorkCount == 0)
            {
                if (_captureWorkStop)
                    break;
                continue;
            }
            packetIndex = _captureWorkPending[0];
            for (int i = 1; i < _captureWorkCount; ++i)
                _captureWorkPending[i - 1] = _captureWorkPending[i];
            --_captureWorkCount;
        }
        const auto workerStartMs = Util::MillisecondsNow();
        ProcessCapturePacket(packetIndex);
        RecordPipelineCaptureWorker(Util::MillisecondsNow() - workerStartMs);
    }
}

void AReproj_Dx12::FailCapturePacket(int packetIndex)
{
    if (packetIndex < 0 || packetIndex >= BUFFER_COUNT)
        return;
    auto& packet = _packets[packetIndex];
    // Complete the reserved value CPU-side so RetirePackets can recycle the
    // packet without waiting on a submission that never happened.
    if (_captureFence != nullptr && packet.captureFenceValue != 0)
        _captureFence->Signal(packet.captureFenceValue);
    PacketState expected = PacketState::Capturing;
    if (packet.state.compare_exchange_strong(expected, PacketState::Retired))
        _presentCv.notify_all();
}

void AReproj_Dx12::ProcessCapturePacket(int packetIndex)
{
    if (packetIndex < 0 || packetIndex >= BUFFER_COUNT)
        return;
    auto& packet = _packets[packetIndex];

    if (_captureQueue == nullptr || _captureInputFence == nullptr || _captureFence == nullptr)
    {
        FailCapturePacket(packetIndex);
        return;
    }

    // PHASE 1 - world (color) copy. Gated on the mid-frame world fence when a
    // snapshot CL was marked this frame AND the source is the isolated world
    // texture (complete at world-end); the composed-backbuffer fallback must
    // stay gated on the present-time input fence - its final composite is only
    // complete once the game's frame CLs execute.
    const bool worldGated = packet.worldFenceValue != 0 && packet.captureSrcColor != packet.captureSrcComposed;
    if (worldGated)
    {
        if (_worldFence == nullptr || FAILED(_captureQueue->Wait(_worldFence, packet.worldFenceValue)))
        {
            FailCapturePacket(packetIndex);
            return;
        }
    }
    else if (FAILED(_captureQueue->Wait(_captureInputFence, packet.captureInputFenceValue)))
    {
        FailCapturePacket(packetIndex);
        return;
    }

    // Recycle this packet's capture allocator. This is the worker thread, so a
    // bounded wait here never eats the game's frame budget.
    if (!WaitForCaptureAllocator(packetIndex))
    {
        FailCapturePacket(packetIndex);
        return;
    }

    auto* capList = GetCaptureCommandList(packetIndex);
    if (capList == nullptr)
    {
        FailCapturePacket(packetIndex);
        return;
    }

    bool ok = CopyPacketResource(capList, packet.captureSrcColor, packet.captureSrcColorState, &packet.color,
                                 packet.colorState, L"Reproj_PacketColor");
    if (!ok || FAILED(capList->Close()))
    {
        FailCapturePacket(packetIndex);
        return;
    }
    _captureQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &capList);
    _captureCommandListResetted[packetIndex] = false;
    _captureAllocatorFenceValues[packetIndex] = packet.colorFenceValue;
    if (FAILED(_captureQueue->Signal(_captureFence, packet.colorFenceValue)))
    {
        FailCapturePacket(packetIndex);
        return;
    }

    // PHASE 2 - UI copy. Gated on the present-time input fence (the isolated UI
    // is only complete once the game's frame CLs execute). The allocator reuse
    // wait below doubles as the phase-1 completion wait: the color copy runs on
    // the same COPY queue and finishes long before the present gate fires.
    if (packet.hasUi)
    {
        if (!WaitForCaptureAllocator(packetIndex))
        {
            FailCapturePacket(packetIndex);
            return;
        }
        capList = GetCaptureCommandList(packetIndex);
        if (capList == nullptr)
        {
            FailCapturePacket(packetIndex);
            return;
        }
        if (FAILED(_captureQueue->Wait(_captureInputFence, packet.captureInputFenceValue)))
        {
            FailCapturePacket(packetIndex);
            return;
        }
        packet.hasUi = CopyPacketResource(capList, packet.captureSrcUi, packet.captureSrcUiState, &packet.ui,
                                          packet.uiState, L"Reproj_PacketUI");
        if (!packet.hasUi)
        {
            // This occurs after the game thread chose its virtual-buffer handoff.
            // ReprojPipe exposes it explicitly; it must never be inferred from hud=.
            RecordPipelineWorkerUiFallback();
            RecordPipelineComposedFallback();
            LOG_WARN("Reproj: UI capture failed; using composed frame");
            ok = CopyPacketResource(capList, packet.captureSrcComposed, D3D12_RESOURCE_STATE_PRESENT, &packet.color,
                                    packet.colorState, L"Reproj_PacketColor");
        }
        if (!ok || FAILED(capList->Close()))
        {
            FailCapturePacket(packetIndex);
            return;
        }
        _captureQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &capList);
        _captureCommandListResetted[packetIndex] = false;
        _captureAllocatorFenceValues[packetIndex] = packet.captureFenceValue;
        if (FAILED(_captureQueue->Signal(_captureFence, packet.captureFenceValue)))
        {
            FailCapturePacket(packetIndex);
            return;
        }
    }

    // Publish Capturing -> Ready. The game thread may have abandoned the packet
    // (failed-publication downgrade); the CAS keeps this from resurrecting it,
    // while the capture itself stays submitted so any pending handoff wait
    // still observes the reserved fence value.
    PacketState expected = PacketState::Capturing;
    if (packet.state.compare_exchange_strong(expected, PacketState::Ready))
    {
        _readyFrameId.store(packet.frameId);
        _presentCv.notify_all();
    }
}

void AReproj_Dx12::StopCaptureWorker()
{
    {
        std::scoped_lock lock(_captureWorkMutex);
        _captureWorkStop = true;
        _captureWorkCv.notify_all();
    }
    if (_captureThread.joinable())
        _captureThread.join();
}

bool AReproj_Dx12::DisplayPacket(int packetIndex, bool composeUi, int uiPacketIndex, uint32_t telemetryQueryStart)
{
    auto& packet = _packets[packetIndex];
    if (_swapChain == nullptr || packet.color == nullptr)
        return false;
    auto& uiPacket = _packets[uiPacketIndex >= 0 && uiPacketIndex < BUFFER_COUNT ? uiPacketIndex : packetIndex];

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();

    // Unwarped blits are pure copies: keep them on the DIRECT SC queue where the
    // rasterizing UI pass also lives. Routing a copy through COMPUTE cost an extra
    // ExecuteCommandLists plus a cross-queue Wait and a second SC submit per slot.
    if (!WaitForSCAllocator(outputIndex))
        return false;

    auto cmdList = GetSCCommandList(outputIndex);
    if (cmdList == nullptr)
        return false;

    // Reserve the SC retirement fence value for this slot.
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
        if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr &&
            queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
        {
            cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1);
            cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2,
                                      _warpTimestampReadback, queryStart * sizeof(UINT64));
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
    // Batch the restore transitions: one driver call instead of two.
    {
        D3D12_RESOURCE_BARRIER barriers[2] {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = backBuffer;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = packet.color;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateAfter = packet.colorState;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(packet.colorState != D3D12_RESOURCE_STATE_COPY_SOURCE ? 2 : 1, barriers);
    }
    backBuffer->Release();

    const bool composeUiNow = composeUi && uiPacket.hasUi && _renderUI != nullptr && _renderUI->IsInit();

    if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr &&
        queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
    {
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1);
        cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2, _warpTimestampReadback,
                                  queryStart * sizeof(UINT64));
    }

    if (composeUiNow)
        _renderUI->Dispatch(realSwapChain, cmdList, uiPacket.ui, uiPacket.uiState);
    if (!SubmitSCCommandList(outputIndex))
        return false;

    // The real swapchain was created on the game's DIRECT queue, while this
    // copy/UI list runs on _presentQueue. Order Present on the actual swapchain
    // queue with a GPU-side wait; never CPU-wait here.
    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    const auto directGateStartMs = Util::MillisecondsNow();
    const bool directGateQueued = _gameCommandQueue != nullptr && _scFence != nullptr &&
                                  SUCCEEDED(_gameCommandQueue->Wait(_scFence, packet.retirementFenceValue));
    RecordPipelineDirectGate(Util::MillisecondsNow() - directGateStartMs);
    if (!directGateQueued)
        return false;
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    return true;
}

bool AReproj_Dx12::DispatchPacketWarp(int packetIndex, int uiPacketIndex, float timeStep,
                                      double scanoutDeadlineMs, uint32_t telemetryQueryStart)
{
    auto& packet = _packets[packetIndex];
    auto& content = static_cast<ContentFrame&>(packet);
    if (_swapChain == nullptr || _warp == nullptr || !_warp->IsInit() || content.color == nullptr ||
        !packet.warpAllowed)
        return false;
    // UI comes from the newest completed UI packet (borrowed from the held
    // previous anchor when this anchor's own UI copy still trails).
    auto& uiPacket = _packets[uiPacketIndex >= 0 && uiPacketIndex < BUFFER_COUNT ? uiPacketIndex : packetIndex];

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(outputIndex, IID_PPV_ARGS(&backBuffer))))
        return false;

    // Use COMPUTE queue when available (avoids VKD3D serialization with game queue).
    // Fall back to SC (DIRECT) queue when compute is unavailable.
    const bool useCompute = _computeQueue != nullptr;
    auto* warpQueue = useCompute ? _computeQueue : _presentQueue;

    if (!CreateWarpOutput(outputIndex, backBuffer) ||
        (useCompute ? !WaitForComputeAllocator(outputIndex) : !WaitForSCAllocator(outputIndex)))
    {
        backBuffer->Release();
        return false;
    }

    auto cmdList = useCompute ? GetComputeCommandList(outputIndex) : GetSCCommandList(outputIndex);
    if (cmdList == nullptr)
    {
        backBuffer->Release();
        return false;
    }

    // For the SC (DIRECT) fallback, reserve the retirement fence value here;
    // SubmitSCCommandList uses it. The COMPUTE path reserves its own values
    // inside SubmitComputeCommandList.
    if (!useCompute)
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

    // Sequence-indexed timestamps are emitted only while telemetry is active.
    const UINT timestampStart = useTelemetryQuery ? queryStart : UINT32_MAX;
    if (useTelemetryQuery && _warpTimestampHeap != nullptr && timestampStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart);
    auto constants = content.constants;
    constants.timeStep = timeStep;
    // The independent COMPUTE queue can safely wait for a CPU fence without
    // being serialized behind KCD2's DIRECT queue. Sample at scanout instead
    // of four milliseconds early; DIRECT fallback stays immediate.
    const bool deferredLateLatch = useCompute && _lateLatchFence != nullptr;
    if (!deferredLateLatch)
    {
        if (!ApplyLateInput(constants, packet))
            PrepareRotationConstants(constants, false);
    }
    const bool ok = _warp->Dispatch(cmdList, content.color, content.colorState, _warpOutput[outputIndex], constants,
                                    outputIndex, deferredLateLatch, uiPacket.ui, uiPacket.uiState);
    if (!ok)
    {
        backBuffer->Release();
        if (useCompute)
            SubmitComputeCommandList(outputIndex);
        else
            SubmitSCCommandList(outputIndex);
        packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
        return false;
    }

    content.colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ResourceBarrier(cmdList, _warpOutput[outputIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(backBuffer, _warpOutput[outputIndex]);
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    backBuffer->Release();

    if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr &&
        timestampStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
    {
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart + 1);
        cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, timestampStart, 2,
                                  _warpTimestampReadback, timestampStart * sizeof(UINT64));
    }

    UINT64 lateLatchValue = 0;
    if (deferredLateLatch)
    {
        lateLatchValue = ++_lateLatchFenceValue;
        if (FAILED(warpQueue->Wait(_lateLatchFence, lateLatchValue)))
        {
            _lateLatchFence->Signal(lateLatchValue);
            return false;
        }
    }

    // Submit the warp (compute) or the whole warp+UI (DIRECT fallback).
    if (useCompute)
    {
        if (!SubmitComputeCommandList(outputIndex))
        {
            if (lateLatchValue != 0)
                _lateLatchFence->Signal(lateLatchValue);
            return false;
        }
    }
    else
    {
        if (!SubmitSCCommandList(outputIndex))
        {
            if (lateLatchValue != 0)
                _lateLatchFence->Signal(lateLatchValue);
            return false;
        }
    }

    ++_metricsHudComposites;

    // Retirement is tracked on _scFence; SubmitComputeCommandList signals it
    // after the combined world-warp/UI-composite dispatch.
    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    // 0/auto (default) = adaptive: hunt the mouse sample as close to the
    // present deadline as the warp actually allows. A fixed ms value
    // (>0.5) overrides and keeps the old constant-lead behavior.
    const double lateLeadCfg = Config::Instance()->ReprojLateSampleLead.value_or_default();
    const bool adaptiveLateSample = !(lateLeadCfg > 0.5);
    if (lateLatchValue != 0)
    {
        // Submit early enough to sit behind Proton's game-queue backlog, then
        // release it with a target sampled immediately before the present
        // deadline. The target itself is predicted to scanout midpoint.
        // Leave enough time for the lightweight warp/composite/copy to finish
        // before Present while still sampling substantially later than the
        // normal dispatch wake.
        const double lateLeadMs =
            adaptiveLateSample ? std::clamp(_lateSampleLeadMs, SAMPLE_LEAD_MIN_MS, SAMPLE_LEAD_MAX_MS) : lateLeadCfg;
        _lastLateSampleLeadMs.store(lateLeadMs);
        WaitForPresenterDeadline(scanoutDeadlineMs - lateLeadMs);
        auto lateConstants = content.constants;
        lateConstants.timeStep = timeStep;
        if (!ApplyLateInput(lateConstants, packet))
            PrepareRotationConstants(lateConstants, false);

        const bool constantsWritten = _warp->WriteConstants(outputIndex, lateConstants);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (_currentTelemetrySlot)
            _currentTelemetrySlot->lateLatchSignalQpc = _telemetry.NowQpc();
        const auto signalResult = _lateLatchFence->Signal(lateLatchValue);
        if (!constantsWritten || FAILED(signalResult))
            return false;
    }

    // Present belongs to the swapchain's creation queue (the game's DIRECT
    // queue), but poisoning that queue with a 120 Hz wait serializes all later
    // KCD2 rendering behind presenter work.  Wait on the presenter thread for
    // the independent compute submission instead.  The late latch already
    // releases the short warp before the slot, so this wait normally observes
    // an already-completed fence and never enters the game queue.
    if (useCompute && _computeFence != nullptr)
    {
        if (!WaitForComputeAllocator(outputIndex))
            return false;
        // Adaptive late sample: this wait returns when the warp completed on
        // the GPU. Slide the sample later (smaller lead) when the warp left
        // more than ~2 ms of headroom (it was released too early), earlier
        // when it is crowding the vblank below ~1 ms. The controller settles
        // at signal+warp+copy cost plus ~1.5 ms of CPU wake/Present margin, so
        // the mouse is sampled as late as the hardware allows — typically a
        // fixed 4.0 ms lead never hunts at all because the warp cost leaves
        // 2-3 ms of slack that a 1.2-3.0 ms deadband would not touch.
        if (adaptiveLateSample && lateLatchValue != 0)
        {
            const double warpDoneMs = Util::MillisecondsNow();
            const double headroomMs = scanoutDeadlineMs - warpDoneMs;
            if (headroomMs > SAMPLE_LEAD_REDUCE_HEADROOM_MS)
                _lateSampleLeadMs = std::max(SAMPLE_LEAD_MIN_MS, _lateSampleLeadMs - SAMPLE_LEAD_STEP_MS);
            else if (headroomMs < SAMPLE_LEAD_GROW_HEADROOM_MS)
                _lateSampleLeadMs = std::min(SAMPLE_LEAD_MAX_MS, _lateSampleLeadMs + SAMPLE_LEAD_STEP_MS);
        }
    }
    return true;
}

bool AReproj_Dx12::DispatchWarp(int fIndex, float timeStep)
{
    if (_warp == nullptr || !_warp->IsInit())
        return false;

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

    // Rotation-only reprojection: extrapolate the last rendered camera pair to
    // the display slot. No prediction machinery; the async path additionally
    // composes fresh raw-mouse motion via ApplyLateInput.
    PrepareRotationConstants(cb);

    bool ok = _warp->Dispatch(cmdList, _lastColor[fIndex], _lastColorState[fIndex], _warpOutput[fIndex], cb);

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

bool AReproj_Dx12::DrainGpuWork()
{
    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        if (_uiCommandListResetted[i] && !SubmitUICommandList(i))
            return false;
        if (_scCommandListResetted[i] && !SubmitSCCommandList(i))
            return false;
        if (_computeCommandListResetted[i] && !SubmitComputeCommandList(i))
            return false;
        if (_captureCommandListResetted[i] && !SubmitCaptureCommandList(i))
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
            !waitForFence(_scFence, _scFenceEvent, _scAllocatorFenceValues[i], "SC", i) ||
            !waitForFence(_computeFence, _scFenceEvent, _computeAllocatorFenceValues[i], "COMPUTE", i) ||
            !waitForFence(_captureFence, _captureFenceEvent ? _captureFenceEvent : _scFenceEvent,
                          _captureAllocatorFenceValues[i], "CAPTURE", i))
            return false;
    }

    return true;
}

namespace
{
uint64_t ReprojPipeUs(double elapsedMs) { return static_cast<uint64_t>(std::max(0.0, elapsedMs * 1000.0)); }

void ReprojPipeMax(std::atomic<uint64_t>& target, uint64_t value)
{
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}
} // namespace

void AReproj_Dx12::RecordPipelinePublication(double totalMs, double captureSetupMs, double submitMs, double advanceMs,
                                             bool skipped)
{
    const auto totalUs = ReprojPipeUs(totalMs);
    _pipePubCount.fetch_add(1, std::memory_order_relaxed);
    _pipePubSkipped.fetch_add(skipped, std::memory_order_relaxed);
    _pipePubTotalUs.fetch_add(totalUs, std::memory_order_relaxed);
    _pipeCaptureSetupTotalUs.fetch_add(ReprojPipeUs(captureSetupMs), std::memory_order_relaxed);
    _pipeSubmitTotalUs.fetch_add(ReprojPipeUs(submitMs), std::memory_order_relaxed);
    _pipeAdvanceTotalUs.fetch_add(ReprojPipeUs(advanceMs), std::memory_order_relaxed);
    ReprojPipeMax(_pipePubMaxUs, totalUs);
}

void AReproj_Dx12::RecordPipelineGameSignal(double elapsedMs)
{
    const auto elapsedUs = ReprojPipeUs(elapsedMs);
    _pipeGameSignalCount.fetch_add(1, std::memory_order_relaxed);
    _pipeGameSignalTotalUs.fetch_add(elapsedUs, std::memory_order_relaxed);
    ReprojPipeMax(_pipeGameSignalMaxUs, elapsedUs);
}

void AReproj_Dx12::RecordPipelineCaptureQueueDepth(uint32_t depth) { ReprojPipeMax(_pipeCaptureQueueHigh, depth); }

void AReproj_Dx12::RecordPipelineCapturing(uint32_t count) { ReprojPipeMax(_pipeCapturingHigh, count); }

void AReproj_Dx12::RecordPipelineCaptureWorker(double elapsedMs)
{
    const auto elapsedUs = ReprojPipeUs(elapsedMs);
    _pipeCaptureWorkerCount.fetch_add(1, std::memory_order_relaxed);
    _pipeCaptureWorkerTotalUs.fetch_add(elapsedUs, std::memory_order_relaxed);
    ReprojPipeMax(_pipeCaptureWorkerMaxUs, elapsedUs);
}

void AReproj_Dx12::RecordPipelineCapturePath(bool isolated, bool nonBlocking, bool blocking)
{
    _pipeIsolated.fetch_add(isolated, std::memory_order_relaxed);
    _pipeNonBlocking.fetch_add(nonBlocking, std::memory_order_relaxed);
    _pipeBlocking.fetch_add(blocking, std::memory_order_relaxed);
}

void AReproj_Dx12::RecordPipelineWorkerUiFallback() { _pipeWorkerUiFallback.fetch_add(1, std::memory_order_relaxed); }

void AReproj_Dx12::RecordPipelineComposedFallback() { _pipeComposedFallback.fetch_add(1, std::memory_order_relaxed); }

void AReproj_Dx12::RecordPipelineOutput(bool warped, bool newContent)
{
    if (warped)
        (newContent ? _pipeNewWarp : _pipeRepeatWarp).fetch_add(1, std::memory_order_relaxed);
    else
        (newContent ? _pipeNewBlit : _pipeRepeatBlit).fetch_add(1, std::memory_order_relaxed);
}

void AReproj_Dx12::RecordPipelineDirectGate(double elapsedMs)
{
    const auto elapsedUs = ReprojPipeUs(elapsedMs);
    _pipeDirectGateCount.fetch_add(1, std::memory_order_relaxed);
    _pipeDirectGateTotalUs.fetch_add(elapsedUs, std::memory_order_relaxed);
    ReprojPipeMax(_pipeDirectGateMaxUs, elapsedUs);
}

void AReproj_Dx12::RecordPipelineFencePoll(bool colorPending, bool uiPending)
{
    _pipeFencePolls.fetch_add(1, std::memory_order_relaxed);
    _pipeColorPending.fetch_add(colorPending, std::memory_order_relaxed);
    _pipeUiPending.fetch_add(uiPending, std::memory_order_relaxed);
}

void AReproj_Dx12::LogPipelineMetricsIfDue()
{
    constexpr double PIPE_INTERVAL_MS = 250.0;
    const auto now = Util::MillisecondsNow();
    if (_pipeMetricsTimestamp == 0.0)
    {
        _pipeMetricsTimestamp = now;
        return;
    }
    const auto elapsedMs = now - _pipeMetricsTimestamp;
    if (elapsedMs < PIPE_INTERVAL_MS)
        return;
    _pipeMetricsTimestamp = now;

    const auto take = [](std::atomic<uint64_t>& value) { return value.exchange(0, std::memory_order_relaxed); };
    const auto pubCount = take(_pipePubCount);
    const auto pubSkipped = take(_pipePubSkipped);
    const auto pubTotalUs = take(_pipePubTotalUs);
    const auto pubMaxUs = take(_pipePubMaxUs);
    const auto captureSetupUs = take(_pipeCaptureSetupTotalUs);
    const auto submitUs = take(_pipeSubmitTotalUs);
    const auto advanceUs = take(_pipeAdvanceTotalUs);
    const auto signalCount = take(_pipeGameSignalCount);
    const auto signalTotalUs = take(_pipeGameSignalTotalUs);
    const auto signalMaxUs = take(_pipeGameSignalMaxUs);
    const auto capQueueHigh = take(_pipeCaptureQueueHigh);
    const auto workerCount = take(_pipeCaptureWorkerCount);
    const auto workerTotalUs = take(_pipeCaptureWorkerTotalUs);
    const auto workerMaxUs = take(_pipeCaptureWorkerMaxUs);
    const auto isolated = take(_pipeIsolated);
    const auto nonBlocking = take(_pipeNonBlocking);
    const auto blocking = take(_pipeBlocking);
    const auto workerUiFallback = take(_pipeWorkerUiFallback);
    const auto composedFallback = take(_pipeComposedFallback);
    const auto newWarp = take(_pipeNewWarp);
    const auto repeatWarp = take(_pipeRepeatWarp);
    const auto newBlit = take(_pipeNewBlit);
    const auto repeatBlit = take(_pipeRepeatBlit);
    const auto directGateCount = take(_pipeDirectGateCount);
    const auto directGateTotalUs = take(_pipeDirectGateTotalUs);
    const auto directGateMaxUs = take(_pipeDirectGateMaxUs);
    const auto capturingHigh = take(_pipeCapturingHigh);
    const auto fencePolls = take(_pipeFencePolls);
    const auto colorPending = take(_pipeColorPending);
    const auto uiPending = take(_pipeUiPending);
    const auto handoff = _wrappedSwapChain != nullptr ? _wrappedSwapChain->ConsumeReprojectionAdvanceWaitStats()
                                                      : WrappedIDXGISwapChain4::ReprojectionAdvanceWaitStats {};
    const auto meanMs = [](uint64_t totalUs, uint64_t count)
    { return count != 0 ? static_cast<double>(totalUs) / count / 1000.0 : 0.0; };

    // Flat key=value fields are intentionally stable for scripts; each line is
    // a 250 ms aggregate, not an event trace. Units are encoded in key names.
    LOG_INFO("ReprojPipe v=1 dtMs={:.0f} pubN={} pubSkip={} pubMeanMs={:.3f} pubMaxMs={:.3f} "
             "pubCapMs={:.3f} pubSubmitMs={:.3f} pubAdvanceMs={:.3f} signalN={} signalMeanMs={:.3f} "
             "signalMaxMs={:.3f} handoffWaitN={} handoffWaitMeanMs={:.3f} handoffWaitMaxMs={:.3f} "
             "capQHi={} capWorkerN={} capWorkerMeanMs={:.3f} capWorkerMaxMs={:.3f} capCapturingHi={} "
             "fencePollN={} colorPending={} uiPending={} iso={} handoffNB={} handoffBlock={} workerUiFail={} "
             "composedFallback={} newWarp={} repeatWarp={} newBlit={} repeatBlit={} directGateN={} "
             "directGateMeanMs={:.3f} directGateMaxMs={:.3f}",
             elapsedMs, pubCount, pubSkipped, meanMs(pubTotalUs, pubCount), pubMaxUs / 1000.0,
             meanMs(captureSetupUs, pubCount), meanMs(submitUs, pubCount), meanMs(advanceUs, pubCount), signalCount,
             meanMs(signalTotalUs, signalCount), signalMaxUs / 1000.0, handoff.count,
             meanMs(handoff.totalUs, handoff.count), handoff.maxUs / 1000.0, capQueueHigh, workerCount,
             meanMs(workerTotalUs, workerCount), workerMaxUs / 1000.0, capturingHigh, fencePolls, colorPending,
             uiPending, isolated, nonBlocking, blocking, workerUiFallback, composedFallback, newWarp, repeatWarp,
             newBlit, repeatBlit, directGateCount, meanMs(directGateTotalUs, directGateCount),
             directGateMaxUs / 1000.0);
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
    LogPipelineMetricsIfDue();
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
    _runtimeMetrics.droppedAnchors = _metricsSkippedAnchorSamples;
    _runtimeMetrics.directCaptures = _metricsDirectCaptures;
    _runtimeMetrics.captureNotReady = _metricsCaptureNotReady;
    _runtimeMetrics.uiBorrows = _metricsUiBorrows;
    _runtimeMetrics.repeatWarpShed = _repeatWarpShed.load(std::memory_order_relaxed);
    _runtimeMetrics.stallEmaMs = static_cast<float>(_stallEmaMs.load(std::memory_order_relaxed));
    // block/pace report the worst game-thread cost in the one-second window.
    // Reporting the last sample hid exactly the intermittent handoff stalls
    // that pull an otherwise 60+ FPS source into the mid-50s.
    _runtimeMetrics.gamePresentBlockMs = _metricsGamePresentBlockMaxMs;
    _runtimeMetrics.gamePresentPaceMs = _metricsGamePresentPaceMaxMs;
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
    const double lateCamAge =
        _metricsLateCamAgeSamples > 0 ? _metricsLateCamAgeTotalMs / _metricsLateCamAgeSamples : 0.0;
    LOG_INFO("Reproj: source={:.1f} FPS display={:.1f} FPS (new={} repeat={}) missed={} "
             "interval={:.2f}/{:.2f}ms lead={:.2f}ms sampLead={:.2f}ms poseAge={:.1f}ms queue={} "
             "late={}/{} maxDeg={:.2f} hud={} dropAnchor={} capC={} capWait={} uiBorrow={} latch={}/{}/{} "
             "lateAge={:.1f}ms sensX={:.7f} hold={} shed={} stallEma={:.1f}ms ({}, block={:.2f}ms pace={:.2f}ms)",
             _metricsRealFrames * scale, _metricsWarpFrames * scale, _metricsNewAnchorDisplays,
             _metricsRepeatedAnchorDisplays, _metricsMissedDisplaySlots, _runtimeMetrics.meanPresentIntervalMs,
             _runtimeMetrics.p95PresentIntervalMs, _dispatchLeadMs, _lastLateSampleLeadMs.load(), poseAge,
             _runtimeMetrics.queueDepth,
             _metricsLateInputApplied, _metricsLateInputSamples, _metricsLateInputMaxDegrees, _metricsHudComposites,
             _metricsSkippedAnchorSamples, _metricsDirectCaptures, _metricsCaptureNotReady, _metricsUiBorrows,
             _metricsLateCamHits, _metricsPacketBaseHits, _metricsLateFallbacks, lateCamAge,
             _trackedMouseSensitivityX.load(std::memory_order_relaxed), _metricsHitchHolds,
             _repeatWarpShed.load(std::memory_order_relaxed), _stallEmaMs.load(std::memory_order_relaxed), presenter,
             _runtimeMetrics.gamePresentBlockMs, _runtimeMetrics.gamePresentPaceMs);
    _metricsTimestamp = now;
    _metricsRealFrames = 0;
    _metricsWarpFrames = 0;
    _metricsDroppedWarps = 0;
    _metricsMaxWarpsPerReal = 0;
    _metricsPoseAgeTotalMs = 0.0;
    _metricsPoseSamples = 0;
    _metricsNewAnchorDisplays = 0;
    _metricsRepeatedAnchorDisplays = 0;
    _metricsSkippedAnchorSamples = 0;
    _metricsMissedDisplaySlots = 0;
    _metricsLateInputSamples = 0;
    _metricsLateInputApplied = 0;
    _metricsLateCamHits = 0;
    _metricsPacketBaseHits = 0;
    _metricsLateFallbacks = 0;
    _metricsLateCamAgeTotalMs = 0.0;
    _metricsLateCamAgeSamples = 0;
    _metricsHudComposites = 0;
    _metricsDirectCaptures = 0;
    _metricsUiBorrows = 0;
    _metricsCaptureNotReady = 0;
    _metricsHitchHolds = 0;
    _metricsLateInputMaxDegrees = 0.0f;
    _metricsGamePresentBlockMaxMs = 0.0f;
    _metricsGamePresentPaceMaxMs = 0.0f;
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
        // If the compute queue is present, the warp command lists must be too.
        if (_computeQueue != nullptr && (_computeAllocator[i] == nullptr || _computeCommandList[i] == nullptr))
            return false;
    }
    return true;
}

HRESULT AReproj_Dx12::PresentVirtualFrameSync(int fIndex, ID3D12Resource* source, UINT virtualBufferIndex,
                                              UINT syncInterval, UINT flags, bool allowWarps)
{
    (void) allowWarps;
    if (_wrappedSwapChain == nullptr || source == nullptr || _presentThread.joinable() ||
        _presenterState.load() == PresenterState::Running)
        return DXGI_ERROR_INVALID_CALL;

    if (!CopyLastFrame(fIndex, source))
    {
        LOG_WARN("Reproj: synchronous fallback could not copy the source frame");
        return E_FAIL;
    }

    auto* realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto realIndex = realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* realBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(realIndex, IID_PPV_ARGS(&realBuffer))))
    {
        LOG_WARN("Reproj: synchronous fallback GetBuffer({}) failed", realIndex);
        return E_FAIL;
    }

    auto* cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        LOG_WARN("Reproj: synchronous fallback has no UI command list");
        realBuffer->Release();
        return E_FAIL;
    }
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(realBuffer, source);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    realBuffer->Release();

    if (_syncHasUi[fIndex] && _uiColor[fIndex] != nullptr)
    {
        if (_renderUI == nullptr && _device != nullptr)
            _renderUI = std::make_unique<RUI_Dx12>("ReprojUI", _device,
                                                   Config::Instance()->FGUIPremultipliedAlpha.value_or_default());

        if (_renderUI != nullptr && _renderUI->IsInit())
        {
            _renderUI->Dispatch(realSwapChain, cmdList, _uiColor[fIndex], _uiColorState[fIndex]);
        }
    }

    if (!SubmitUICommandList(static_cast<UINT>(fIndex)))
    {
        LOG_WARN("Reproj: synchronous fallback command submission failed");
        return E_FAIL;
    }
    const auto captureValue = _uiAllocatorFenceValues[fIndex];
    if (FAILED(_wrappedSwapChain->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence, captureValue)))
    {
        LOG_WARN("Reproj: synchronous fallback buffer submission failed");
        return E_FAIL;
    }
    const auto advanceResult = _wrappedSwapChain->AdvanceReprojectionBuffer();
    if (FAILED(advanceResult))
    {
        LOG_WARN("Reproj: synchronous fallback buffer advance failed {:X}", (UINT) advanceResult);
        return advanceResult;
    }
    return PresentFrame(syncInterval, flags);
}

bool AReproj_Dx12::Present()
{
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
    if (!virtualized || !IsActive() || IsPaused())
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
    constexpr bool captureThisPresent = true;
    float poseAge = 0.0f;
    constexpr bool needsCameraPose = true;
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
        _runtimeMetrics.anchorStale = cameraAvailable && !poseFresh;
        _runtimeMetrics.focusLost = !focused;
        _runtimeMetrics.rotationOnly = true;
        _runtimeMetrics.hudWarped = false;
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
    const bool warpAllowed = !stalled && !_reset[fIndex] && !focusLost;
    if (!warpAllowed)
        LOG_DEBUG("Reproj: publishing unwarped anchor (reset:{} focused:{} poseAge:{:.1f}ms)", _reset[fIndex], focused,
                  poseAge);

    if (virtualized && _presenterState.load() == PresenterState::Running)
    {
        if (!captureThisPresent)
        {
            // The virtual game buffer still needs its ownership handoff and ring advance, but no packet is copied.
            // This is deliberately non-blocking: the presenter keeps reprojecting its active anchor while KCD2
            // continues rendering at its natural rate.
            const auto fenceValue = ++_uiFenceValue;
            _uiAllocatorFenceValues[fIndex] = fenceValue;
            const bool advanced =
                _gameCommandQueue != nullptr && _uiFence != nullptr &&
                SUCCEEDED(_gameCommandQueue->Signal(_uiFence, fenceValue)) &&
                SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, _uiFence, fenceValue)) &&
                SUCCEEDED(wrapped->AdvanceReprojectionBuffer());
            if (!advanced)
                _presenterState.store(PresenterState::Failed);
            else
            {
                std::scoped_lock metricsLock(_metricsMutex);
                ++_metricsSkippedAnchorSamples;
            }
            // Pace only after handing this virtual buffer back so the sleep
            // never delays its GPU ownership transition.
            const auto paceStart = Util::MillisecondsNow();
            FrameLimit::paceReprojectionSource(true);
            const auto paceEnd = Util::MillisecondsNow();
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            // block= covers real game-thread work only; the pacing sleep is
            // reported separately in pace= so queue-pin costs stay visible.
            _metricsGamePresentBlockMaxMs =
                std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(paceStart - presentStart));
            _metricsGamePresentPaceMaxMs =
                std::max(_metricsGamePresentPaceMaxMs, static_cast<float>(paceEnd - paceStart));
            _latestGameStallMs.store(static_cast<float>(paceStart - presentStart), std::memory_order_relaxed);
            return advanced;
        }

        RecordRealFrame();
        auto packetIndex = AcquirePacket();
        if (packetIndex < 0)
        {
            // No free packet slot: NEVER stall the game thread here. A 2 ms
            // wait eats directly into the SourceFramerateLimit budget (the
            // pacer can only delay frames, never speed them up), while a
            // skipped anchor is invisible — the presenter keeps re-warping
            // its active anchor until a newer packet arrives. AcquirePacket
            // already retired completed packets, so one immediate re-scan is
            // enough to catch a packet the presenter retired concurrently.
            packetIndex = AcquirePacket();
        }
        if (packetIndex < 0)
        {
            // ++_metricsSkippedAnchorSamples (counted in SkipAnchorPublication)
            SkipAnchorPublication(fIndex, gameBackBuffer, virtualBufferIndex, wrapped, presentStart);
            return true;
        }

        auto& packet = _packets[packetIndex];
        // Never stall the game thread on capture-allocator pressure either:
        // when the GPU runs behind (streaming), this slot's prior UI
        // submission may still be in flight and GetUICommandList would block
        // inside CaptureFramePacket. Drop the anchor instead — the presenter
        // holds its active anchor and a skipped anchor is invisible, while a
        // stalled game thread deepens the hitch for everything behind it.
        if (!CaptureAllocatorReady(packetIndex))
        {
            packet.state.store(PacketState::Free);
            SkipAnchorPublication(fIndex, gameBackBuffer, virtualBufferIndex, wrapped, presentStart);
            return true;
        }
        const auto captureSetupStartMs = Util::MillisecondsNow();
        const bool captured = CaptureFramePacket(fIndex, packetIndex, gameBackBuffer, virtualBufferIndex, warpAllowed);
        const auto captureSetupMs = Util::MillisecondsNow() - captureSetupStartMs;
        // Packet readiness and virtual-buffer reuse have separate fences on the
        // isolated-HUD path: reuse need not wait for copies that only read the
        // separate HUD-less/UI resources.
        auto* captureFence = packet.completionFence != nullptr ? packet.completionFence : _uiFence;
        // CaptureFramePacket deliberately clears both handoff fields for an
        // isolated HUD-less/UI capture: COPY never reads the virtual game
        // backbuffer, so ring reuse must not wait for its fence. Do not fall
        // back to completionFence/captureFenceValue here; that silently turns
        // the non-blocking path back into ownership back-pressure.
        auto* handoffFence = packet.handoffFence;
        const auto handoffFenceValue = packet.handoffFenceValue;
        const auto submitStartMs = Util::MillisecondsNow();
        const bool submitted = captured && SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, handoffFence,
                                                                                       handoffFenceValue));
        const auto submitMs = Util::MillisecondsNow() - submitStartMs;
        HRESULT advanceHr = E_FAIL;
        const auto advanceStartMs = Util::MillisecondsNow();
        const bool advanced = submitted && SUCCEEDED(advanceHr = wrapped->AdvanceReprojectionBuffer());
        const auto advanceMs = Util::MillisecondsNow() - advanceStartMs;
        RecordPipelinePublication(Util::MillisecondsNow() - presentStart, captureSetupMs, submitMs, advanceMs, false);
        if (captured && submitted && advanced)
        {
            packet.state.store(PacketState::Ready);
            _readyFrameId.store(packet.frameId);
            _presentCv.notify_one();
            // SourceFramerateLimit is an explicit GPU-budget contract. Honor it
            // even when anchor sampling is non-blocking; otherwise an uncapped
            // KCD2 render queue starves the 120 Hz presenter behind 15-27 ms of
            // work. NonBlockingAnchorSampling controls capture frequency only.
            const auto paceStart = Util::MillisecondsNow();
            FrameLimit::paceReprojectionSource(true);
            const auto paceEnd = Util::MillisecondsNow();
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            // block= covers real game-thread work only (capture submit,
            // publication); the pacing sleep is reported separately in pace=.
            _metricsGamePresentBlockMaxMs =
                std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(paceStart - presentStart));
            _metricsGamePresentPaceMaxMs =
                std::max(_metricsGamePresentPaceMaxMs, static_cast<float>(paceEnd - paceStart));
            _latestGameStallMs.store(static_cast<float>(paceStart - presentStart), std::memory_order_relaxed);
            return true;
        }

        // Any failed virtual-buffer handoff is a hard ownership failure. The
        // current virtual buffer remains Capturing, so returning to the game
        // would let it render into a resource still read by capture.
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
            SUCCEEDED(_gameCommandQueue->Wait(captureFence, packet.captureFenceValue)) &&
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

    // Async ownership is required for timewarp. If it is unavailable, present the
    // game frame unchanged; never run a blocking generated-frame fallback.
    HRESULT fallbackResult = virtualized ? PresentVirtualFrameSync(fIndex, gameBackBuffer, virtualBufferIndex,
                                                                   syncInterval, presentFlags, false)
                                         : PresentFrame(syncInterval, presentFlags);
    SAFE_RELEASE(gameBackBuffer);
    return SUCCEEDED(fallbackResult);

#if 0
    // Removed synchronous reprojection path.
    // Synchronous path.  With virtualization the game source is copied to the real
    // anchor while the worker is stopped; otherwise retain the legacy raw-buffer path.
    RecordRealFrame();
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
            {
                if (_syncHasUi[fIndex] && _uiColor[fIndex] != nullptr)
                {
                    if (_renderUI == nullptr && _device != nullptr)
                        _renderUI = std::make_unique<RUI_Dx12>(
                            "ReprojUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());

                    if (_renderUI != nullptr && _renderUI->IsInit())
                    {
                        auto* cmdList = GetUICommandList(fIndex);
                        if (cmdList != nullptr)
                        {
                            _renderUI->Dispatch(realSwapChain, cmdList, _uiColor[fIndex], _uiColorState[fIndex]);
                            SubmitUICommandList(static_cast<UINT>(fIndex));
                        }
                    }
                }
                realResult = PresentFrame(syncInterval, presentFlags);
            }
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
        constexpr float configuredStep = 1.0f;
        const auto timeStep =
            std::clamp(static_cast<float>((refreshPeriodMs * warp) / realPeriodMs) * configuredStep, 0.0f, 1.0f);
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
#endif
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
        _metricsSkippedAnchorSamples = 0;
        _metricsLateInputSamples = 0;
        _metricsLateInputApplied = 0;
        _metricsLateCamHits = 0;
        _metricsPacketBaseHits = 0;
        _metricsLateFallbacks = 0;
        _metricsLateCamAgeTotalMs = 0.0;
        _metricsLateCamAgeSamples = 0;
        _metricsHudComposites = 0;
        _metricsDirectCaptures = 0;
        _metricsCaptureNotReady = 0;
        _metricsHitchHolds = 0;
        _metricsLateInputMaxDegrees = 0.0f;
        _runtimeMetrics = {};
    }
    _cachedRefreshHz = 0.0;
    _lastRefreshQueryMs = 0.0;
    _lastRealFrameTimestamp = 0.0;
    _lastCapturedMouseTimestamp = 0.0;
    _lastCapturedMouseX = 0;
    _lastCapturedMouseY = 0;
    _trackedMouseSensitivityX.store(0.00015f, std::memory_order_relaxed);
    _trackedMouseSensitivityY.store(0.00015f, std::memory_order_relaxed);
    _hasTrackedMouseSensitivity.store(false, std::memory_order_relaxed);
    _kcd2CalibrationYawRadians = 0.0;
    _kcd2CalibrationPitchRadians = 0.0;
    _kcd2CalibrationMouseX = 0;
    _kcd2CalibrationMouseY = 0;
    _kcd2CalibrationSamples = 0;
    if (_renderUI == nullptr)
        _renderUI = std::make_unique<RUI_Dx12>("ReprojUI", _device,
                                               Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
    _asyncDowngraded = false;
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
            pkt.completionFence = nullptr;
            pkt.completionFenceValue = 0;
            pkt.handoffFence = nullptr;
            pkt.handoffFenceValue = 0;
            pkt.retirementFenceValue = 0;
            pkt.frameId = 0;
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

            auto bufferCount = std::max<UINT>(desc->BufferCount, 3u);
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
    constexpr bool asyncRequested = true;
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

            auto bufferCount = std::max<UINT>(desc->BufferCount, 3u);
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
    constexpr bool asyncRequested = true;
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
    Kcd2HudIsolation::Reset();
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
        SAFE_RELEASE(_packets[i].ui);

        _lastColorState[i] = D3D12_RESOURCE_STATE_COMMON;
        _uiColorState[i] = D3D12_RESOURCE_STATE_COMMON;
        _syncHasUi[i] = false;
        _packets[i].colorState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].uiState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].captureFenceValue = 0;
        _packets[i].completionFence = nullptr;
        _packets[i].completionFenceValue = 0;
        _packets[i].handoffFence = nullptr;
        _packets[i].handoffFenceValue = 0;
        _packets[i].retirementFenceValue = 0;
        _packets[i].syncInterval = 0;
        _packets[i].presentFlags = 0;
        _packets[i].warpAllowed = false;
        _packets[i].state.store(PacketState::Free);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _computeCommandListResetted[i] = false;
        _computeAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;

        _captureCommandListResetted[i] = false;
        _captureAllocatorFenceValues[i] = 0;
    }

    if (_captureFenceEvent)
    {
        CloseHandle(_captureFenceEvent);
        _captureFenceEvent = nullptr;
    }
    SAFE_RELEASE(_captureFence);
    _captureFenceValue = 0;
    SAFE_RELEASE(_captureInputFence);
    _captureInputFenceValue = 0;
    SAFE_RELEASE(_uiFence);
    SAFE_RELEASE(_scFence);
    SAFE_RELEASE(_lateLatchFence);
    SAFE_RELEASE(_computeFence);
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
    _lateLatchFenceValue = 0;
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

            if (_lateLatchFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_lateLatchFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create late-latch fence failed: {:X}", (UINT) result);
                    break;
                }
                _lateLatchFence->SetName(L"Reproj_LateLatchFence");
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
