#include "pch.h"
#include "Kcd2HudIsolation.h"
#include "Config.h"
#include "Logger.h"
#include "State.h"

#include <array>
#include <mutex>

namespace Kcd2HudIsolation
{
namespace
{
struct FrameSlot
{
    ID3D12Resource* backBuffer = nullptr;
    ID3D12Resource* hudlessTexture = nullptr;
    D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* uiTexture = nullptr;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE uiRtv {};
    ID3D12Fence* captureFence = nullptr;
    UINT64 captureFenceValue = 0;
    uint64_t frameSerial = 0;
    bool snapshotTakenThisScope = false;
    bool hasValidData = false;
};

std::array<FrameSlot, 8> g_slots {};
std::mutex g_mutex;

void ResourceBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after)
{
    if (before == after || res == nullptr || cmdList == nullptr)
        return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void ReleaseSlot(FrameSlot& slot)
{
    SAFE_RELEASE(slot.hudlessTexture);
    SAFE_RELEASE(slot.uiTexture);
    SAFE_RELEASE(slot.rtvHeap);
    SAFE_RELEASE(slot.captureFence);
    slot.backBuffer = nullptr;
    slot.hudlessState = D3D12_RESOURCE_STATE_COMMON;
    slot.uiState = D3D12_RESOURCE_STATE_COMMON;
    slot.uiRtv = {};
    slot.captureFenceValue = 0;
    slot.frameSerial = 0;
    slot.snapshotTakenThisScope = false;
    slot.hasValidData = false;
}

bool IsReusable(FrameSlot& slot)
{
    if (slot.captureFence == nullptr || slot.captureFenceValue == 0)
        return true;
    const auto completed = slot.captureFence->GetCompletedValue();
    if (completed != UINT64_MAX && completed < slot.captureFenceValue)
        return false;
    SAFE_RELEASE(slot.captureFence);
    slot.captureFenceValue = 0;
    return completed != UINT64_MAX;
}

FrameSlot* FindOrCreateSlot(ID3D12Device* device, ID3D12Resource* backBuffer)
{
    if (backBuffer == nullptr)
        return nullptr;

    FrameSlot* matchingScope = nullptr;
    FrameSlot* matchingReusable = nullptr;
    FrameSlot* reusable = nullptr;

    for (auto& slot : g_slots)
    {
        if (slot.backBuffer == backBuffer && slot.snapshotTakenThisScope)
        {
            matchingScope = &slot;
            break;
        }
        if (!IsReusable(slot))
            continue;
        if (slot.backBuffer == backBuffer && matchingReusable == nullptr)
            matchingReusable = &slot;
        if (reusable == nullptr || (slot.backBuffer == nullptr && reusable->backBuffer != nullptr) ||
            slot.frameSerial < reusable->frameSerial)
            reusable = &slot;
    }

    FrameSlot* slot = matchingScope ? matchingScope : (matchingReusable ? matchingReusable : reusable);
    // All isolation generations are still being copied. Fail closed for this
    // frame instead of overwriting a resource that the COPY queue is reading.
    if (slot == nullptr)
        return nullptr;
    if (slot->backBuffer != nullptr && slot->backBuffer != backBuffer)
        ReleaseSlot(*slot);

    const auto desc = backBuffer->GetDesc();

    if (slot->hudlessTexture != nullptr)
    {
        const auto curDesc = slot->hudlessTexture->GetDesc();
        if (curDesc.Width != desc.Width || curDesc.Height != desc.Height || curDesc.Format != desc.Format)
            ReleaseSlot(*slot);
    }

    slot->backBuffer = backBuffer;

    if (slot->hudlessTexture == nullptr && device != nullptr)
    {
        D3D12_HEAP_PROPERTIES heapProps {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC hudlessDesc = desc;
        hudlessDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr =
            device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &hudlessDesc, D3D12_RESOURCE_STATE_COMMON,
                                            nullptr, IID_PPV_ARGS(&slot->hudlessTexture));
        if (FAILED(hr))
        {
            LOG_ERROR("KCD2 HUD: failed to create hudless texture: {:X}", (UINT64) hr);
            return nullptr;
        }
        slot->hudlessTexture->SetName(L"Kcd2Hud_HudlessWorld");
        slot->hudlessState = D3D12_RESOURCE_STATE_COMMON;

        D3D12_RESOURCE_DESC uiDesc = desc;
        uiDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &uiDesc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                             IID_PPV_ARGS(&slot->uiTexture));
        if (FAILED(hr))
        {
            LOG_ERROR("KCD2 HUD: failed to create UI texture: {:X}", (UINT64) hr);
            SAFE_RELEASE(slot->hudlessTexture);
            return nullptr;
        }
        slot->uiTexture->SetName(L"Kcd2Hud_IsolatedUI");
        slot->uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&slot->rtvHeap));
        if (FAILED(hr))
        {
            LOG_ERROR("KCD2 HUD: failed to create RTV heap: {:X}", (UINT64) hr);
            SAFE_RELEASE(slot->hudlessTexture);
            SAFE_RELEASE(slot->uiTexture);
            return nullptr;
        }

        slot->uiRtv = slot->rtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc {};
        rtvDesc.Format = desc.Format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(slot->uiTexture, &rtvDesc, slot->uiRtv);
        LOG_INFO("KCD2 HUD: allocated isolation buffers for backbuffer {:X} ({}x{} format {})", (size_t) backBuffer,
                 desc.Width, desc.Height, (UINT) desc.Format);
    }

    return slot;
}
} // namespace

void ArmForFrame(int frameIndex)
{
    (void) frameIndex;
    std::scoped_lock lock(g_mutex);
    // Discovery is frame-local. Packet copies keep their own resource
    // references, so hiding older generations here cannot invalidate work in
    // flight and prevents a frame with no Scaleform redirect from reusing
    // stale world/UI content.
    for (auto& slot : g_slots)
        slot.hasValidData = false;
}

bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv)
{
    if (!Config::Instance()->ReprojHudIsolation.value_or_default() || commandList == nullptr || source == nullptr ||
        replacementRtv == nullptr)
        return false;

    ID3D12Device* device = nullptr;
    if (FAILED(source->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        device = State::Instance().currentD3D12Device;
        if (device == nullptr)
            return false;
    }
    else
    {
        device->Release();
    }

    std::scoped_lock lock(g_mutex);
    auto* slot = FindOrCreateSlot(device, source);
    if (slot == nullptr || slot->uiTexture == nullptr || slot->hudlessTexture == nullptr)
    {
        // Do not let Present consume an older isolation generation as if it
        // belonged to this frame. Existing packet copies own their resources
        // independently; clearing discoverability here is safe and makes the
        // current composed frame fail closed.
        for (auto& candidate : g_slots)
            if (candidate.backBuffer == source)
                candidate.hasValidData = false;
        return false;
    }

    if (!slot->snapshotTakenThisScope)
    {
        // 1. Snapshot world before Scaleform writes
        ResourceBarrier(commandList, source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ResourceBarrier(commandList, slot->hudlessTexture, slot->hudlessState, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->CopyResource(slot->hudlessTexture, source);
        ResourceBarrier(commandList, slot->hudlessTexture, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        slot->hudlessState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        ResourceBarrier(commandList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // 2. Prepare transparent UI texture
        ResourceBarrier(commandList, slot->uiTexture, slot->uiState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        slot->uiState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        commandList->ClearRenderTargetView(slot->uiRtv, clearColor, 0, nullptr);

        slot->snapshotTakenThisScope = true;
        slot->hasValidData = true;
        static uint64_t nextFrameSerial = 0;
        slot->frameSerial = ++nextFrameSerial;
    }

    *replacementRtv = slot->uiRtv;
    return true;
}

bool TryRedirect(ID3D12GraphicsCommandList* commandList, ID3D12Resource* source, int frameIndex,
                 D3D12_CPU_DESCRIPTOR_HANDLE* replacementRtv)
{
    (void) frameIndex;
    return TryRedirect(commandList, source, replacementRtv);
}

void OnEndDisplay()
{
    std::scoped_lock lock(g_mutex);
    for (auto& slot : g_slots)
        slot.snapshotTakenThisScope = false;
}

ID3D12Resource* GetHudlessColor(ID3D12Resource* backBuffer, D3D12_RESOURCE_STATES* state)
{
    if (state != nullptr)
        *state = D3D12_RESOURCE_STATE_COMMON;
    if (!Config::Instance()->ReprojHudIsolation.value_or_default() || backBuffer == nullptr)
        return nullptr;

    std::scoped_lock lock(g_mutex);
    FrameSlot* newest = nullptr;
    for (auto& slot : g_slots)
    {
        if (slot.backBuffer == backBuffer && slot.hasValidData && slot.hudlessTexture != nullptr &&
            (newest == nullptr || slot.frameSerial > newest->frameSerial))
            newest = &slot;
    }
    if (newest == nullptr)
        return nullptr;
    if (state != nullptr)
        *state = newest->hudlessState;
    return newest->hudlessTexture;
}

ID3D12Resource* GetHudlessColor(int frameIndex, D3D12_RESOURCE_STATES* state)
{
    if (state != nullptr)
        *state = D3D12_RESOURCE_STATE_COMMON;
    if (!Config::Instance()->ReprojHudIsolation.value_or_default() || frameIndex < 0 ||
        frameIndex >= (int) g_slots.size())
        return nullptr;

    std::scoped_lock lock(g_mutex);
    const auto& slot = g_slots[frameIndex];
    if (slot.hasValidData && slot.hudlessTexture != nullptr)
    {
        if (state != nullptr)
            *state = slot.hudlessState;
        return slot.hudlessTexture;
    }
    return nullptr;
}

ID3D12Resource* GetUIColor(ID3D12Resource* backBuffer, D3D12_RESOURCE_STATES* state)
{
    if (state != nullptr)
        *state = D3D12_RESOURCE_STATE_COMMON;
    if (!Config::Instance()->ReprojHudIsolation.value_or_default() || backBuffer == nullptr)
        return nullptr;

    std::scoped_lock lock(g_mutex);
    FrameSlot* newest = nullptr;
    for (auto& slot : g_slots)
    {
        if (slot.backBuffer == backBuffer && slot.hasValidData && slot.uiTexture != nullptr &&
            (newest == nullptr || slot.frameSerial > newest->frameSerial))
            newest = &slot;
    }
    if (newest == nullptr)
        return nullptr;
    if (state != nullptr)
        *state = newest->uiState;
    return newest->uiTexture;
}

ID3D12Resource* GetUIColor(int frameIndex, D3D12_RESOURCE_STATES* state)
{
    if (state != nullptr)
        *state = D3D12_RESOURCE_STATE_COMMON;
    if (!Config::Instance()->ReprojHudIsolation.value_or_default() || frameIndex < 0 ||
        frameIndex >= (int) g_slots.size())
        return nullptr;

    std::scoped_lock lock(g_mutex);
    const auto& slot = g_slots[frameIndex];
    if (slot.hasValidData && slot.uiTexture != nullptr)
    {
        if (state != nullptr)
            *state = slot.uiState;
        return slot.uiTexture;
    }
    return nullptr;
}

void MarkFrameCaptured(ID3D12Resource* backBuffer, ID3D12Resource* hudless, ID3D12Resource* ui, ID3D12Fence* fence,
                       UINT64 fenceValue)
{
    if (backBuffer == nullptr || hudless == nullptr || ui == nullptr || fence == nullptr || fenceValue == 0)
        return;

    std::scoped_lock lock(g_mutex);
    FrameSlot* captured = nullptr;
    for (auto& slot : g_slots)
    {
        if (slot.backBuffer == backBuffer && slot.hudlessTexture == hudless && slot.uiTexture == ui)
        {
            captured = &slot;
            break;
        }
    }
    if (captured == nullptr)
        return;

    SAFE_RELEASE(captured->captureFence);
    fence->AddRef();
    captured->captureFence = fence;
    captured->captureFenceValue = fenceValue;
}

void Reset()
{
    std::scoped_lock lock(g_mutex);
    for (auto& slot : g_slots)
        ReleaseSlot(slot);
}
} // namespace Kcd2HudIsolation
