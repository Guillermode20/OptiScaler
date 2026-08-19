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

// Standard perspective depth -> view-space Z (positive distance from camera plane)
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
    }

    // Static regions (HUD / static geometry) must not move: blend out where MV ~ 0
    conf *= saturate(length(delta) * 4.0f);

    float2 clampedUV = clamp(reprojUV, 0.0f, 1.0f);
    float4 warped = LastColor.SampleLevel(Bilinear, clampedUV, 0);
    float4 result = lerp(mvWarp, warped, conf);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);
    result = lerp(original, result, Strength);

    if (DebugView)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, conf, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
