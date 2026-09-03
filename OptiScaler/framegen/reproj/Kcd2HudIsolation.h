#pragma once

#include <d3d12.h>

namespace Kcd2HudIsolation
{
void ArmForFrame(int frameIndex);
bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv);
bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, int frameIndex,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv);
ID3D12Resource* GetHudlessColor(ID3D12Resource* backBuffer, D3D12_RESOURCE_STATES* state);
ID3D12Resource* GetHudlessColor(int frameIndex, D3D12_RESOURCE_STATES* state);
ID3D12Resource* GetUIColor(ID3D12Resource* backBuffer, D3D12_RESOURCE_STATES* state);
ID3D12Resource* GetUIColor(int frameIndex, D3D12_RESOURCE_STATES* state);
void MarkFrameCaptured(ID3D12Resource* backBuffer, ID3D12Resource* hudless, ID3D12Resource* ui,
                       ID3D12Fence* fence, UINT64 fenceValue);
void OnEndDisplay();

// Mid-frame world-completion signal (latency pass): the Scaleform CL that
// contains the world snapshot is submitted before present on per-pass
// renderers, so signaling a fence the moment that CL is submitted lets the
// capture worker copy the world while the game still finishes its frame.
// Falls back gracefully (value 0) when no snapshot was recorded this frame.
void SetWorldSignalContext(ID3D12Fence* worldFence);
UINT64 MarkWorldSnapshotCl(ID3D12GraphicsCommandList* commandList);
bool OnWorldSnapshotSubmitted(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count);
UINT64 TakeWorldSignalValue(ID3D12Resource* backBuffer);
void Reset();
} // namespace Kcd2HudIsolation
