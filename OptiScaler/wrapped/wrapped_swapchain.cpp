#include "pch.h"
#include "wrapped_swapchain.h"

#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <hooks/Reflex_Hooks.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/FG_Hooks.h>

#include <menu/menu_overlay_dx.h>

#include <misc/FrameLimit.h>
#include <upscaler_time/UpscalerTime_Dx11.h>
#include <upscaler_time/UpscalerTime_Dx12.h>

#include <d3d11.h>
#include <d3d12.h>
#include <misc/IdentifyGpu.h>
#include <hooks/Xell_Hooks.h>

#ifdef LOW_LATENCY_INPUTS
#include <low_latency/input/input_antilag2.h>
#endif

#ifdef DXGI_DEBUG_ENABLED
#include <magic_enum.hpp>
#include <dxgidebug.h>

#pragma comment(lib, "dxguid.lib")

#ifdef ENABLE_DEBUG_LAYER_DX12
#include <d3d12sdklayers.h>
#endif
#endif

#pragma intrinsic(_ReturnAddress)

// Used RenderDoc's wrapped object as referance
// https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/dxgi/dxgi_wrapped.cpp

static int scCount = 0;
static UINT64 _frameCounter = 0;
static double _lastFrameTime = 0;
static bool _dx11Device = false;
static bool _dx12Device = false;

const GUID IID_IUnwrappedDXGISwapChain = {
    0xe8a33b4a, 0x1405, 0x424c, { 0xae, 0x88, 0xd, 0x3e, 0x9d, 0x46, 0xc9, 0x14 }
};

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;

static void WaitForGPUIdle(IUnknown* object)
{
    if (State::Instance().currentD3D12Device == nullptr || object == nullptr)
        return;

    ID3D12CommandQueue* queue = nullptr;

    if (object->QueryInterface(IID_PPV_ARGS(&queue)) == S_OK)
    {
        LOG_DEBUG("Command queue obtained for GPU idle wait");
        queue->Release();
    }

    if (queue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            if (resizeFence != nullptr)
            {
                resizeFence->Release();
                resizeFence = nullptr;
            }

            if (resizeFenceEvent != nullptr)
            {
                CloseHandle(resizeFenceEvent);
                resizeFenceEvent = nullptr;
            }

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        queue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }
}

#ifdef DXGI_DEBUG_ENABLED
void ReportDXGILiveObjects()
{
    IDXGIDebug1* dxgiDebug = nullptr;

    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    {
        dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
        dxgiDebug->Release();
    }
}

void ReadDxgiInfoQueue()
{
    IDXGIInfoQueue* dxgiInfoQueue = nullptr;
    if (DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiInfoQueue)) == S_OK)
    {
        UINT64 msgCount = dxgiInfoQueue->GetNumStoredMessages(DXGI_DEBUG_ALL);
        for (UINT64 i = 0; i < msgCount; ++i)
        {
            SIZE_T msgLen = 0;
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, nullptr, &msgLen);
            std::vector<char> buf(msgLen);
            auto* msg = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(buf.data());
            dxgiInfoQueue->GetMessage(DXGI_DEBUG_ALL, i, msg, &msgLen);

            auto description = std::string(msg->pDescription, msg->DescriptionByteLength);
            LOG_DEBUG("DXGI Debug Message: Category: {}, Severity: {}, ID: {}, Description: {}",
                      magic_enum::enum_name(msg->Category), magic_enum::enum_name(msg->Severity), msg->ID, description);
        }
    }
}

#ifdef ENABLE_DEBUG_LAYER_DX12
void ReportD3D12LiveObjects(ID3D12Device* device)
{
    ID3D12DebugDevice* debugDevice = nullptr;

    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debugDevice))))
    {
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        debugDevice->Release();
    }
}
#endif
#endif

static HRESULT LocalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)
{
    if (State::Instance().isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return pSwapChain->Present(SyncInterval, Flags);
        else
            return ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);
    }

    LOG_DEBUG("{}", _frameCounter);

    HRESULT presentResult;

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        double ftDelta = 0.0;

        auto now = Util::MillisecondsNow();

        if (_lastFrameTime != 0)
            ftDelta = now - _lastFrameTime;

        _lastFrameTime = now;
        State::Instance().presentFrameTime = ftDelta;

        if (State::Instance().currentFG == nullptr)
            State::Instance().lastFGFrameTime = ftDelta;

        LOG_DEBUG("SyncInterval: {}, Flags: {:X}, Frametime: {:0.3f} ms", SyncInterval, Flags, ftDelta);

        // Update swapchain info evey frame
        if (pSwapChain->GetDesc(&State::Instance().currentSwapchainDesc) != S_OK)
            LOG_WARN("Can't get swapchain desc!");
    }

    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D12CommandQueue* cq = nullptr;

    bool isD3D11 = false;

    // try to obtain directx objects and find the path
    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        isD3D11 = true;
        device->Release();

        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
        State::Instance().swapchainApi = DX11;
        State::Instance().currentD3D11Device = device;
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        cq->Release();

        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        ID3D12CommandQueue* realQueue = nullptr;
        if (Util::CheckForRealObject(__FUNCTION__, cq, (IUnknown**) &realQueue))
            cq = realQueue;

        State::Instance().swapchainApi = DX12;

        if (State::Instance().currentCommandQueue == nullptr)
            State::Instance().currentCommandQueue = cq;

        if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            device12->Release();

            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;

            State::Instance().currentD3D12Device = device12;
            D3D12Hooks::HookDevice(device12);
        }
    }

    auto fg = State::Instance().currentFG;
    if (willPresent && fg != nullptr)
        ReflexHooks::update(fg->IsActive(), false);
    else
        ReflexHooks::update(false, false);

    XellHooks::update();

    // Upscaler GPU time computation
    if (willPresent && (fg == nullptr || !fg->IsActive() || fg->IsPaused()))
    {
        if (cq != nullptr)
        {
            UpscalerTimeDx12::ReadUpscalingTime(cq);
        }
        else if (device != nullptr)
        {
            ID3D11DeviceContext* context = nullptr;
            device->GetImmediateContext(&context);
            UpscalerTimeDx11::ReadUpscalingTime(context);
            context->Release();
        }
    }

    // Fallback when FGPresent is not hooked for V-sync
    if (willPresent && Config::Instance()->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  Config::Instance()->ForceVsync.value(), Config::Instance()->VsyncInterval.value_or_default(),
                  State::Instance().SCAllowTearing, State::Instance().realExclusiveFullscreen);

        if (!Config::Instance()->ForceVsync.value())
        {
            SyncInterval = 0;

            if (State::Instance().SCAllowTearing && !State::Instance().realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            // Remove allow tearing
            SyncInterval = Config::Instance()->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    // DXVK check, it's here because of upscaler time calculations
    if (IdentifyGpu::getPrimaryGpu().usesDxvk)
    {
        if (pPresentParameters == nullptr)
            presentResult = pSwapChain->Present(SyncInterval, Flags);
        else
            presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

        if (presentResult == S_OK)
        {
            LOG_TRACE("3 {}", (UINT) presentResult);
        }
        else if (presentResult == DXGI_ERROR_DEVICE_REMOVED)
        {
            if (isD3D11)
            {
                if (State::Instance().currentD3D11Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D11Device);
            }
            else
            {
                if (State::Instance().currentD3D12Device != nullptr)
                    Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
            }
        }
        else
        {
            LOG_ERROR("3 {:X}", (UINT) presentResult);
        }

        return presentResult;
    }

    if (willPresent)
    {
        // Tick feature to let it know if it's frozen
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
            currentFeature->TickFrozenCheck();

        // Draw overlay
        MenuOverlayDx::Present(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP);

#ifdef LOW_LATENCY_INPUTS
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();
            InputAntiLag2::injectAl2Context(pSwapChain, fgIsActive);
        }
#else
        if (State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            LOG_DEBUG("Calling fakenvapi");

            static UINT64 fgPresentFrame = 0;
            auto fgIsActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

            if (State::Instance().fgPresentIsCalled)
            {
                State::Instance().fgPresentIsCalled = false;
                fgPresentFrame = _frameCounter;
            }

            auto isInterpolated = fgIsActive && (_frameCounter - fgPresentFrame) > 0;

            fakenvapi::reportFGPresent(pSwapChain, fgIsActive, isInterpolated);
        }
#endif

        _frameCounter++;
        State::Instance().frameCount = _frameCounter;
    }

    LOG_DEBUG("Calling original present");

    // swapchain present
    if (pPresentParameters == nullptr)
        presentResult = pSwapChain->Present(SyncInterval, Flags);
    else
        presentResult = ((IDXGISwapChain1*) pSwapChain)->Present1(SyncInterval, Flags, pPresentParameters);

    if (presentResult == S_OK)
    {
        LOG_DEBUG("Original present result: {:X}", (UINT) presentResult);
    }
    else
    {
        LOG_ERROR("Original present result: {:X}", (UINT) presentResult);

        if (presentResult == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);
    }

    return presentResult;
}

WrappedIDXGISwapChain4::WrappedIDXGISwapChain4(IDXGISwapChain* real, IUnknown* pDevice, HWND hWnd, UINT flags,
                                               bool isUWP)
    : _real(real), _device(pDevice), _handle(hWnd), _refcount(1), _uwp(isUWP)
{
    _id = ++scCount;
    _lastFlags = flags;

    _real->QueryInterface(IID_PPV_ARGS(&_real1));
    if (_real1 != nullptr)
        _real1->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real2));
    if (_real2 != nullptr)
        _real2->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real3));
    if (_real3 != nullptr)
        _real3->Release();

    _real->QueryInterface(IID_PPV_ARGS(&_real4));
    if (_real4 != nullptr)
        _real4->Release();

    _real->AddRef();
    auto refCount = _real->Release();

    _device2 = _device;

    LOG_INFO("{} created, real: {:X}, refCount: {}", _id, (UINT64) real, refCount);
}

WrappedIDXGISwapChain4::~WrappedIDXGISwapChain4() { ShutdownReprojectionVirtualization(); }

DXGI_FORMAT WrappedIDXGISwapChain4::ReprojectionResourceFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return format;
    }
}

bool WrappedIDXGISwapChain4::PopulateSwapchainBuffers(bool virtualized)
{
    State::Instance().scBuffers.clear();
    if (virtualized)
    {
        for (const auto& entry : _reprojectionBuffers)
            State::Instance().scBuffers.push_back(entry.resource);
        return !_reprojectionBuffers.empty();
    }

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(_real->GetDesc(&desc)))
        return false;
    for (UINT i = 0; i < desc.BufferCount; ++i)
    {
        IUnknown* buffer = nullptr;
        if (SUCCEEDED(_real->GetBuffer(i, IID_PPV_ARGS(&buffer))))
        {
            State::Instance().scBuffers.push_back(buffer);
            buffer->Release();
        }
    }
    return State::Instance().scBuffers.size() == desc.BufferCount;
}

bool WrappedIDXGISwapChain4::InitializeReprojectionVirtualization()
{
    std::scoped_lock lock(_reprojectionMutex);
    if (_reprojectionVirtualized)
        return true;
    if (_reprojectionShuttingDown || !Config::Instance()->ReprojAsync.value_or_default() || _real2 == nullptr ||
        _real3 == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(_real->GetDesc(&desc)) ||
        (desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD && desc.SwapEffect != DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL))
        return false;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    if (_device == nullptr || FAILED(_device->QueryInterface(IID_PPV_ARGS(&queue))) ||
        FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
    {
        SAFE_RELEASE(queue);
        return false;
    }

    _reprojectionWaitableObject = _real2->GetFrameLatencyWaitableObject();
    if (_reprojectionWaitableObject == nullptr)
    {
        device->Release();
        queue->Release();
        return false;
    }

    std::vector<VirtualBackBuffer> buffers(desc.BufferCount);
    HRESULT result = S_OK;
    for (UINT i = 0; i < desc.BufferCount; ++i)
    {
        ID3D12Resource* realBuffer = nullptr;
        result = _real->GetBuffer(i, IID_PPV_ARGS(&realBuffer));
        if (FAILED(result))
            break;
        auto resourceDesc = realBuffer->GetDesc();
        resourceDesc.Format = ReprojectionResourceFormat(resourceDesc.Format);
        realBuffer->Release();

        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        result =
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_PRESENT,
                                            nullptr, IID_PPV_ARGS(&buffers[i].resource));
        if (FAILED(result))
            break;
        buffers[i].resource->SetName(L"Reproj_VirtualBackBuffer");
    }
    device->Release();
    queue->Release();

    if (FAILED(result))
    {
        for (auto& entry : buffers)
            SAFE_RELEASE(entry.resource);
        LOG_WARN("Reproj: virtual backbuffer creation failed: {:X}", (UINT) result);
        return false;
    }

    _reprojectionBuffers = std::move(buffers);
    _reprojectionIndex = 0;
    _reprojectionBuffers[0].state = VirtualBufferState::Rendering;
    _reprojectionDegraded = false;
    _reprojectionVirtualized = true;
    ++_reprojectionGeneration;
    PopulateSwapchainBuffers(true);
    LOG_INFO("Reproj: virtualized {} game backbuffers", _reprojectionBuffers.size());
    return true;
}

bool WrappedIDXGISwapChain4::IsReprojectionVirtualized() const
{
    std::scoped_lock lock(_reprojectionMutex);
    return _reprojectionVirtualized;
}

bool WrappedIDXGISwapChain4::IsReprojectionVirtualizationDegraded() const
{
    std::scoped_lock lock(_reprojectionMutex);
    return _reprojectionDegraded;
}

HRESULT WrappedIDXGISwapChain4::GetReprojectionBuffer(UINT index, REFIID riid, void** resource)
{
    if (resource == nullptr)
        return E_POINTER;
    *resource = nullptr;
    std::scoped_lock lock(_reprojectionMutex);
    if (!_reprojectionVirtualized || index >= _reprojectionBuffers.size())
        return DXGI_ERROR_INVALID_CALL;
    return _reprojectionBuffers[index].resource->QueryInterface(riid, resource);
}

HRESULT WrappedIDXGISwapChain4::SubmitReprojectionBuffer(UINT index, ID3D12Fence* captureFence,
                                                         UINT64 captureFenceValue)
{
    if (captureFence == nullptr || captureFenceValue == 0)
        return E_INVALIDARG;
    std::scoped_lock lock(_reprojectionMutex);
    if (_reprojectionShuttingDown || !_reprojectionVirtualized || index != _reprojectionIndex ||
        index >= _reprojectionBuffers.size() || _reprojectionBuffers[index].state != VirtualBufferState::Rendering)
        return DXGI_ERROR_INVALID_CALL;
    auto& entry = _reprojectionBuffers[index];
    captureFence->AddRef();
    entry.captureFence = captureFence;
    entry.captureFenceValue = captureFenceValue;
    entry.state = VirtualBufferState::Capturing;
    return S_OK;
}

HRESULT WrappedIDXGISwapChain4::AdvanceReprojectionBuffer()
{
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    UINT next = 0;
    uint64_t generation = 0;
    {
        std::scoped_lock lock(_reprojectionMutex);
        if (_reprojectionShuttingDown || !_reprojectionVirtualized || _reprojectionBuffers.empty() ||
            _reprojectionIndex >= _reprojectionBuffers.size() ||
            _reprojectionBuffers[_reprojectionIndex].state != VirtualBufferState::Capturing)
            return DXGI_ERROR_INVALID_CALL;
        generation = _reprojectionGeneration;
        ++_reprojectionAdvancesInFlight;
        next = (_reprojectionIndex + 1) % static_cast<UINT>(_reprojectionBuffers.size());
        auto& entry = _reprojectionBuffers[next];
        fence = entry.captureFence;
        fenceValue = entry.captureFenceValue;
        if (fence != nullptr)
            fence->AddRef();
    }

    HRESULT result = S_OK;
    const auto completedValue = fence != nullptr ? fence->GetCompletedValue() : 0;
    if (fence != nullptr && completedValue == UINT64_MAX)
    {
        result = DXGI_ERROR_DEVICE_REMOVED;
        fence->Release();
    }
    else if (fence != nullptr && completedValue < fenceValue)
    {
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr || FAILED(fence->SetEventOnCompletion(fenceValue, event)) ||
            WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
            result = DXGI_ERROR_WAS_STILL_DRAWING;
        SAFE_CLOSE_HANDLE(event);
        fence->Release();
    }
    else
        SAFE_RELEASE(fence);

    std::scoped_lock lock(_reprojectionMutex);
    const auto finishAdvance = [this]()
    {
        --_reprojectionAdvancesInFlight;
        _reprojectionCv.notify_all();
    };
    if (_reprojectionShuttingDown || !_reprojectionVirtualized || generation != _reprojectionGeneration ||
        _reprojectionBuffers.empty() || next >= _reprojectionBuffers.size() ||
        _reprojectionIndex >= _reprojectionBuffers.size())
    {
        finishAdvance();
        return DXGI_ERROR_INVALID_CALL;
    }
    if (FAILED(result))
    {
        _reprojectionDegraded = true;
        LOG_ERROR("Reproj: virtual backbuffer {} did not retire: {:X}", next, (UINT) result);
        finishAdvance();
        return result;
    }
    auto& current = _reprojectionBuffers[_reprojectionIndex];
    auto& entry = _reprojectionBuffers[next];
    current.state = VirtualBufferState::Available;
    SAFE_RELEASE(entry.captureFence);
    entry.captureFenceValue = 0;
    entry.state = VirtualBufferState::Rendering;
    _reprojectionIndex = next;
    finishAdvance();
    return S_OK;
}

void WrappedIDXGISwapChain4::AbortReprojectionBuffer(UINT index)
{
    std::scoped_lock lock(_reprojectionMutex);
    if (_reprojectionVirtualized && index == _reprojectionIndex && index < _reprojectionBuffers.size() &&
        _reprojectionBuffers[index].state == VirtualBufferState::Rendering)
        _reprojectionBuffers[index].state = VirtualBufferState::Rendering;
}

bool WrappedIDXGISwapChain4::VirtualBuffersHaveExternalReferences() const
{
    for (const auto& entry : _reprojectionBuffers)
    {
        entry.resource->AddRef();
        if (entry.resource->Release() > 1)
            return true;
    }
    return false;
}

void WrappedIDXGISwapChain4::ShutdownReprojectionVirtualization()
{
    std::vector<VirtualBackBuffer> buffers;
    {
        std::unique_lock lock(_reprojectionMutex);
        if (_reprojectionShuttingDown)
        {
            _reprojectionCv.wait(lock, [this]() { return !_reprojectionShuttingDown; });
            return;
        }
        if (!_reprojectionVirtualized && _reprojectionBuffers.empty() && _reprojectionAdvancesInFlight == 0)
            return;
        _reprojectionShuttingDown = true;
        _reprojectionVirtualized = false;
        _reprojectionDegraded = false;
        _reprojectionWaitableObject = nullptr;
        ++_reprojectionGeneration;
        _reprojectionCv.wait(lock, [this]() { return _reprojectionAdvancesInFlight == 0; });
        buffers.swap(_reprojectionBuffers);
        _reprojectionIndex = 0;
    }

    uint32_t leakedBuffers = 0;
    for (auto& entry : buffers)
    {
        bool safeToRelease = true;
        if (entry.captureFence != nullptr && entry.captureFenceValue != 0)
        {
            const auto completedValue = entry.captureFence->GetCompletedValue();
            if (completedValue != UINT64_MAX && completedValue < entry.captureFenceValue)
            {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                safeToRelease = event != nullptr &&
                                SUCCEEDED(entry.captureFence->SetEventOnCompletion(entry.captureFenceValue, event)) &&
                                WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
                SAFE_CLOSE_HANDLE(event);
            }
        }
        if (safeToRelease)
        {
            SAFE_RELEASE(entry.captureFence);
            SAFE_RELEASE(entry.resource);
        }
        else
        {
            ++leakedBuffers;
            LOG_ERROR(
                "Reproj: virtual backbuffer fence did not retire; preserving GPU resources to avoid use-after-free");
        }
    }
    PopulateSwapchainBuffers(false);
    {
        std::scoped_lock lock(_reprojectionMutex);
        _reprojectionShuttingDown = false;
        _reprojectionCv.notify_all();
    }
    if (leakedBuffers != 0)
        LOG_ERROR("Reproj: intentionally retained {} unsafe virtual backbuffer allocation(s)", leakedBuffers);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain1*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain2*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain3*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4)
        {
            AddRef();
            *ppvObject = (IDXGISwapChain4*) this;
            return S_OK;
        }
        else
        {
            return E_NOINTERFACE;
        }
    }
    else if (riid == __uuidof(WrappedIDXGISwapChain4))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }
    else if (riid == __uuidof(IUnknown))
    {
        AddRef();
        *ppvObject = (IUnknown*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIObject))
    {
        AddRef();
        *ppvObject = (IDXGIObject*) this;
        return S_OK;
    }
    else if (riid == __uuidof(IDXGIDeviceSubObject))
    {
        AddRef();
        *ppvObject = (IDXGIDeviceSubObject*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::AddRef()
{
    InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));
    return _refcount;
}

ULONG STDMETHODCALLTYPE WrappedIDXGISwapChain4::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", _refcount, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
#ifdef USE_LOCAL_MUTEX
        OwnedLockGuard lock(_localMutex, 999);
#endif

        MenuOverlayDx::CleanupRenderTarget(true, _handle);

        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == this)
            State::Instance().currentRealSwapchain = nullptr;

        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);

            if (State::Instance().currentFGSwapchain != nullptr)
                State::Instance().currentFGSwapchain = nullptr;
        }

        ShutdownReprojectionVirtualization();
        auto refCount = _real->Release();

        // Disabled for now, cause issues with some games
        /*
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing of main swapchain");
        }
        else
        {
            // Release real swapchain, otherwise it can cause issues when re-creating swapchain with same handle
            while (refCount > 0)
            {
                LOG_DEBUG("Waiting for real swapchain to be released, refCount: {}", refCount);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                refCount = _real->Release();
            }
        }
        */

        LOG_DEBUG("Real swapchain released, refCount: {}", refCount);

        delete this;
    }

    return ret;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetParent(REFIID riid, void** ppParent)
{
    return _real->GetParent(riid, ppParent);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDevice(REFIID riid, void** ppDevice)
{
    return _real->GetDevice(riid, ppDevice);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 4);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real, SyncInterval, Flags, nullptr, _device, _handle, _uwp);

        // When Reflex can't be used to limit, sleep in present
        if (!State::Instance().reflexLimitsFps && State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep(false);
    }
    else
    {
        result = _real->Present(SyncInterval, Flags);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    {
        std::scoped_lock lock(_reprojectionMutex);
        if (_reprojectionVirtualized)
        {
            if (Buffer >= _reprojectionBuffers.size())
            {
                LOG_INFO("Reproj diag: GetBuffer out of range idx={} count={}", Buffer, _reprojectionBuffers.size());
                return DXGI_ERROR_INVALID_CALL;
            }
            // Crash breadcrumb: game re-acquiring buffers post-resize
            static std::atomic<uint32_t> getCount { 0 };
            auto n = getCount.fetch_add(1);
            if (n < 24 || (n % 512) == 0)
                LOG_INFO("Reproj diag: GetBuffer[{}] call #{}", Buffer, n + 1);
            return _reprojectionBuffers[Buffer].resource->QueryInterface(riid, ppSurface);
        }
    }
    auto result = _real->GetBuffer(Buffer, riid, ppSurface);
    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    HRESULT result = S_OK;

    bool ffxLock = false;

    {
#ifdef USE_LOCAL_MUTEX
        // dlssg calls this from present it seems
        // don't try to get a mutex when present owns it while dlssg mod is enabled
        if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
        {
            OwnedLockGuard lock(_localMutex, 3);
        }
#endif
        if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
        {

            if (State::Instance().currentFG != nullptr && State::Instance().currentFG->IsActive() &&
                State::Instance().currentFG->Mutex.getOwner() != 3)
            {
                LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
                State::Instance().currentFG->Mutex.lock(3);
                ffxLock = true;
                LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
            }
            else
            {
                LOG_TRACE("Skipping ffxMutex, owner is already 3");
            }
        }

        State::Instance().realExclusiveFullscreen = Fullscreen;

        result = _real->SetFullscreenState(Fullscreen, pTarget);

        if (result != S_OK)
            LOG_ERROR("result: {:X}", (UINT) result);
        else
            LOG_DEBUG("result: {:X}", result);
    }

    if (ffxLock)
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return _real->GetDesc(pDesc); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                                DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    LOG_DEBUG("");
    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    const bool recreateVirtualization = IsReprojectionVirtualized();
    if (recreateVirtualization)
    {
        std::scoped_lock reprojectionLock(_reprojectionMutex);
        if (VirtualBuffersHaveExternalReferences())
        {
            LOG_WARN("Reproj: ResizeBuffers rejected while virtual backbuffers are referenced");
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    if (recreateVirtualization)
        ShutdownReprojectionVirtualization();

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 1);
    }
#endif

    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
    }

    HRESULT result;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {0}, Width: {1}, Height: {2}, NewFormat: {3}, SwapChainFlags: {4:X}", BufferCount, Width,
              Height, (UINT) NewFormat, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    /*

    const bool outputRequiresRelease =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (outputRequiresRelease && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG("Found SK swapchain, skip releasing backbuffers of main swapchain");
        }
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    */

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    // The real swapchain shares the vtable detoured by FGHooks. Bypass that hook while
    // forwarding the internal resize or it re-enters this wrapper indefinitely.
    FGHooks::SkipResize(true);
    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    FGHooks::SkipResize(false);

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        do
        {
            if (_real3 == nullptr)
                break;

            NewFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

            if (Config::Instance()->UseHDR10.value_or_default())
            {
                NewFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
                hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }

            if (!Config::Instance()->SkipColorSpace.value_or_default())
            {
                UINT css = 0;

                result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                if (result != S_OK)
                {
                    LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                    break;
                }

                if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                {
                    result = _real3->SetColorSpace1(hdrCS);

                    if (result != S_OK)
                    {
                        LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                        break;
                    }
                }

                LOG_INFO("HDR format and color space are set");
            }

        } while (false);
    }

    State::Instance().scBuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().scBuffers.push_back(buffer);
            buffer->Release();
        }
    }

    if (result == S_OK && recreateVirtualization && !InitializeReprojectionVirtualization())
        LOG_WARN("Reproj: virtualization unavailable after ResizeBuffers; synchronous fallback required");

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (State::Instance().currentFG != nullptr && Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _real->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _real1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::Present1(UINT SyncInterval, UINT Flags,
                                                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if (_real1 == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

#ifdef USE_LOCAL_MUTEX
    OwnedLockGuard lock(_localMutex, 5);
#endif

    HRESULT result;

    if ((Flags & DXGI_PRESENT_TEST) == 0)
    {
        result = LocalPresent(_real1, SyncInterval, Flags, pPresentParameters, _device, _handle, _uwp);

        // When Reflex can't be used to limit, sleep in present
        if (!State::Instance().reflexLimitsFps && State::Instance().activeFgOutput == FGOutput::NoFG &&
            !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit())
            FrameLimit::sleep(false);
    }
    else
    {
        result = _real1->Present1(SyncInterval, Flags, pPresentParameters);
    }

    return result;
}

BOOL STDMETHODCALLTYPE WrappedIDXGISwapChain4::IsTemporaryMonoSupported(void)
{
    return _real1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _real2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE WrappedIDXGISwapChain4::GetCurrentBackBufferIndex(void)
{
    {
        std::scoped_lock lock(_reprojectionMutex);
        if (_reprojectionVirtualized)
            return _reprojectionIndex;
    }
    auto index = _real3->GetCurrentBackBufferIndex();
    // LOG_TRACE("index: {}", index);
    return index;
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                         UINT* pColorSpaceSupport)
{
    return _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    State::Instance().isHdrActive = ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 ||
                                    ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

    return _real3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                                 DXGI_FORMAT Format, UINT SwapChainFlags,
                                                                 const UINT* pCreationNodeMask,
                                                                 IUnknown* const* ppPresentQueue)
{
    LOG_DEBUG("");
    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    const bool recreateVirtualization = IsReprojectionVirtualized();
    if (recreateVirtualization)
    {
        std::scoped_lock reprojectionLock(_reprojectionMutex);
        if (VirtualBuffersHaveExternalReferences())
        {
            LOG_WARN("Reproj: ResizeBuffers1 rejected while virtual backbuffers are referenced");
            return DXGI_ERROR_INVALID_CALL;
        }
    }
    if (recreateVirtualization)
        ShutdownReprojectionVirtualization();

#ifdef USE_LOCAL_MUTEX
    // dlssg calls this from present it seems
    // don't try to get a mutex when present owns it while dlssg mod is enabled
    if (!(_localMutex.getOwner() == 4 && State::Instance().activeFgNvngx != FGNvngxReplacement::None))
    {
        OwnedLockGuard lock(_localMutex, 2);
    }
#endif

    if (State::Instance().activeFgOutput == FGOutput::FSRFG &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Waiting ffxMutex 3, current: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.lock(3);
        LOG_TRACE("Accuired ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
    }

    HRESULT result = E_FAIL;
    DXGI_SWAP_CHAIN_DESC desc {};
    _real->GetDesc(&desc);

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        State::Instance().fgResetCapturedResources = true;
        State::Instance().fgOnlyUseCapturedResources = false;
        State::Instance().fgChanged = true;
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);

    State::Instance().scChanged = true;

    if (Config::Instance()->OverrideVsync.value_or_default() && !State::Instance().SCExclusiveFullscreen &&
        State::Instance().currentFG == nullptr)
    {
        LOG_DEBUG("Overriding flags");
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (BufferCount < 2)
            BufferCount = 2;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat: {}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    WaitForGPUIdle(_device);

    // Release swapchain backbuffers to prevent errors when resizing
    const bool isUsingOptiFgFeature =
        State::Instance().activeFgOutput == FGOutput::FSRFG || State::Instance().activeFgOutput == FGOutput::XeFG;

    if (isUsingOptiFgFeature && State::Instance().currentFG != nullptr)
    {
        IDXGISwapChain* skSC = nullptr;
        if (_real->QueryInterface(IID_IUnwrappedDXGISwapChain, (void**) &skSC) == S_OK && skSC != nullptr)
        {
            skSC->Release();
            LOG_DEBUG(
                "Found SK swapchain, skip releasing backbuffersand using ResizeBuffers instead of ResizeBuffers1");

#ifdef DXGI_DEBUG_ENABLED
            ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
            ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

            if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            {
                ScopedSkipHeapCapture skipHeapCapture {};

                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
            else
            {
                _lastFlags = SwapChainFlags;
                result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
            }
        }
        /*
        else
        {
            LOG_DEBUG("Releasing backbuffers, count: {}", desc.BufferCount);

            for (UINT i = 0; i < desc.BufferCount; i++)
            {
                ID3D12Resource* backBuffer = nullptr;
                auto bbResult = _real->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                if (bbResult == S_OK)
                {
                    backBuffer->AddRef();

                    auto refCount = backBuffer->Release();
                    refCount = backBuffer->Release();
                    LOG_DEBUG("Current backbuffer {}, RefCount {}", i, refCount);

                    while (refCount > 1 && refCount < 4294967200ul)
                    {
                        refCount = backBuffer->Release();
                        LOG_DEBUG("Releasing backbuffer {}, RefCount {}", i, refCount);
                    }

                    LOG_DEBUG("Backbuffer {}, RefCount {}", i, refCount);
                }
                else
                {
                    LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                    break;
                }
            }
        }
        */

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef DXGI_DEBUG_ENABLED
    ReportDXGILiveObjects();

#ifdef ENABLE_DEBUG_LAYER_DX12
    ReportD3D12LiveObjects(State::Instance().currentD3D12Device);
#endif
#endif

    // ResizeBuffers1 can itself fall through to ResizeBuffers on some DXGI
    // implementations, so bypass both detours for the internal real call.
    FGHooks::SkipResize(true);
    FGHooks::SkipResize1(true);
    if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    else
    {
        _lastFlags = SwapChainFlags;
        result = _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    FGHooks::SkipResize1(false);
    FGHooks::SkipResize(false);

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

#ifdef DXGI_DEBUG_ENABLED
    if (result != S_OK)
        ReadDxgiInfoQueue();
#endif

    if (result == S_OK && State::Instance().currentFeature == nullptr)
    {
        State::Instance().screenWidth = static_cast<float>(Width);
        State::Instance().screenHeight = static_cast<float>(Height);
        State::Instance().lastMipBias = 100.0f;
        State::Instance().lastMipBiasMax = -100.0f;
    }

    // Crude implementation of EndlesslyFlowering's AutoHDR-ReShade
    // https://github.com/EndlesslyFlowering/AutoHDR-ReShade
    if (Config::Instance()->ForceHDR.value_or_default())
    {
        LOG_INFO("Force HDR on");

        do
        {
            Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            DXGI_COLOR_SPACE_TYPE hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

            if (Config::Instance()->UseHDR10.value_or_default())
            {
                Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                hdrCS = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }

            if (!Config::Instance()->SkipColorSpace.value_or_default())
            {
                UINT css = 0;

                auto result = _real3->CheckColorSpaceSupport(hdrCS, &css);

                if (result != S_OK)
                {
                    LOG_ERROR("CheckColorSpaceSupport error: {:X}", (UINT) result);
                    break;
                }

                if (DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT & css)
                {
                    result = _real3->SetColorSpace1(hdrCS);

                    if (result != S_OK)
                    {
                        LOG_ERROR("SetColorSpace1 error: {:X}", (UINT) result);
                        break;
                    }
                }

                LOG_INFO("HDR format and color space are set");
            }

        } while (false);
    }

    State::Instance().scBuffers.clear();
    UINT bc = BufferCount;
    if (bc == 0 && _real1 != nullptr)
    {
        DXGI_SWAP_CHAIN_DESC1 desc {};

        if (_real1->GetDesc1(&desc) == S_OK)
            bc = desc.BufferCount;
    }

    for (UINT i = 0; i < bc; i++)
    {
        IUnknown* buffer;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&buffer)) == S_OK)
        {
            State::Instance().scBuffers.push_back(buffer);
            buffer->Release();
        }
    }

    if (result == S_OK && recreateVirtualization && !InitializeReprojectionVirtualization())
        LOG_WARN("Reproj: virtualization unavailable after ResizeBuffers1; synchronous fallback required");

    LOG_DEBUG("result: {0:X}", (UINT) result);

    if (State::Instance().activeFgOutput == FGOutput::FSRFG &&
        Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing ffxMutex: {}", State::Instance().currentFG->Mutex.getOwner());
        State::Instance().currentFG->Mutex.unlockThis(3);
    }

    return result;
}

//
HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size,
                                                                 void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}
