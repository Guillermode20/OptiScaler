#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/reprojection/RP_Dx12.h>
#include "ReprojTelemetry.h"
#include "ContentFrame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

/// Async reprojection (ASW-style) FG output.
/// See AsyncReprojection.md for the full design.
///
/// Virtualizes the game-visible backbuffers when async mode is available.  The game
/// renders only into private textures while the worker exclusively presents the real
/// DXGI swapchain.  The synchronous fallback retains the same ownership boundary.
class AReproj_Dx12 : public virtual IFGFeature_Dx12
{
  public:
    struct RuntimeMetrics
    {
        float realFps = 0.0f;
        float warpFps = 0.0f; // total display FPS (all scheduled outputs are warped)
        float displayFps = 0.0f;
        float poseAgeMs = 0.0f;
        float targetRefreshHz = 0.0f;
        uint32_t warpsPerReal = 0;
        uint32_t droppedWarps = 0;
        uint32_t queueDepth = 0;
        bool depthReady = false;
        bool anchorStale = false;
        bool focusLost = false;
        bool rotationOnly = false;
        bool hudWarped = true;
        bool asyncPresenter = false;
        float gamePresentBlockMs = 0.0f; // game-thread Present() work, pacing sleep excluded
        float gamePresentPaceMs = 0.0f;  // SourceFramerateLimit pacing sleep after the last published anchor
        float meanPresentIntervalMs = 0.0f;
        float p95PresentIntervalMs = 0.0f;
        float dispatchLeadMs = 3.0f;
        uint32_t newAnchorDisplays = 0;
        uint32_t repeatedAnchorDisplays = 0;
        uint32_t missedDisplaySlots = 0;
        uint32_t droppedAnchors = 0;
        uint32_t directCaptures = 0;
        uint32_t captureNotReady = 0;
    };

  private:
    enum class PacketState : uint8_t
    {
        Free,
        Capturing,
        Ready,
        Presenting,
        Retired,
    };

    enum class PresenterState : uint8_t
    {
        Stopped,
        Starting,
        Running,
        Paused,
        Draining,
        Stopping,
        Failed,
    };

    struct ReprojFramePacket : ContentFrame
    {
        UINT64 frameId = 0;
        UINT64 captureFenceValue = 0;
        ID3D12Fence* handoffFence = nullptr; // non-owning; protects virtual-buffer reuse
        UINT64 handoffFenceValue = 0;
        UINT64 retirementFenceValue = 0;
        double frameDelta = 0.0;
        double rawFrameDelta = 0.0; // interval represented by this MV field (pre-EMA, for timestep)
        UINT syncInterval = 0;
        UINT presentFlags = 0;
        int64_t sourceMouseX = 0;
        int64_t sourceMouseY = 0;
        double sourceMouseTimestamp = 0.0;
        bool inputLatchReady = false;
        bool hasCamera = false;
        bool hasUi = false;
        bool warpAllowed = false;
        std::atomic<PacketState> state { PacketState::Free };
    };

    std::unique_ptr<RP_Dx12> _warp;                // the reprojection pass (v1/v2 PSOs)
    ID3D12Resource* _lastColor[BUFFER_COUNT] = {}; // copy of the last presented real frame
    D3D12_RESOURCE_STATES _lastColorState[BUFFER_COUNT] = {};
    ID3D12Resource* _uiColor[BUFFER_COUNT] = {}; // sync-path UI capture composited after warping
    D3D12_RESOURCE_STATES _uiColorState[BUFFER_COUNT] = {};
    bool _syncHasUi[BUFFER_COUNT] = {};
    ID3D12Resource* _warpOutput[BUFFER_COUNT] = {}; // private UAV the warp writes into (backbuffers can't be UAVs)
    bool _forceBorderless = false;

    ReprojFramePacket _packets[BUFFER_COUNT];
    std::atomic<UINT64> _publishedFrameId { 0 };
    std::atomic<UINT64> _readyFrameId { 0 };
    std::mutex _presentMutex;
    std::condition_variable _presentCv;
    std::thread _presentThread;
    std::atomic<bool> _stopPresenter { false };
    std::atomic<PresenterState> _presenterState { PresenterState::Stopped };
    std::atomic<uint32_t> _stageTraceCount { 0 };

    ID3D12CommandQueue* _presentQueue = nullptr;
    ID3D12QueryHeap* _warpTimestampHeap = nullptr;
    ID3D12Resource* _warpTimestampReadback = nullptr;
    UINT64 _presentTimestampFrequency = 0;
    ID3D12Fence* _lateLatchFence = nullptr;
    UINT64 _lateLatchFenceValue = 0;
    ReprojTelemetry _telemetry;
    ReprojSlotRecord* _currentTelemetrySlot = nullptr;
    class WrappedIDXGISwapChain4* _wrappedSwapChain = nullptr; // game-owned, identity checked before use
    HANDLE _presentWaitableObject = nullptr;
    bool _asyncDowngraded = false;

    // COMPUTE queue for async warp — not serialized with the game's DIRECT queue
    // on VKD3D, eliminating the 10-17ms scheduling delay that breaks per-slot steering.
    ID3D12CommandQueue* _computeQueue = nullptr;
    ID3D12CommandAllocator* _computeAllocator[BUFFER_COUNT] = {};
    ID3D12GraphicsCommandList* _computeCommandList[BUFFER_COUNT] = {};
    bool _computeCommandListResetted[BUFFER_COUNT] = {};
    ID3D12Fence* _computeFence = nullptr;
    UINT64 _computeFenceValue = 0;
    UINT64 _computeAllocatorFenceValues[BUFFER_COUNT] = {};

    // Anchor capture runs on a dedicated COPY queue (fallback: game DIRECT) so
    // the color/UI/depth copies overlap rendering instead of extending the
    // game's frame. _captureInputFence orders DIRECT producers before COPY;
    // _captureFence tracks COPY completion for packet readiness. Keeping the
    // fence timelines separate prevents a later DIRECT signal from falsely
    // completing an earlier COPY packet. The async warp stays on COMPUTE.
    ID3D12CommandQueue* _captureQueue = nullptr;
    ID3D12CommandAllocator* _captureAllocator[BUFFER_COUNT] = {};
    ID3D12GraphicsCommandList* _captureCommandList[BUFFER_COUNT] = {};
    bool _captureCommandListResetted[BUFFER_COUNT] = {};
    ID3D12Fence* _captureInputFence = nullptr;
    UINT64 _captureInputFenceValue = 0;
    ID3D12Fence* _captureFence = nullptr;
    HANDLE _captureFenceEvent = nullptr;
    UINT64 _captureFenceValue = 0;
    UINT64 _captureAllocatorFenceValues[BUFFER_COUNT] = {};
    uint32_t _metricsCaptureDepth = 0;
    uint32_t _metricsDepthWarps = 0;
    float _metricsTxCmTotal = 0.0f;
    uint32_t _metricsTxSamples = 0;

    UINT _bufferCount = 0;
    UINT _gameBufferCount = 0; // count requested before FGHooks coerces the private chain
    UINT64 _scFenceValue = 0;  // monotonic SC fence value (fence outlives context recreate)

    bool CopyLastFrame(int fIndex, ID3D12Resource* source);
    static DXGI_FORMAT NormalizeReprojFormat(DXGI_FORMAT format);
    bool VirtualAnchorReady() const;
    HRESULT PresentVirtualFrameSync(int fIndex, ID3D12Resource* source, UINT virtualBufferIndex, UINT syncInterval,
                                    UINT flags, bool allowWarps);
    bool DispatchWarp(int fIndex, float timeStep); // _lastColor[fIndex] rotation warp -> current backbuffer
    bool CaptureFramePacket(int sourceIndex, int packetIndex, ID3D12Resource* gameBackBuffer, UINT virtualBufferIndex,
                            bool warpAllowed);
    bool CaptureAllocatorReady(int packetIndex); // game thread: non-blocking UI-allocator poll, never waits
    bool PollCaptureAllocator(int packetIndex) { return CaptureAllocatorReady(packetIndex); }
    void SkipAnchorPublication(int fIndex, ID3D12Resource* gameBackBuffer, UINT virtualBufferIndex,
                               class WrappedIDXGISwapChain4* wrapped, double presentStartMs);
    bool DispatchPacketWarp(int packetIndex, float timeStep, double scanoutDeadlineMs = 0.0,
                            uint32_t telemetryQueryStart = UINT32_MAX);
    bool DisplayPacket(int packetIndex, bool composeUi, uint32_t telemetryQueryStart = UINT32_MAX);
    bool CopyPacketResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                            D3D12_RESOURCE_STATES sourceState, ID3D12Resource** target,
                            D3D12_RESOURCE_STATES& targetState, const wchar_t* name);
    void FillConstants(int fIndex, RP_Constants& constants);
    bool ApplyLateInput(RP_Constants& constants, const ReprojFramePacket& packet);
    void UpdateMouseSensitivity(int sourceIndex, double sourcePoseTimestamp);
    int AcquirePacket();
    void RetirePackets();
    uint32_t PacketQueueDepth() const;
    bool CreateAsyncPresenter();
    void DestroyAsyncPresenter();
    bool StartAsyncPresenter();
    void StopAsyncPresenter();
    void PresenterMain();
    HRESULT WaitForPresentSlot();
    HRESULT PresentCompositorFrame(UINT syncInterval, UINT flags, bool interpolated, bool waitForSlot = true);
    bool SampleDisplayClock(double nowMs); // lock pacing to DXGI_FRAME_STATISTICS vblanks
    double TargetRefreshHz();
    uint32_t WarpCountForFrame(double refreshHz) const;
    uint32_t WarpCountForPeriod(double realFrameMs, double refreshHz) const;
    void WaitUntil(double deadlineMs) const;
    bool WaitForPresenterDeadline(double deadlineMs);
    bool DrainGpuWork();
    HRESULT PresentFrame(UINT SyncInterval, UINT Flags, bool interpolated = false); // skip-flag wrapped present
    bool SubmitSCCommandList(int fIndex); // close + execute the SC command list
    bool WaitForSCAllocator(int fIndex);  // wait for the previous warp on this slot to finish
    ID3D12GraphicsCommandList* GetComputeCommandList(int fIndex);
    bool SubmitComputeCommandList(int fIndex);
    bool WaitForComputeAllocator(int fIndex);
    ID3D12GraphicsCommandList* GetCaptureCommandList(int fIndex);
    bool SubmitCaptureCommandList(int fIndex);
    bool WaitForCaptureAllocator(int fIndex);
    bool CreateWarpOutput(int fIndex, ID3D12Resource* source); // private UAV buffer, SRGB -> typeless
    bool IsCameraAllZero(int fIndex) const;
    bool IsPoseFresh(double timestamp, float* ageMs = nullptr) const;
    bool HasFreshCameraPose(int fIndex, float* ageMs = nullptr) const;
    void RecordRealFrame();
    void RecordWarpFrame(bool warpPresented, bool dropped, float poseAgeMs);
    void LogMetricsIfDue();

    double _metricsTimestamp = 0.0;
    uint32_t _metricsRealFrames = 0;
    uint32_t _metricsWarpFrames = 0;
    uint32_t _metricsDroppedWarps = 0;
    uint32_t _metricsMaxWarpsPerReal = 0;
    double _metricsPoseAgeTotalMs = 0.0;
    uint32_t _metricsPoseSamples = 0;
    uint32_t _metricsNewAnchorDisplays = 0;
    uint32_t _metricsRepeatedAnchorDisplays = 0;
    uint32_t _metricsMissedDisplaySlots = 0;
    uint32_t _metricsLateInputSamples = 0;
    uint32_t _metricsLateInputApplied = 0;
    // Steering-path distribution per second: authoritative late-camera pose +
    // residual mouse, packet-baseline mouse, or rendered-velocity fallback.
    // Mean age of the late-camera pose at use discriminates a laggy pose pipe.
    uint32_t _metricsLateCamHits = 0;
    uint32_t _metricsPacketBaseHits = 0;
    uint32_t _metricsLateFallbacks = 0;
    double _metricsLateCamAgeTotalMs = 0.0;
    uint32_t _metricsLateCamAgeSamples = 0;
    uint32_t _metricsHudComposites = 0;
    uint32_t _metricsDirectCaptures = 0;
    uint32_t _metricsCaptureNotReady = 0;
    uint32_t _metricsHitchHolds = 0; // slots held at timeStep 0 during a publish stall
    float _metricsLateInputMaxDegrees = 0.0f;
    float _metricsGamePresentBlockMaxMs = 0.0f;
    float _metricsGamePresentPaceMaxMs = 0.0f;
    double _presentIntervals[240] = {};
    uint32_t _presentIntervalCount = 0;
    uint32_t _presentIntervalCursor = 0;
    double _lastDisplayPresentMs = 0.0;
    double _dispatchLeadMs = 3.0;
    RuntimeMetrics _runtimeMetrics {};
    mutable std::mutex _metricsMutex;
    std::mutex _refreshMutex;
    double _cachedRefreshHz = 0.0;
    double _lastRefreshQueryMs = 0.0;
    double _lastRealFrameTimestamp = 0.0;
    uint32_t _metricsSkippedAnchorSamples = 0;
    double _realPeriodEmaMs = 0.0;         // smoothed source-frame period used for warp scaling
    double _measuredRefreshPeriodMs = 0.0; // scanout period measured from DXGI_FRAME_STATISTICS
    double _displayClockAnchorMs = 0.0;    // MillisecondsNow()-domain estimate of the last reported vblank
    double _lastStatsQueryMs = 0.0;
    UINT64 _lastStatsSyncRefreshCount = 0;
    LONGLONG _lastStatsSyncQpc = 0;
    double _lastCapturedMouseTimestamp = 0.0;
    int64_t _lastCapturedMouseX = 0;
    int64_t _lastCapturedMouseY = 0;
    std::atomic<float> _trackedMouseSensitivityX { 0.00015f };
    std::atomic<float> _trackedMouseSensitivityY { 0.00015f };
    std::atomic<bool> _hasTrackedMouseSensitivity { false };
    double _kcd2CalibrationYawRadians = 0.0;
    double _kcd2CalibrationPitchRadians = 0.0;
    uint64_t _kcd2CalibrationMouseX = 0;
    uint64_t _kcd2CalibrationMouseY = 0;
    uint32_t _kcd2CalibrationSamples = 0;

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final;
    feature_version Version() override final;
    HWND Hwnd() override final;

    bool Present() override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override final;
    // FGHooks may raise the private real-chain count before CreateSwapchain runs.
    void SetGameBufferCount(UINT count) { _gameBufferCount = count; }
    RuntimeMetrics GetRuntimeMetrics() const;
    ReprojTelemetrySnapshot GetTelemetrySnapshot() const { return _telemetry.GetSnapshot(); }
    ReprojTelemetry* GetTelemetry() { return &_telemetry; }

    // IFGFeature_Dx12
    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;
    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    AReproj_Dx12() : IFGFeature_Dx12(), IFGFeature() {}
    ~AReproj_Dx12() override;
};
