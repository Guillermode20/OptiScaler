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
void Reset();
} // namespace Kcd2HudIsolation
