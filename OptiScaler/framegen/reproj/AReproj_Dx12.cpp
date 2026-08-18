#include "pch.h"
#include "AReproj_Dx12.h"

#include <State.h>
#include <Config.h>
#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <magic_enum.hpp>

const char* AReproj_Dx12::Name() { return "Async Reproj"; }

feature_version AReproj_Dx12::Version() { return feature_version { 1, 0, 0 }; }

HWND AReproj_Dx12::Hwnd() { return _hwnd; }

void* AReproj_Dx12::FrameGenerationContext() { return nullptr; }

void* AReproj_Dx12::SwapchainContext() { return nullptr; }

bool AReproj_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount)
{
    // Only 1x is supported: one reprojected frame per real frame
    return true;
}

void AReproj_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

void AReproj_Dx12::PresentFrame(UINT SyncInterval, UINT Flags)
{
    if (_swapChain == nullptr)
        return;

    // Route through the hooked vtable with the skip flag set, so hkFGPresent passes
    // straight through to the original present (no recursion, no double handling).
    FGHooks::SkipPresent(true);
    auto result = _swapChain->Present(SyncInterval, Flags);
    FGHooks::SkipPresent(false);

    if (result == S_OK)
        LOG_DEBUG("Presented frame, SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    else
        LOG_DEBUG("Present result: {:X}", (UINT) result);
}

bool AReproj_Dx12::Present()
{
    LOG_FUNC();

    if (!IsActive() || IsPaused() || _swapChain == nullptr)
        return true;

    auto fIndex = GetIndex();

    // Flush any pending deferred command lists (resource copies etc.)
    if (_uiCommandListResetted[fIndex] && !SubmitUICommandList((UINT) fIndex))
        LOG_ERROR("Failed to submit pending UI command list for slot {}", fIndex);

    if (_scCommandListResetted[fIndex])
    {
        LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
        auto closeResult = _scCommandList[fIndex]->Close();

        if (closeResult == S_OK)
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
        else
            LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

        _scCommandListResetted[fIndex] = false;
    }

    // M0: present the real frame only.
    // M1: copy backbuffer -> present real -> wait half frame -> warp -> present reprojected.
    PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());

    return true;
}

void AReproj_Dx12::Activate()
{
    if (_isActive)
        return;

    _isActive = true;
    _lastDispatchedFrame = 0;
    LOG_INFO("Reproj: activated");
}

void AReproj_Dx12::Deactivate()
{
    if (!_isActive)
        return;

    auto fIndex = GetIndex();

    if (_uiCommandListResetted[fIndex] && _gameCommandQueue != nullptr && _uiFence != nullptr)
    {
        LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
        auto closeResult = _uiCommandList[fIndex]->Close();

        if (closeResult == S_OK)
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
        else
            LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

        _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);
        _uiCommandListResetted[fIndex] = false;
    }

    _isActive = false;
    LOG_INFO("Reproj: deactivated");
}

void AReproj_Dx12::DestroyFGContext()
{
    LOG_DEBUG("");

    _frameCount = 1;

    Deactivate();

    ReleaseObjects();
}

bool AReproj_Dx12::Shutdown()
{
    Deactivate();

    if (_swapChain != nullptr)
        ReleaseSwapchain(_hwnd);

    ReleaseObjects();

    return true;
}

bool AReproj_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                   IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("Reproj swapchain already created for the same output window!");

            auto bufferCount = (desc->BufferCount < 3) ? 3 : desc->BufferCount;
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              bufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating a new swapchain without releasing the old one
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // A free backbuffer slot is required for the reprojected frame: force >= 3 buffers
    if (desc->BufferCount < 3)
        desc->BufferCount = 3;

    // The reprojected frame is presented as a tear, which requires the tearing flag
    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // FGHooks already coerces these, belt and braces
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    else if (desc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    auto result = realFactory->CreateSwapChain(realQueue, desc, swapChain);

    if (result == S_OK)
    {
        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = desc->OutputWindow;
        _bufferCount = desc->BufferCount;

        LOG_INFO("Reproj swapchain created: {} buffers, {}x{}", _bufferCount, desc->BufferDesc.Width,
                 desc->BufferDesc.Height);
        return true;
    }

    LOG_ERROR("Reproj swapchain creation failed: {:X}", (UINT) result);
    return false;
}

bool AReproj_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                    DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                    IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("Reproj swapchain already created for the same output window!");

            auto bufferCount = (desc->BufferCount < 3) ? 3 : desc->BufferCount;
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              bufferCount, desc->Width, desc->Height, desc->Format,
                              desc->Flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating a new swapchain without releasing the old one
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    IDXGIFactory* realFactory = nullptr;
    ID3D12CommandQueue* realQueue = nullptr;

    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    if (!CheckForRealObject(__FUNCTION__, cmdQueue, (IUnknown**) &realQueue))
        realQueue = cmdQueue;

    // A free backbuffer slot is required for the reprojected frame: force >= 3 buffers
    if (desc->BufferCount < 3)
        desc->BufferCount = 3;

    // The reprojected frame is presented as a tear, which requires the tearing flag
    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // FGHooks already coerces these, belt and braces
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    else if (desc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGIFactory2* factory2 = nullptr;
    auto result = realFactory->QueryInterface(IID_PPV_ARGS(&factory2));

    if (result != S_OK || factory2 == nullptr)
    {
        LOG_ERROR("Reproj swapchain creation failed, factory does not support CreateSwapChainForHwnd: {:X}",
                  (UINT) result);
        return false;
    }

    result = factory2->CreateSwapChainForHwnd(realQueue, hwnd, desc, pFullscreenDesc, nullptr,
                                              (IDXGISwapChain1**) swapChain);
    factory2->Release();

    if (result == S_OK)
    {
        _gameCommandQueue = realQueue;
        _swapChain = *swapChain;
        _hwnd = hwnd;
        _bufferCount = desc->BufferCount;

        LOG_INFO("Reproj swapchain created: {} buffers, {}x{}", _bufferCount, desc->Width, desc->Height);
        return true;
    }

    LOG_ERROR("Reproj swapchain creation failed: {:X}", (UINT) result);
    return false;
}

bool AReproj_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Acquired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    if (!State::Instance().isShuttingDown)
        State::Instance().currentFGSwapchain = nullptr;

    ReleaseObjects();

    if (_swapChain != nullptr)
    {
        // currentFGSwapchain is cleared above, so hkFGRelease passes through instead of recursing
        SAFE_RELEASE(_swapChain);
    }

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

void AReproj_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    CreateObjects(device);

    _constants = fgConstants;
}

template <auto Flag> void ReprojCheckAndUpdateFlag(auto currentFlags, std::string_view flagName)
{
    static std::optional<bool> lastState;
    bool currentState = static_cast<bool>(currentFlags & Flag);

    if (lastState.has_value() && *lastState != currentState)
    {
        LOG_DEBUG("{} changed: {}", flagName, currentState);

        State::Instance().fgChanged = true;
        State::Instance().scChanged = true;
    }

    lastState = currentState;
}

void AReproj_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    _constants = fgConstants;

    // FG swapchain is not created yet
    if (State::Instance().currentFGSwapchain == nullptr)
        return;

    if (State::Instance().isShuttingDown)
    {
        DestroyFGContext();
        return;
    }

    // Track flag changes; they require recreating our internal resources
    ReprojCheckAndUpdateFlag<FG_Flags::Hdr>(fgConstants.flags, "HDR");
    ReprojCheckAndUpdateFlag<FG_Flags::InvertedDepth>(fgConstants.flags, "Inverted Depth");
    ReprojCheckAndUpdateFlag<FG_Flags::JitteredMVs>(fgConstants.flags, "Jittered MVs");
    ReprojCheckAndUpdateFlag<FG_Flags::DisplayResolutionMVs>(fgConstants.flags, "Display Resolution MVs");

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (_uiCommandAllocator[0] == nullptr)
            CreateObjects(device);

        if (State::Instance().fgChanged)
        {
            Deactivate();

            // Pause for a few frames
            UpdateTarget();

            if (State::Instance().scChanged)
                DestroyFGContext();
        }

        if (!IsPaused() && !IsActive())
            Activate();
    }
    else if (IsActive())
    {
        Deactivate();
    }

    if (State::Instance().fgChanged)
    {
        LOG_DEBUG("FG changed");

        State::Instance().fgChanged = false;

        // Pause for a few frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    State::Instance().scChanged = false;
}

bool AReproj_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources we use the provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (_frameResources[fIndex].contains(type) &&
        _frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow)
    {
        return false;
    }

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor && Config::Instance()->FGDisableUI.value_or_default())
        return false;

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    _frameResources[fIndex][type] = {};
    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    // Resource flipping fixes per-game MV/depth orientation (same as FSR-FG)
    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (fResource->type == FG_ResourceType::Velocity || fResource->type == FG_ResourceType::Depth);

    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    // Ensure the resource stays valid until Present() consumes it
    if (fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
        fResource->validity = FG_ResourceValidity::ValidNow;

    fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                              ? FG_ResourceValidity::UntilPresent
                              : FG_ResourceValidity::ValidNow;

    // Copy ValidNow resources so they survive until Present() consumes them
    if (fResource->validity == FG_ResourceValidity::ValidNow)
    {
        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex].at(type);

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        copyOutput->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());

        _resourceCopy[fIndex][type] = copyOutput;
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        LOG_TRACE("Made a copy: {:X} of input: {:X}", (size_t) fResource->copy, (size_t) fResource->resource);
    }

    SetResourceReady(type, fIndex);

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());
    return true;
}

void AReproj_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        SAFE_RELEASE(_lastColor[i]);

        _lastColorState[i] = D3D12_RESOURCE_STATE_COMMON;

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }
}

void AReproj_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;

        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _uiCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _scCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

AReproj_Dx12::~AReproj_Dx12() { Shutdown(); }
