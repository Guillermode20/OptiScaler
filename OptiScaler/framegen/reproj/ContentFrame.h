#pragma once

#include <d3d12.h>

#include <cstdint>

#include <shaders/reprojection/RP_Common.h>

// Optional, CPU-only diagnostics for one displayed source frame. Angles are
// deltas relative to the frame's render camera (radians), rather than ambiguous
// engine-global Euler angles. The packet owns the immutable render metadata;
// the presenter fills a temporary copy at late latch and only aggregates it.
struct WarpFrameTelemetry
{
    std::uint64_t frameId = 0;
    double renderCameraTimestamp = 0.0;
    double sourceObservedReadyTimestamp = 0.0;
    double warpTimestamp = 0.0;
    float predictedYawDelta = 0.0f;
    float predictedPitchDelta = 0.0f;
    float actualYawDelta = 0.0f;
    float actualPitchDelta = 0.0f;
    float residualYaw = 0.0f;
    float residualPitch = 0.0f;
    float requiredPixelsLeft = 0.0f;
    float requiredPixelsRight = 0.0f;
    float requiredPixelsTop = 0.0f;
    float requiredPixelsBottom = 0.0f;
    float maxOobUvLeft = 0.0f;
    float maxOobUvRight = 0.0f;
    float maxOobUvTop = 0.0f;
    float maxOobUvBottom = 0.0f;
    float predictionHorizonMs = 0.0f;
    float predictorConfidence = 0.0f;
    std::uint32_t guardPixelsLeft = 0;
    std::uint32_t guardPixelsRight = 0;
    std::uint32_t guardPixelsTop = 0;
    std::uint32_t guardPixelsBottom = 0;
    bool coverageClamped = false;
};

// Owned presenter input. Resources are private copies and remain immutable
// until completionFence reaches completionFenceValue. UI is a reference to the
// latest separately captured overlay and is never fed into content generation.
struct ContentFrame
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* ui = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* velocity = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES velocityState = D3D12_RESOURCE_STATE_COMMON;
    RP_Constants constants {};
    double renderTimestamp = 0.0;
    double sourcePoseTimestamp = 0.0;
    double sourcePoseInterval = 0.0;
    double sourceFrameInterval = 0.0;
    double virtualContentTimestamp = 0.0;
    // Late-latch mouse baseline: raw-input totals at sourcePoseTimestamp, so a
    // warp of this content measures only motion after its own pose. Real
    // anchors snapshot the producer baseline; generated midpoints look it up
    // from the timestamped history at their (older) midpoint timestamp.
    std::int64_t sourceMouseX = 0;
    std::int64_t sourceMouseY = 0;
    double sourceMouseTimestamp = 0.0;
    std::uint64_t sourceCutGeneration = 0;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    bool invertedDepth = false;
    bool hdr = false;
    bool jitteredMotionVectors = false;
    bool displayResolutionMotionVectors = false;
    bool generated = false;
    ID3D12Fence* completionFence = nullptr; // non-owning
    std::uint64_t completionFenceValue = 0;
};
