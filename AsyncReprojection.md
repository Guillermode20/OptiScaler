# Async Reprojection for OptiScaler — Implementation Plan

> **Display-clock update (2026-08-23):** Async virtual-swapchain presentation is display-owned. Rendering publishes anchors but never presents them directly; the worker selects the newest completed anchor at a refresh slot and issues exactly one vblank-synchronized present. The first display of a new valid anchor is warped as well, preventing renderer-rate snap-back. Input-side late latch, auto-calibration, and the projection-space mouse homography were removed after live testing showed they degraded feel and pacing; warps extrapolate rendered poses by `TimeStep` only. This still detaches warp cadence from renderer rate, but not translation, world animation, recoil, head bob, or scripted camera motion.

> Superseded presentation architecture (2026-08-22): `docs/NativeAsyncTimewarpPlan.md` replaces the experimental DirectComposition/secondary-swapchain design. The game now renders into wrapper-owned virtual backbuffers while the worker exclusively owns the real main swapchain. Deep Rock Galactic on Proton live-validated this async path on 2026-08-22: the worker maintained ~60 real plus ~60 warp FPS with ~0.1 ms game-present blocking.

**Status:** Superseded by `docs/NativeAsyncTimewarpPlan.md`
**Scope:** DX12 (matches OptiFG's constraint), 2x reprojection (one fake frame per real frame)
**Audience:** anyone implementing this feature

---

## 1. What we are building

**Async reprojection** (the technique VR uses — SteamVR/Meta ASW): run the game at
~half the display rate, and *reproject* (warp) the previous real frame forward in
time, presenting it halfway between real frames. The warp is driven by the motion
vectors + depth + camera data the game already provides. No ML model, no external
FG library, no optical-flow network. Cost is one cheap compute pass (~0.1–0.5 ms).

| | Frame Gen (FSR-FG / XeFG / DLSSG) | Async Reprojection (this plan) |
|---|---|---|
| New image content | Synthesized by a neural net | No — previous frame warped |
| Cost | Significant (dedicated passes + model) | One compute dispatch |
| Hardware reqs | Vendor libraries + capable GPUs | Any DX12 GPU |
| Motion artifacts | Interpolation ghosting | Smearing at disocclusions |
| Latency | +1 frame | +1 frame |
| HUD ghosting | Needs HUDless/UI infra | Mostly immune (static HUD ⇒ MV≈0) |

The single most important architectural fact that makes this cheap to build: **every
input the warp shader needs is already captured per-frame and pushed into
`IFGFeature`** (depth, velocity, jitter, MV scale, camera pos/up/right/forward,
near/far, vFov, aspect, frame-time delta). We are adding a *fourth consumer* of the
existing FG-input plumbing, not new plumbing.

---

## 2. How FG is wired today (map of everything we touch)

### 2.1 Inputs — captured per frame, pushed into `IFGFeature`

- **OptiFG input** (`OptiScaler/inputs/FG/Upscaler_Inputs_Dx12.cpp`): during the
  upscaler's `UpscaleStart`, OptiScaler calls
  `fg->EvaluateState()`, `fg->StartNewFrame()`, then
  `SetCameraValues()`, `SetFrameTimeDelta()`, `SetMVScale()`, `SetJitter()`,
  `SetReset()`, `SetInterpolationRect()`, and `SetResource()` for Velocity and
  Depth (`Upscaler_Inputs_Dx12.cpp:115-240`).
- **Streamline inputs** (`OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp`):
  `slSetConstants` → `CheckForFrame()` calls `fg->StartNewFrame()`
  (`Streamline_Inputs_Dx12.cpp:44-90`); `slSetTag` → `reportResource()` maps
  `kBufferTypeDepth`/`kBufferTypeMotionVectors`/`kBufferTypeHUDLessColor` to
  `FG_ResourceType::Depth/Velocity/HudlessColor` and calls `fg->SetResource()`
  (`Streamline_Inputs_Dx12.cpp:289-430`). `markPresent()` → `fg->SetFrameCount()`
  (`:445-456`). There is a parallel SL1 path
  (`Streamline_Inputs_Sl1_Dx12.cpp`).
- **FSR3 input** (`OptiScaler/inputs/FG/FSR3_Dx12_FG.cpp`) and **FFX API input**
  (`OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp`): same `SetResource`/camera-setters
  pattern, driven off the game's FSR3 calls (`FfxApi_Dx12_FG.cpp:1040-1240`).

All of these funnel into `IFGFeature` state:
- `_frameResources[BUFFER_COUNT]` per-slot resource table (via `SetResource`)
- camera arrays (`_cameraPosition/Up/Right/Forward`, `_cameraNear/Far/VFov/Aspect`)
- `_jitterX/Y`, `_mvScaleX/Y`, `_ftDelta`, `_reset`, `_interpolationRect`
- per-frame validity/readiness (`_resourceReady`, `FG_ResourceValidity`)

### 2.2 Outputs — the `IFGFeature_Dx12` implementations

`OptiScaler/framegen/` holds the three backends, all subclasses of
`IFGFeature_Dx12` (`OptiScaler/framegen/IFGFeature_Dx12.h`):

- `ffx/FSRFG_Dx12.{h,cpp}` — FSR3/4-FG through `amd_fidelityfx_dx12.dll`
- `xefg/XeFG_Dx12.{h,cpp}` — Intel XeFG
- `dlssg/DLSSG_Dx12.{h,cpp}` — NVIDIA DLSSG through Streamline

Each implements, among others:
- `CreateSwapchain()` / `CreateSwapchain1()` — creates the FG library's own
  swapchain (more buffers than the game requested, owns pacing)
- `SetResource(Dx12Resource*)` — receives depth/MV/HUDless/UI per frame
- `Present()` — called from the present hook; drives the FG dispatch
- `EvaluateState()`, `CreateContext()`, `Activate()`, `Deactivate()`,
  `DestroyFGContext()`, `ReleaseSwapchain()`, `Shutdown()`

**Chosen output is selected by the `FGOutput` enum** (`OptiScaler/State.h:43-48`):
`NoFG, FSRFG, DLSSG, XeFG`. `State::activeFgOutput` mirrors it
(`State.h:136`). The object itself is created in `FGHooks::CreateSwapChain` /
`CreateSwapChainForHwnd`:

```cpp
// FG_Hooks.cpp:111-124 (and :227-235)
if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    State::Instance().currentFG = new FSRFG_Dx12();
else if (State::Instance().activeFgOutput == FGOutput::XeFG)
    State::Instance().currentFG = new XeFG_Dx12();
else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
    State::Instance().currentFG = new DLSSG_Dx12();
```

### 2.3 The present path

For FSRFG/XeFG/DLSSG the flow per game present is:

1. Game calls `Present` on the FG swapchain (the game's swapchain was *replaced*
   during creation; see `FGHooks::SetFGSwapchain` at `FG_Hooks.cpp:314-331`).
2. That vtable is detoured in `FGHooks::HookFGSwapchain()`
   (`FG_Hooks.cpp:396-410`) → `hkFGPresent` / `hkFGPresent1`
   (`FG_Hooks.cpp:1035-1100`).
3. `hkFGPresent` calls `FGPresent()` (`FG_Hooks.cpp:1142`), which:
   - measures frame time (`Util::MillisecondsNow()`)
   - for FSRFG inputs calls `ffxPresentCallback()` (`FfxApi_Dx12_FG.cpp:1247`)
   - calls `fg->Present()` (`FG_Hooks.cpp:1195-1199`) — this submits the FG
     dispatch (depth/MV copies, camera data, ML inference) into the FG swapchain
   - calls `o_FGSCPresent` / `o_FGSCPresent1` (`FG_Hooks.cpp:1233-1240`) — the
     FG library's swapchain present, which does **frame pacing** and presents
     both the real frame and the generated frames
   - handles ForceVsync, Reflex markers, `FrameLimit::sleep(fgActive)`
     (`FG_Hooks.cpp:1276`), HUDfix (`Hudfix_Dx12::PresentStart/End`)
4. For the wrapped (non-FG) path, `WrappedIDXGISwapChain4::Present` →
   `LocalPresent` (`OptiScaler/wrapped/wrapped_swapchain.cpp:91-400`) runs; the
   `fgPresentIsCalled` flag + `_frameCounter` delta tell fakenvapi whether the
   current present is the real or an interpolated frame
   (`wrapped_swapchain.cpp:386-397`).

### 2.4 Frame pacing & FPS capping

- `FrameLimit::sleep(bool fgActive)` (`OptiScaler/misc/FrameLimit.cpp:44-60`):
  when `fgActive`, it doubles the enforced interval — i.e. it caps the game at
  **half** the `FramerateLimit`. This is exactly the half-rate cap reprojection
  needs; the call site `FG_Hooks.cpp:1276` already runs for any
  `activeFgOutput != FGOutput::NoFG`.
- `FrameLimit::combined_sleep` / `busywait_sleep` / `timer_sleep`
  (`FrameLimit.cpp:13-42`) are the pacing primitives we will reuse.

### 2.5 Shader infrastructure

- Base class `Shader_Dx12` (`OptiScaler/shaders/Shader_Dx12.h/.cpp`):
  `SetupRootSignature(device, srcCount, uavCount, cbvCount, rtvCount, samplerCount, staticSamplerCount, pStaticSamplers, flags)` (`Shader_Dx12.cpp:247`),
  `InitHeaps()` for `FrameDescriptorHeap` arrays, `CreateComputePipeline()`
  (with runtime-compile fallback via `CompileShader` when
  `UsePrecompiledShaders=false`, `Shader_Dx12.cpp:97-112`),
  `CreateShaderResourceView/UnorderedAccessView/RenderTargetView`,
  `TranslateTypelessFormats`.
- Reference pass: `format_transfer/FT_Dx12.{h,cpp}` — HLSL source lives inline
  in `FT_Common.h`, precompiled via `shaders/shader_tools/build_precompiled_shader.bat`
  (`dxc -T cs_6_0 -E CSMain` → `.cso` → `create_header.py` → C array header),
  `Dispatch()` cycles `FT_NUM_OF_HEAPS` descriptor heaps.
- `FrameDescriptorHeap` API (from `FT_Dx12.cpp` usage):
  `heap.Initialize(device, srvCount, uavCount, cbvCount, rtvCount)`,
  `GetSrvCPU(i)`, `GetUavCPU(i)`, `GetHeapCSU()`, `GetTableGPUStart()`.

### 2.6 Config & menu

- `Config.h:514` `CustomOptional<FGOutput> FGOutput { FGOutput::NoFG };`
- INI parse: `Config.cpp:95-107` (`FrameGen` → `FGOutput` string→enum);
  save: `Config.cpp:901-913`.
- Menu output combo: `menu_common.cpp:3033-3046` (`outputOptions` vector) +
  `PopulateCombo("FG Output", ...)` at `:3186`.
- Validation on startup: `dllmain.cpp:1857-1865` (generic; no per-output code
  beyond the NVNGX special case).

---

## 3. Core design decision: own swapchain + own Present(), no library

### 3.1 Why an own swapchain

FSR3-FG / XeFG work because their libraries own a swapchain with **many buffers**
and do pacing + multiple presents internally. Reprojection has no library, so we
must own that responsibility. Two options were considered:

**Option A (chosen): implement `CreateSwapchain` ourselves, like FSRFG does, but
calling the real DXGI factory instead of an FFX context.**

- We create a real `IDXGISwapChain` with `BufferCount = max(game, 3)` and
  `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` forced on.
- `FGHooks::CreateSwapChain` already calls `fg->CreateSwapchain(...)` then
  `SetFGSwapchain(...)` + `HookFGSwapchain(...)` (`FG_Hooks.cpp:129-180`), so all
  the existing swapchain hooking (present, resize, release) applies to **our**
  swapchain automatically.
- Why ≥3 buffers: at fake-present time the game's render thread may already be
  rendering frame N+2 into the buffer we want to warp into. With ≥3 buffers there
  is always a free slot, so we never stall or fight the game. (FSR-FG's swapchain
  uses even more.)

**Option B (rejected): reuse the game's swapchain as-is.** With 2-buffer FLIP
swapchains there is no guaranteed-free slot for the fake frame; we'd have to
stall the game or risk presenting garbage. Too fragile.

### 3.2 Why all present logic lives in `AReproj_Dx12::Present()`

`FGPresent()` calls `fg->Present()` and *then* calls `o_FGSCPresent` once
(`FG_Hooks.cpp:1195-1240`). For reprojection the pacing and the double-present
must happen between the game's real frame and the fake frame, so:

- `AReproj_Dx12::Present()` does: **copy → present real → wait half frame →
  warp → present fake**.
- `FGPresent()` **skips its trailing `o_FGSCPresent` call** when
  `activeFgOutput == FGOutput::Reproj` (the feature already presented).

Recursion guard: `Present()` calls the swapchain's `Present` through the hooked
vtable, which re-enters `hkFGPresent`. The existing `_skipPresent` /
`_skipPresent1` flags (`FG_Hooks.h:74-76`, checked at `FG_Hooks.cpp:1038-1052`)
were built for exactly this (XeFG's internal presents). We expose two tiny
setters on `FGHooks` and wrap each internal present:

```cpp
FGHooks::SkipPresent(true);
_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING); // re-enters hkFGPresent → passthrough
FGHooks::SkipPresent(false);
```

---

## 4. File-by-file change list

### 4.1 `OptiScaler/State.h`

- Add enum value:
  ```cpp
  enum class FGOutput : uint32_t
  {
      NoFG,
      FSRFG,
      DLSSG,
      XeFG,
      Reproj,   // NEW — Async Reprojection
  };
  ```
- No other state needed (the feature keeps everything in its own members).
- Do **not** touch `activeFgOutput` initialization (`dllmain.cpp:1857` is
  generic and picks up the new value for free).

### 4.2 `OptiScaler/Config.h`

- Add, after the `FGOutput` line (`Config.h:514`):
  ```cpp
  // Async Reprojection
  CustomOptional<bool> ReprojEnabled { false };       // master switch (mirrors FGEnabled)
  CustomOptional<int>  ReprojMode { 0 };             // 0 = MV warp (v1), 1 = depth-aware (v2)
  CustomOptional<float> ReprojStrength { 1.0f };     // blend of warp result with original
  CustomOptional<float> ReprojTimeStep { 0.5f };     // warp fraction (0.5 = midpoint)
  CustomOptional<bool> ReprojInvertMV { false };     // per-game MV sign convention
  CustomOptional<bool> ReprojUseJitterCancel { true };// subtract jitter from sample pos
  CustomOptional<bool> ReprojCapAtHalfRefresh { true };// auto half-rate cap via FrameLimit
  CustomOptional<bool> ReprojDebugView { false };    // false-color warp debug
  CustomOptional<bool> ReprojForceBorderless { false };// like FGXeFGForceBorderless (excl. FS)
  ```

### 4.3 `OptiScaler/Config.cpp`

- Parse `FrameGen` → `FGOutput=reproj` (add to the chain at `Config.cpp:95-107`):
  ```cpp
  else if (lstrcmpiA(FGOutputString.value().c_str(), "reproj") == 0)
      FGOutput.set_from_config(FGOutput::Reproj);
  ```
- Save (add to the writer at `Config.cpp:901-913`):
  ```cpp
  else if (FGOutputHeld.value() == FGOutput::Reproj)
      FGOutputString = "Reproj";
  ```
- New `[Reproj]` section read block (mirror the `// FSR FG` block at
  `Config.cpp:164-178`) + matching `SaveReproj()` (mirror `SaveXeFG`).

### 4.4 `OptiScaler/hooks/FG_Hooks.h`

- Make the skip flags reachable:
  ```cpp
  public:
      static void SkipPresent(bool skip)  { _skipPresent = skip; }
      static void SkipPresent1(bool skip) { _skipPresent1 = skip; }
  ```

### 4.5 `OptiScaler/hooks/FG_Hooks.cpp`

1. `#include <framegen/reproj/AReproj_Dx12.h>`.
2. `CheckForFGStatus()` (`FG_Hooks.cpp:33-105`): **no guard needed** for
   `Reproj` — there is no library to init. (Leave the FSRFG/XeFG/DLSSG branches
   untouched.)
3. `CreateSwapChain` / `CreateSwapChainForHwnd` creation switch
   (`FG_Hooks.cpp:111-124` and `:227-235`):
   ```cpp
   else if (State::Instance().activeFgOutput == FGOutput::Reproj)
       State::Instance().currentFG = new AReproj_Dx12();
   ```
4. `FGPresent()` (`FG_Hooks.cpp:1142-1260`):
   - After `fg->Present()` succeeds, skip the trailing present:
     ```cpp
     if (state.activeFgOutput == FGOutput::Reproj)
         return result; // AReproj_Dx12::Present() already presented real + fake frames
     ```
     (keep the `fgPresentIsCalled = true` line — see §6.5).
   - Keep everything else (Reflex markers, ForceVsync, FrameLimit) — it is
     output-agnostic and already correct for a non-`NoFG` output.
5. `HookFGSwapchain()` (`FG_Hooks.cpp:396-410`): the XeFG-only detours
   (`hkGetFullscreenState`, `hkGetFullscreenDesc`, waitable-object semaphore) are
   gated on `activeFgOutput == FGOutput::XeFG` — leave as-is; `Reproj` gets the
   generic present/resize/release detours, which is all we need.
6. `hkResizeBuffers` / `hkResizeBuffers1`: for `Reproj`, re-force
   `BufferCount >= 3` and `ALLOW_TEARING` on pass-through (one `if` per function,
   next to the XeFG flag handling at `FG_Hooks.cpp:655-675`).

### 4.6 `OptiScaler/wrapped/wrapped_swapchain.cpp`

- `isUsingOptiFgFeature` (`:762`, `:1141`) and `outputRequiresRelease` (`:764`)
  are already `FSRFG || XeFG` — `Reproj` is excluded automatically, which is what
  we want (no backbuffer-release dance; we own a normal swapchain).
- Optional (stretch): add `Reproj` to the fakenvapi `reportFGPresent` block at
  `:372-397` so LatencyFlex/AntiLag see FG activity. Default: skip.

### 4.7 `OptiScaler/menu/menu_common.cpp`

- Add to `outputOptions` (`:3033-3046`):
  ```cpp
  { FGOutput::Reproj, "Async Reproj",
    "Reprojects the previous frame at half rate (ASW-style)\n"
    "Cheapest, works on any DX12 GPU, smears at disocclusions\n"
    "Set FramerateLimit = refresh rate; reproj halves it" },
  ```
- Disable gating (next to the other outputs at `:3071-3077`):
  `outputOptions[reprojOutputIndex].set_disabled(state.swapchainApi == API::Vulkan, "Unsupported API");`
- When `activeFgOutput == FGOutput::Reproj`, show the `Reproj*` options group
  (mode, strength, time-step, invert MV, debug view) — mirror how FSR-FG options
  are gated at `:3520-3577`.

### 4.8 Project file

- `OptiScaler/OptiScaler.vcxproj` (+ `.filters`): add the new files (§5).
- No `.def` changes (no exports).

---

## 5. New files

### 5.1 `OptiScaler/framegen/reproj/AReproj_Dx12.h`

```cpp
#pragma once
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/reprojection/RP_Dx12.h>

class AReproj_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    std::unique_ptr<RP_Dx12> _warp;                 // the reprojection pass (v1/v2 PSOs)
    ID3D12Resource* _lastColor[BUFFER_COUNT] = {};  // copy of the last presented real frame
    D3D12_RESOURCE_STATES _lastColorState[BUFFER_COUNT] = { D3D12_RESOURCE_STATE_COMMON, ... };

    UINT _bufferCount = 0;                          // our swapchain's buffer count (>=3)
    bool _forceBorderless = false;

    bool CopyLastFrame(int fIndex);                 // backbuffer → _lastColor[fIndex]
    bool DispatchWarp(int fIndex);                  // _lastColor[fIndex] → current backbuffer
    void WaitHalfFrame();                           // pacing (FrameLimit primitives)
    void PresentFrame(UINT SyncInterval, UINT Flags);// skip-flag wrapped present
    static void BuildCameraConstants(FG_Constants& out, int fIndex);

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final;              // "Async Reproj"
    feature_version Version() override final;       // { 1, 0, 0 }
    HWND Hwnd() override final;
    bool Present() override final;                  // the whole double-present
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;
    bool SetInterpolatedFrameCount(UINT) override final; // only 1x; return true

    // IFGFeature_Dx12
    void* FrameGenerationContext() override final { return nullptr; } // no library
    void* SwapchainContext() override final { return nullptr; }      // no library
    bool CreateSwapchain(IDXGIFactory*, ID3D12CommandQueue*, DXGI_SWAP_CHAIN_DESC*,
                         IDXGISwapChain**, bool) override final;
    bool CreateSwapchain1(IDXGIFactory*, ID3D12CommandQueue*, HWND, DXGI_SWAP_CHAIN_DESC1*,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**, bool) override final;
    bool ReleaseSwapchain(HWND) override final;
    void CreateContext(ID3D12Device*, FG_Constants&) override final;
    void EvaluateState(ID3D12Device*, FG_Constants&) override final;
    bool SetResource(Dx12Resource*) override final;
    void SetCommandQueue(FG_ResourceType, ID3D12CommandQueue*) override final;

    AReproj_Dx12() : IFGFeature_Dx12(), IFGFeature() {}
    ~AReproj_Dx12() override;
};
```

### 5.2 `OptiScaler/framegen/reproj/AReproj_Dx12.cpp` — the meat

#### 5.2.1 `CreateSwapchain` / `CreateSwapchain1`

Port `FSRFG_Dx12::CreateSwapchain` (`FSRFG_Dx12.cpp:800-905`) but replace the FFX
context creation with a plain DXGI call:

```cpp
bool AReproj_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue,
                                   DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain, bool readyToRelease)
{
    // reuse the currentFGSwapchain preserve/release logic from FSRFG_Dx12.cpp:816-880 verbatim

    IDXGIFactory* realFactory = nullptr;
    if (!CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &realFactory))
        realFactory = factory;

    // force what we need
    if (desc->BufferCount < 3) desc->BufferCount = 3;          // free slot for fake frame
    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;          // tear presents for fake frame
    if (desc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;    // FG_Hooks already coerces, belt & braces

    HRESULT hr = realFactory->CreateSwapChain(cmdQueue, desc, swapChain);
    if (FAILED(hr)) return false;

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = desc->OutputWindow;
    _bufferCount = desc->BufferCount;
    return true;
}
```

`CreateSwapchain1` mirrors it via `CreateSwapChainForHwnd` (see
`FSRFG_Dx12.cpp:900-990` for the skeleton to port). The DXGI factory here is the
**real** factory (unwrapped via `CheckForRealObject`), so we never recurse into
our own factory hooks.

#### 5.2.2 `Present()` — the heart

```cpp
bool AReproj_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();

    // 1. flush pending UI/SC command lists — copy FSRFG_Dx12::Present() (FSRFG_Dx12.cpp:1731-1770)
    //    (this is where deferred resource copies / HUDless transfers get submitted)

    // 2. stall guard, same as FSRFG_Dx12.cpp:1772-1781
    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }
    _fgFramePresentId++;

    if (!IsActive() || IsPaused())
        return true;

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Velocity))
    {
        LOG_WARN("Reproj: no motion vectors for frame {}, skipping fake frame", _frameCount);
        return true; // game still runs; we just don't fake
    }

    // 3. copy the real frame BEFORE presenting it
    if (!CopyLastFrame(fIndex))
        return true;

    // 4. present the real frame
    PresentFrame(_lastSyncInterval, _lastFlags);              // skip-flag wrapped

    // 5. pace: wait ~half a frame (measured, clamped)
    WaitHalfFrame();

    // 6. warp last real frame into the *current* backbuffer
    if (!DispatchWarp(fIndex))
        return true;

    // 7. present the fake frame (tearing present; vsync'd if no tearing allowed)
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
```

#### 5.2.3 `CopyLastFrame(int fIndex)`

Reuses the base-class command-list machinery (`GetUICommandList` + `_uiFence`,
`IFGFeature_Dx12.h:130-150`; see how `FSRFG_Dx12::HudlessFormatTransfer` uses
it, `FSRFG_Dx12.cpp:143-175`):

```cpp
bool AReproj_Dx12::CopyLastFrame(int fIndex)
{
    IDXGISwapChain3* sc = (IDXGISwapChain3*) _swapChain;
    auto bbIndex = sc->GetCurrentBackBufferIndex();

    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb)))) return false;

    if (!CreateBufferResource(_device, bb, D3D12_RESOURCE_STATE_COPY_DEST, &_lastColor[fIndex],
                              /*UAV=*/false, /*depth=*/false))
    { bb->Release(); return false; }

    auto cmdList = GetUICommandList(fIndex);
    if (cmdList == nullptr) { bb->Release(); return false; }

    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(_lastColor[fIndex], bb);
    ResourceBarrier(cmdList, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

    _lastColorState[fIndex] = D3D12_RESOURCE_STATE_COPY_DEST;   // warp will transition it
    bb->Release();
    return true;
}
```

Note: the copy is submitted on the game queue in the present hook, i.e. strictly
after the game's render lists for frame N — same-queue ordering guarantees the
copy sees the finished frame. `CreateBufferResource` (base, `IFGFeature_Dx12.cpp:332-400`)
auto-recreates on size/format change, which handles resize for free.

#### 5.2.4 `DispatchWarp(int fIndex)`

```cpp
bool AReproj_Dx12::DispatchWarp(int fIndex)
{
    if (_warp == nullptr || !_warp->IsInit()) return false;

    auto& state = State::Instance();
    auto depth = GetResource(FG_ResourceType::Depth, fIndex);     // optional (v2)
    auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
    if (!velocity) return false;

    IDXGISwapChain3* sc = (IDXGISwapChain3*) _swapChain;
    auto bbIndex = sc->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(bbIndex, IID_PPV_ARGS(&bb)))) return false;

    auto cmdList = GetSCCommandList(fIndex);       // dedicated list, like FSRFG uses for UI overlay

    RP_Constants cb {};                            // see §6.1
    cb.displayWidth  = state.currentSwapchainDesc.BufferDesc.Width;
    cb.displayHeight = state.currentSwapchainDesc.BufferDesc.Height;
    cb.mvWidth  = (uint32_t) velocity->width;      // may be render-res
    cb.mvHeight = (uint32_t) velocity->height;
    cb.timeStep = Config::Instance()->ReprojTimeStep.value_or_default();
    cb.strength = Config::Instance()->ReprojStrength.value_or_default();
    cb.mvScaleX = _mvScaleX[fIndex]; cb.mvScaleY = _mvScaleY[fIndex];
    cb.jitterX  = _jitterX[fIndex];  cb.jitterY  = _jitterY[fIndex];
    cb.invertMV = Config::Instance()->ReprojInvertMV.value_or_default();
    cb.jitterCancelled = IsJitteredMVs();          // FG_Flags::JitteredMVs
    cb.invertedDepth = IsInvertedDepth();
    cb.mode = Config::Instance()->ReprojMode.value_or_default();
    cb.debugView = Config::Instance()->ReprojDebugView.value_or_default();
    // v2 camera block (only if mode==1 && camera data present && not all-zero, like FSRFG_Dx12.cpp:514-524)
    std::memcpy(cb.cameraPosition, _cameraPosition[fIndex], sizeof(cb.cameraPosition));
    // ... up/right/forward, near/far/fov/aspect from _cameraNear/Far/VFov/AspectRatio[fIndex]

    bool ok = _warp->Dispatch(cmdList, _lastColor[fIndex], _lastColorState[fIndex],
                              velocity->GetResource(), velocity->state,
                              depth ? depth->GetResource() : nullptr,
                              depth ? depth->state : D3D12_RESOURCE_STATE_COMMON,
                              bb, cb);
    bb->Release();
    return ok;
}
```

#### 5.2.5 `WaitHalfFrame()`

```cpp
void AReproj_Dx12::WaitHalfFrame()
{
    double target = State::Instance().lastFGFrameTime * Config::Instance()->ReprojTimeStep.value_or_default();
    target = std::clamp(target, 0.0, 20.0);           // ms
    if (target <= 0.1) return;

    auto start = Util::MillisecondsNow();
    // busy-wait then timer-sleep, exactly the combined_sleep pattern in FrameLimit.cpp:20-42
    FrameLimit::sleepForMs(target);   // NEW tiny public wrapper over combined_sleep
}
```

(`FrameLimit::combined_sleep` is private; add `static void sleepForMs(double ms)`
public wrapper that calls `combined_sleep((int64_t)(ms * 1'000'000))` — one-line
change, `FrameLimit.h:8-16`.)

#### 5.2.6 `EvaluateState` / `Activate` / `Deactivate` / `CreateContext`

- `CreateContext`: `CreateObjects(device)` → `_warp = std::make_unique<RP_Dx12>("ReprojWarp", device)`; store `_constants = fgConstants`.
- `EvaluateState`: mirror `FSRFG_Dx12::EvaluateState` (`FSRFG_Dx12.cpp:1290-1350`)
  minus the FFX parts: track flag changes via the same `CheckAndUpdateFlag`
  template (`FSRFG_Dx12.cpp:1272-1290`) so HDR/depth-invert changes trigger
  `state.fgChanged`; create/destroy context on `FGEnabled` toggles; handle
  `isShuttingDown`.
- `Activate`/`Deactivate`: `_isActive` flip + flush pending cmdlists
  (`FSRFG_Dx12.cpp:1215-1270`); no library configure calls.
- `SetResource`: port `FSRFG_Dx12::SetResource` (`FSRFG_Dx12.cpp:1404-1519`)
  minus FFX-specific bits (no hudless format transfer, no UI format transfer);
  accept `Depth`/`Velocity` (+ optionally `HudlessColor`/`UIColor` for v2 HUD
  work). Keep `_resourceReady` tracking, `frameIndex` handling, and the
  `FGResourceFlip` path (`FlipResource`, `IFGFeature_Dx12.cpp:276-330`) since it
  fixes per-game MV/depth orientation.
- `ReleaseSwapchain`: port `FSRFG_Dx12::ReleaseSwapchain` (`FSRFG_Dx12.cpp:1000-1030`)
  minus FFX destroy calls; release `_lastColor[]`, `_warp`, reset `_bufferCount`.
- `Shutdown`: `Deactivate()` + `ReleaseSwapchain(_hwnd)` + `ReleaseObjects()`.

### 5.3 `OptiScaler/shaders/reprojection/`

New shader family following the `format_transfer` pattern:

| File | Purpose |
|---|---|
| `RP_Common.h` | Inline HLSL strings (`RPMV_ShaderCode`, `RPD_ShaderCode`) for runtime-compile fallback |
| `RP_Shader.hlsl` | Same source, on disk for the precompile bat |
| `precompile/RP_Shader.h` | Generated C arrays (`RPMV_cso`, `RPD_cso`) via `shader_tools/build_precompiled_shader.bat` |
| `RP_Dx12.h` / `RP_Dx12.cpp` | `RP_Dx12` : `Shader_Dx12` wrapper, two PSOs (MV-only + depth-aware), `Dispatch(...)` |

`RP_Dx12::Dispatch` signature (heap layout: 2 SRVs for v2 [color, depth], 1 SRV
MV, 1 UAV out, 1 CBV):

```cpp
bool Dispatch(ID3D12GraphicsCommandList* cmdList,
              ID3D12Resource* lastColor, D3D12_RESOURCE_STATES lastColorState,
              ID3D12Resource* velocity, D3D12_RESOURCE_STATES velocityState,
              ID3D12Resource* depth,   D3D12_RESOURCE_STATES depthState,   // nullable
              ID3D12Resource* output, RP_Constants& cb);
```

Constructor: `SetupRootSignature(device, 2, 1, 1)` (2 SRVs for v2; v1 uses one),
`CreateComputePipeline` twice (MV + depth PSOs), `InitHeaps(device, _frameHeaps, 2)`
(the double-heap rotation from `FT_Dx12.cpp:98-101`).

---

## 6. The reprojection math

### 6.1 Constants struct (C++ + HLSL mirrored)

```hlsl
cbuffer RP_Constants : register(b0)
{
    uint2  DisplaySize;      // backbuffer size
    uint2  MVSize;           // motion vector texture size (render or display res)
    float  TimeStep;         // 0.5 default
    float  Strength;         // 1.0 default
    float  MVScaleX, MVScaleY;
    float  JitterX, JitterY;
    uint   InvertMV;         // flip MV sign (per-game)
    uint   JitterCancelled;  // FG_Flags::JitteredMVs → subtract jitter
    uint   InvertedDepth;
    uint   Mode;             // 0 = MV warp, 1 = depth-aware
    uint   DebugView;
    // --- Mode 1 only ---
    float4 CameraPos;        // _cameraPosition[fIndex], w = 1
    float4 CameraUp, CameraRight, CameraForward;
    float  CameraNear, CameraFar, CameraVFov, CameraAspect;
};
```

### 6.2 v1 — motion-vector warp (HLSL, full)

The game's MVs conventionally point from a pixel to **where it is next frame**
(forward). We want, for output pixel `p` at time `t = TimeStep` between frame N
and frame N+1, the color that frame N had at the place that *moves to* `p`:
sample `_lastColor` at `p - MV·t`.

```hlsl
Texture2D<float4> LastColor : register(t0);
Texture2D<float4> Velocity  : register(t1);
RWTexture2D<float4> Output  : register(u0);
#include "RP_Constants.hlsl"

SamplerState Bilinear : register(s0);

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    float2 uv = (dtid.xy + 0.5f) / float2(DisplaySize);

    // sample MV (bilinear if MV is lower-res than display)
    float2 mvUV = uv;
    if (MVSize.x != DisplaySize.x || MVSize.y != DisplaySize.y)
        mvUV = uv * float2(DisplaySize.xy) / float2(MVSize.xy);
    float2 mv = Velocity.SampleLevel(Bilinear, mvUV, 0).xy;

    // apply scale (pixels), jitter cancellation, sign convention
    float2 delta = mv * float2(MVScaleX, MVScaleY);
    if (JitterCancelled) delta -= float2(JitterX, JitterY);
    if (InvertMV) delta = -delta;

    // move BACKWARD along the flow to fetch the source texel
    float2 srcUV = uv - delta * TimeStep;

    // clamp to edge (no wrap) — disocclusion smear is accepted for v1
    srcUV = clamp(srcUV, 0.0f, 1.0f);

    float4 c = LastColor.SampleLevel(Bilinear, srcUV, 0);

    if (DebugView)
        Output[dtid.xy] = float4(length(delta) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
    else
        Output[dtid.xy] = float4(c.rgb * Strength + LastColor.SampleLevel(Bilinear, uv, 0).rgb * (1.0f - Strength), 1.0f);
}
```

Design notes:
- **Sampler**: add one static sampler (bilinear clamp) to the root signature
  (`SetupRootSignature(..., 1, 0, 0, pStaticSampler)` — the base class already
  supports `staticSamplerCount`/`pStaticSamplers`, `Shader_Dx12.cpp:247-280`).
- **MV sign**: `ReprojInvertMV` exists because games disagree (the project
  already fights this war — see the `FGResourceFlip` option and
  `FlipResource`, `IFGFeature_Dx12.cpp:276-330`).
- **MV res**: if MVs are render-res (`IsLowResMV()`), the sample UV is scaled;
  scale factors come from `_interpolationWidth/Height` vs backbuffer size.
- **Alpha**: backbuffer alpha is 1; keep it.

### 6.3 v2 — depth-aware reprojection (HLSL sketch)

Reconstruct the world position of each output pixel using frame N's depth and
camera, move the camera to its midpoint pose, reproject, and sample — with the
MV-warp as the fallback where the depth test fails:

```hlsl
// build view & proj from CameraPos/Up/Right/Forward + Near/Far/VFov/Aspect
float3 Forward = normalize(CameraForward.xyz);
float3 Right   = normalize(CameraRight.xyz);
float3 Up      = normalize(CameraUp.xyz);

float tanHalf = tan(CameraVFov * 0.5);
// ndc → view space
float3 viewDir = normalize(Right * ndc.x * CameraAspect * tanHalf +
                           Up * ndc.y * tanHalf + Forward);
float viewZ = LinearizeDepth(depth, CameraNear, CameraFar, InvertedDepth);
float3 worldPos = CameraPos.xyz + viewDir * viewZ;

// midpoint camera (only position + rotation change; no FOV change in v1)
float t = TimeStep;
float3 midPos = lerp(CameraPos.xyz, NextCameraPos.xyz, t);   // NextCamera = prev frame's? see note
float3x3 midRot = ...;                                        // slerp/nlerp of camera bases

float3 pMid = mul(midRot, worldPos - midPos);                 // view space at mid time
float2 reprojUV = ndcFromView(pMid);
float depthMid = ...;

// confidence: compare depthMid against depth at reprojUV — if it differs too much
// (disocclusion), fall back to the v1 MV-warp sample
float2 mvSample = /* v1 sample */;
float4 warped = LastColor.SampleLevel(Bilinear, reprojUV, 0);
float4 mvWarp  = LastColor.SampleLevel(Bilinear, mvSample, 0);
float conf = saturate(1.0 - abs(depthMid - depthAt(reprojUV)) * someScale);
Output[dtid.xy] = lerp(mvWarp, warped, conf);
```

Camera pose for "mid time": since we present the fake frame between frame N and
N+1 using **frame N's** color, the sensible reprojection target is frame N's
camera moved forward by `TimeStep` of its own motion. Two sources for the
"next" pose: (a) `_cameraPosition[fIndex-1]` history (needs one extra frame of
camera history stored in the feature — add a small ring buffer); (b) simpler and
good enough for v1.5: extrapolate from the current frame's own camera by
`(CameraPos[fIndex] - CameraPos[fIndex-1])`. The exact choice is a quality
tuning decision; keep it behind `ReprojMode` so v1 always works.

Known v2 limitations (document in the menu tooltip):
- Disocclusion edges get the MV-warp fallback (smear) — acceptable.
- HUD/UI: static UI has MV≈0, so the depth path will *move* it (ghosting). Fix:
  blend weight to 0 where `length(mv) < epsilon` (UI is static) or reuse the
  existing HUDless/UI infra (`FGDisableHudless`, `FGHudCutoff`, `HudCopy_Dx12`)
  that FSR-FG uses (`FSRFG_Dx12.cpp:660-700`). v1 (MV-only) is naturally
  HUD-safe; v2 needs this.

---

## 7. Frame cadence, pacing, vsync — the reasoning

### 7.1 Present sequence (one game frame = two presents)

```
t=0      game renders frame N into buffer B
         game calls Present(B)  →  hkFGPresent  →  FGPresent
                                            │
                                            ├─ fg->Present():
                                            │    ├─ CopyLastFrame:  B → _lastColor[fIndex]   (same queue, after game lists)
                                            │    ├─ present real:   o_FGSCPresent(B, game interval)
                                            │    ├─ WaitHalfFrame:  ~frameTime/2  (busy/timer wait)
                                            │    ├─ DispatchWarp:   warp _lastColor → buffer A (current index)
                                            │    └─ present fake:   o_FGSCPresent(A, 0, ALLOW_TEARING)
                                            └─ (skipped for Reproj — feature already presented)
t=frameTime  game renders frame N+1 into buffer A … (A was presented as fake, now free for reuse)
```

### 7.2 Why the fake present must be a tearing present

With vsync on (`SyncInterval=1`), a present submitted at `t+frameTime/2` would
wait for the *next* vsync — i.e. it would land exactly where frame N+1 should,
destroying the cadence. The fake frame must be presented as a **tear**:
`SyncInterval=0 + DXGI_PRESENT_ALLOW_TEARING` (requires the swapchain
`ALLOW_TEARING` flag — we force it in `CreateSwapchain`). This is precisely how
FSR3-FG presents generated frames (its swapchain is tearing-capable). When
tearing is unavailable (no `SCAllowTearing`, or exclusive fullscreen), fall back
to `SyncInterval=1` (fake lands on the next vsync; quality degrades to "no real
benefit" — surface this in the stats overlay as a warning).

### 7.3 Half-rate capping — zero new code

`FG_Hooks.cpp:1276` already calls `FrameLimit::sleep(fg != nullptr ? fg->IsActive() && !fg->IsPaused() : false)`
for every `activeFgOutput != NoFG`, and `FrameLimit::sleep(true)` doubles the
enforced interval (`FrameLimit.cpp:50-52`). So: user sets
`FramerateLimit = refresh rate` and reprojection naturally halves it — matching
how FSR-FG users already configure the game. `ReprojCapAtHalfRefresh` (default
true) is therefore **documentation**, not code, unless we want to auto-set the
cap (out of scope v1).

### 7.4 Latency

Exactly the same trade as FSR-FG: game renders at half rate ⇒ +1 real frame of
input latency; the fake frame never adds latency. Reflex markers
(`PCLSetMarker`) in `FGPresent` (`FG_Hooks.cpp:1176-1183`, `:1260-1268`) are
gated on `activeFgOutput == FGOutput::DLSSG` today; optionally extend the same
treatment to `Reproj` so fakenvapi/Reflex pacing sees the real-frame cadence
(can reuse the exact block with the output check widened). v1: skip.

### 7.5 Truly "async" (stretch)

v1 paces synchronously inside the present hook (the game's present thread sleeps
half a frame) — this is what FSR-FG's swapchain does internally anyway, so it is
not a regression in behavior. A later milestone can move the warp dispatch +
fake present to a dedicated thread driven by a fence + waitable timer
(`_rpFence`/`CreateWaitableTimerExW`, the pattern in `FrameLimit.cpp:15-19`), so
the game's present thread never sleeps. This is M5; the interface
(`Present()` returning after both presents) is unchanged, so it is purely
internal.

---

## 8. Edge cases & hardening

| Case | Handling |
|---|---|
| MVs missing for a frame | Skip fake frame (game runs at half rate visibly). Log once. |
| Reset / scene cut (`_reset[fIndex]`) | Skip fake frame that frame; optionally force `Strength=0` (plain copy). |
| First frames after activation | No fake frames until ≥1 real frame captured (`_lastColor` non-null). |
| Resize / format change | `CreateBufferResource` auto-recreates `_lastColor`; `hkResizeBuffers` passes through; re-force `BufferCount ≥ 3`. |
| Exclusive fullscreen | Fake present can't tear → `SyncInterval=1` fallback; `ReprojForceBorderless` (mirror `FGXeFGForceBorderless`, `FG_Hooks.cpp:461-500`). |
| Alt-tab / minimized | Present returns `DXGI_STATUS_OCCLUDED`/`DXGI_ERROR_DEVICE_REMOVED` → skip fake present, don't spin. |
| Toggle off mid-game | `FGEnabled` → `EvaluateState` → `Deactivate()` (mirror FSRFG `CheckAndUpdateFlag` + `fgChanged` path). |
| `DXGI_ERROR_DEVICE_REMOVED` | Pass through; reuse `Util::GetDeviceRemovedReason` (`FG_Hooks.cpp:1244-1248`). |
| HDR | Warp is format-agnostic (samples & copies); `IsHdr()` flag only matters for v2 depth math. |
| D3D11 / Vulkan | Out of scope (matches OptiFG's DX12-only constraint). Menu gates on `swapchainApi == DX12`. |
| 3+ buffer swapchains | Our own swapchain guarantees ≥3; game's requested count preserved when ≥3. |
| Waitable-object games | We don't hook `GetFrameLatencyWaitableObject` (that's XeFG-only, `FG_Hooks.cpp:437-458`) — nothing to do. |
| fakenvapi / LatencyFlex | Optional: add `Reproj` to `reportFGPresent` block (`wrapped_swapchain.cpp:372-397`) so `isInterpolated` is correct; v1 can ship without it. |

---

## 9. Verification / testing plan

Use the existing stats overlay (PageUp) and the log (spdlog, set log level via
INI `LogLevel=debug`/`trace`).

1. **Smoke**: game with OptiFG input (e.g. Cyberpunk 2077, DLSS input, FSR-FG
   known-working) → set `FGOutput=Reproj`, `FGInput=Upscaler`. Expect: game runs,
   menu shows "Async Reproj", no swapchain errors, stats overlay shows ~2×
   present rate.
2. **Frame flow**: trace log shows per present: copy → present real → wait → warp
   → present fake; `_fgFramePresentId` increments once per game present.
3. **Pacing**: 120Hz display, game capped at 60 → fake lands ~8.3ms after real
   (add a `LOG_TRACE` with measured interval during M2).
4. **Artifacts**: fast camera pan — v1 shows smear at edges (expected); v2 shows
   correct parallax with smear only at disocclusions.
5. **Resize / fullscreen toggle / alt-tab**: no hangs, no device-removed spam.
6. **HUD**: static HUD should look stable in v1; v2 may need the MV-epsilon
   blend.
7. **Second input path**: repeat with Streamline input (game with native DLSSG
   + `FGInput=DLSSG`) to validate the SL resource path end-to-end.
8. **Regression**: FSR-FG and XeFG outputs still work (we only added an enum
   value + one skip branch in `FGPresent`).

---

## 10. Milestones (each independently shippable)

- **M0 — Scaffold**: enum/config/menu/class skeleton; `CreateSwapchain` creates
  the 3-buffer real swapchain; `Present()` presents the real frame only (no
  fake). *Exit: game runs with `FGOutput=Reproj` selected, swapchain survives,
  menu toggles.*
- **M1 — v1 warp + double present (no pacing)**: `CopyLastFrame` +
  `DispatchWarp` (MV-only) + immediate fake present. *Exit: 2× presents visible,
  artifacts expected.*
- **M2 — Pacing & vsync**: `WaitHalfFrame`, tearing flags, `SyncInterval`
  fallback, `FrameLimit::sleepForMs`. *Exit: fake lands at frameTime/2; stats
  overlay confirms.*
- **M3 — Hardening**: resize, fullscreen, alt-tab, pause, toggle-off, device
  removed, `_skipPresent` re-entry proof, log polish. *Exit: 30 min gameplay, no
  errors at debug log level.*
- **M4 — v2 depth-aware + HUD**: second PSO, camera constants, depth test,
  MV-fallback, HUD epsilon blend, `ReprojMode/Strength/TimeStep` menu options.
  *Exit: quality parity with "acceptable ASW" in motion.*
- **M5 (stretch) — true async**: fence + timer-driven fake present thread; and/or
  fakenvapi `reportFGPresent` integration.

---

## 11. Open questions

1. **MV sign convention** is per-game (the project already has
   `FGResourceFlip`/`FlipResource` for this); `ReprojInvertMV` covers it, but we
   should default it from the same signals FSR-FG uses.
2. **v2 "next camera pose"**: extrapolate from the current frame's own motion vs.
   a stored previous-pose ring buffer — decide during M4.
3. **Naming**: `Reproj` (short, matches `FSRFG`/`XeFG` style) vs `AReproj` vs
   `ASW` — pick one before M0.
4. **FPS cap UX**: document-only (`FramerateLimit = refresh`) vs auto-applying a
   half cap when `Reproj` is active (needs a `ReflexLimitsFps`-aware hook — more
   invasive; keep for later).
5. Whether `Reproj` should appear in the FG **Input** constraints (it works with
   OptiFG, Streamline, and FSR3 inputs — no new input type needed).

---

## 12. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Double-present fights game's present thread | High | Own 3-buffer swapchain; pacing in present thread like FSR-FG does; M3 stress test |
| Backbuffer UAV not supported on some drivers | Low | Fallback: warp into private UAV then `CopyResource` into backbuffer (pattern exists in `CopyResource`, `IFGFeature_Dx12.cpp:430-446`) |
| MV quality per game (missing/wrong sign) | Medium | `ReprojInvertMV`, `FGResourceFlip` reuse, skip-fake fallback |
| Vsync cadence drift (tear presents) | Medium | Tearing fallback to `SyncInterval=1`; document half-refresh requirement |
| HUD ghosting in v2 | Medium | MV-epsilon blend; reuse HUDless/HudCopy infra |
| Current FG outputs regress | Low | Only additive enum value + one guarded skip branch in `FGPresent` |

---

## 13. Reference index (files cited)

- `OptiScaler/State.h` — `FGOutput` enum `:43-48`; `activeFgOutput` `:136`; `currentFG` `:326`
- `OptiScaler/Config.h` — `FGOutput` `:514`; `FGEnabled` `:534`; `FGAsync` `:579`
- `OptiScaler/Config.cpp` — parse `:70-180`; save `:881-974`
- `OptiScaler/hooks/FG_Hooks.{h,cpp}` — creation switch `:111-124`,`:227-235`;
  `HookFGSwapchain` `:396-410`; `hkFGPresent/1` `:1035-1100`; `FGPresent` `:1142-1260`;
  skip flags `FG_Hooks.h:74-76`
- `OptiScaler/framegen/IFGFeature.{h,cpp}` — `StartNewFrame` `:30-55`;
  `GetDispatchIndex` `:66-89`; `UpdateTarget`; all setters
- `OptiScaler/framegen/IFGFeature_Dx12.{h,cpp}` — cmdlist/fence machinery;
  `NewFrame` `:243`; `FlipResource` `:276-330`; `CopyResource` `:430-446`
- `OptiScaler/framegen/ffx/FSRFG_Dx12.cpp` — `Dispatch` `:330`; `CreateSwapchain`
  `:800`; `CreateContext` `:1040`; `SetResource` `:1404`; `Present` `:1731`
- `OptiScaler/inputs/FG/Upscaler_Inputs_Dx12.cpp` — OptiFG input capture `:94-250`
- `OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp` — `CheckForFrame` `:44-90`;
  `reportResource` `:289-430`; `markPresent` `:445`
- `OptiScaler/inputs/FG/FfxApi_Dx12_FG.cpp` — `ffxPresentCallback` `:1247`
- `OptiScaler/shaders/Shader_Dx12.cpp` — `CreateComputePipeline` `:97-112`;
  `SetupRootSignature` `:247`; `InitHeaps` `:330`
- `OptiScaler/shaders/format_transfer/FT_Dx12.{h,cpp}` — reference pass pattern
- `OptiScaler/shaders/shader_tools/build_precompiled_shader.bat` — precompile flow
- `OptiScaler/misc/FrameLimit.cpp` — `sleep` `:44-60`; `combined_sleep` `:20-42`
- `OptiScaler/wrapped/wrapped_swapchain.cpp` — `LocalPresent` `:91-400`;
  `isUsingOptiFgFeature` `:762`,`:1141`; fakenvapi `:372-397`
- `OptiScaler/menu/menu_common.cpp` — `outputOptions` `:3033-3046`;
  `PopulateCombo` `:3186`
- `OptiScaler/dllmain.cpp` — FG init `:1857-1865`
