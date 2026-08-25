#include "pch.h"
#include "Kcd2Camera.h"

#include "Logger.h"
#include "Util.h"
#include "shaders/reprojection/RP_Common.h"
#include "scanner/scanner.h"
#include <detours/detours.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Kcd2Camera
{
namespace
{
constexpr uintptr_t CViewCameraOffset = 0xE8;
using FrustumBuildFn = uintptr_t(__fastcall*)(uintptr_t camera);
FrustumBuildFn g_original = nullptr;
// 0 waits for WHGame.dll, 1 installs, 2 installed, 3 permanent signature/detour failure.
std::atomic<int> g_initState { 0 };
std::atomic<uint64_t> g_sequence { 0 };

struct Pose
{
    float position[3] {};
    float right[3] {};
    float up[3] {};
    float forward[3] {};
    float verticalFov = 0.0f;
    // Raw CCamera floats at +0x30..+0x64 (14 floats, stride 4). Stock CryEngine layout:
    // [0]=fov [1]=width [2]=height [3]=projRatio [4]=pixelAspect
    // [5..7]=m_edge_nlt xyz (y @0x48 == near plane) [8..10]=m_edge_plt xyz
    // [11..13]=m_edge_flt xyz (y @0x60 == far plane). KCD2's fork may differ; the raw
    // block is published so live telemetry can confirm or correct the mapping.
    static constexpr int PROJECTION_FLOAT_COUNT = 14;
    float projectionRaw[PROJECTION_FLOAT_COUNT] {};
    double timestampMs = 0.0;
};

// Seqlock: the render thread writes; game/presenter capture threads read without blocking.
Pose g_current {};
Pose g_previous {};

bool IsFiniteBasis(const Pose& p)
{
    const auto length2 = [](const float* v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; };
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    for (float value : p.position)
        if (!std::isfinite(value))
            return false;
    return std::isfinite(p.verticalFov) && p.verticalFov > 0.05f && p.verticalFov < 3.0f &&
           length2(p.right) > 0.8f && length2(p.right) < 1.2f && length2(p.up) > 0.8f &&
           length2(p.up) < 1.2f && length2(p.forward) > 0.8f && length2(p.forward) < 1.2f &&
           std::abs(dot(p.right, p.up)) < 0.1f && std::abs(dot(p.right, p.forward)) < 0.1f &&
           std::abs(dot(p.up, p.forward)) < 0.1f;
}

bool PoseChanged(const Pose& current, const Pose& next)
{
    // The gameplay CView can build the same frustum several times per rendered frame. Timestamp-only
    // filtering occasionally promoted a delayed duplicate to "previous", producing a zero-velocity
    // camera pair and a visible hitch. Matrix values are copied from the same CView, so a tiny squared
    // delta reliably separates exact duplicate callbacks while retaining slow camera movement.
    double delta2 = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double positionDelta = static_cast<double>(next.position[i]) - current.position[i];
        const double rightDelta = static_cast<double>(next.right[i]) - current.right[i];
        const double upDelta = static_cast<double>(next.up[i]) - current.up[i];
        const double forwardDelta = static_cast<double>(next.forward[i]) - current.forward[i];
        delta2 += positionDelta * positionDelta + rightDelta * rightDelta + upDelta * upDelta +
                  forwardDelta * forwardDelta;
    }
    const double fovDelta = static_cast<double>(next.verticalFov) - current.verticalFov;
    return delta2 + fovDelta * fovDelta > 1.0e-12;
}

bool IsCViewVtable(uintptr_t vtable)
{
    if (vtable < 0x10000)
        return false;
    __try
    {
        // MSVC x64 vtable[-1] is _RTTICompleteObjectLocator. Its pTypeDescriptor is an image-relative RVA.
        const auto locator = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uintptr_t*>(vtable)[-1]);
        if (!locator || locator[0] != 1)
            return false;
        const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"WHGame.dll"));
        const auto type = reinterpret_cast<const char*>(module + locator[3] + 16); // TypeDescriptor::name
        return std::strcmp(type, ".?AVCView@@") == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void PublishPose(uintptr_t camera)
{
    __try
    {
        const auto cview = camera - CViewCameraOffset;
        if (!IsCViewVtable(*reinterpret_cast<const uintptr_t*>(cview)))
            return;

        const auto matrix = reinterpret_cast<const float(*)[4]>(camera);
        Pose pose {};
        for (int i = 0; i < 3; ++i)
        {
            pose.right[i] = matrix[i][0];
            pose.forward[i] = matrix[i][1];
            pose.up[i] = matrix[i][2];
            pose.position[i] = matrix[i][3];
        }
        pose.verticalFov = *reinterpret_cast<const float*>(camera + 0x30);
        std::memcpy(pose.projectionRaw, reinterpret_cast<const float*>(camera + 0x30),
                    sizeof(float) * Pose::PROJECTION_FLOAT_COUNT);
        pose.timestampMs = Util::MillisecondsNow();
        if (!IsFiniteBasis(pose))
            return;

        g_sequence.fetch_add(1, std::memory_order_acq_rel); // odd: write in progress
        // Frustum construction can run repeatedly for one rendered view. Exact duplicates inside the
        // callback burst must not become a false zero-velocity pair, but a stationary camera still needs
        // a fresh zero-velocity pair on the next rendered frame or the last nonzero turn delta keeps being
        // extrapolated as a slow creep. KCD2's capped source frames are ~16.7 ms apart; 8 ms separates
        // callback bursts without the overly aggressive old 2 ms threshold.
        constexpr double DISTINCT_FRAME_GAP_MS = 8.0;
        if (g_current.timestampMs <= 0.0)
            g_previous = pose;
        else if (PoseChanged(g_current, pose) || pose.timestampMs - g_current.timestampMs >= DISTINCT_FRAME_GAP_MS)
            g_previous = g_current;
        g_current = pose;
        g_sequence.fetch_add(1, std::memory_order_release); // even: published
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

uintptr_t __fastcall Hook(uintptr_t camera)
{
    PublishPose(camera);
    return g_original(camera);
}

bool ReadPoses(Pose& current, Pose& previous)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto before = g_sequence.load(std::memory_order_acquire);
        if ((before & 1) != 0 || before == 0)
            continue;
        current = g_current;
        previous = g_previous;
        const auto after = g_sequence.load(std::memory_order_acquire);
        if (before == after)
            return previous.timestampMs > 0.0;
    }
    return false;
}
} // namespace

bool Initialize()
{
    if (g_initState.load(std::memory_order_acquire) == 2)
        return true;
    const auto module = GetModuleHandleW(L"WHGame.dll");
    if (!module)
        return false; // KCD2 loads WHGame.dll after OptiScaler initialization; retry from packet capture.
    int expected = 0;
    if (!g_initState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 2;

    static constexpr const char* patterns[] = {
        "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ? 48 81 EC ? ? 00 00 F3 0F 10 09 48 8B D9",
        "55 53 56 57 41 54 41 55 41 56 41 57 48 8D 68 ? 48 81 EC ? ? 00 00 F3 0F 10 09 48 8B D9 F3 0F 10 59 08",
        "F3 0F 10 09 48 8B D9 F3 0F 10 59 08 F3 0F 10 51 10 F3 0F 10 41 24 F3 0F 10 61 28"
    };
    static constexpr ptrdiff_t offsets[] = { 0, -3, -0x1A };
    uintptr_t address = 0;
    for (size_t i = 0; i < std::size(patterns) && !address; ++i)
        address = scanner::GetAddress(module, patterns[i], offsets[i]);
    if (!address)
    {
        LOG_WARN("KCD2 camera: CCamera::UpdateFrustumPlanes signature not found");
        g_initState.store(3, std::memory_order_release);
        return false;
    }

    g_original = reinterpret_cast<FrustumBuildFn>(address);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_original), Hook);
    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("KCD2 camera: detour failed: {}", result);
        g_original = nullptr;
        g_initState.store(3, std::memory_order_release);
        return false;
    }
    g_initState.store(2, std::memory_order_release);
    LOG_INFO("KCD2 camera: gameplay CView acquisition installed at {:X}", address);
    return true;
}

bool IsAvailable()
{
    Pose current, previous;
    return ReadPoses(current, previous) && Util::MillisecondsNow() - current.timestampMs < 250.0;
}

double ApplyToConstants(RP_Constants& constants, float fallbackAspect, double* poseIntervalMs)
{
    if (poseIntervalMs)
        *poseIntervalMs = 0.0;
    if (g_initState.load(std::memory_order_acquire) == 0)
        Initialize();
    Pose current, previous;
    if (!ReadPoses(current, previous) || Util::MillisecondsNow() - current.timestampMs > 250.0)
        return 0.0;

    if (poseIntervalMs)
        *poseIntervalMs = current.timestampMs - previous.timestampMs;
    std::memcpy(constants.cameraPosition, current.position, sizeof(current.position));
    std::memcpy(constants.cameraRight, current.right, sizeof(current.right));
    std::memcpy(constants.cameraUp, current.up, sizeof(current.up));
    std::memcpy(constants.cameraForward, current.forward, sizeof(current.forward));
    std::memcpy(constants.prevCameraPosition, previous.position, sizeof(previous.position));
    std::memcpy(constants.prevCameraRight, previous.right, sizeof(previous.right));
    std::memcpy(constants.prevCameraUp, previous.up, sizeof(previous.up));
    std::memcpy(constants.prevCameraForward, previous.forward, sizeof(previous.forward));
    constants.cameraVFov = current.verticalFov;
    // Prefer the game's own viewport dimensions over the render-target fallback.
    const auto projWidth = static_cast<int>(current.projectionRaw[1]);
    const auto projHeight = static_cast<int>(current.projectionRaw[2]);
    if (projWidth >= 16 && projWidth <= 16384 && projHeight >= 16 && projHeight <= 16384)
        constants.cameraAspect = static_cast<float>(projWidth) / static_cast<float>(projHeight);
    else
        constants.cameraAspect = fallbackAspect;
    // Near/far candidates from the stock CryEngine CCamera layout. Published into the constants
    // for telemetry visibility only: mode stays rotation-only until these values are validated
    // against in-game view distance and clipping behaviour.
    const float nearCandidate = current.projectionRaw[6]; // m_edge_nlt.y @ +0x48
    const float farCandidate = current.projectionRaw[12]; // m_edge_flt.y @ +0x60
    if (std::isfinite(nearCandidate) && nearCandidate > 0.0f && nearCandidate < 100.0f &&
        std::isfinite(farCandidate) && farCandidate > 100.0f && farCandidate < 200000.0f &&
        farCandidate > nearCandidate)
    {
        constants.cameraNear = nearCandidate;
        constants.cameraFar = farCandidate;
    }
    // KCD2 currently supplies depth but no trustworthy near/far projection constants. Mode 1 combines
    // camera/depth with motion vectors and produced severe double-warp wobble/artifacts. Validate the
    // acquired pose independently in camera rotation-only mode; enable depth only after its projection
    // convention and near/far values are extracted and verified.
    constants.mode = 2;
    return current.timestampMs;
}

bool DescribeProjection(char* buffer, size_t size)
{
    if (buffer == nullptr || size == 0)
        return false;
    Pose current, previous;
    if (!ReadPoses(current, previous))
        return false;

    const auto& r = current.projectionRaw;
    std::snprintf(buffer, size,
                  "fov=%.4f dim=%dx%d ratio=%.4f pixA=%.4f | "
                  "edge_nlt=(%.3f,%.3f,%.3f) edge_plt=(%.3f,%.3f,%.3f) edge_flt=(%.3f,%.3f,%.3f)",
                  r[0], static_cast<int>(r[1]), static_cast<int>(r[2]), r[3], r[4], r[5], r[6], r[7], r[8], r[9],
                  r[10], r[11], r[12], r[13]);
    return true;
}
} // namespace Kcd2Camera
