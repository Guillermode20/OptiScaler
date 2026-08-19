#include "pch.h"
#include "AReproj_Dx12.h"

#include <algorithm>
#include <cstring>

#include <State.h>
#include <Config.h>
#include <Util.h>
#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>
#include <misc/FrameLimit.h>

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

HRESULT AReproj_Dx12::PresentFrame(UINT SyncInterval, UINT Flags)
{
    if (_swapChain == nullptr)
        return E_FAIL;

    // Route through the hooked vtable with the skip flag set, so hkFGPresent passes
    // straight through to the original present (no recursion, no double handling).
    FGHooks::SkipPresent(true);
    auto result = _swapChain->Present(SyncInterval, Flags);
    FGHooks::SkipPresent(false);

    if (result == S_OK)
        LOG_DEBUG("Presented frame, SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    else
        LOG_DEBUG("Present result: {:X}", (UINT) result);

    if (result == DXGI_ERROR_DEVICE_REMOVED && State::Instance().currentD3D12Device != nullptr)
        Util::GetDeviceRemovedReason(State::Instance().currentD3D12Device);

    return result;
}

bool AReproj_Dx12::SubmitSCCommandList(int fIndex)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT || !_scCommandListResetted[fIndex])
        return true;

    if (_gameCommandQueue == nullptr)
    {
        LOG_ERROR("Can't submit SC command list, queue is nullptr");
        return false;
    }

    LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
    auto closeResult = _scCommandList[fIndex]->Close();

    if (closeResult != S_OK)
    {
        LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);
        return false;
    }

    _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
    _scCommandListResetted[fIndex] = false;

    return true;
}

bool AReproj_Dx12::CopyLastFrame(int fIndex)
{
    IDXGISwapChain3* sc = (IDXGISwapChain3*) _swapChain;
    auto bbIndex = sc->GetCurrentBackBufferIndex();

    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))))
        return false;

    // CreateBufferResource reuses _lastColor when format/size match (leaving it in the
    // state the previous warp put it in), but a (re)created resource starts in COPY_DEST.
    ID3D12Resource* oldLastColor = _lastColor[fIndex];

    if (!CreateBufferResource(_device, bb, D3D12_RESOURCE_STATE_COPY_DEST, &_lastColor[fIndex], false, false))
    {
        bb->Release();
        return false;
    }

    if (_lastColor[fIndex] != oldLastColor)
        _lastColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;

    auto cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr)
    {
        bb->Release();
        return false;
    }

    // The backbuffer is expected to be in PRESENT state at present time (RUI_Dx12 does the same)
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // _lastColor may be left in NON_PIXEL_SHADER_RESOURCE by the previous warp
    ResourceBarrier(cmdList, _lastColor[fIndex], _lastColorState[fIndex], D3D12_RESOURCE_STATE_COPY_DEST);

    cmdList->CopyResource(_lastColor[fIndex], bb);

    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    _lastColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;

    bb->Release();

    // Submit now: the copy must be queued before the real present so the warp (submitted
    // after the present) runs on the same queue after it.
    if (!SubmitUICommandList((UINT) fIndex))
    {
        LOG_ERROR("Reproj: failed to submit last-frame copy");
        return false;
    }

    return true;
}

bool AReproj_Dx12::CreateWarpOutput(int fIndex, ID3D12Resource* source)
{
    auto inDesc = source->GetDesc();

    // sRGB formats can't be used as UAVs; use the typeless parent instead. Typeless and
    // sRGB are in the same DXGI type group, so CopyResource into the backbuffer still works.
    switch (inDesc.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        inDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        inDesc.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        break;
    default:
        break;
    }

    if (_warpOutput[fIndex] != nullptr)
    {
        auto bufDesc = _warpOutput[fIndex]->GetDesc();

        if (bufDesc.Width == inDesc.Width && bufDesc.Height == inDesc.Height && bufDesc.Format == inDesc.Format)
            return true;

        SAFE_RELEASE(_warpOutput[fIndex]);
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = source->GetHeapProperties(&heapProperties, &heapFlags);

    if (hr != S_OK)
    {
        LOG_ERROR("GetHeapProperties result: {:X}", (UINT64) hr);
        return false;
    }

    inDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = _device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, D3D12_RESOURCE_STATE_COMMON,
                                          nullptr, IID_PPV_ARGS(&_warpOutput[fIndex]));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateWarpOutput result: {:X}", (UINT64) hr);
        return false;
    }

    _warpOutput[fIndex]->SetName(L"Reproj_WarpOutput");

    return true;
}

bool AReproj_Dx12::IsCameraAllZero(int fIndex)
{
    for (int i = 0; i < 3; i++)
    {
        if (_cameraPosition[fIndex][i] != 0.0f || _cameraUp[fIndex][i] != 0.0f ||
            _cameraRight[fIndex][i] != 0.0f || _cameraForward[fIndex][i] != 0.0f)
        {
            return false;
        }
    }

    return true;
}

bool AReproj_Dx12::DispatchWarp(int fIndex)
{
    if (_warp == nullptr || !_warp->IsInit())
        return false;

    auto& state = State::Instance();
    auto config = Config::Instance();

    auto depth = GetResource(FG_ResourceType::Depth, fIndex);
    auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
    if (!velocity)
    {
        LOG_WARN("Reproj: no motion vectors for frame {}, skipping fake frame", _frameCount);
        return false;
    }

    IDXGISwapChain3* sc = (IDXGISwapChain3*) _swapChain;
    auto bbIndex = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb))))
        return false;

    // Warp into a private UAV buffer (backbuffers don't expose UAV), then copy it into the backbuffer
    if (!CreateWarpOutput(fIndex, bb))
    {
        bb->Release();
        return false;
    }

    auto cmdList = GetSCCommandList(fIndex);
    if (cmdList == nullptr)
    {
        bb->Release();
        return false;
    }

    RP_Constants cb {};
    cb.displayWidth = (uint32_t) state.currentSwapchainDesc.BufferDesc.Width;
    cb.displayHeight = (uint32_t) state.currentSwapchainDesc.BufferDesc.Height;
    cb.mvWidth = (uint32_t) velocity->width;
    cb.mvHeight = (uint32_t) velocity->height;
    cb.timeStep = config->ReprojTimeStep.value_or_default();
    cb.strength = config->ReprojStrength.value_or_default();
    cb.mvScaleX = _mvScaleX[fIndex];
    cb.mvScaleY = _mvScaleY[fIndex];
    cb.jitterX = _jitterX[fIndex];
    cb.jitterY = _jitterY[fIndex];
    cb.invertMV = config->ReprojInvertMV.value_or_default() ? 1 : 0;
    cb.jitterCancelled = (config->ReprojUseJitterCancel.value_or_default() && IsJitteredMVs()) ? 1 : 0;
    cb.invertedDepth = IsInvertedDepth() ? 1 : 0;
    cb.mode = config->ReprojMode.value_or_default();
    cb.debugView = config->ReprojDebugView.value_or_default() ? 1 : 0;

    cb.cameraNear = _cameraNear[fIndex];
    cb.cameraFar = _cameraFar[fIndex];
    cb.cameraVFov = _cameraVFov[fIndex];
    cb.cameraAspect = _cameraAspectRatio[fIndex];

    std::memcpy(cb.cameraPosition, _cameraPosition[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraUp, _cameraUp[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraRight, _cameraRight[fIndex], 3 * sizeof(float));
    std::memcpy(cb.cameraForward, _cameraForward[fIndex], 3 * sizeof(float));

    // Previous-frame camera pose, used to extrapolate the camera to the fake-frame time
    auto prevIndex = (fIndex + BUFFER_COUNT - 1) % BUFFER_COUNT;
    std::memcpy(cb.prevCameraPosition, _cameraPosition[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraUp, _cameraUp[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraRight, _cameraRight[prevIndex], 3 * sizeof(float));
    std::memcpy(cb.prevCameraForward, _cameraForward[prevIndex], 3 * sizeof(float));

    // v2 needs depth + a valid camera pair; otherwise fall back to the MV warp
    bool hasDepth = depth ? true : false;
    bool hasCamera = _cameraVFov[fIndex] > 0.0f && _cameraAspectRatio[fIndex] > 0.0f &&
                     !IsCameraAllZero(fIndex) && !IsCameraAllZero(prevIndex);
    cb.mode = (hasDepth && hasCamera) ? cb.mode : 0;

    bool ok = _warp->Dispatch(cmdList, _lastColor[fIndex], _lastColorState[fIndex], velocity->GetResource(),
                              velocity->state, hasDepth ? depth->GetResource() : nullptr,
                              hasDepth ? depth->state : D3D12_RESOURCE_STATE_COMMON, _warpOutput[fIndex], cb);

    if (!ok)
    {
        bb->Release();
        return false;
    }

    _lastColorState[fIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // Copy the warp result into the current backbuffer, then submit everything now
    ResourceBarrier(cmdList, _warpOutput[fIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(bb, _warpOutput[fIndex]);
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    ResourceBarrier(cmdList, _warpOutput[fIndex], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    bb->Release();

    if (!SubmitSCCommandList(fIndex))
    {
        LOG_ERROR("Reproj: failed to submit warp command list");
        return false;
    }

    return true;
}

void AReproj_Dx12::WaitHalfFrame()
{
    double target = State::Instance().lastFGFrameTime * Config::Instance()->ReprojTimeStep.value_or_default();
    target = std::clamp(target, 0.0, 20.0); // ms

    if (target <= 0.1)
        return;

    // busy-wait then timer-sleep, exactly the combined_sleep pattern in FrameLimit.cpp
    FrameLimit::sleepForMs(target);
}

bool AReproj_Dx12::Present()
{
    LOG_FUNC();

    if (!IsActive() || IsPaused() || _swapChain == nullptr)
    {
        // FGPresent skips its own present for Reproj, so make sure the real frame still lands
        PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());
        return true;
    }

    auto fIndex = GetIndex();

    // 1. Flush any pending deferred command lists (resource copies from SetResource etc.)
    if (_uiCommandListResetted[fIndex] && !SubmitUICommandList((UINT) fIndex))
        LOG_ERROR("Failed to submit pending UI command list for slot {}", fIndex);

    if (_scCommandListResetted[fIndex])
        SubmitSCCommandList(fIndex);

    // 2. Stall guard: no new frame data for a while, pause instead of presenting garbage
    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing reproj (no new frame data)");
        Deactivate();
        _waitingNewFrameData = true;
        PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());
        return false;
    }

    _fgFramePresentId++;

    // 3. Reset/scene cut or missing motion vectors -> present the real frame only
    if (_reset[fIndex])
    {
        LOG_DEBUG("Reproj: reset frame {}, skipping fake frame", _frameCount);
        PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());
        return true;
    }

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Velocity))
    {
        LOG_WARN("Reproj: no motion vectors for frame {}, skipping fake frame", _frameCount);
        PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());
        return true;
    }

    // 4. Copy the real frame BEFORE presenting it
    if (!CopyLastFrame(fIndex))
    {
        LOG_WARN("Reproj: failed to copy last frame, skipping fake frame");
        PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());
        return true;
    }

    // 5. Present the real frame
    auto realResult = PresentFrame(FGHooks::LastPresentSyncInterval(), FGHooks::LastPresentFlags());

    // 6. If the real present failed (occluded/device removed), don't spin or fake
    if (realResult != S_OK && realResult != DXGI_STATUS_OCCLUDED)
        return true;

    // 7. Pace: wait ~half a frame so the fake frame lands between real frames
    WaitHalfFrame();

    // 8. Warp the last real frame into the current backbuffer
    if (!DispatchWarp(fIndex))
    {
        LOG_WARN("Reproj: failed to dispatch warp, skipping fake frame");
        return true;
    }

    // 9. Present the fake frame (tear; fall back to vsync when tearing is unavailable)
    UINT fakeFlags = DXGI_PRESENT_ALLOW_TEARING;
    UINT fakeInterval = 0;

    if (!State::Instance().SCAllowTearing || State::Instance().realExclusiveFullscreen)
    {
        fakeFlags = 0;
        fakeInterval = 1;
    }

    PresentFrame(fakeInterval, fakeFlags);

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

        // We force ALLOW_TEARING on our swapchain, so tearing fake presents are always allowed
        State::Instance().SCAllowTearing = true;

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

        // We force ALLOW_TEARING on our swapchain, so tearing fake presents are always allowed
        State::Instance().SCAllowTearing = true;

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

    if (_warp == nullptr)
        _warp = std::make_unique<RP_Dx12>("ReprojWarp", device);

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
        if (_uiCommandAllocator[0] == nullptr || _warp == nullptr)
            CreateContext(device, fgConstants);

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
    _warp.reset();

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        SAFE_RELEASE(_lastColor[i]);
        SAFE_RELEASE(_warpOutput[i]);

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
