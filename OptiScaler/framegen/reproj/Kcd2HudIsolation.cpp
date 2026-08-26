#include "pch.h"
#include "Kcd2HudIsolation.h"

#include "Config.h"
#include "Logger.h"
#include "State.h"

#include <array>
#include <include/d3dx/d3dx12.h>
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

bool IsKcd2Reprojection()
{
    auto config = Config::Instance();
    return config != nullptr && config->FGDrawUIOverFG.value_or_default() &&
           config->ReprojKcd2HudIsolation.value_or_default() &&
           State::Instance().activeFgOutput == FGOutput::Reproj && GetModuleHandleW(L"WHGame.dll") != nullptr;
}

bool IsVirtualGameBuffer(ID3D12Resource* resource)
{
    for (auto* buffer : State::Instance().scBuffers)
        if (buffer == resource)
            return true;
    return false;
}

void Release(UiFrame& frame)
{
    SAFE_RELEASE(frame.texture);
    SAFE_RELEASE(frame.rtvHeap);
    frame = {};
}

bool EnsureTarget(UiFrame& frame, ID3D12Resource* source)
{
    const auto desc = source->GetDesc();
    if (frame.texture != nullptr)
    {
        const auto old = frame.texture->GetDesc();
        if (old.Width == desc.Width && old.Height == desc.Height && old.Format == desc.Format &&
            old.SampleDesc.Count == desc.SampleDesc.Count)
            return true;
        Release(frame);
    }

    auto* device = State::Instance().currentD3D12Device;
    if (device == nullptr)
        return false;

    D3D12_RESOURCE_DESC targetDesc = desc;
    targetDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_CLEAR_VALUE clear {};
    clear.Format = targetDesc.Format;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &targetDesc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                               IID_PPV_ARGS(&frame.texture))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&frame.rtvHeap))))
    {
        Release(frame);
        return false;
    }
    frame.rtv = frame.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(frame.texture, nullptr, frame.rtv);
    frame.texture->SetName(L"KCD2_Reproj_UI");
    return true;
}
} // namespace

void ArmForFrame(int frameIndex)
{
    if (!IsKcd2Reprojection() || frameIndex < 0 || frameIndex >= BUFFER_COUNT)
        return;
    std::scoped_lock lock(g_mutex);
    auto& frame = g_frames[frameIndex];
    frame.armed = true;
    frame.redirected = false;
}

bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, int frameIndex,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv)
{
    if (!IsKcd2Reprojection() || commandList == nullptr || replacementRtv == nullptr || frameIndex < 0 ||
        frameIndex >= BUFFER_COUNT || !IsVirtualGameBuffer(source))
        return false;
    std::scoped_lock lock(g_mutex);
    auto& frame = g_frames[frameIndex];
    if (!frame.armed || !EnsureTarget(frame, source))
        return false;
    if (!frame.redirected)
    {
        const float transparent[4] = {};
        commandList->ClearRenderTargetView(frame.rtv, transparent, 0, nullptr);
        frame.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        frame.redirected = true;
        LOG_INFO("KCD2 HUD isolation: redirecting late UI for frame {}", frameIndex);
    }
    *replacementRtv = frame.rtv;
    return true;
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
