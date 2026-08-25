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
    uint   HudlessSource;
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

// D3D clip-space convention: d is in [0,1], 0 at the near plane and 1 at the far plane.
// Returns the positive AXIAL view-space distance along the forward axis.
float LinearizeDepth(float d, float nearZ, float farZ, uint inverted)
{
    float z = inverted ? 1.0f - d : d;
    return (nearZ * farZ) / (farZ - z * (farZ - nearZ));
}

float3 ReconstructWorld(float2 ndc, float viewZ,
                        float3 camPos, float3 right, float3 up, float3 forward,
                        float tanHalf, float aspect)
{
    // viewZ is axial along forward, so scale the unnormalized frustum ray by
    // t = viewZ / dot(ray, forward) instead of renormalizing the ray.
    float3 ray = right * ndc.x * aspect * tanHalf + up * ndc.y * tanHalf + forward;
    return camPos + ray * (viewZ / max(dot(ray, forward), 1.0e-4f));
}

float3 RotateAxis(float3 v, float3 axis, float angle)
{
    float s, c;
    sincos(angle, s, c);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0f - c);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);
    if (DebugView == 2 && any(abs(uv - 0.5f) > 0.25f))
    {
        Output[dtid.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }
    float2 mvUV = uv;
    float2 delta = 0.0f;

    // Rotation-only warps with a separately composited UI do not need motion
    // vectors. Keep the sample only when it is required for HUD rejection or
    // by the depth/MV paths.
    if (Mode != 2 || !HudlessSource)
    {
        delta = Velocity.SampleLevel(Bilinear, mvUV, 0).xy * float2(MVScaleX, MVScaleY);
        if (JitterCancelled)
            delta -= float2(JitterX, JitterY);
        if (InvertMV)
            delta = -delta;
    }

    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    if (Mode == 2)
    {
        // C++ builds source-to-predicted rotation rows and tan(vfov / 2)
        // once per dispatch. Per pixel, only apply that homography.
        float tanHalf = CameraVFov;
        float3 predictedRay = float3(ndc.x * CameraAspect * tanHalf, ndc.y * tanHalf, 1.0f);
        float3 pSource = float3(dot(PrevCameraRight.xyz, predictedRay), dot(PrevCameraUp.xyz, predictedRay),
                               dot(PrevCameraForward.xyz, predictedRay));
        float2 sourceNdc =
            float2(pSource.x / (pSource.z * CameraAspect * tanHalf), pSource.y / (pSource.z * tanHalf));
        float2 reprojUV = float2(sourceNdc.x * 0.5f + 0.5f, 0.5f - sourceNdc.y * 0.5f);

        bool covered = pSource.z > 0.0f && all(reprojUV >= 0.0f) && all(reprojUV <= 1.0f);
        float2 edgePixels = min(reprojUV, 1.0f - reprojUV) * float2(DisplaySize);
        float conf = covered ? saturate(min(edgePixels.x, edgePixels.y) * 0.5f) : 0.0f;
        if (!HudlessSource)
            conf *= saturate((length(delta) - 0.02f) * 8.0f);

        float4 warped = LastColor.SampleLevel(Bilinear, clamp(reprojUV, 0.0f, 1.0f), 0);
        float4 result = lerp(original, warped, Strength * conf);

        if (DebugView == 1)
            Output[dtid.xy] = float4(0.0f, conf, 1.0f, 1.0f);
        else
            Output[dtid.xy] = float4(result.rgb, 1.0f);
        return;
    }
    float tanHalf = tan(CameraVFov * 0.5f);

    float3 right = normalize(CameraRight.xyz);
    float3 up = normalize(CameraUp.xyz);
    float3 forward = normalize(CameraForward.xyz);
    const float s = 1.0f + TimeStep;
    float3 midRight = normalize(lerp(PrevCameraRight.xyz, right, s));
    float3 midUp = normalize(lerp(PrevCameraUp.xyz, up, s));
    float3 midForward = normalize(lerp(PrevCameraForward.xyz, forward, s));

    // MV-warp fallback sample (also the v1 result).
    float2 deltaUV = delta / float2(MVSize);
    float2 unboundedSrcUV = uv - deltaUV * TimeStep;
    bool mvCovered = all(unboundedSrcUV >= 0.0f) && all(unboundedSrcUV <= 1.0f);
    float2 mvEdgePixels = min(unboundedSrcUV, 1.0f - unboundedSrcUV) * float2(DisplaySize);
    float mvCoverage = mvCovered ? saturate(min(mvEdgePixels.x, mvEdgePixels.y) * 0.5f) : 0.0f;
    float4 mvWarp = LastColor.SampleLevel(Bilinear, clamp(unboundedSrcUV, 0.0f, 1.0f), 0);

    // Depth-aware reprojection.
    float3 camPos = CameraPos.xyz;
    float depthZ =
        LinearizeDepth(Depth.SampleLevel(Bilinear, mvUV, 0).r, CameraNear, CameraFar, InvertedDepth);
    float3 worldPos = ReconstructWorld(ndc, depthZ, camPos, right, up, forward, tanHalf, CameraAspect);
    float3 midPos = lerp(PrevCameraPos.xyz, camPos, s);
    float3 pMid = float3(dot(midRight, worldPos - midPos), dot(midUp, worldPos - midPos),
                         dot(midForward, worldPos - midPos));
    float2 ndcMid =
        float2(pMid.x / (pMid.z * CameraAspect * tanHalf), pMid.y / (pMid.z * tanHalf));
    float2 reprojUV = float2(ndcMid.x * 0.5f + 0.5f, 0.5f - ndcMid.y * 0.5f);

    // Confidence: compare the reprojected surface against the depth buffer at
    // the target and reject depth discontinuities.
    float conf = 0.0f;
    if (pMid.z > 0.0f && all(reprojUV >= 0.0f) && all(reprojUV <= 1.0f))
    {
        float2 targetNdc = float2(reprojUV.x * 2.0f - 1.0f, 1.0f - reprojUV.y * 2.0f);
        float targetZ =
            LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV, 0).r, CameraNear, CameraFar, InvertedDepth);
        float3 targetWorld =
            ReconstructWorld(targetNdc, targetZ, camPos, right, up, forward, tanHalf, CameraAspect);

        float relErr = length(worldPos - targetWorld) / max(depthZ, 0.01f);
        conf = saturate(1.0f - relErr);

        float2 depthTexel = 1.0f / float2(DisplaySize);
        float d1 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV + float2(depthTexel.x, 0), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d2 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV - float2(depthTexel.x, 0), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d3 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV + float2(0, depthTexel.y), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float d4 = LinearizeDepth(Depth.SampleLevel(Bilinear, reprojUV - float2(0, depthTexel.y), 0).r,
                                  CameraNear, CameraFar, InvertedDepth);
        float depthSpread =
            max(max(abs(d1 - targetZ), abs(d2 - targetZ)), max(abs(d3 - targetZ), abs(d4 - targetZ))) /
            max(targetZ, 0.01f);
        conf *= saturate(1.0f - depthSpread * 8.0f);

        float2 targetMv = Velocity.SampleLevel(Bilinear, reprojUV, 0).xy * float2(MVScaleX, MVScaleY);
        if (InvertMV)
            targetMv = -targetMv;
        float mvDisagreement = length(targetMv - delta) / max(max(length(delta), length(targetMv)), 1.0f);
        conf *= saturate(1.0f - mvDisagreement * 0.5f);

        float2 edgePixels = min(reprojUV, 1.0f - reprojUV) * float2(DisplaySize);
        conf *= saturate(min(edgePixels.x, edgePixels.y) * 0.5f);
    }

    if (!HudlessSource)
        conf *= saturate((length(delta) - 0.02f) * 8.0f);

    float4 warped = LastColor.SampleLevel(Bilinear, clamp(reprojUV, 0.0f, 1.0f), 0);
    float4 fallback = lerp(original, mvWarp, mvCoverage);
    float4 result = lerp(original, lerp(fallback, warped, conf), Strength);

    if (DebugView == 1)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, conf, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
