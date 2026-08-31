#include "pch.h"
#include "Kcd2Camera.h"
#include "Kcd2Input.h"

#include "Logger.h"
#include "Util.h"
#include "shaders/reprojection/RP_Common.h"
#include "menu/menu_common.h"
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
std::atomic<uint64_t> g_poseSequence { 0 };
std::atomic<uint64_t> g_cutGeneration { 1 };

struct Pose
{
    float position[3] {};
    float right[3] {};
    float up[3] {};
    float forward[3] {};
    float verticalFov = 0.0f;
    // Raw CCamera floats at +0x30..+0x7C (20 floats, stride 4). Live-validated mapping (retail
    // 1.5.6, see "KCD2 camera projection" log line):
    //   [0] @0x30 = vertical FOV (rad)
    //   [1],[2] @0x34/0x38 = repurposed (reads 1,0 - NOT viewport dims)
    //   [4] @0x40 = pixel aspect (16/9 -> 1.7778)
    //   [8..10] @0x50..0x58 = near-edge vector: y @0x54 = near plane (0.05),
    //                       x/z = near*tan(half-h/v FOV) (sign-flipped x)
    //   [11..13] @0x5C..0x64 = projection-plane edge: y @0x60 = (1/tan(fov/2))*height/2,
    //                       x/z = +/- half extents (e.g. -1280, 720 for 2560x1440)
    //   [14..19] @0x68..0x7C = unmapped; captured to locate the far plane.
    // Stock CryEngine stores these one Vec3 earlier; KCD2 shifted the block.
    static constexpr int PROJECTION_FLOAT_COUNT = 20;
    float projectionRaw[PROJECTION_FLOAT_COUNT] {};
    double timestampMs = 0.0;
    uintptr_t cameraIdentity = 0;
    uint64_t sequence = 0;
    uint64_t cutGeneration = 0;
};

// Seqlock: the render thread writes; game/presenter capture threads read without blocking.
Pose g_current {};
Pose g_previous {};

// EMA smoothing state for camera angular velocity (0=off). Lives in the anonymous namespace
// so the menu-visible reset can clear it.
float g_smoothedDeltaPos[3] {};
float g_smoothedDeltaRight[3] {};
float g_smoothedDeltaUp[3] {};
float g_smoothedDeltaForward[3] {};
bool g_smoothingInit = false;
float g_lastSmoothing = -1.0f;

bool IsFiniteBasis(const Pose& p)
{
    const auto length2 = [](const float* v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; };
    const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    for (float value : p.position)
        if (!std::isfinite(value))
            return false;
    return std::isfinite(p.verticalFov) && p.verticalFov > 0.05f && p.verticalFov < 3.0f && length2(p.right) > 0.8f &&
           length2(p.right) < 1.2f && length2(p.up) > 0.8f && length2(p.up) < 1.2f && length2(p.forward) > 0.8f &&
           length2(p.forward) < 1.2f && std::abs(dot(p.right, p.up)) < 0.1f &&
           std::abs(dot(p.right, p.forward)) < 0.1f && std::abs(dot(p.up, p.forward)) < 0.1f;
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
        delta2 +=
            positionDelta * positionDelta + rightDelta * rightDelta + upDelta * upDelta + forwardDelta * forwardDelta;
    }
    const double fovDelta = static_cast<double>(next.verticalFov) - current.verticalFov;
    return delta2 + fovDelta * fovDelta > 1.0e-12;
}

bool IsCameraCut(const Pose& current, const Pose& next)
{
    if (current.timestampMs <= 0.0)
        return true;
    if (current.cameraIdentity != next.cameraIdentity)
        return true;

    double positionDelta2 = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const auto delta = static_cast<double>(next.position[i]) - current.position[i];
        positionDelta2 += delta * delta;
    }
    const auto forwardDot = static_cast<double>(current.forward[0]) * next.forward[0] +
                            static_cast<double>(current.forward[1]) * next.forward[1] +
                            static_cast<double>(current.forward[2]) * next.forward[2];
    return positionDelta2 > 25.0 || forwardDot < 0.5 ||
           std::abs(static_cast<double>(next.verticalFov) - current.verticalFov) > 0.05;
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

        const auto matrix = reinterpret_cast<const float (*)[4]>(camera);
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
        pose.cameraIdentity = cview;
        if (!IsFiniteBasis(pose))
            return;

        g_sequence.fetch_add(1, std::memory_order_acq_rel); // odd: write in progress
        const bool cut = IsCameraCut(g_current, pose);
        pose.cutGeneration = cut ? g_cutGeneration.fetch_add(1, std::memory_order_relaxed) + 1
                                 : g_cutGeneration.load(std::memory_order_relaxed);
        pose.sequence = g_poseSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        // Frustum construction can run repeatedly for one rendered view. Exact duplicates inside the
        // callback burst must not become a false zero-velocity pair, but a stationary camera still needs
        // a fresh zero-velocity pair on the next rendered frame or the last nonzero turn delta keeps being
        // extrapolated as a slow creep. KCD2's capped source frames are ~16.7 ms apart; 8 ms separates
        // callback bursts without the overly aggressive old 2 ms threshold.
        constexpr double DISTINCT_FRAME_GAP_MS = 8.0;
        if (cut || g_current.timestampMs <= 0.0)
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

bool ReadSnapshots(Snapshot& current, Snapshot& previous)
{
    Pose currentPose, previousPose;
    if (!ReadPoses(currentPose, previousPose))
        return false;

    const auto copy = [](const Pose& source, Snapshot& target)
    {
        std::memcpy(target.position, source.position, sizeof(target.position));
        std::memcpy(target.right, source.right, sizeof(target.right));
        std::memcpy(target.up, source.up, sizeof(target.up));
        std::memcpy(target.forward, source.forward, sizeof(target.forward));
        target.verticalFov = source.verticalFov;
        target.pixelAspect = source.projectionRaw[4];
        target.nearPlane = source.projectionRaw[9];
        target.farPlane = source.projectionRaw[15];
        target.timestampMs = source.timestampMs;
        target.cameraIdentity = source.cameraIdentity;
        target.sequence = source.sequence;
        target.cutGeneration = source.cutGeneration;
    };
    copy(currentPose, current);
    copy(previousPose, previous);
    return true;
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
    // Live-validated: near = near-edge y @0x54, far = far-edge y @0x6C (see tail@68 dump).
    // Aspect: the game's own pixel-aspect field (+0x40); the w/h ints at +0x34/38 are
    // repurposed in KCD2 and must not be used.
    const auto pixAspect = current.projectionRaw[4];
    if (std::isfinite(pixAspect) && pixAspect > 0.5f && pixAspect < 4.0f)
        constants.cameraAspect = pixAspect;
    else
        constants.cameraAspect = fallbackAspect;
    const float nearCandidate = current.projectionRaw[9]; // near-edge y @ +0x54
    if (std::isfinite(nearCandidate) && nearCandidate > 0.0f && nearCandidate < 100.0f)
        constants.cameraNear = nearCandidate;
    const float farCandidate = current.projectionRaw[15]; // far-edge y @ +0x6C (=8000 in 1.5.6)
    if (std::isfinite(farCandidate) && farCandidate > 100.0f && farCandidate < 200000.0f &&
        farCandidate > constants.cameraNear)
        constants.cameraFar = farCandidate;
    // Optional EMA smoothing on angular velocity to counter pose jitter. 0=off.
    // Smoothed delta replaces the raw (current-previous) used for extrapolation, so
    // high-frequency shake is attenuated at the cost of a few ms of lag.
    // Bypass while the overlay is visible: the menu cursor must stay raw, and the
    // background world behind the menu should not accumulate smoothed momentum.
    if (MenuCommon::IsVisible())
    {
        g_smoothingInit = false;
    }
    else
    {
        // Phase calibration must see the same raw camera delta that will be
        // used after the model locks. Calibrating against the EMA and then
        // removing it changes both phase and gain at engagement time.
        const bool phaseAlignedMousePrediction = Config::Instance()->ReprojInputPredictor.value_or_default() &&
                                                 Kcd2Input::IsAvailable();
        const float rawSmoothing =
            phaseAlignedMousePrediction ? 0.0f : Config::Instance()->ReprojSmoothing.value_or_default();
        const float smoothing = std::clamp(rawSmoothing, 0.0f, 0.95f);
        if (smoothing > 0.001f)
        {
            if (!g_smoothingInit || std::abs(smoothing - g_lastSmoothing) > 0.01f)
            {
                for (int i = 0; i < 3; ++i)
                {
                    g_smoothedDeltaRight[i] = current.right[i] - previous.right[i];
                    g_smoothedDeltaUp[i] = current.up[i] - previous.up[i];
                    g_smoothedDeltaForward[i] = current.forward[i] - previous.forward[i];
                    g_smoothedDeltaPos[i] = current.position[i] - previous.position[i];
                }
                g_smoothingInit = true;
                g_lastSmoothing = smoothing;
            }
            else
            {
                const float a = smoothing;
                const float b = 1.0f - smoothing;
                float directionDot = 0.0f;
                float rawMagnitude2 = 0.0f;
                float smoothedMagnitude2 = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float dR = current.right[i] - previous.right[i];
                    const float dU = current.up[i] - previous.up[i];
                    const float dF = current.forward[i] - previous.forward[i];
                    directionDot +=
                        dR * g_smoothedDeltaRight[i] + dU * g_smoothedDeltaUp[i] + dF * g_smoothedDeltaForward[i];
                    rawMagnitude2 += dR * dR + dU * dU + dF * dF;
                    smoothedMagnitude2 += g_smoothedDeltaRight[i] * g_smoothedDeltaRight[i] +
                                          g_smoothedDeltaUp[i] * g_smoothedDeltaUp[i] +
                                          g_smoothedDeltaForward[i] * g_smoothedDeltaForward[i];
                }
                // An EMA must not carry the old angular velocity through a
                // direction reversal. That briefly predicts the opposite of
                // the player's input and reads as a camera wobble.
                const bool directionReversed =
                    rawMagnitude2 > 1.0e-10f && smoothedMagnitude2 > 1.0e-10f && directionDot <= 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float dR = current.right[i] - previous.right[i];
                    const float dU = current.up[i] - previous.up[i];
                    const float dF = current.forward[i] - previous.forward[i];
                    const float dP = current.position[i] - previous.position[i];
                    g_smoothedDeltaRight[i] = directionReversed ? dR : g_smoothedDeltaRight[i] * a + dR * b;
                    g_smoothedDeltaUp[i] = directionReversed ? dU : g_smoothedDeltaUp[i] * a + dU * b;
                    g_smoothedDeltaForward[i] = directionReversed ? dF : g_smoothedDeltaForward[i] * a + dF * b;
                    g_smoothedDeltaPos[i] = g_smoothedDeltaPos[i] * a + dP * b;
                }
            }
            for (int i = 0; i < 3; ++i)
            {
                constexpr float SMOOTHED_EPS = 1e-6f;
                if (std::abs(g_smoothedDeltaRight[i]) < SMOOTHED_EPS)
                    g_smoothedDeltaRight[i] = 0.0f;
                if (std::abs(g_smoothedDeltaUp[i]) < SMOOTHED_EPS)
                    g_smoothedDeltaUp[i] = 0.0f;
                if (std::abs(g_smoothedDeltaForward[i]) < SMOOTHED_EPS)
                    g_smoothedDeltaForward[i] = 0.0f;
                if (std::abs(g_smoothedDeltaPos[i]) < SMOOTHED_EPS)
                    g_smoothedDeltaPos[i] = 0.0f;
                constants.prevCameraRight[i] = current.right[i] - g_smoothedDeltaRight[i];
                constants.prevCameraUp[i] = current.up[i] - g_smoothedDeltaUp[i];
                constants.prevCameraForward[i] = current.forward[i] - g_smoothedDeltaForward[i];
                constants.prevCameraPosition[i] = current.position[i] - g_smoothedDeltaPos[i];
            }
        }
        else
        {
            // Smoothing disabled: ensure next enable re-seeds cleanly.
            g_smoothingInit = false;
        }
    }
    // Near/far are extracted and live-validated above (near-edge y @0x54, far-edge y @0x6C), and the
    // RPD depth path falls back to the rotation homography instead of the MV warp when a HUDless
    // source is present, so the configured warp mode is honored. Clamp only unsupported values
    // (mode 3+ extrapolation was removed with input latching).
    if (constants.mode > 2)
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
                  "fov=%.4f pixA=%.4f | nearEdge@50=(%.4f,%.4f,%.4f) | "
                  "projEdge@5c=(%.3f,%.3f,%.3f) | tail@68=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f)",
                  r[0], r[4], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17], r[18], r[19]);
    return true;
}
} // namespace Kcd2Camera
