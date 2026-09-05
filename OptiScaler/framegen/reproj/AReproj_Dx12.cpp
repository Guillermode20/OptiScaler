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

bool AReproj_Dx12::SignalLateLatch()
{
    if (_lateLatchPendingValue == 0)
        return true;

    if (_lateLatchFence == nullptr)
    {
        LOG_ERROR("Reproj: cannot release pending late-latch value {} without a fence", _lateLatchPendingValue);
        return false;
    }

    const auto value = _lateLatchPendingValue;
    const auto result = _lateLatchFence->Signal(value);
    if (FAILED(result))
    {
        LOG_ERROR("Reproj: late-latch fence signal failed for value {}: {:X}", value, (UINT) result);
        return false;
    }

    _lateLatchPendingValue = 0;
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
        return false;

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
        return false;

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
    ++_metricsLateInputApplied;
    _metricsLateInputMaxDegrees = std::max(
        _metricsLateInputMaxDegrees, static_cast<float>(std::hypot(yaw, pitch) * 180.0 / std::numbers::pi_v<double>));
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
    for (int i = 0; i < kReprojFrameSlots; ++i)
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
    // async-simple: capture is one inline submit on the game DIRECT queue's UI
    // command list. Non-blocking poll of that allocator's fence — if the
    // previous capture on this packet slot is still in flight the caller drops
    // the anchor instead of stalling the game thread inside GetUICommandList.
    if (packetIndex < 0 || packetIndex >= kReprojFrameSlots)
        return false;
    if (_uiCommandAllocator[packetIndex] == nullptr || _uiFence == nullptr)
        return true;
    const auto fenceValue = _uiAllocatorFenceValues[packetIndex];
    return fenceValue == 0 || _uiFence->GetCompletedValue() >= fenceValue;
}

void AReproj_Dx12::SkipAnchorPublication(int fIndex, ID3D12Resource* gameBackBuffer, UINT virtualBufferIndex,
                                         WrappedIDXGISwapChain4* wrapped, double presentStartMs)
{
    Kcd2HudIsolation::OnFrameCaptured(gameBackBuffer);

    // Drop publication but still retire and advance the logical game buffer.
    // The fence signal is enqueued on the game queue behind all prior work,
    // so it correctly orders after even the in-flight submission that forced
    // the skip; no allocator is touched, so nothing is reset under the GPU.
    // If the handoff itself cannot complete, fail the publication rather than
    // returning a virtual resource to the game while capture still owns it.
    const auto fenceValue = ++_uiFenceValue;
    _uiAllocatorFenceValues[fIndex] = fenceValue;
    if (_gameCommandQueue != nullptr && _uiFence != nullptr)
        _gameCommandQueue->Signal(_uiFence, fenceValue);
    // async-simple: never pass a handoff fence — AdvanceReprojectionBuffer must
    // not stall the game thread, and nothing reads the skipped buffer.
    const bool ok = wrapped != nullptr &&
                    SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, nullptr, 0));
    if (ok)
    {
        const auto advanceHr = wrapped->AdvanceReprojectionBuffer();
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
    RecordWarpFrame(false, true, 0.0f);
    SAFE_RELEASE(gameBackBuffer);
    // async-simple: OptiScaler never paces the game thread. block= covers all
    // game-present work here (the SourceFramerateLimit pacer is gone).
    const auto doneMs = Util::MillisecondsNow();
    std::scoped_lock metricsLock(_metricsMutex);
    ++_metricsSkippedAnchorSamples;
    _metricsGamePresentBlockMaxMs =
        std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(doneMs - presentStartMs));
}

bool AReproj_Dx12::CaptureFramePacket(int sourceIndex, int packetIndex, ID3D12Resource* gameBackBuffer,
                                      UINT virtualBufferIndex, bool warpAllowed)
{
    (void) virtualBufferIndex;
    auto& packet = _packets[packetIndex];
    if (gameBackBuffer == nullptr)
        return false;

    // HUD isolation (KCD2 Scaleform, live-validated on the parent branch): the
    // OM hook redirected the HUD into an isolated UI texture this frame, so the
    // warp source is the HUD-less world snapshot and the isolated UI is
    // composited unwarped by the warp shader. Both are copied in the same
    // inline submit below (on the queue the Scaleform CL ran on), so the UI is
    // always as fresh as the color — no borrow, no separate UI gate. Falls
    // back to the composed frame (HUD warps) when isolation is unavailable.
    ID3D12Resource* color = gameBackBuffer;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_PRESENT;
    packet.hasUi = false;
    D3D12_RESOURCE_STATES kcd2HudlessState = D3D12_RESOURCE_STATE_COMMON;
    auto* hudless = Kcd2HudIsolation::GetHudlessColor(gameBackBuffer, &kcd2HudlessState);
    D3D12_RESOURCE_STATES kcd2UiState = D3D12_RESOURCE_STATE_COMMON;
    auto* ui = Kcd2HudIsolation::GetUIColor(gameBackBuffer, &kcd2UiState);
    if (hudless != nullptr && ui != nullptr)
    {
        const auto hudlessDesc = hudless->GetDesc();
        const auto backBufferDesc = gameBackBuffer->GetDesc();
        if (hudlessDesc.Width == backBufferDesc.Width && hudlessDesc.Height == backBufferDesc.Height &&
            NormalizeReprojFormat(hudlessDesc.Format) == NormalizeReprojFormat(backBufferDesc.Format))
        {
            color = hudless;
            colorState = kcd2HudlessState;
            packet.hasUi = true;
        }
    }

    // Record the color (and, with isolation, the UI) copies on the packet's UI
    // command list. It is submitted on the game DIRECT queue below — the same
    // queue the frame rendered on, so the copies are GPU-ordered after the
    // frame's work (Scaleform CL included) and before any later render into
    // this virtual buffer. No handoff fence needed.
    bool ok = false;
    packet.completionFence = nullptr;
    packet.completionFenceValue = 0;
    packet.captureFenceValue = 0;
    auto cmdList = GetUICommandList(packetIndex);
    ok = cmdList != nullptr &&
         CopyPacketResource(cmdList, color, colorState, &packet.color, packet.colorState, L"Reproj_PacketColor") &&
         (!packet.hasUi || CopyPacketResource(cmdList, ui, kcd2UiState, &packet.ui, packet.uiState, L"Reproj_PacketUI"));
    if (!ok)
        return false;

    Kcd2HudIsolation::OnFrameCaptured(gameBackBuffer);

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
    // 0 = no isolated UI, 1 = premultiplied alpha, 2 = straight alpha (parent
    // branch semantics). The warp shader composites the UI unwarped after the
    // rotation warp. Derive the fallback aspect from the pinned source
    // (identical resource and resolution to the copy target). Scaleform in KCD2
    // outputs straight alpha (un-multiplied RGB, live-validated in pre25).
    packet.constants.hudlessSource =
        packet.hasUi ? (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() ? 2u : 1u) : 0u;
    const auto colorDesc = color->GetDesc();
    const float fallbackAspect = colorDesc.Height > 0 ? static_cast<float>(colorDesc.Width) / colorDesc.Height : 0.0f;
    double kcd2PoseIntervalMs = 0.0;
    // async-simple: camera comes straight from the Kcd2Camera hook. No
    // sensitivity-calibration accumulation on this branch — ApplyLateInput falls
    // back to rendered-camera extrapolation until a later phase.
    const auto kcd2CameraTimestamp =
        Kcd2Camera::ApplyToConstants(packet.constants, fallbackAspect, &kcd2PoseIntervalMs);
    if (kcd2CameraTimestamp > 0.0)
    {
        SetCameraData(packet.constants.cameraPosition, packet.constants.cameraUp, packet.constants.cameraRight,
                      packet.constants.cameraForward, sourceIndex);
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
    // An anchor is warpable when a valid camera pose exists. HUD isolation
    // never gates the anchor: color and UI are one submit, so both are equally
    // fresh by the time the capture fence completes.
    packet.warpAllowed = warpAllowed && packet.hasCamera;
    packet.retirementFenceValue = 0;
    packet.frameId = ++_publishedFrameId;
    packet.sourcePoseTimestamp = sourceTimestamp;

    // Submit the inline capture on the game DIRECT queue and record the fence
    // value the presenter waits on before warping this anchor.
    const bool submitted = SubmitUICommandList((UINT) packetIndex);
    packet.captureFenceValue = _uiAllocatorFenceValues[packetIndex];
    packet.completionFence = _uiFence;
    packet.completionFenceValue = packet.captureFenceValue;
    if (submitted)
    {
        std::scoped_lock lock(_metricsMutex);
        ++_metricsDirectCaptures;
    }
    return submitted;
}

bool AReproj_Dx12::DisplayPacket(int packetIndex)
{
    auto& packet = _packets[packetIndex];
    if (_swapChain == nullptr || packet.color == nullptr)
        return false;

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();

    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(outputIndex, IID_PPV_ARGS(&backBuffer))))
        return false;

    const bool compositeUi = packet.hasUi && packet.ui != nullptr && _warp != nullptr && _warp->IsInit();
    if (compositeUi)
    {
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

        auto constants = packet.constants;
        constants.timeStep = 0.0f;
        constants.mode = 0;
        std::memset(&constants.prevCameraRight, 0, sizeof(constants.prevCameraRight));
        std::memset(&constants.prevCameraUp, 0, sizeof(constants.prevCameraUp));
        std::memset(&constants.prevCameraForward, 0, sizeof(constants.prevCameraForward));

        const bool ok = _warp->Dispatch(cmdList, packet.color, packet.colorState, _warpOutput[outputIndex], constants,
                                        outputIndex, false /* deferConstants */, packet.ui, packet.uiState);
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
        backBuffer->Release();

        if (!SubmitSCCommandList(outputIndex))
            return false;

        packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
        const bool directGateQueued = _gameCommandQueue != nullptr && _scFence != nullptr &&
                                      SUCCEEDED(_gameCommandQueue->Wait(_scFence, packet.retirementFenceValue));
        if (!directGateQueued)
            return false;
        return true;
    }

    // Unwarped blits without isolated UI are pure copies on the presenter's DIRECT SC queue
    if (!WaitForSCAllocator(outputIndex))
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

    // Reserve the SC retirement fence value for this slot.
    _scAllocatorFenceValues[outputIndex] = ++_scFenceValue;

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

    if (!SubmitSCCommandList(outputIndex))
        return false;

    // The real swapchain was created on the game's DIRECT queue, while this
    // copy/UI list runs on _presentQueue. Order Present on the actual swapchain
    // queue with a GPU-side wait; never CPU-wait here.
    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
    const bool directGateQueued = _gameCommandQueue != nullptr && _scFence != nullptr &&
                                  SUCCEEDED(_gameCommandQueue->Wait(_scFence, packet.retirementFenceValue));
    if (!directGateQueued)
        return false;
    return true;
}

bool AReproj_Dx12::DispatchPacketWarp(int packetIndex, float timeStep, double scanoutDeadlineMs)
{
    auto& packet = _packets[packetIndex];
    auto& content = static_cast<ContentFrame&>(packet);
    if (_swapChain == nullptr || _warp == nullptr || !_warp->IsInit() || content.color == nullptr ||
        !packet.warpAllowed)
        return false;

    auto realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto outputIndex = (int) realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(outputIndex, IID_PPV_ARGS(&backBuffer))))
        return false;

    // async-simple: the presenter's one DIRECT queue (_presentQueue) owns the
    // warp. The warp and the final copy-to-backbuffer are recorded on the same
    // SC command list, and _scFence is the single retirement fence. The only
    // extra synchronization is the CPU-signaled late-latch fence: the queue
    // waits on it before the dispatch, while the presenter writes the selected
    // output's upload constants near the display deadline.
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

    // Reserve the SC retirement fence value for this slot; SubmitSCCommandList
    // signals it after the warp+copy dispatch.
    _scAllocatorFenceValues[outputIndex] = ++_scFenceValue;

    auto constants = content.constants;
    constants.timeStep = timeStep;
    const bool deferredLateLatch = _lateLatchFence != nullptr && _presentQueue != nullptr;
    if (!deferredLateLatch)
    {
        // Safe fallback for partial initialization: constants are written before
        // execution when no latch fence is available.
        if (!ApplyLateInput(constants, packet))
            PrepareRotationConstants(constants, false);
    }
    else
    {
        // Populate a valid baseline before queuing the command list. The GPU is
        // parked before it can read this upload buffer; the final pose replaces
        // it after submission and before the latch fence is released.
        PrepareRotationConstants(constants, false);
        if (!_warp->WriteConstants(outputIndex, constants))
        {
            backBuffer->Release();
            SubmitSCCommandList(outputIndex);
            packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
            return false;
        }
    }
    // The isolated UI (when the anchor was captured with HUD isolation) is
    // composited unwarped in the same dispatch; a composed capture dispatches
    // with ui == nullptr (RPD then samples color for both SRVs).
    const bool ok = _warp->Dispatch(cmdList, content.color, content.colorState, _warpOutput[outputIndex], constants,
                                    outputIndex, deferredLateLatch, packet.hasUi ? packet.ui : nullptr,
                                    packet.hasUi ? packet.uiState : D3D12_RESOURCE_STATE_COMMON);
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

    UINT64 lateLatchValue = 0;
    if (deferredLateLatch)
    {
        lateLatchValue = ++_lateLatchFenceValue;
        _lateLatchPendingValue = lateLatchValue;
        const auto waitResult = _presentQueue->Wait(_lateLatchFence, lateLatchValue);
        if (FAILED(waitResult))
        {
            SignalLateLatch();
            SubmitSCCommandList(outputIndex);
            packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];
            return false;
        }
    }

    if (!SubmitSCCommandList(outputIndex))
    {
        SignalLateLatch();
        return false;
    }

    // async-simple: _scFence is the single retirement fence — SubmitSCCommandList
    // signaled it after the warp+copy dispatch on _presentQueue.
    packet.retirementFenceValue = _scAllocatorFenceValues[outputIndex];

    if (deferredLateLatch)
    {
        const auto lateLeadCfg = Config::Instance()->ReprojLateSampleLead.value_or_default();
        const auto refreshHz = TargetRefreshHz();
        const auto refreshPeriodMs = refreshHz > 1.0 ? 1000.0 / refreshHz : 8.333;
        const auto maxUsableLeadMs = std::max(3.0, std::min(LATE_LATCH_MAX_MS, refreshPeriodMs * 0.75));
        const auto lateLatchLeadMs =
            std::min(std::clamp(lateLeadCfg > 0.5f ? static_cast<double>(lateLeadCfg) : LATE_LATCH_DEFAULT_MS,
                                LATE_LATCH_MIN_MS, LATE_LATCH_MAX_MS),
                     maxUsableLeadMs);
        _lastLateSampleLeadMs.store(lateLatchLeadMs, std::memory_order_relaxed);
        if (!WaitForPresenterDeadline(scanoutDeadlineMs - lateLatchLeadMs))
        {
            SignalLateLatch();
            return false;
        }

        auto lateConstants = content.constants;
        lateConstants.timeStep = timeStep;
        if (!ApplyLateInput(lateConstants, packet))
            PrepareRotationConstants(lateConstants, false);

        const bool constantsWritten = _warp->WriteConstants(outputIndex, lateConstants);
        // Publish the persistent upload-buffer write before releasing the GPU
        // wait. The fence is intentionally CPU-signaled; it never enters the
        // game's DIRECT queue.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const bool latchReleased = SignalLateLatch();
        if (!constantsWritten || !latchReleased)
            return false;
    }
    return true;
}

bool AReproj_Dx12::DrainGpuWork()
{
    // A presenter failure can leave the DIRECT queue parked behind the latch
    // gate. Release it before submitting or waiting for any remaining command
    // lists, and before the fence/queue objects can be destroyed.
    if (!SignalLateLatch())
        return false;

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
    _metricsRealFrames.fetch_add(1, std::memory_order_relaxed);
    if (_presenterState.load(std::memory_order_relaxed) != PresenterState::Running)
    {
        LogMetricsIfDue();
    }
}

void AReproj_Dx12::RecordWarpFrame(bool warpPresented, bool dropped, float poseAgeMs)
{
    {
        std::scoped_lock lock(_metricsMutex);
        _metricsWarpFrames += warpPresented;
        _metricsDroppedWarps += dropped;
        if (warpPresented)
        {
            _metricsPoseAgeTotalMs += poseAgeMs;
            ++_metricsPoseSamples;
        }
    }

    LogMetricsIfDue();
}

void AReproj_Dx12::LogMetricsIfDue()
{
    double elapsed = 0.0;
    double scale = 1.0;
    double poseAge = 0.0;
    uint32_t realFrames = 0;
    uint32_t warpFrames = 0;
    uint32_t newAnchorDisplays = 0;
    uint32_t repeatedAnchorDisplays = 0;
    uint32_t missedDisplaySlots = 0;
    uint32_t lateInputSamples = 0;
    uint32_t lateInputApplied = 0;
    uint32_t skippedAnchorSamples = 0;
    uint32_t directCaptures = 0;
    uint32_t captureNotReady = 0;
    float lateInputMaxDegrees = 0.0f;
    float gamePresentBlockMaxMs = 0.0f;
    float meanPresentIntervalMs = 0.0f;
    float p95PresentIntervalMs = 0.0f;
    int queueDepth = 0;
    const char* presenter = nullptr;

    {
        std::scoped_lock lock(_metricsMutex);
        const auto now = Util::MillisecondsNow();
        if (_metricsTimestamp == 0.0)
        {
            _metricsTimestamp = now;
            return;
        }

        elapsed = now - _metricsTimestamp;
        if (elapsed < 1000.0)
            return;

        scale = 1000.0 / elapsed;
        realFrames = _metricsRealFrames.exchange(0, std::memory_order_relaxed);
        warpFrames = _metricsWarpFrames;
        newAnchorDisplays = _metricsNewAnchorDisplays;
        repeatedAnchorDisplays = _metricsRepeatedAnchorDisplays;
        missedDisplaySlots = _metricsMissedDisplaySlots;
        lateInputSamples = _metricsLateInputSamples;
        lateInputApplied = _metricsLateInputApplied;
        skippedAnchorSamples = _metricsSkippedAnchorSamples;
        directCaptures = _metricsDirectCaptures;
        captureNotReady = _metricsCaptureNotReady;
        lateInputMaxDegrees = _metricsLateInputMaxDegrees;
        gamePresentBlockMaxMs = _metricsGamePresentBlockMaxMs;
        poseAge = _metricsPoseSamples > 0 ? _metricsPoseAgeTotalMs / _metricsPoseSamples : 0.0;

        _runtimeMetrics.realFps = static_cast<float>(realFrames * scale);
        _runtimeMetrics.warpFps = static_cast<float>(warpFrames * scale);
        _runtimeMetrics.displayFps = _runtimeMetrics.warpFps;
        _runtimeMetrics.poseAgeMs = static_cast<float>(poseAge);
        _runtimeMetrics.targetRefreshHz = static_cast<float>(TargetRefreshHz());
        _runtimeMetrics.warpsPerReal = _metricsMaxWarpsPerReal;
        _runtimeMetrics.droppedWarps = _metricsDroppedWarps;
        _runtimeMetrics.queueDepth = PacketQueueDepth();
        _runtimeMetrics.asyncPresenter = _presenterState.load() == PresenterState::Running &&
                                         _wrappedSwapChain != nullptr && _wrappedSwapChain->IsReprojectionVirtualized();
        _runtimeMetrics.newAnchorDisplays = newAnchorDisplays;
        _runtimeMetrics.repeatedAnchorDisplays = repeatedAnchorDisplays;
        _runtimeMetrics.missedDisplaySlots = missedDisplaySlots;
        _runtimeMetrics.droppedAnchors = skippedAnchorSamples;
        _runtimeMetrics.directCaptures = directCaptures;
        _runtimeMetrics.captureNotReady = captureNotReady;
        _runtimeMetrics.gamePresentBlockMs = gamePresentBlockMaxMs;

        if (_presentIntervalCount > 0)
        {
            std::vector<double> intervals(_presentIntervals, _presentIntervals + _presentIntervalCount);
            meanPresentIntervalMs =
                static_cast<float>(std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size());
            const auto p95 = intervals.begin() + static_cast<size_t>((intervals.size() - 1) * 0.95);
            std::nth_element(intervals.begin(), p95, intervals.end());
            p95PresentIntervalMs = static_cast<float>(*p95);
            _runtimeMetrics.meanPresentIntervalMs = meanPresentIntervalMs;
            _runtimeMetrics.p95PresentIntervalMs = p95PresentIntervalMs;
        }

        queueDepth = _runtimeMetrics.queueDepth;
        presenter = _runtimeMetrics.asyncPresenter ? "async virtual swapchain" : "safe sync";

        _metricsTimestamp = now;
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
        _metricsDirectCaptures = 0;
        _metricsCaptureNotReady = 0;
        _metricsLateInputMaxDegrees = 0.0f;
        _metricsGamePresentBlockMaxMs = 0.0f;
    }

    LOG_INFO("Reproj: source={:.1f} FPS display={:.1f} FPS (new={} repeat={}) missed={} "
             "interval={:.2f}/{:.2f}ms latchLead={:.2f}ms poseAge={:.1f}ms queue={} "
             "late={}/{} maxDeg={:.2f} dropAnchor={} capC={} capWait={} "
             "({}, block={:.2f}ms)",
             realFrames * scale, warpFrames * scale, newAnchorDisplays,
             repeatedAnchorDisplays, missedDisplaySlots, meanPresentIntervalMs,
             p95PresentIntervalMs, _lastLateSampleLeadMs.load(std::memory_order_relaxed), poseAge, queueDepth,
             lateInputApplied, lateInputSamples, lateInputMaxDegrees,
             skippedAnchorSamples, directCaptures, captureNotReady,
             presenter, gamePresentBlockMaxMs);
}

AReproj_Dx12::RuntimeMetrics AReproj_Dx12::GetRuntimeMetrics() const
{
    std::scoped_lock lock(_metricsMutex);
    return _runtimeMetrics;
}

bool AReproj_Dx12::VirtualAnchorReady() const
{
    if (_device == nullptr || _gameCommandQueue == nullptr || _uiFence == nullptr || _uiFenceEvent == nullptr ||
        _scFence == nullptr || _scFenceEvent == nullptr || _lateLatchFence == nullptr)
        return false;

    for (int i = 0; i < BUFFER_COUNT; ++i)
    {
        if (_uiCommandAllocator[i] == nullptr || _uiCommandList[i] == nullptr || _scCommandAllocator[i] == nullptr ||
            _scCommandList[i] == nullptr)
            return false;
    }
    return true;
}

// async-simple: minimal passthrough display for a virtualized real swapchain
// whose presenter is stopped (feature inactive/paused, or permanently failed).
// Copies the game's just-rendered virtual buffer into the current real
// backbuffer on the game DIRECT queue so the following PresentFrame shows the
// real frame unchanged — no warp, no generated frames. Same-queue ordering
// after the game's render makes the copy safe without a cross-queue fence.
bool AReproj_Dx12::BlitGameFrameToReal(int fIndex, ID3D12Resource* gameBackBuffer)
{
    if (_swapChain == nullptr || gameBackBuffer == nullptr)
        return false;
    auto* realSwapChain = static_cast<IDXGISwapChain3*>(_swapChain);
    const auto realIndex = realSwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* realBuffer = nullptr;
    if (FAILED(realSwapChain->GetBuffer(realIndex, IID_PPV_ARGS(&realBuffer))))
        return false;

    auto* cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        realBuffer->Release();
        return false;
    }
    ResourceBarrier(cmdList, gameBackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(realBuffer, gameBackBuffer);
    ResourceBarrier(cmdList, realBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, gameBackBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    realBuffer->Release();
    return SubmitUICommandList(static_cast<UINT>(fIndex));
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

    // async-simple: there is no game-thread pacing grid to discard. OptiScaler
    // never throttles the source, whatever the virtualization/active state.
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
        // async-simple: no generated-frame fallback. Stop the presenter (it
        // owns the real swapchain), blit the game's virtual frame into the real
        // chain when virtualization is up, and present unchanged.
        if (virtualized)
        {
            StopAsyncPresenter();
            DrainGpuWork();
            DestroyAsyncPresenter();
            if (!BlitGameFrameToReal(GetIndexWillBeDispatched(), gameBackBuffer))
            {
                SAFE_RELEASE(gameBackBuffer);
                return false;
            }
        }
        const auto result = PresentFrame(syncInterval, presentFlags);
        SAFE_RELEASE(gameBackBuffer);
        return SUCCEEDED(result);
    }

    if (_presenterState.load() == PresenterState::Failed)
    {
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
            if (_gameCommandQueue != nullptr && _uiFence != nullptr)
                _gameCommandQueue->Signal(_uiFence, fenceValue);
            // async-simple: never pass a handoff fence on a skipped anchor —
            // Advance must not stall the game thread.
            const bool advanced =
                wrapped != nullptr &&
                SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, nullptr, 0)) &&
                SUCCEEDED(wrapped->AdvanceReprojectionBuffer());
            if (!advanced)
                _presenterState.store(PresenterState::Failed);
            else
            {
                std::scoped_lock metricsLock(_metricsMutex);
                ++_metricsSkippedAnchorSamples;
            }
            Kcd2HudIsolation::OnFrameCaptured(gameBackBuffer);
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            // async-simple: no source pacing; block= covers the whole present.
            const auto doneMs = Util::MillisecondsNow();
            _metricsGamePresentBlockMaxMs =
                std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(doneMs - presentStart));
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
        const bool captured = CaptureFramePacket(fIndex, packetIndex, gameBackBuffer, virtualBufferIndex, warpAllowed);
        // async-simple: capture is inline on the game's DIRECT queue, so the
        // copy is GPU-ordered before any later render into this virtual buffer
        // on the same queue — the handoff needs no fence. Release the buffer
        // immediately; the warp gate is the _uiFence capture value below.
        const bool submitted = captured && SUCCEEDED(wrapped->SubmitReprojectionBuffer(virtualBufferIndex, nullptr, 0));
        HRESULT advanceHr = E_FAIL;
        const bool advanced = submitted && SUCCEEDED(advanceHr = wrapped->AdvanceReprojectionBuffer());
        if (captured && submitted && advanced)
        {
            packet.state.store(PacketState::Ready);
            _readyFrameId.store(packet.frameId);
            _presentCv.notify_one();
            SAFE_RELEASE(gameBackBuffer);
            std::scoped_lock metricsLock(_metricsMutex);
            // async-simple: no source pacing. The presenter owns display
            // cadence and needs no game-thread cap; block= covers the whole
            // present (capture submit + publication) here.
            const auto doneMs = Util::MillisecondsNow();
            _metricsGamePresentBlockMaxMs =
                std::max(_metricsGamePresentBlockMaxMs, static_cast<float>(doneMs - presentStart));
            return true;
        }

        // Any failed virtual-buffer handoff is a hard ownership failure. The
        // current virtual buffer remains Capturing, so returning to the game
        // would let it render into a resource still read by capture. Downgrade
        // permanently: stop the presenter and display this frame unchanged via
        // a direct virtual->real blit — never a generated-frame fallback.
        packet.state.store(PacketState::Retired);
        _presenterState.store(PresenterState::Failed);
        StopAsyncPresenter();
        DrainGpuWork();
        DestroyAsyncPresenter();
        _asyncDowngraded = true;
        _presenterState.store(PresenterState::Stopped);
        bool ringAdvanced = advanced;
        if (submitted && !ringAdvanced)
            ringAdvanced = SUCCEEDED(wrapped->AdvanceReprojectionBuffer());
        HRESULT fallbackResult = E_FAIL;
        // The blit is queued after the game's render on the same DIRECT queue,
        // so no cross-queue fence is needed to read the virtual buffer here.
        if (ringAdvanced && BlitGameFrameToReal(fIndex, gameBackBuffer))
            fallbackResult = PresentFrame(syncInterval, presentFlags);
        LOG_WARN("Reproj: async publication failed; async downgrade result {:X}", (UINT) fallbackResult);
        SAFE_RELEASE(gameBackBuffer);
        return SUCCEEDED(fallbackResult);
    }

    // Async ownership is required for timewarp. If it is unavailable, display
    // the game frame unchanged: blit the virtual buffer into the real chain
    // when virtualization is up, else present straight through. Never run a
    // blocking generated-frame fallback.
    HRESULT fallbackResult = E_FAIL;
    if (virtualized)
    {
        if (BlitGameFrameToReal(fIndex, gameBackBuffer))
            fallbackResult = PresentFrame(syncInterval, presentFlags);
    }
    else
        fallbackResult = PresentFrame(syncInterval, presentFlags);
    SAFE_RELEASE(gameBackBuffer);
    return SUCCEEDED(fallbackResult);
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
        _metricsDirectCaptures = 0;
        _metricsCaptureNotReady = 0;
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
            pkt.retirementFenceValue = 0;
            pkt.frameId = 0;
            pkt.hasCamera = false;
            pkt.warpAllowed = false;
        }
    }
    _publishedFrameId.store(0);
    _readyFrameId.store(0);
    _presenterState.store(PresenterState::Stopped);

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

    // Real-chain/output resources stay at BUFFER_COUNT.
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        SAFE_RELEASE(_warpOutput[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }
    // FrameSlot[3] packet ring.
    for (size_t i = 0; i < kReprojFrameSlots; i++)
    {
        SAFE_RELEASE(_packets[i].color);
        SAFE_RELEASE(_packets[i].ui);
        _packets[i].colorState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].uiState = D3D12_RESOURCE_STATE_COMMON;
        _packets[i].captureFenceValue = 0;
        _packets[i].completionFence = nullptr;
        _packets[i].completionFenceValue = 0;
        _packets[i].retirementFenceValue = 0;
        _packets[i].warpAllowed = false;
        _packets[i].state.store(PacketState::Free);
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
    _lateLatchPendingValue = 0;
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
