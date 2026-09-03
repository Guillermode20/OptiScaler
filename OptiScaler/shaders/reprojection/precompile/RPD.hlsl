cbuffer RP_Constants : register(b0)
{
    uint2 DisplaySize;
    uint2 MVSize;
    float TimeStep;
    float Strength;
    float MVScaleX, MVScaleY;
    float JitterX, JitterY;
    uint InvertMV;
    uint JitterCancelled;
    uint InvertedDepth;
    uint Mode;
    uint DebugView;
    uint HudlessSource;
    float4 CameraPos;
    float4 CameraUp;
    float4 CameraRight;
    float4 CameraForward;
    float4 PrevCameraPos;
    float4 PrevCameraUp;
    float4 PrevCameraRight;
    float4 PrevCameraForward;
    float CameraNear;
    float CameraFar;
    float CameraVFov;
    float CameraAspect;
    float4 TargetPosition;
    float4 TargetRight;
    float4 TargetUp;
    float4 TargetForward;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> UI : register(t1);
RWTexture2D<float4> Output : register(u0);
SamplerState Bilinear : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= DisplaySize))
        return;

    // PrevCamera* contains a CPU-baked homogeneous output-pixel -> source-UV
    // transform. This replaces per-pixel UV/NDC and camera-ray reconstruction.
    float3 position = float3(float2(dtid.xy) + 0.5f, 1.0f);
    float3 sourceH = float3(dot(PrevCameraRight.xyz, position), dot(PrevCameraUp.xyz, position),
                            dot(PrevCameraForward.xyz, position));
    bool inFront = sourceH.z > 1.0e-6f;
    float2 sourceUv = inFront ? sourceH.xy * rcp(sourceH.z) : float2(-1.0f, -1.0f);
    bool covered = inFront && all(sourceUv >= 0.0f) && all(sourceUv <= 1.0f);
    float2 edgePixels = min(sourceUv, 1.0f - sourceUv) * float2(DisplaySize);
    float coverage = covered ? saturate(min(edgePixels.x, edgePixels.y) * 0.5f) : 0.0f;

    // The original-frame fetch is needed only for the narrow disoccluded edge.
    // Interior pixels now issue one filtered world sample instead of both a load
    // and a sample; fully uncovered pixels avoid the filtered sample entirely.
    float3 world;
    [branch]
    if (coverage > 0.0f)
    {
        float3 warped = LastColor.SampleLevel(Bilinear, sourceUv, 0).rgb;
        if (coverage < 1.0f)
        {
            float3 original = LastColor.Load(int3(dtid.xy, 0)).rgb;
            warped = lerp(original, warped, coverage);
        }
        world = warped;
    }
    else
    {
        world = LastColor.Load(int3(dtid.xy, 0)).rgb;
    }
    if (HudlessSource != 0)
    {
        float4 ui = UI.Load(int3(dtid.xy, 0));
        float alpha = saturate(ui.a);
        float3 uiRgb = HudlessSource == 1 ? ui.rgb : ui.rgb * alpha;
        world = uiRgb + world * (1.0f - alpha);
    }
    Output[dtid.xy] = float4(world, 1.0f);
}
