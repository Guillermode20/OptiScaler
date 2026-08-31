#include "pch.h"
#include "AReproj_Dx12.h"
#include "Kcd2Camera.h"
#include "Kcd2HudIsolation.h"
#include "Kcd2Input.h"
#include "Kcd2Scaleform.h"
#include "ReprojInputPredictor.h"
#include "TargetPoseResolver.h"

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

const char* AReproj_Dx12::Name()
{
    return State::Instance().activeFgOutput == FGOutput::HybridTimewarp ? "Hybrid Timewarp" : "Async Timewarp";
}

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

ReprojVec3 CombineReprojVec3(ReprojVec3 first, float firstScale, ReprojVec3 second, float secondScale)
{
    return { first.x * firstScale + second.x * secondScale, first.y * firstScale + second.y * secondScale,
             first.z * firstScale + second.z * secondScale };
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
                              float latePitch = 0.0f)
{
    // Mode 1 takes the input-predicted rotation as an explicit warp target:
    // the shader composes it onto the current pose and keeps the rendered
    // velocity for the position lerp. Mode 2 rotates the predicted basis here.
    constants.targetPosition[3] = 0.0f;

    const auto right = NormalizeReprojVec3(LoadReprojVec3(constants.cameraRight));
    const auto up = NormalizeReprojVec3(LoadReprojVec3(constants.cameraUp));
    const auto forward = NormalizeReprojVec3(LoadReprojVec3(constants.cameraForward));
    ReprojVec3 predictedRight {};
    ReprojVec3 predictedUp {};
    ReprojVec3 predictedForward {};

    if (inputLatched)
    {
        const float yawSin = std::sin(lateYaw);
        const float yawCos = std::cos(lateYaw);
        const float pitchSin = std::sin(latePitch);
        const float pitchCos = std::cos(latePitch);
        const auto yawRight = CombineReprojVec3(right, yawCos, forward, -yawSin);
        const auto yawForward = CombineReprojVec3(forward, yawCos, right, yawSin);
        predictedRight = NormalizeReprojVec3(yawRight);
        predictedUp = NormalizeReprojVec3(CombineReprojVec3(up, pitchCos, yawForward, -pitchSin));
        predictedForward = NormalizeReprojVec3(CombineReprojVec3(yawForward, pitchCos, up, pitchSin));
    }
    else if (constants.mode == 2)
    {
        const float scale = 1.0f + constants.timeStep;
        predictedRight = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraRight), right, scale);
        predictedUp = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraUp), up, scale);
        predictedForward = ExtrapolateReprojVec3(LoadReprojVec3(constants.prevCameraForward), forward, scale);
    }

    if (inputLatched && constants.mode == 1)
    {
        StoreReprojVec3(constants.targetRight, predictedRight);
        StoreReprojVec3(constants.targetUp, predictedUp);
        StoreReprojVec3(constants.targetForward, predictedForward);
        std::memcpy(constants.targetPosition, constants.cameraPosition, 3 * sizeof(float));
        constants.targetPosition[3] = 1.0f;
        return;
    }

    if (constants.mode != 2)
        return;

    StoreReprojVec3(constants.prevCameraRight,
                    ReprojTransformRow(right, predictedRight, predictedUp, predictedForward));
    StoreReprojVec3(constants.prevCameraUp, ReprojTransformRow(up, predictedRight, predictedUp, predictedForward));
    StoreReprojVec3(constants.prevCameraForward,
                    ReprojTransformRow(forward, predictedRight, predictedUp, predictedForward));
    constants.cameraVFov = std::tan(constants.cameraVFov * 0.5f);
}

void PrepareTargetPoseConstants(RP_Constants& constants, const TargetPoseResolver::Pose& target)
{
    const auto right = NormalizeReprojVec3(LoadReprojVec3(constants.cameraRight));
    const auto up = NormalizeReprojVec3(LoadReprojVec3(constants.cameraUp));
    const auto forward = NormalizeReprojVec3(LoadReprojVec3(constants.cameraForward));
    const auto targetRight = NormalizeReprojVec3(LoadReprojVec3(target.right));
    const auto targetUp = NormalizeReprojVec3(LoadReprojVec3(target.up));
    const auto targetForward = NormalizeReprojVec3(LoadReprojVec3(target.forward));

    if (constants.mode == 1)
    {
        std::memcpy(constants.targetPosition, target.position, 3 * sizeof(float));
        StoreReprojVec3(constants.targetRight, targetRight);
        StoreReprojVec3(constants.targetUp, targetUp);
        StoreReprojVec3(constants.targetForward, targetForward);
        constants.targetPosition[3] = 1.0f;
        return;
    }
    if (constants.mode != 2)
        return;

    StoreReprojVec3(constants.prevCameraRight, ReprojTransformRow(right, targetRight, targetUp, targetForward));
    StoreReprojVec3(constants.prevCameraUp, ReprojTransformRow(up, targetRight, targetUp, targetForward));
    StoreReprojVec3(constants.prevCameraForward, ReprojTransformRow(forward, targetRight, targetUp, targetForward));
    constants.cameraVFov = std::tan(constants.cameraVFov * 0.5f);
}
} // namespace

bool AReproj_Dx12::ApplyLateInput(RP_Constants& constants, const ReprojFramePacket& packet)
{
    if (!Config::Instance()->ReprojLateLatch.value_or_default())
        return false;

    if (!packet.inputLatchReady || constants.mode != 2)
        return false;

    OptiInput::RefreshMouseMotion();
    const auto current = OptiInput::GetRawMouseMotion();

    const double deltaX = static_cast<double>(current.TotalX - packet.sourceMouseX);
    const double deltaY = static_cast<double>(current.TotalY - packet.sourceMouseY);

    float sensX = Config::Instance()->ReprojMouseSensitivityX.value_or_default();
    float sensY = Config::Instance()->ReprojMouseSensitivityY.value_or_default();
    const float trackedX = _trackedMouseSensitivityX.load(std::memory_order_relaxed);
    const float trackedY = _trackedMouseSensitivityY.load(std::memory_order_relaxed);
    if (sensX <= 0.0f)
        sensX = trackedX > 1e-5f ? trackedX : 0.001f;
    if (sensY <= 0.0f)
        sensY = trackedY > 1e-5f ? trackedY : 0.001f;

    double yaw = deltaX * sensX;
    double pitch = -deltaY * sensY;

    if (!std::isfinite(yaw) || !std::isfinite(pitch))
        return false;

    constexpr double maxRotation = 0.35;
    const double rotation = std::hypot(yaw, pitch);
    if (rotation > maxRotation)
    {
        yaw *= maxRotation / rotation;
        pitch *= maxRotation / rotation;
    }

    PrepareRotationConstants(constants, true, static_cast<float>(yaw), static_cast<float>(pitch));
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
    const auto currentMouse = OptiInput::GetRawMouseMotion();
    if (_lastCapturedMouseTimestamp > 0.0 && sourcePoseTimestamp > _lastCapturedMouseTimestamp)
    {
        const double dX = static_cast<double>(currentMouse.TotalX - _lastCapturedMouseX);
        const double dY = static_cast<double>(currentMouse.TotalY - _lastCapturedMouseY);

        if (std::abs(dX) >= 4.0 && std::abs(yaw) > 1e-4 && (dX * yaw > 0.0))
        {
            const float measuredSensX = static_cast<float>(std::abs(yaw) / std::abs(dX));
            if (measuredSensX > 1e-5f && measuredSensX < 0.01f)
            {
                if (!_hasTrackedMouseSensitivity.load(std::memory_order_relaxed))
                {
                    _trackedMouseSensitivityX.store(measuredSensX, std::memory_order_relaxed);
                    _trackedMouseSensitivityY.store(measuredSensX, std::memory_order_relaxed);
                    _hasTrackedMouseSensitivity.store(true, std::memory_order_relaxed);
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
            if (measuredSensY > 1e-5f && measuredSensY < 0.01f)
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

// Feed the input-predictor estimator from the per-slot camera arrays (sync
// path, FfxApi/Streamline supplied poses). One sample per rendered frame.
void AReproj_Dx12::FeedInputPredictor(int fIndex)
{
    if (!Config::Instance()->ReprojInputPredictor.value_or_default())
        return;

    const auto poseTimestamp = _cameraTimestamp[fIndex];
    if (poseTimestamp <= 0.0 || poseTimestamp <= _lastPredictorFeedPoseMs)
        return;

    const auto prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    if (_reset[fIndex] || _reset[prevIndex] || IsCameraAllZero(fIndex) || IsCameraAllZero(prevIndex))
        return;

    const auto prevPoseTimestamp = _cameraTimestamp[prevIndex];
    const double intervalMs =
        prevPoseTimestamp > 0.0 && poseTimestamp > prevPoseTimestamp ? poseTimestamp - prevPoseTimestamp : 16.6;

    OptiInput::RefreshMouseMotion();
    // The camera pair spans [prevPose, pose]; correlate input over the same
    // window so the gain estimate stays a closed-loop calibration.
    const auto mouseAtPose = OptiInput::GetRawMouseMotionAt(poseTimestamp);
    const auto mouseAtPrevPose = OptiInput::GetRawMouseMotionAt(prevPoseTimestamp);

    float deltaYaw = 0.0f;
    float deltaPitch = 0.0f;
    DecomposeCameraPairRotation(_cameraForward[fIndex], _cameraForward[prevIndex], _cameraRight[prevIndex],
                                _cameraUp[prevIndex], &deltaYaw, &deltaPitch);

    ReprojInputPredictor::OnPoseSample(poseTimestamp, intervalMs, deltaYaw, deltaPitch,
                                       static_cast<float>(mouseAtPose.TotalX - mouseAtPrevPose.TotalX),
                                       static_cast<float>(mouseAtPose.TotalY - mouseAtPrevPose.TotalY));
    _lastPredictorFeedPoseMs = poseTimestamp;
}

// Feed the input-predictor estimator from an async packet pose. The KCD2 hook
// pose pair is authoritative when available; otherwise fall back to the
// per-slot camera arrays.
void AReproj_Dx12::FeedInputPredictorFromPacket(int sourceIndex, const RP_Constants& constants, double poseTimestampMs,
                                                double poseIntervalMs, bool hookPose)
{
    if (!Config::Instance()->ReprojInputPredictor.value_or_default())
        return;
    if (poseTimestampMs <= 0.0 || poseTimestampMs <= _lastPredictorFeedPoseMs)
        return;

    const double intervalMs = poseIntervalMs > 1.0 ? poseIntervalMs : 16.6;
    float inputX = 0.0f;
    float inputY = 0.0f;
    Kcd2Input::MouseInterval kcd2Input {};
    if (hookPose && Kcd2Input::QueryMouseInterval(poseTimestampMs - intervalMs, poseTimestampMs, kcd2Input) &&
        kcd2Input.complete && (kcd2Input.yawEvents > 0 || kcd2Input.pitchEvents > 0))
    {
        inputX = static_cast<float>(kcd2Input.yaw);
        inputY = static_cast<float>(kcd2Input.pitch);
    }
    else
    {
        OptiInput::RefreshMouseMotion();
        const auto mouseAtPose = OptiInput::GetRawMouseMotionAt(poseTimestampMs);
        const auto mouseAtPrevPose = OptiInput::GetRawMouseMotionAt(poseTimestampMs - intervalMs);
        inputX = static_cast<float>(mouseAtPose.TotalX - mouseAtPrevPose.TotalX);
        inputY = static_cast<float>(mouseAtPose.TotalY - mouseAtPrevPose.TotalY);
    }

    float deltaYaw = 0.0f;
    float deltaPitch = 0.0f;
    if (hookPose)
    {
        DecomposeCameraPairRotation(constants.cameraForward, constants.prevCameraForward, constants.prevCameraRight,
                                    constants.prevCameraUp, &deltaYaw, &deltaPitch);
    }
    else
    {
        const auto prevIndex = (sourceIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
        if (_reset[sourceIndex] || _reset[prevIndex] || IsCameraAllZero(sourceIndex) || IsCameraAllZero(prevIndex))
            return;
        DecomposeCameraPairRotation(_cameraForward[sourceIndex], _cameraForward[prevIndex], _cameraRight[prevIndex],
                                    _cameraUp[prevIndex], &deltaYaw, &deltaPitch);
    }

    ReprojInputPredictor::OnPoseSample(poseTimestampMs, intervalMs, deltaYaw, deltaPitch, inputX, inputY);
    _lastPredictorFeedPoseMs = poseTimestampMs;
}

// True-timewarp prediction: compose the rotation the camera will have at the
// display deadline from the raw input stream (fresh mouse deltas since the
// rendered pose), calibrated against rendered pose history. Replaces the
// velocity-extrapolation term - it must never be added on top of it.
bool AReproj_Dx12::TryInputPredictedRotation(double poseTimestampMs, float* yawRadians, float* pitchRadians)
{
    if (yawRadians == nullptr || pitchRadians == nullptr || poseTimestampMs <= 0.0)
        return false;

    // Kcd2Input remains passive until its timing and camera response are
    // validated live. Do not alternate KCD2 between this estimator and the
    // rendered-pose fallback in the middle of a turn.
    if (Kcd2Camera::IsAvailable())
        return false;

    const auto nowMs = Util::MillisecondsNow();
    const auto windowMs = nowMs - poseTimestampMs;
    if (windowMs <= 0.0 || windowMs > Config::Instance()->ReprojMaxPoseAgeMs.value_or_default())
        return false;

    OptiInput::RefreshMouseMotion();
    const auto now = OptiInput::GetRawMouseMotion();
    const auto atPose = OptiInput::GetRawMouseMotionAt(poseTimestampMs);
    const float inputX = static_cast<float>(now.TotalX - atPose.TotalX);
    const float inputY = static_cast<float>(now.TotalY - atPose.TotalY);
    if (!std::isfinite(inputX) || !std::isfinite(inputY))
        return false;

    float gainX = Config::Instance()->ReprojMouseSensitivityX.value_or_default();
    float gainY = Config::Instance()->ReprojMouseSensitivityY.value_or_default();
    const bool manualGain = gainX > 0.0f && gainY > 0.0f;
    if (!manualGain && !ReprojInputPredictor::GetEstimatedGain(&gainX, &gainY))
    {
        _inputPredictorActive = false;
        return false;
    }

    // Hysteresis keeps the warp path from flapping between prediction and
    // velocity extrapolation while confidence hovers at the threshold.
    constexpr float enterThreshold = 0.45f;
    constexpr float exitThreshold = 0.30f;
    const bool confident =
        manualGain || ReprojInputPredictor::GetConfidence() >= (_inputPredictorActive ? exitThreshold : enterThreshold);
    if (!confident || !ReprojInputPredictor::IsInputDriven(inputX, inputY))
    {
        _inputPredictorActive = false;
        return false;
    }

    ReprojInputPredictor::RotationEstimate estimate {};
    constexpr float maxRotationRad = 0.35f;
    if (!ReprojInputPredictor::PredictRotation(gainX, gainY, inputX, inputY,
                                               Config::Instance()->ReprojInputPredictorResponse.value_or_default(),
                                               maxRotationRad, &estimate))
    {
        _inputPredictorActive = false;
        return false;
    }

    *yawRadians = estimate.yawRadians;
    *pitchRadians = estimate.pitchRadians;
    _inputPredictorActive = true;
    return true;
}

TargetPoseResolver::Result AReproj_Dx12::ResolveTargetPose(const ContentFrame& packet, double targetScanoutMs)
{
    TargetPoseResolver::Request request {};
    std::memcpy(request.content.position, packet.constants.cameraPosition, 3 * sizeof(float));
    std::memcpy(request.content.right, packet.constants.cameraRight, 3 * sizeof(float));
    std::memcpy(request.content.up, packet.constants.cameraUp, 3 * sizeof(float));
    std::memcpy(request.content.forward, packet.constants.cameraForward, 3 * sizeof(float));
    request.content.verticalFov = packet.constants.cameraVFov;
    request.content.timestampMs = packet.sourcePoseTimestamp;
    request.content.cutGeneration = packet.sourceCutGeneration;
    std::memcpy(request.previous.position, packet.constants.prevCameraPosition, 3 * sizeof(float));
    std::memcpy(request.previous.right, packet.constants.prevCameraRight, 3 * sizeof(float));
    std::memcpy(request.previous.up, packet.constants.prevCameraUp, 3 * sizeof(float));
    std::memcpy(request.previous.forward, packet.constants.prevCameraForward, 3 * sizeof(float));
    request.previous.verticalFov = packet.constants.cameraVFov;
    request.previous.timestampMs = packet.sourcePoseTimestamp - packet.sourcePoseInterval;
    request.previous.cutGeneration = packet.sourceCutGeneration;
    request.targetScanoutMs = targetScanoutMs;
    request.responseScale = Config::Instance()->ReprojInputPredictorResponse.value_or_default();
    return TargetPoseResolver::Resolve(request);
}

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

    if (Config::Instance()->FGDrawUIOverFG.value_or_default() && hudlessResource && uiResource && hudlessReady &&
        uiReady)
    {
        const auto hudlessDesc = hudlessResource->GetDesc();
        const auto backBufferDesc = gameBackBuffer->GetDesc();
        if (hudlessDesc.Width == backBufferDesc.Width && hudlessDesc.Height == backBufferDesc.Height &&
            NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(backBufferDesc.Format))
        {
            color = hudlessResource;
            colorState = hudlessState;
            packet.hasUi = true;
        }
    }

    auto cmdList = GetUICommandList(packetIndex);
    bool ok = cmdList != nullptr &&
              CopyPacketResource(cmdList, color, colorState, &packet.color, packet.colorState, L"Reproj_PacketColor");
    if (ok && velocity)
        ok = CopyPacketResource(cmdList, velocity->GetResource(), velocity->state, &packet.velocity,
                                packet.velocityState, L"Reproj_PacketVelocity");

    const bool hybrid = State::Instance().activeFgOutput == FGOutput::HybridTimewarp;
    packet.hasDepth = ok && warpAllowed && depth && (hybrid || Config::Instance()->ReprojUseDepth.value_or_default());
    if (packet.hasDepth)
        packet.hasDepth = CopyPacketResource(cmdList, depth->GetResource(), depth->state, &packet.depth,
                                             packet.depthState, L"Reproj_PacketDepth");

    if (ok && packet.hasUi)
    {
        packet.hasUi = CopyPacketResource(cmdList, uiResource, uiState, &packet.ui, packet.uiState, L"Reproj_PacketUI");
        if (!packet.hasUi)
        {
            LOG_WARN("Reproj: UI capture failed; using the composed game frame for this packet");
            ok = CopyPacketResource(cmdList, gameBackBuffer, D3D12_RESOURCE_STATE_PRESENT, &packet.color,
                                    packet.colorState, L"Reproj_PacketColor");
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
    packet.virtualContentTimestamp = now;
    packet.kind = ContentFrameKind::Real;
    packet.interpolationFraction = 1.0f;
    FillConstants(sourceIndex, packet.constants);
    packet.constants.hudlessSource = packet.hasUi ? 1u : 0u;
    const auto colorDesc = packet.color->GetDesc();
    const float fallbackAspect = colorDesc.Height > 0 ? static_cast<float>(colorDesc.Width) / colorDesc.Height : 0.0f;
    double kcd2PoseIntervalMs = 0.0;
    // Keep the HUD trace lazy: WHGame.dll is loaded after OptiScaler in KCD2. This is read-only
    // and fails closed on an unknown game build.
    Kcd2Scaleform::Initialize();
    Kcd2Input::Initialize();
    const auto kcd2CameraTimestamp =
        Kcd2Camera::ApplyToConstants(packet.constants, fallbackAspect, &kcd2PoseIntervalMs);
    if (kcd2CameraTimestamp > 0.0)
    {
        // KCD2's depth/projection conventions are not validated for reprojection.
        // Its hook supplies an authoritative orientation, so do not let a stale
        // generic depth-warp setting override the safe title-specific path.
        packet.constants.mode = 2;
    }
    packet.sourcePoseInterval = kcd2PoseIntervalMs;
    Kcd2Camera::Snapshot currentCamera {};
    Kcd2Camera::Snapshot previousCamera {};
    packet.sourceCutGeneration =
        Kcd2Camera::ReadSnapshots(currentCamera, previousCamera) ? currentCamera.cutGeneration : 0;
    // Rate-limited raw CCamera projection dump: lets a live session confirm the near/far field
    // mapping (stock CryEngine layout assumed) against in-game view distance before depth mode
    // is enabled. Values also land in telemetry slots via packet.constants.cameraNear/Far.
    if (kcd2CameraTimestamp > 0.0 && Config::Instance()->ReprojTelemetry.value_or_default())
    {
        static double lastProjectionLogMs = 0.0;
        const auto nowLogMs = Util::MillisecondsNow();
        if (nowLogMs - lastProjectionLogMs > 10000.0)
        {
            lastProjectionLogMs = nowLogMs;
            char projectionDescription[256];
            if (Kcd2Camera::DescribeProjection(projectionDescription, sizeof(projectionDescription)))
                LOG_INFO("KCD2 camera projection: {}", projectionDescription);
        }

        static double lastInputLogMs = 0.0;
        const auto inputLogMs = Util::MillisecondsNow();
        if (inputLogMs - lastInputLogMs > 10000.0)
        {
            lastInputLogMs = inputLogMs;
            char inputDescription[256];
            if (Kcd2Input::DescribeStats(inputDescription, sizeof(inputDescription)))
                LOG_INFO("KCD2 late input: {}", inputDescription);
        }
    }
    const auto cameraTimestamp = kcd2CameraTimestamp > 0.0 ? kcd2CameraTimestamp : _cameraTimestamp[sourceIndex];
    // Anchor pose age is measured from the camera timestamp; without one, fall
    // back to the frame delta so MaxPoseAgeMs still rejects stale anchors.
    const double sourceTimestamp =
        cameraTimestamp > 0.0 ? cameraTimestamp : now - std::clamp(packet.frameDelta, 0.0, 150.0);
    packet.hasCamera = packet.constants.mode != 0;
    if (!packet.hasDepth && !packet.hasCamera)
        packet.constants.mode = 0;
    OptiInput::RefreshMouseMotion();
    const auto mouse = OptiInput::GetRawMouseMotion();
    packet.sourceMouseX = mouse.TotalX;
    packet.sourceMouseY = mouse.TotalY;
    packet.sourceMouseTimestamp = sourceTimestamp > 0.0 ? sourceTimestamp : mouse.TimestampMs;
    packet.inputLatchReady = true;
    UpdateMouseSensitivity(sourceIndex, sourceTimestamp);
    // Estimator feed for input-predicted timewarp: prefer the authoritative
    // KCD2 hook pose pair; otherwise the per-slot camera arrays.
    FeedInputPredictorFromPacket(sourceIndex, packet.constants, sourceTimestamp, kcd2PoseIntervalMs,
                                 kcd2CameraTimestamp > 0.0);
    packet.warpAllowed = warpAllowed && velocity;
    packet.retirementFenceValue = 0;
    packet.frameId = ++_publishedFrameId;
    packet.sourcePoseTimestamp = sourceTimestamp;
    packet.virtualContentTimestamp = sourceTimestamp;
    packet.syncInterval = FGHooks::LastPresentSyncInterval();
    packet.presentFlags = FGHooks::LastPresentFlags();

    packet.generatedCount = 0;
    if (hybrid && packet.warpAllowed && packet.hasDepth && packet.hasCamera && packet.hasUi)
    {
        // FSR supplies distinct content motion while the final pass remains
        // rotation-only for both real and generated content. Alternating depth
        // behavior between frame kinds would create a visible cadence artifact.
        packet.constants.mode = 2;
        if (_hybridGenerator == nullptr)
            _hybridGenerator = std::make_unique<HybridFsrGenerator>();

        auto& generated = packet.generated[0];
        generated.kind = ContentFrameKind::Generated;
        generated.interpolationFraction = 0.5f;
        generated.renderTimestamp = now;
        const auto contentInterval = packet.sourcePoseInterval > 1.0 ? packet.sourcePoseInterval : packet.frameDelta;
        generated.sourcePoseInterval = contentInterval;
        generated.sourcePoseTimestamp = sourceTimestamp - contentInterval * 0.5;
        generated.virtualContentTimestamp = generated.sourcePoseTimestamp;
        generated.sourceCutGeneration = packet.sourceCutGeneration;
        generated.resetGeneration = packet.resetGeneration;
        generated.constants = packet.constants;
        generated.constants.mode = 2;
        generated.constants.hudlessSource = packet.constants.hudlessSource;

        for (int axis = 0; axis < 3; ++axis)
            generated.constants.cameraPosition[axis] =
                packet.constants.prevCameraPosition[axis] +
                (packet.constants.cameraPosition[axis] - packet.constants.prevCameraPosition[axis]) * 0.5f;

        // Linearly average forward, right, and up, then orthonormalize via Gram-Schmidt
        // to prevent shear or non-orthogonal rows in the explicit target pose.
        float fwd[3] = {};
        float rgt[3] = {};
        float upVec[3] = {};
        for (int axis = 0; axis < 3; ++axis)
        {
            fwd[axis] = packet.constants.prevCameraForward[axis] +
                        (packet.constants.cameraForward[axis] - packet.constants.prevCameraForward[axis]) * 0.5f;
            rgt[axis] = packet.constants.prevCameraRight[axis] +
                        (packet.constants.cameraRight[axis] - packet.constants.prevCameraRight[axis]) * 0.5f;
            upVec[axis] = packet.constants.prevCameraUp[axis] +
                          (packet.constants.cameraUp[axis] - packet.constants.prevCameraUp[axis]) * 0.5f;
        }

        const auto normalize3 = [](float* v)
        {
            const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (len2 > 1e-12f && std::isfinite(len2))
            {
                const float invLen = 1.0f / std::sqrt(len2);
                v[0] *= invLen;
                v[1] *= invLen;
                v[2] *= invLen;
            }
        };
        const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

        normalize3(fwd);
        const float rgtDotFwd = dot3(rgt, fwd);
        for (int axis = 0; axis < 3; ++axis)
            rgt[axis] -= fwd[axis] * rgtDotFwd;
        normalize3(rgt);

        // Orthonormal up from cross(forward, right), preserving sign with interpolated upVec
        float orthoUp[3] = {
            fwd[1] * rgt[2] - fwd[2] * rgt[1],
            fwd[2] * rgt[0] - fwd[0] * rgt[2],
            fwd[0] * rgt[1] - fwd[1] * rgt[0],
        };
        normalize3(orthoUp);
        if (dot3(orthoUp, upVec) < 0.0f)
        {
            orthoUp[0] = -orthoUp[0];
            orthoUp[1] = -orthoUp[1];
            orthoUp[2] = -orthoUp[2];
        }

        std::memcpy(generated.constants.cameraForward, fwd, sizeof(fwd));
        std::memcpy(generated.constants.cameraRight, rgt, sizeof(rgt));
        std::memcpy(generated.constants.cameraUp, orthoUp, sizeof(orthoUp));
        std::memcpy(generated.constants.prevCameraPosition, generated.constants.cameraPosition,
                    sizeof(generated.constants.cameraPosition));
        std::memcpy(generated.constants.prevCameraRight, generated.constants.cameraRight,
                    sizeof(generated.constants.cameraRight));
        std::memcpy(generated.constants.prevCameraUp, generated.constants.cameraUp,
                    sizeof(generated.constants.cameraUp));
        std::memcpy(generated.constants.prevCameraForward, generated.constants.cameraForward,
                    sizeof(generated.constants.cameraForward));

        const auto requestedFrames = std::clamp(Config::Instance()->HybridGeneratedFrames.value_or_default(), 1, 3);
        if (requestedFrames != 1)
        {
            static bool warnedThreeFrameRequest = false;
            if (!warnedThreeFrameRequest)
            {
                LOG_WARN("HybridTimewarp: {} generated frames requested; using the validated one-midpoint path",
                         requestedFrames);
                warnedThreeFrameRequest = true;
            }
        }

        const auto fgBeginMs = Util::MillisecondsNow();
        if (_hybridGenerator->Generate(_device, cmdList, packet, generated, packet.frameId, _reset[sourceIndex] != 0))
        {
            packet.generatedCount = 1;
            generated.fgDurationMs = Util::MillisecondsNow() - fgBeginMs;
        }
    }

    const bool submitted = SubmitUICommandList((UINT) packetIndex);
    packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex];
    packet.completionFence = _uiFence;
    packet.completionFenceValue = packet.captureFenceValue;
    for (uint32_t generatedIndex = 0; generatedIndex < packet.generatedCount; ++generatedIndex)
    {
        packet.generated[generatedIndex].completionFence = _uiFence;
        packet.generated[generatedIndex].completionFenceValue = packet.captureFenceValue;
    }
    if (!submitted)
        return false;
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
    ResourceBarrier(cmdList, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, packet.color, D3D12_RESOURCE_STATE_COPY_SOURCE, packet.colorState);
    backBuffer->Release();

    if (composeUi && packet.hasUi && _renderUI != nullptr && _renderUI->IsInit())
        _renderUI->Dispatch(realSwapChain, cmdList, packet.ui, packet.uiState);

    if (useTelemetryQuery && _warpTimestampHeap != nullptr && _warpTimestampReadback != nullptr &&
        queryStart + 1 < ReprojTelemetry::GPU_QUERY_COUNT)
    {
        cmdList->EndQuery(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart + 1);
        cmdList->ResolveQueryData(_warpTimestampHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryStart, 2, _warpTimestampReadback,
                                  queryStart * sizeof(UINT64));
    }

    if (!SubmitSCCommandList(outputIndex))
        return false;

    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    return true;
}

bool AReproj_Dx12::DispatchPacketWarp(int packetIndex, float timeStep, double scanoutDeadlineMs,
                                      uint32_t telemetryQueryStart, bool inputPredicted, float predictedYaw,
                                      float predictedPitch, const TargetPoseResolver::Pose* targetPose,
                                      ContentFrame* contentFrame)
{
    auto& packet = _packets[packetIndex];
    auto& content = contentFrame != nullptr ? *contentFrame : static_cast<ContentFrame&>(packet);
    if (_swapChain == nullptr || _warp == nullptr || !_warp->IsInit() || content.color == nullptr ||
        packet.velocity == nullptr || !packet.warpAllowed)
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
    const bool deferredLateLatch = (Config::Instance()->ReprojLateLatchFence.value_or_default() ||
                                    State::Instance().activeFgOutput == FGOutput::HybridTimewarp) &&
                                   _lateLatchFence != nullptr && _presentQueue != nullptr &&
                                   scanoutDeadlineMs > Util::MillisecondsNow();
    if (!deferredLateLatch)
    {
        if (targetPose != nullptr)
            PrepareTargetPoseConstants(constants, *targetPose);
        else if (inputPredicted)
            PrepareRotationConstants(constants, true, predictedYaw, predictedPitch);
        else if (content.kind == ContentFrameKind::Generated || !ApplyLateInput(constants, packet))
            PrepareRotationConstants(constants);
    }
    const bool useDepth = packet.hasDepth;
    const bool ok = _warp->Dispatch(cmdList, content.color, content.colorState, packet.velocity, packet.velocityState,
                                    useDepth ? packet.depth : nullptr, packet.depthState, _warpOutput[outputIndex],
                                    constants, outputIndex, deferredLateLatch);
    if (!ok)
    {
        backBuffer->Release();
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

    if (constants.debugView != 2 && packet.hasUi && _renderUI != nullptr && _renderUI->IsInit())
        _renderUI->Dispatch(realSwapChain, cmdList, packet.ui, packet.uiState);

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
        if (FAILED(_presentQueue->Wait(_lateLatchFence, lateLatchValue)))
        {
            _lateLatchFence->Signal(lateLatchValue);
            return false;
        }
    }

    if (!SubmitSCCommandList(outputIndex))
    {
        if (lateLatchValue != 0)
            _lateLatchFence->Signal(lateLatchValue);
        return false;
    }

    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    if (_currentTelemetrySlot && useTelemetryQuery)
        _currentTelemetrySlot->scFenceValue = packet.retirementFenceValue;
    if (lateLatchValue != 0)
    {
        // Submit early enough to sit behind Proton's game-queue backlog, then
        // release it with a target sampled immediately before the present
        // deadline. The target itself is predicted to scanout midpoint.
        constexpr double LATE_SAMPLE_LEAD_MS = 0.75;
        WaitForPresenterDeadline(scanoutDeadlineMs - LATE_SAMPLE_LEAD_MS);
        auto lateConstants = content.constants;
        lateConstants.timeStep = timeStep;
        const auto scanoutMidpointMs = scanoutDeadlineMs + 500.0 / std::max(1.0, TargetRefreshHz());
        bool prepared = false;
        if (Config::Instance()->ReprojTargetPoseResolver.value_or_default())
        {
            const auto lateTarget = ResolveTargetPose(content, scanoutMidpointMs);
            if (lateTarget.qualified && !Config::Instance()->ReprojTargetPoseShadow.value_or_default())
            {
                PrepareTargetPoseConstants(lateConstants, lateTarget.target);
                prepared = true;
            }
        }
        if (!prepared)
        {
            float latePredictedYaw = 0.0f;
            float latePredictedPitch = 0.0f;
            if (TryInputPredictedRotation(content.sourcePoseTimestamp, &latePredictedYaw, &latePredictedPitch))
                PrepareRotationConstants(lateConstants, true, latePredictedYaw, latePredictedPitch);
            else if (content.kind == ContentFrameKind::Generated || !ApplyLateInput(lateConstants, packet))
                PrepareRotationConstants(lateConstants);
        }

        const bool constantsWritten = _warp->WriteConstants(outputIndex, lateConstants);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (_currentTelemetrySlot)
            _currentTelemetrySlot->lateLatchSignalQpc = _telemetry.NowQpc();
        const auto signalResult = _lateLatchFence->Signal(lateLatchValue);
        if (!constantsWritten || FAILED(signalResult))
            return false;
    }
    return true;
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

    // True-timewarp prediction: rotate the warp by what the camera will have
    // done by this display deadline (fresh raw input since the rendered pose),
    // not by the last rendered velocity. Falls back to velocity extrapolation
    // when the estimator is not confident or the motion is not mouse-driven.
    // Input-predicted and extrapolated warps REPLACE each other; they are
    // never combined.
    FeedInputPredictor(fIndex);
    float predictedYaw = 0.0f;
    float predictedPitch = 0.0f;
    if (TryInputPredictedRotation(_cameraTimestamp[fIndex], &predictedYaw, &predictedPitch))
        PrepareRotationConstants(cb, true, predictedYaw, predictedPitch);
    else
        PrepareRotationConstants(cb);

    // Rate-limited predictor diagnostics (async stats land in the slot dumps).
    static double lastPredictorLogMs = 0.0;
    const auto predictorLogMs = Util::MillisecondsNow();
    if (Config::Instance()->ReprojInputPredictor.value_or_default() && predictorLogMs - lastPredictorLogMs > 10000.0)
    {
        lastPredictorLogMs = predictorLogMs;
        char predictorDescription[160];
        if (ReprojInputPredictor::DescribeStats(predictorDescription, sizeof(predictorDescription)))
            LOG_INFO("Reproj input predictor: {}", predictorDescription);
    }

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

bool AReproj_Dx12::ShouldCaptureAnchor(double nowMs)
{
    auto* config = Config::Instance();
    if (!config->ReprojNonBlockingAnchorSampling.value_or_default())
    {
        _nextAnchorSampleMs = 0.0;
        _anchorSampleHz = 0.0f;
        return true;
    }

    float requestedHz = config->ReprojAnchorSampleHz.value_or_default();
    if (!(std::isfinite(requestedHz) && requestedHz > 0.0f))
        requestedHz = config->ReprojSourceFramerateLimit.value_or_default();
    if (!(std::isfinite(requestedHz) && requestedHz > 0.0f))
        requestedHz = static_cast<float>(TargetRefreshHz() * 0.5);
    const float sampleHz = std::clamp(requestedHz, 1.0f, 1000.0f);
    const double periodMs = 1000.0 / sampleHz;

    // A changed setting or a resume after a long game stall starts a clean grid.
    if (_nextAnchorSampleMs <= 0.0 || std::abs(_anchorSampleHz - sampleHz) > 0.01f ||
        nowMs > _nextAnchorSampleMs + periodMs * 4.0)
    {
        _nextAnchorSampleMs = nowMs + periodMs;
        _anchorSampleHz = sampleHz;
        return true;
    }
    if (nowMs < _nextAnchorSampleMs)
        return false;

    const auto missedPeriods = std::floor((nowMs - _nextAnchorSampleMs) / periodMs);
    _nextAnchorSampleMs += (missedPeriods + 1.0) * periodMs;
    return true;
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
    LOG_INFO("Reproj: source={:.1f} FPS display={:.1f} FPS (new={} repeat={} sampledSkip={}) missed={} "
             "interval={:.2f}/{:.2f}ms "
             "lead={:.2f}ms poseAge={:.1f}ms queue={} pose=rendered ({}, block={:.2f}ms)",
             _metricsRealFrames * scale, _metricsWarpFrames * scale, _metricsNewAnchorDisplays,
             _metricsRepeatedAnchorDisplays, _metricsSkippedAnchorSamples, _metricsMissedDisplaySlots,
             _runtimeMetrics.meanPresentIntervalMs, _runtimeMetrics.p95PresentIntervalMs, _dispatchLeadMs, poseAge,
             _runtimeMetrics.queueDepth, presenter, _runtimeMetrics.gamePresentBlockMs);
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
    if (!virtualized || !IsActive() || IsPaused() ||
        (!Config::Instance()->ReprojAsync.value_or_default() &&
         State::Instance().activeFgOutput != FGOutput::HybridTimewarp))
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
    const bool nonBlockingAnchorSampling = Config::Instance()->ReprojNonBlockingAnchorSampling.value_or_default();
    const bool captureThisPresent = !virtualized || !nonBlockingAnchorSampling || ShouldCaptureAnchor(presentStart);
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
        if (!captureThisPresent)
        {
            // The virtual game buffer still needs its ownership handoff and ring advance, but no packet is copied.
            // This is deliberately non-blocking: the presenter keeps reprojecting its active anchor while KCD2
            // continues rendering at its natural rate.
            FrameLimit::paceReprojectionSource(false);
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
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            _runtimeMetrics.gamePresentBlockMs = static_cast<float>(Util::MillisecondsNow() - presentStart);
            return advanced;
        }

        RecordRealFrame();
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
            // Fixed source pacing remains available for A/B comparison, but non-blocking sampling
            // intentionally never sleeps the KCD2 game thread.
            FrameLimit::paceReprojectionSource(!nonBlockingAnchorSampling);
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
    // Fresh calibration per FG session; stale gains across mode/context
    // switches would poison the prediction.
    ReprojInputPredictor::Reset();
    _lastPredictorFeedPoseMs = 0.0;
    _inputPredictorActive = false;
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
        _runtimeMetrics = {};
    }
    _cachedRefreshHz = 0.0;
    _lastRefreshQueryMs = 0.0;
    _lastRealFrameTimestamp = 0.0;
    _nextAnchorSampleMs = 0.0;
    _anchorSampleHz = 0.0f;
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

            auto bufferCount = (Config::Instance()->ReprojAsync.value_or_default() ||
                                State::Instance().activeFgOutput == FGOutput::HybridTimewarp) &&
                                       desc->BufferCount < 3
                                   ? 3
                                   : desc->BufferCount;
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
    const bool asyncRequested = Config::Instance()->ReprojAsync.value_or_default() ||
                                State::Instance().activeFgOutput == FGOutput::HybridTimewarp;
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

            auto bufferCount = (Config::Instance()->ReprojAsync.value_or_default() ||
                                State::Instance().activeFgOutput == FGOutput::HybridTimewarp) &&
                                       desc->BufferCount < 3
                                   ? 3
                                   : desc->BufferCount;
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
    const bool asyncRequested = Config::Instance()->ReprojAsync.value_or_default() ||
                                State::Instance().activeFgOutput == FGOutput::HybridTimewarp;
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
    const bool asyncRequest = Config::Instance()->ReprojAsync.value_or_default() ||
                              State::Instance().activeFgOutput == FGOutput::HybridTimewarp;
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
    Kcd2HudIsolation::Reset();
    TargetPoseResolver::Reset();
    ReprojInputPredictor::Reset();
    _warp.reset();
    if (_hybridGenerator != nullptr)
        _hybridGenerator->Shutdown();
    _hybridGenerator.reset();

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
        for (auto& generated : _packets[i].generated)
        {
            SAFE_RELEASE(generated.color);
            generated.colorState = D3D12_RESOURCE_STATE_COMMON;
            generated.completionFence = nullptr;
            generated.completionFenceValue = 0;
        }

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
        _packets[i].generatedCount = 0;
        _packets[i].state.store(PacketState::Free);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    SAFE_RELEASE(_uiFence);
    SAFE_RELEASE(_scFence);
    SAFE_RELEASE(_lateLatchFence);
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
