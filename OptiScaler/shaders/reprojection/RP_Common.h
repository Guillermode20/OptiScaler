#pragma once

#include "SysUtils.h"

// Constant buffer shared by the reprojection compute shaders (mirrored in
// RP.hlsl / RPD.hlsl). Aligned to 256 bytes for D3D12 CBV requirements.
struct alignas(256) RP_Constants
{
    uint32_t displayWidth; // backbuffer size
    uint32_t displayHeight;
    uint32_t mvWidth; // motion vector texture size (render or display res)
    uint32_t mvHeight;
    float timeStep; // warp fraction (0.5 = midpoint between real frames)
    float strength; // blend of the warp result with the original frame
    float mvScaleX;
    float mvScaleY;
    float jitterX;
    float jitterY;
    uint32_t invertMV;        // flip MV sign convention (per-game)
    uint32_t jitterCancelled; // subtract jitter from the sample position
    uint32_t invertedDepth;
    uint32_t mode; // 0 = MV warp, 1 = depth-aware, 2 = rotation-only camera warp
    uint32_t debugView;
    uint32_t pad0;
    // --- Mode 1 (depth-aware) camera block, current frame ---
    float cameraPosition[4];
    float cameraUp[4];
    float cameraRight[4];
    float cameraForward[4];
    // --- Mode 1 (depth-aware) camera block, previous frame ---
    float prevCameraPosition[4];
    float prevCameraUp[4];
    float prevCameraRight[4];
    float prevCameraForward[4];
    float cameraNear;
    float cameraFar;
    float cameraVFov; // radians, vertical
    float cameraAspect;
};

// v1: motion-vector warp. Sample _lastColor at (p - MV * TimeStep).
inline static std::string RPMV_ShaderCode = R"(
cbuffer RP_Constants : register(b0)
{
    uint2  DisplaySize;
    uint2  MVSize;
    float  TimeStep;
    float  Strength;
    float  MVScaleX, MVScaleY;
    float  JitterX, JitterY;
    uint   InvertMV;
    uint   JitterCancelled;
    uint   InvertedDepth;
    uint   Mode;
    uint   DebugView;
    float4 CameraPos;
    float4 CameraUp;
    float4 CameraRight;
    float4 CameraForward;
    float4 PrevCameraPos;
    float4 PrevCameraUp;
    float4 PrevCameraRight;
    float4 PrevCameraForward;
    float  CameraNear;
    float  CameraFar;
    float  CameraVFov;
    float  CameraAspect;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> Velocity  : register(t1);
Texture2D<float4> Depth     : register(t2);
RWTexture2D<float4> Output  : register(u0);

SamplerState Bilinear : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);

    // The MV texture covers the full frame (possibly at render resolution), so its UV maps 1:1
    float2 mvUV = uv;
    float2 mv = Velocity.SampleLevel(Bilinear, mvUV, 0).xy;

    // Apply scale (in pixels), jitter cancellation and the per-game sign convention
    float2 delta = mv * float2(MVScaleX, MVScaleY);
    if (JitterCancelled)
        delta -= float2(JitterX, JitterY);
    if (InvertMV)
        delta = -delta;

    // Texel displacement -> normalized UV offset, then move BACKWARD along the flow
    float2 deltaUV = delta / float2(MVSize);
    float2 srcUV = clamp(uv - deltaUV * TimeStep, 0.0f, 1.0f);

    float4 warped = LastColor.SampleLevel(Bilinear, srcUV, 0);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);

    float4 result = lerp(original, warped, Strength);

    if (DebugView)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
)";

// v2: depth-aware reprojection. Reconstruct the world position from depth +
// camera, extrapolate the camera to the fake-frame time, reproject and blend
// with the MV warp where the depth test fails (disocclusions) or MV ~ 0 (HUD).
inline static std::string RPD_ShaderCode = R"(
cbuffer RP_Constants : register(b0)
{
    uint2  DisplaySize;
    uint2  MVSize;
    float  TimeStep;
    float  Strength;
    float  MVScaleX, MVScaleY;
    float  JitterX, JitterY;
    uint   InvertMV;
    uint   JitterCancelled;
    uint   InvertedDepth;
    uint   Mode;
    uint   DebugView;
    float4 CameraPos;
    float4 CameraUp;
    float4 CameraRight;
    float4 CameraForward;
    float4 PrevCameraPos;
    float4 PrevCameraUp;
    float4 PrevCameraRight;
    float4 PrevCameraForward;
    float  CameraNear;
    float  CameraFar;
    float  CameraVFov;
    float  CameraAspect;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> Velocity  : register(t1);
Texture2D<float4> Depth     : register(t2);
RWTexture2D<float4> Output  : register(u0);

SamplerState Bilinear : register(s0);

// Standard perspective depth -> view-space Z (positive distance from camera plane).
// Invalid far planes/depth values are handled by the confidence test below.
float LinearizeDepth(float d, float nearZ, float farZ, uint inverted)
{
    float z = inverted ? 1.0f - d : d;
    return (2.0f * nearZ * farZ) / (farZ + nearZ - (2.0f * z - 1.0f) * (farZ - nearZ));
}

float3 ReconstructWorld(float2 ndc, float viewZ,
                        float3 camPos, float3 right, float3 up, float3 forward,
                        float tanHalf, float aspect)
{
    float3 viewDir = normalize(right * ndc.x * aspect * tanHalf + up * ndc.y * tanHalf + forward);
    return camPos + viewDir * viewZ;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);

    // The MV texture covers the full frame (possibly at render resolution), so its UV maps 1:1
    float2 mvUV = uv;
    float2 mv = Velocity.SampleLevel(Bilinear, mvUV, 0).xy;

    float2 delta = mv * float2(MVScaleX, MVScaleY);
    if (JitterCancelled)
        delta -= float2(JitterX, JitterY);
    if (InvertMV)
        delta = -delta;

    // MV-warp fallback sample (also the v1 result)
    float2 deltaUV = delta / float2(MVSize);
    float2 srcUV = clamp(uv - deltaUV * TimeStep, 0.0f, 1.0f);
    float4 mvWarp = LastColor.SampleLevel(Bilinear, srcUV, 0);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);

    // ---- depth-aware reprojection ----
    float tanHalf = tan(CameraVFov * 0.5f);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    float3 camPos    = CameraPos.xyz;
    float3 right     = normalize(CameraRight.xyz);
    float3 up        = normalize(CameraUp.xyz);
    float3 forward   = normalize(CameraForward.xyz);

    float depthZ = LinearizeDepth(Depth.SampleLevel(Bilinear, mvUV, 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
    float3 worldPos = ReconstructWorld(ndc, depthZ, camPos, right, up, forward, tanHalf, CameraAspect);

    // Extrapolate the camera pose to the fake-frame time (TimeStep after frame N)
    float t = TimeStep;
    float3 midPos     = lerp(PrevCameraPos.xyz, camPos, 1.0f + t);
    float3 midRight   = normalize(lerp(PrevCameraRight.xyz, right, 1.0f + t));
    float3 midUp      = normalize(lerp(PrevCameraUp.xyz, up, 1.0f + t));
    float3 midForward = normalize(lerp(PrevCameraForward.xyz, forward, 1.0f + t));

    // Mode 2 is rotation-only timewarp. Keep the source position so the warp
    // cannot reveal geometry through a translation that we cannot synthesize.
    if (Mode == 2)
        midPos = camPos;

    float3 pMid = float3(dot(midRight, worldPos - midPos),
                         dot(midUp, worldPos - midPos),
                         dot(midForward, worldPos - midPos));

    float2 ndcMid = float2(pMid.x / (pMid.z * CameraAspect * tanHalf),
                           pMid.y / (pMid.z * tanHalf));
    float2 reprojUV = float2(ndcMid.x * 0.5f + 0.5f, 0.5f - ndcMid.y * 0.5f);

    // Confidence: compare the reprojected surface against the depth buffer at the target.
    // Behind-camera or off-screen targets get the MV-warp fallback.
    float conf = 0.0f;
    if (pMid.z > 0.0f && all(reprojUV >= 0.0f) && all(reprojUV <= 1.0f))
    {
        float2 targetNdc = float2(reprojUV.x * 2.0f - 1.0f, 1.0f - reprojUV.y * 2.0f);
        float targetZ = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV, 0).r,
                                       CameraNear, CameraFar, InvertedDepth);
        float3 targetWorld = ReconstructWorld(targetNdc, targetZ, camPos, right, up, forward, tanHalf, CameraAspect);

        float relErr = length(worldPos - targetWorld) / max(depthZ, 0.01f);
        conf = saturate(1.0f - relErr);

        // Reject depth discontinuities around the projected target. This is a
        // conservative disocclusion test: a little loss of warp coverage is much
        // less visible than pulling foreground colour across a newly exposed edge.
        float2 depthTexel = 1.0f / float2(DisplaySize);
        float d0 = targetZ;
        float d1 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV + float2(depthTexel.x, 0), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d2 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV - float2(depthTexel.x, 0), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d3 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV + float2(0, depthTexel.y), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d4 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV - float2(0, depthTexel.y), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float depthSpread = max(max(abs(d1 - d0), abs(d2 - d0)), max(abs(d3 - d0), abs(d4 - d0))) / max(d0, 0.01f);
        conf *= saturate(1.0f - depthSpread * 8.0f);

        float2 targetMv = Velocity.SampleLevel(Bilinear, reprojUV, 0).xy * float2(MVScaleX, MVScaleY);
        if (InvertMV)
            targetMv = -targetMv;
        float mvDisagreement = length(targetMv - delta) / max(max(length(delta), length(targetMv)), 1.0f);
        conf *= saturate(1.0f - mvDisagreement * 0.5f);

        float edgeDistance = min(min(reprojUV.x, reprojUV.y), min(1.0f - reprojUV.x, 1.0f - reprojUV.y));
        conf *= saturate(edgeDistance * 32.0f);
    }

    // Preserve static UI in screen space. Camera-only mode still reads motion
    // vectors solely for this HUD exclusion; it never uses them to move world colour.
    conf *= saturate(length(delta) * 4.0f);

    float2 clampedUV = clamp(reprojUV, 0.0f, 1.0f);
    float4 warped = LastColor.SampleLevel(Bilinear, clampedUV, 0);
    float4 fallback = Mode == 2 ? original : mvWarp;
    float4 result = lerp(fallback, warped, conf);
    result = lerp(original, result, Strength);

    if (DebugView)
        Output[dtid.xy] = float4(Mode == 2 ? 0.0f : (length(delta) > 0.5f ? 1.0f : 0.0f), conf,
                                 Mode == 2 ? 1.0f : 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
)";
