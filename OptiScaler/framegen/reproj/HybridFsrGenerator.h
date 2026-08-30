#pragma once

#include <d3d12.h>

#include <cstdint>

#include <ffx_framegeneration.h>

#include "ContentFrame.h"

class HybridFsrGenerator
{
  public:
    bool Generate(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ContentFrame& realFrame,
                  ContentFrame& generatedFrame, std::uint64_t frameId, bool reset);
    void Reset();
    void Shutdown();
    bool IsAvailable() const { return _context != nullptr; }
    ~HybridFsrGenerator();

  private:
    bool EnsureContext(ID3D12Device* device, const ContentFrame& realFrame);
    bool EnsureOutput(ID3D12Device* device, ID3D12Resource* source, ContentFrame& output);
    static FfxApiResourceState FfxState(D3D12_RESOURCE_STATES state);

    ffxContext _context = nullptr;
    ID3D12Device* _device = nullptr;
    std::uint32_t _displayWidth = 0;
    std::uint32_t _displayHeight = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t _cutGeneration = 0;
};
