#pragma once

#include <d3d12.h>

namespace Kcd2HudIsolation
{
void ArmForFrame(int frameIndex);
bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, int frameIndex,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv);
ID3D12Resource* GetUIColor(int frameIndex, D3D12_RESOURCE_STATES* state);
void Reset();
} // namespace Kcd2HudIsolation
