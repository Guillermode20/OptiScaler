#include "pch.h"
#include "Kcd2HudIsolation.h"

#include <array>
#include <mutex>

namespace Kcd2HudIsolation
{
namespace
{
struct UiFrame
{
    ID3D12Resource* texture = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv {};
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool armed = false;
    bool redirected = false;
};

std::array<UiFrame, BUFFER_COUNT> g_frames;
std::mutex g_mutex;

void Release(UiFrame& frame)
{
    SAFE_RELEASE(frame.texture);
    SAFE_RELEASE(frame.rtvHeap);
    frame = {};
}

} // namespace

void ArmForFrame(int frameIndex)
{
    if (frameIndex < 0 || frameIndex >= BUFFER_COUNT)
        return;
    std::scoped_lock lock(g_mutex);
    auto& frame = g_frames[frameIndex];
    frame.armed = true;
    frame.redirected = false;
}

bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, int frameIndex,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv)
{
    // The generic hudless tracker never arms in KCD2. Do not redirect live output based on that
    // unproven signal; the Scaleform trace establishes the real UI bracket first.
    (void)commandList;
    (void)source;
    (void)frameIndex;
    (void)replacementRtv;
    return false;
}

ID3D12Resource* GetUIColor(int frameIndex, D3D12_RESOURCE_STATES* state)
{
    if (state != nullptr)
        *state = D3D12_RESOURCE_STATE_COMMON;
    if (frameIndex < 0 || frameIndex >= BUFFER_COUNT)
        return nullptr;
    std::scoped_lock lock(g_mutex);
    const auto& frame = g_frames[frameIndex];
    if (!frame.redirected || frame.texture == nullptr)
        return nullptr;
    if (state != nullptr)
        *state = frame.state;
    return frame.texture;
}

void Reset()
{
    std::scoped_lock lock(g_mutex);
    for (auto& frame : g_frames)
        Release(frame);
}
} // namespace Kcd2HudIsolation
