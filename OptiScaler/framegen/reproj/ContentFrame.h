#pragma once

#include <d3d12.h>

#include <cstdint>

#include <shaders/reprojection/RP_Common.h>

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
