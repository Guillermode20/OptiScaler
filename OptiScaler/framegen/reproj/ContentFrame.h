#pragma once

#include <d3d12.h>

#include <cstdint>

#include <shaders/reprojection/RP_Common.h>

enum class ContentFrameKind : std::uint8_t
{
    Real = 0,
    Generated = 1,
};

// Owned presenter input. Resources are private copies and remain immutable
// until completionFence reaches completionFenceValue. UI is a reference to the
// latest separately captured overlay and is never fed into content generation.
struct ContentFrame
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* velocity = nullptr;
    ID3D12Resource* ui = nullptr;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES velocityState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;
    RP_Constants constants {};
    double renderTimestamp = 0.0;
    double sourcePoseTimestamp = 0.0;
    double sourcePoseInterval = 0.0;
    double virtualContentTimestamp = 0.0;
    double fgDurationMs = 0.0;
    std::uint64_t sourceCutGeneration = 0;
    std::uint64_t resetGeneration = 0;
    float interpolationFraction = 1.0f;
    ContentFrameKind kind = ContentFrameKind::Real;
    ID3D12Fence* completionFence = nullptr; // non-owning
    std::uint64_t completionFenceValue = 0;
};
