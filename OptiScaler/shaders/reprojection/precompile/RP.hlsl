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
    uint   Extrapolate;
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
    float  LateYaw;
    float  LatePitch;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> Velocity  : register(t1);
Texture2D<float4> Depth     : register(t2);
RWTexture2D<float4> Output  : register(u0);

SamplerState Bilinear : register(s0);

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

    // Projection-only timewarp maps each target display ray back into the source
    // anchor with the exact rotational homography. Unlike a constant UV offset,
    // this remains geometrically correct toward the edges of a wide FOV.
    float lateTanHalfV = tan(CameraVFov * 0.5f);
    float lateTanHalfH = lateTanHalfV * CameraAspect;
    float2 lateUV = (lateTanHalfH > 1e-5f && lateTanHalfV > 1e-5f)
                        ? float2(LateYaw / (2.0f * lateTanHalfH), LatePitch / (2.0f * lateTanHalfV))
                        : float2(0.0f, 0.0f);

    // Clamping an off-screen source stretches the last edge texel across the
    // viewport. Keep the real-frame pixels instead and feather the transition.
    float2 unboundedSrcUV = uv - deltaUV * TimeStep + lateUV;
    if (Mode == 3 && lateTanHalfH > 1e-5f && lateTanHalfV > 1e-5f)
    {
        float2 targetNdc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        float3 sourceRay = normalize(float3(targetNdc.x * lateTanHalfH,
                                             targetNdc.y * lateTanHalfV, 1.0f));
        sourceRay = RotateAxis(sourceRay, float3(0.0f, 1.0f, 0.0f), LateYaw);
        sourceRay = RotateAxis(sourceRay, float3(1.0f, 0.0f, 0.0f), LatePitch);
        float2 sourceNdc = sourceRay.xy / max(sourceRay.z, 1e-5f) /
                           float2(lateTanHalfH, lateTanHalfV);
        unboundedSrcUV = float2(sourceNdc.x * 0.5f + 0.5f, 0.5f - sourceNdc.y * 0.5f);
    }
    float edgeDistance = min(min(unboundedSrcUV.x, unboundedSrcUV.y),
                             min(1.0f - unboundedSrcUV.x, 1.0f - unboundedSrcUV.y));
    float coverage = all(unboundedSrcUV >= 0.0f) && all(unboundedSrcUV <= 1.0f) ? saturate(edgeDistance * 32.0f) : 0.0f;
    float2 srcUV = clamp(unboundedSrcUV, 0.0f, 1.0f);

    float4 warped = LastColor.SampleLevel(Bilinear, srcUV, 0);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);

    float4 result = lerp(original, warped, Strength * coverage);

    if (DebugView)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
