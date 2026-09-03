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
    uint2 DepthSize;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> UI : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float4> PrevColor : register(t3);
RWTexture2D<float4> Output : register(u0);
SamplerState Bilinear : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= DisplaySize))
        return;

    float3 position = float3(float2(dtid.xy) + 0.5f, 1.0f);
    float3 sourceH = float3(dot(PrevCameraRight.xyz, position), dot(PrevCameraUp.xyz, position),
                            dot(PrevCameraForward.xyz, position));
    bool inFront = sourceH.z > 1.0e-6f;
    float2 sourceUv = inFront ? sourceH.xy * rcp(sourceH.z) : float2(-1.0f, -1.0f);
    bool covered = inFront && all(sourceUv >= 0.0f) && all(sourceUv <= 1.0f);
    float2 edgePixels = min(sourceUv, 1.0f - sourceUv) * float2(DisplaySize);
    float coverage = covered ? saturate(min(edgePixels.x, edgePixels.y) * 0.5f) : 0.0f;

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

    // Swap-smooth blend: the first warp of a new anchor lerps the held
    // previous anchor's warped color so the 60 Hz content swap does not snap.
    // Strength carries the blend factor (unused by the rotation-only path;
    // 0 disables). PrevColor mirrors LastColor's warp/edge handling.
    if (Strength > 0.0f)
    {
        float3 prevWarped = PrevColor.Load(int3(dtid.xy, 0)).rgb;
        if (coverage > 0.0f)
        {
            prevWarped = PrevColor.SampleLevel(Bilinear, sourceUv, 0).rgb;
            if (coverage < 1.0f)
                prevWarped = lerp(PrevColor.Load(int3(dtid.xy, 0)).rgb, prevWarped, coverage);
        }
        world = lerp(world, prevWarped, Strength);
    }

    bool saneDepthCfg = DepthSize.x > 0 && DepthSize.y > 0 && CameraNear > 0.0f && CameraFar > CameraNear &&
                        CameraVFov > 0.01f && CameraVFov < 3.0f && CameraAspect > 0.01f;
    if (Mode == 1 && saneDepthCfg && covered)
    {
        int2 dmax = int2(DepthSize) - int2(1, 1);
        int2 dpx = int2(min(max(sourceUv * float2(DepthSize), float2(0.0f, 0.0f)), float2(dmax)));
        float d = Depth.Load(int3(dpx, 0)).x;
        if (InvertedDepth != 0) d = 1.0f - d;
        // 0=near,1=far after inversion - use Far - d*(Far-Near) so 0->Near 1->Far
        float denom = CameraFar - d * (CameraFar - CameraNear);
        float viewZ = denom > 1.0e-6f ? CameraNear * CameraFar / denom : CameraFar;
        // Skip skybox/far-plane (depth~1) to keep rotation homography
        if (d > 0.999f) viewZ = CameraFar;
        float tanHalf = tan(CameraVFov * 0.5f);
        if (tanHalf > 1.0e-6f)
        {
            float2 ndcA = float2(sourceUv.x * 2.0f - 1.0f, 1.0f - sourceUv.y * 2.0f);
            float3 dirA = (ndcA.x * tanHalf * CameraAspect) * CameraRight.xyz + (ndcA.y * tanHalf) * CameraUp.xyz + CameraForward.xyz;
            float3 W = CameraPos.xyz + dirA * viewZ;
            float3 vT = W - TargetPosition.xyz;
            float tx = dot(vT, TargetRight.xyz);
            float ty = dot(vT, TargetUp.xyz);
            float tz = dot(vT, TargetForward.xyz);
            float2 ndcT = float2(tx, ty) / max(tz, 1.0e-6f) / float2(tanHalf * CameraAspect, tanHalf);
            float2 uvT = float2(ndcT.x * 0.5f + 0.5f, 0.5f - ndcT.y * 0.5f);
            float2 outUv = (float2(dtid.xy) + 0.5f) / float2(DisplaySize);
            float2 corrUv = sourceUv + (outUv - uvT);
            float dx = abs(Depth.Load(int3(min(dpx + int2(1, 0), dmax), 0)).x - Depth.Load(int3(max(dpx - int2(1, 0), int2(0, 0)), 0)).x);
            float dy = abs(Depth.Load(int3(min(dpx + int2(0, 1), dmax), 0)).x - Depth.Load(int3(max(dpx - int2(0, 1), int2(0, 0)), 0)).x);
            float disc = max(dx, dy) / max(d, 1.0e-3f);
            float resid = length((outUv - uvT) * float2(DisplaySize));
            float wResid = 1.0f - smoothstep(12.0f, 24.0f, resid);
            float wDisc = 1.0f - smoothstep(0.25f, 0.5f, disc);
            bool covered1 = all(corrUv >= 0.0f) && all(corrUv <= 1.0f);
            float wDepth = (tz > 1.0e-6f && covered1) ? wResid * wDisc : 0.0f;
            float2 e1 = min(corrUv, 1.0f - corrUv) * float2(DisplaySize);
            float cov1 = saturate(min(e1.x, e1.y) * 0.5f);
            float3 dc = LastColor.SampleLevel(Bilinear, corrUv, 0).rgb;
            if (cov1 < 1.0f) dc = lerp(LastColor.Load(int3(dtid.xy, 0)).rgb, dc, cov1);
            if (DebugView == 2) world = float3(1.0f - wDepth, wDepth, 0.2f);
            else world = lerp(world, dc, wDepth);
        }
    }
    if (HudlessSource != 0 && DebugView != 2)
    {
        float4 ui = UI.Load(int3(dtid.xy, 0));
        float alpha = saturate(ui.a);
        float3 uiRgb = HudlessSource == 1 ? ui.rgb : ui.rgb * alpha;
        world = uiRgb + world * (1.0f - alpha);
    }
    Output[dtid.xy] = float4(world, 1.0f);
}
