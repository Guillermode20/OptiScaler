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

    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 targetRay = float3(ndc.x * CameraAspect * CameraVFov, ndc.y * CameraVFov, 1.0f);
    float3 sourceRay = float3(dot(PrevCameraRight.xyz, targetRay), dot(PrevCameraUp.xyz, targetRay),
                              dot(PrevCameraForward.xyz, targetRay));

    float2 sourceNdc = float2(sourceRay.x / (sourceRay.z * CameraAspect * CameraVFov),
                              sourceRay.y / (sourceRay.z * CameraVFov));
    float2 sourceUv = float2(sourceNdc.x * 0.5f + 0.5f, 0.5f - sourceNdc.y * 0.5f);
    bool covered = sourceRay.z > 0.0f && all(sourceUv >= 0.0f) && all(sourceUv <= 1.0f);
    float2 edgePixels = min(sourceUv, 1.0f - sourceUv) * float2(DisplaySize);
    float coverage = covered ? saturate(min(edgePixels.x, edgePixels.y) * 0.5f) : 0.0f;

    // Preserve exact source/UI texels whenever no sub-pixel reconstruction is
    // needed. Only the displaced world lookup uses bilinear filtering.
    float4 original = LastColor.Load(int3(dtid.xy, 0));
    float4 warped = LastColor.SampleLevel(Bilinear, clamp(sourceUv, 0.0f, 1.0f), 0);
    float3 world = lerp(original.rgb, warped.rgb, coverage);
    if (HudlessSource != 0)
    {
        float4 ui = UI.Load(int3(dtid.xy, 0));
        float alpha = saturate(ui.a);
        float3 uiRgb = HudlessSource == 1 ? ui.rgb : ui.rgb * alpha;
        world = uiRgb + world * (1.0f - alpha);
    }
    Output[dtid.xy] = float4(world, 1.0f);
}
