#pragma once

#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace Kcd2Scaleform
{
// Installs a read-only trace around KCD2's CScaleformPlayback::BeginDisplay/EndDisplay.
// The hook is matched through MSVC RTTI and validates the retail function prologues before
// Detours sees them. It intentionally never changes Scaleform render targets.
bool Initialize();

// True only on the thread currently executing a Scaleform display bracket.
bool IsActiveOnThisThread();

// Called from the D3D12 OM hook while the bracket is active. This records the target that a
// later isolation implementation must snapshot/redirect, without changing the command list.
void TraceOmSetRenderTargets(ID3D12GraphicsCommandList* commandList, uint32_t targetCount,
                             ID3D12Resource* const* targets);
} // namespace Kcd2Scaleform
