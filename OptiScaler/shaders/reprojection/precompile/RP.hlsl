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

    // The MV texture may be render-res while the output is display-res
    float2 mvUV = uv;
    if (MVSize.x != DisplaySize.x || MVSize.y != DisplaySize.y)
        mvUV = uv * float2(DisplaySize.xy) / float2(MVSize.xy);
    float2 mv = Velocity.SampleLevel(Bilinear, mvUV, 0).xy;

    // Apply scale (in pixels), jitter cancellation and the per-game sign convention
    float2 delta = mv * float2(MVScaleX, MVScaleY);
    if (JitterCancelled)
        delta -= float2(JitterX, JitterY);
    if (InvertMV)
        delta = -delta;

    // Move BACKWARD along the flow to fetch the source texel
    float2 srcUV = clamp(uv - delta * TimeStep, 0.0f, 1.0f);

    float4 warped = LastColor.SampleLevel(Bilinear, srcUV, 0);
    float4 original = LastColor.SampleLevel(Bilinear, uv, 0);

    float4 result = lerp(original, warped, Strength);

    if (DebugView)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(result.rgb, 1.0f);
}
