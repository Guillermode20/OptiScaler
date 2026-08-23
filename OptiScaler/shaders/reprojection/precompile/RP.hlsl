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

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);
    if (DebugView == 2 && any(abs(uv - 0.5f) > 0.25f))
    {
        Output[dtid.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

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

    // Clamping an off-screen source stretches the last edge texel across the
    // viewport. Keep the real-frame pixels instead and feather the transition.
    float2 unboundedSrcUV = uv - deltaUV * TimeStep;
    bool covered = all(unboundedSrcUV >= 0.0f) && all(unboundedSrcUV <= 1.0f);
    float2 edgePixels = min(unboundedSrcUV, 1.0f - unboundedSrcUV) * float2(DisplaySize);
    float coverage = covered ? saturate(min(edgePixels.x, edgePixels.y) * 0.5f) : 0.0f;
    float2 srcUV = clamp(unboundedSrcUV, 0.0f, 1.0f);

    float4 warped = LastColor.SampleLevel(Bilinear, srcUV, 0);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);

    float4 result = lerp(original, warped, Strength * coverage);

    if (DebugView == 1)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
