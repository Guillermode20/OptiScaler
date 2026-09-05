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
    uint Reserved;
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
    float CameraVFov;
    float CameraAspect;
    // Tail (append-only, lock-step with RP_Constants)
    float EdgeExtensionPx;
    float GuardCropPxX;
    float GuardCropPxY;
    float EdgeBlendPx;
    uint DepthEnabled;
    uint DepthInverted;
    float4 TxCameraSpace;
    float4 DepthPlanes;
    float FocalPxX;
    float FocalPxY;
    float MaxResidualPx;
    float VerticalScale;
};

Texture2D<float4> LastColor : register(t0);
Texture2D<float4> UI : register(t1);
Texture2D<float> Depth : register(t2);
RWTexture2D<float4> Output : register(u0);
SamplerState Bilinear : register(s0);

// Linearize a sampled depth value to anchor-camera-space view Z.
// DepthInverted (reversed-Z): stored 1 at near / 0 at far.
float LinearizeDepth(float d)
{
    float dlin = (DepthInverted != 0) ? 1.0f - d : d; // 0 = near, 1 = far
    float nearP = DepthPlanes.x;
    float farP = DepthPlanes.y;
    if (nearP <= 0.0f || farP <= nearP)
        return farP;
    float denom = farP - dlin * (farP - nearP);
    return denom > 1.0e-6f ? nearP * farP / denom : farP;
}

// Conservative depth-assisted translation residual.
// rotationUv is the canonical mapping (infinite-depth homography). Depth only
// supplies a small per-pixel parallax displacement for the scene point at that
// rotation UV: source pixel shift = f * tau / z minus the forward-motion zoom
// term, gated to 0 on silhouettes (depth discontinuities), invalid depths, and
// outside the captured image. Never replaces the rotation result.
float2 ComputeTranslationResidual(float2 rotationUv, out float weight)
{
    weight = 0.0f;
    uint2 depthDim;
    Depth.GetDimensions(depthDim.x, depthDim.y);
    if (any(depthDim == uint2(0, 0)))
        return float2(0.0f, 0.0f);
    uint2 depthMax = depthDim - uint2(1, 1);
    uint2 dpx = uint2(clamp(uint2(rotationUv * float2(depthDim)), uint2(0, 0), depthMax));

    float zc = LinearizeDepth(Depth.Load(int3(dpx, 0)).x);
    if (!(zc > DepthPlanes.x) || !(zc < DepthPlanes.y))
        return float2(0.0f, 0.0f); // sky/far-plane or invalid depth: rotation stays canonical

    // Depth-continuity confidence: large relative deviations between the center
    // tap and its 4 neighbors mean an object silhouette - never correct there.
    float zm = LinearizeDepth(Depth.Load(int3(uint2(max(dpx.x, 1u) - 1u, dpx.y), 0)).x);
    float zp = LinearizeDepth(Depth.Load(int3(uint2(min(dpx.x + 1u, depthMax.x), dpx.y), 0)).x);
    float ym = LinearizeDepth(Depth.Load(int3(uint2(dpx.x, max(dpx.y, 1u) - 1u), 0)).x);
    float yp = LinearizeDepth(Depth.Load(int3(uint2(dpx.x, min(dpx.y + 1u, depthMax.y)), 0)).x);
    float maxRel = 0.0f;
    maxRel = max(maxRel, abs(zm - zc));
    maxRel = max(maxRel, abs(zp - zc));
    maxRel = max(maxRel, abs(ym - zc));
    maxRel = max(maxRel, abs(yp - zc));
    maxRel = maxRel / max(zc, 1.0e-3f);
    float wDisc = 1.0f - smoothstep(0.05f, 0.25f, maxRel);

    // Camera-space translation of the target pose relative to the anchor.
    // Horizontal (x/y) parallax is applied fully; world-Z was damped CPU-side.
    float3 tau = TxCameraSpace.xyz;
    if (abs(tau.x) + abs(tau.y) + abs(tau.z) < 1.0e-6f)
        return float2(0.0f, 0.0f);

    // Source-pixel offset of this ray from the image center (rotation-only uv).
    float2 offC = rotationUv * float2(DisplaySize) - float2(DisplaySize) * 0.5f;
    float invZ = 1.0f / max(zc, 1.0e-2f);
    float2 flowPx = float2((FocalPxX * tau.x - tau.z * offC.x) * invZ,
                           (-FocalPxY * tau.y - tau.z * offC.y) * invZ);
    float mag = length(flowPx);
    if (mag < 1.0e-3f)
        return float2(0.0f, 0.0f);

    // Bounded residual: hard magnitude clamp plus a soft fade near the ceiling
    // so nothing ever exceeds MaxResidualPx or pops when the clamp engages.
    float2 clamped = flowPx * min(1.0f, MaxResidualPx / mag);
    float wMag = 1.0f - smoothstep(MaxResidualPx * 0.75f, MaxResidualPx, mag);

    // Never sample past the captured image with the correction: outside the
    // source the disocclusion handling (edge fill) owns the pixel.
    float2 corrected = rotationUv + clamped * rcp(float2(DisplaySize));
    bool inside = all(corrected >= 0.0f) && all(corrected <= 1.0f);
    if (!inside)
        return float2(0.0f, 0.0f);

    weight = wDisc * wMag;
    return clamped * rcp(float2(DisplaySize)) * weight;
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= DisplaySize))
        return;

    float3 position = float3(float2(dtid.xy) + 0.5f, 1.0f);
    float3 sourceH = float3(dot(PrevCameraRight.xyz, position), dot(PrevCameraUp.xyz, position),
                            dot(PrevCameraForward.xyz, position));
    bool inFront = sourceH.z > 1.0e-6f;
    float2 rawUv = inFront ? sourceH.xy * rcp(sourceH.z) : float2(-1000.0f, -1000.0f);

    // Optional depth translation residual. Rotation stays the canonical
    // mapping; depth is only allowed to nudge pixels where confident.
    float2 flowUv = 0.0f;
    if (DepthEnabled != 0 && inFront && all(rawUv >= 0.0f) && all(rawUv <= 1.0f))
    {
        float depthWeight = 0.0f;
        float2 residual = ComputeTranslationResidual(rawUv, depthWeight);
        flowUv = residual; // weight already folded in
    }

    // Fixed guard-crop window: sample through a per-axis inset so the outer
    // ring of the source stays as reserve content the rotation can reveal
    // before the captured edge is exhausted. 0 when the guard is off.
    float2 cropUv = float2(GuardCropPxX, GuardCropPxY) * rcp(float2(DisplaySize));
    float2 uv = (rawUv + flowUv) * (1.0f - 2.0f * cropUv) + cropUv;

    float3 world;
    if (!inFront)
    {
        // Behind the camera (extreme edge case): static anchor pixel.
        world = LastColor.Load(int3(dtid.xy, 0)).rgb;
    }
    else if (all(uv >= 0.0f) && all(uv <= 1.0f))
    {
        // Real captured content (the reserve band included when a guard crop
        // is active): plain warped sample. Legacy (edge extension off) keeps
        // the validated 2 px feather into the static anchor at the edges.
        float3 warped = LastColor.SampleLevel(Bilinear, uv, 0).rgb;
        if (EdgeExtensionPx <= 0.0f)
        {
            float2 edgePixels = min(uv, 1.0f - uv) * float2(DisplaySize);
            float coverage = saturate(min(edgePixels.x, edgePixels.y) * 0.5f);
            if (coverage < 1.0f)
            {
                float3 staticColor = LastColor.Load(int3(dtid.xy, 0)).rgb;
                warped = lerp(staticColor, warped, coverage);
            }
        }
        world = warped;
    }
    else if (EdgeExtensionPx > 0.0f)
    {
        // Uncovered edge: the correct pixels do not exist in the capture.
        // Clamp the UV onto the source edge and stretch boundary color outward
        // with a small directional blur of interior taps, so the wedge moves
        // with the warp instead of freezing to a stationary strip.
        float2 beyondPx = max(float2(0.0f, 0.0f), max(-uv, uv - 1.0f)) * float2(DisplaySize);
        float outsidePx = max(beyondPx.x, beyondPx.y);
        float3 staticColor = LastColor.Load(int3(dtid.xy, 0)).rgb;
        if (outsidePx < EdgeExtensionPx)
        {
            bool xDominant = beyondPx.x >= beyondPx.y;
            float2 boundary = saturate(uv);
            // Step inward (toward the image) along the uncovered axis.
            float2 inward = xDominant ? float2(uv.x > 1.0f ? -1.0f : 1.0f, 0.0f)
                                      : float2(0.0f, uv.y > 1.0f ? -1.0f : 1.0f);
            float2 stepPx = inward * (EdgeBlendPx > 1.0f ? EdgeBlendPx : 2.0f) *
                            rcp(float2(xDominant ? DisplaySize.x : 1, xDominant ? 1 : DisplaySize.y));
            // 3 taps inward from the boundary: boundary, 1 step, 2 steps.
            float3 ext0 = LastColor.SampleLevel(Bilinear, boundary, 0).rgb;
            float3 ext1 = LastColor.SampleLevel(Bilinear, saturate(boundary + stepPx), 0).rgb;
            float3 ext2 = LastColor.SampleLevel(Bilinear, saturate(boundary + 2.0f * stepPx), 0).rgb;
            float3 ext = ext0 * 0.5f + ext1 * 0.3f + ext2 * 0.2f;
            // Fade back to the static fallback near the extension limit so the
            // extreme (flick / huge disocclusion) case degrades gracefully.
            float fade = smoothstep(EdgeExtensionPx * 0.75f, EdgeExtensionPx, outsidePx);
            world = lerp(ext, staticColor, fade);
        }
        else
        {
            world = staticColor;
        }
    }
    else
    {
        // Legacy static fallback (outside the image, edge extension off).
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
