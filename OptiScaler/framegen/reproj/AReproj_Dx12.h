#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/reprojection/RP_Dx12.h>
#include "ReprojTelemetry.h"

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
        float gamePresentBlockMs = 0.0f;
        float meanPresentIntervalMs = 0.0f;
        float p95PresentIntervalMs = 0.0f;
        float dispatchLeadMs = 3.0f;
        uint32_t newAnchorDisplays = 0;
        uint32_t repeatedAnchorDisplays = 0;
        uint32_t missedDisplaySlots = 0;
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

    struct ReprojFramePacket
    {
        ID3D12Resource* color = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* velocity = nullptr;
        ID3D12Resource* ui = nullptr;
        D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES velocityState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;
        RP_Constants constants {};
        UINT64 frameId = 0;
        UINT64 captureFenceValue = 0;
        UINT64 retirementFenceValue = 0;
        double renderTimestamp = 0.0;
        double sourcePoseTimestamp = 0.0;
        double sourcePoseInterval = 0.0; // exact interval represented by camera current/previous poses
        double frameDelta = 0.0;
        double rawFrameDelta = 0.0; // interval represented by this MV field (pre-EMA, for timestep)
        UINT syncInterval = 0;
        UINT presentFlags = 0;
        bool hasDepth = false;
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
    bool _syncHudlessOnly[BUFFER_COUNT] = {}; // hudless captured without separate UI (KCD2 pre-UI FSR)
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
    ReprojTelemetry _telemetry;
    ReprojSlotRecord* _currentTelemetrySlot = nullptr;
    class WrappedIDXGISwapChain4* _wrappedSwapChain = nullptr; // game-owned, identity checked before use
    HANDLE _presentWaitableObject = nullptr;
    bool _asyncDowngraded = false;

    UINT _bufferCount = 0;
    UINT _gameBufferCount = 0; // count requested before FGHooks coerces the private chain
    UINT64 _scFenceValue = 0; // monotonic SC fence value (fence outlives context recreate)

    bool CopyLastFrame(int fIndex, ID3D12Resource* source);
    static DXGI_FORMAT NormalizeReprojFormat(DXGI_FORMAT format);
    bool VirtualAnchorReady() const;
    HRESULT PresentVirtualFrameSync(int fIndex, ID3D12Resource* source, UINT virtualBufferIndex, UINT syncInterval,
                                    UINT flags, bool allowWarps);
    bool DispatchWarp(int fIndex, float timeStep); // _lastColor[fIndex] + MV (+depth) -> current backbuffer
    bool CaptureFramePacket(int sourceIndex, int packetIndex, ID3D12Resource* gameBackBuffer, UINT virtualBufferIndex,
                            bool warpAllowed);
    bool DispatchPacketWarp(int packetIndex, float timeStep, double scanoutDeadlineMs = 0.0, uint32_t telemetryQueryStart = UINT32_MAX);
    bool DisplayPacket(int packetIndex, bool composeUi, uint32_t telemetryQueryStart = UINT32_MAX);
    bool CopyPacketResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                            D3D12_RESOURCE_STATES sourceState, ID3D12Resource** target,
                            D3D12_RESOURCE_STATES& targetState, const wchar_t* name);
    void FillConstants(int fIndex, RP_Constants& constants);
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
    void UpdateWarpGpuDuration(int outputIndex);
    bool SampleDisplayClock(double nowMs); // lock pacing to DXGI_FRAME_STATISTICS vblanks
    double TargetRefreshHz();
    uint32_t WarpCountForFrame(double refreshHz) const;
    uint32_t WarpCountForPeriod(double realFrameMs, double refreshHz) const;
    void WaitUntil(double deadlineMs) const;
    bool WaitForPresenterDeadline(double deadlineMs);
    bool DrainGpuWork();
    HRESULT PresentFrame(UINT SyncInterval, UINT Flags, bool interpolated = false); // skip-flag wrapped present
    bool SubmitSCCommandList(int fIndex);                      // close + execute the SC command list
    bool WaitForSCAllocator(int fIndex);                       // wait for the previous warp on this slot to finish
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
    double _presentIntervals[240] = {};
    uint32_t _presentIntervalCount = 0;
    uint32_t _presentIntervalCursor = 0;
    double _lastDisplayPresentMs = 0.0;
    double _dispatchLeadMs = 3.0;
    double _warpDurationEmaMs = 2.0;
    RuntimeMetrics _runtimeMetrics {};
    mutable std::mutex _metricsMutex;
    std::mutex _refreshMutex;
    double _cachedRefreshHz = 0.0;
    double _lastRefreshQueryMs = 0.0;
    double _lastRealFrameTimestamp = 0.0;
    double _realPeriodEmaMs = 0.0;         // smoothed source-frame period used for warp scaling
    double _measuredRefreshPeriodMs = 0.0; // scanout period measured from DXGI_FRAME_STATISTICS
    uint64_t _warpRateFrameId = 0;         // frame the rate-limited warp step belongs to
    float _lastWarpTimeStep = 0.0f;        // last dispatched warp step (rate limiting)
    double _displayClockAnchorMs = 0.0;    // MillisecondsNow()-domain estimate of the last reported vblank
    double _lastStatsQueryMs = 0.0;
    UINT64 _lastStatsSyncRefreshCount = 0;
    LONGLONG _lastStatsSyncQpc = 0;
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
