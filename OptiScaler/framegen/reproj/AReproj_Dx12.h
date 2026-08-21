#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/reprojection/RP_Dx12.h>
#include <menu/input/input_system.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;

/// Async reprojection (ASW-style) FG output.
/// See AsyncReprojection.md for the full design.
///
/// Owns the game's real DXGI swapchain plus, when enabled, a worker-only
/// DirectComposition swapchain. The synchronous fallback emits bounded warps before
/// returning; the async path publishes owned packets and never writes game backbuffers.
class AReproj_Dx12 : public virtual IFGFeature_Dx12
{
  public:
    struct RuntimeMetrics
    {
        float realFps = 0.0f;
        float warpFps = 0.0f;
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
        bool latePoseEstimated = false;
        bool calibrationReady = false;
        float mouseCalibrationConfidence = 0.0f;
        int mouseCalibrationLagMs = 0;
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

    struct CalibrationBin
    {
        double xx = 0.0, xy = 0.0, yy = 0.0;
        double xYaw = 0.0, yYaw = 0.0, xPitch = 0.0, yPitch = 0.0;
        double yaw2 = 0.0, pitch2 = 0.0;
        uint32_t samples = 0;
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
        double frameDelta = 0.0;
        bool hasDepth = false;
        bool hasCamera = false;
        bool hasUi = false;
        OptiInput::RawMouseMotion sourceMouse {};
        std::atomic<PacketState> state { PacketState::Free };
    };

    std::unique_ptr<RP_Dx12> _warp;                // the reprojection pass (v1/v2 PSOs)
    ID3D12Resource* _lastColor[BUFFER_COUNT] = {}; // copy of the last presented real frame
    D3D12_RESOURCE_STATES _lastColorState[BUFFER_COUNT] = {};
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

    ID3D12CommandQueue* _presentQueue = nullptr;
    IDXGISwapChain3* _presentSwapChain = nullptr;
    IDCompositionDevice* _compositionDevice = nullptr;
    IDCompositionTarget* _compositionTarget = nullptr;
    IDCompositionVisual* _compositionVisual = nullptr;
    HWND _presentHwnd = nullptr; // Proton fallback: worker-owned child HWND swapchain
    bool _compositionAttached = false;
    bool _presenterUsesComposition = false;

    UINT _bufferCount = 0;
    UINT64 _scFenceValue = 0; // monotonic SC fence value (fence outlives context recreate)

    bool CopyLastFrame(int fIndex);                // backbuffer -> _lastColor[fIndex], submitted before present
    bool DispatchWarp(int fIndex, float timeStep); // _lastColor[fIndex] + MV (+depth) -> current backbuffer
    bool CaptureFramePacket(int sourceIndex, int packetIndex);
    bool DispatchPacketWarp(int packetIndex, float timeStep);
    bool DisplayPacket(int packetIndex, bool composeUi);
    bool CopyPacketResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                            D3D12_RESOURCE_STATES sourceState, ID3D12Resource** target,
                            D3D12_RESOURCE_STATES& targetState, const wchar_t* name);
    void FillConstants(int fIndex, RP_Constants& constants);
    void UpdateMouseCalibration(int fIndex);
    void ApplyLateLatch(RP_Constants& constants, const OptiInput::RawMouseMotion& sourceMouse) const;
    int AcquirePacket();
    void RetirePackets();
    uint32_t PacketQueueDepth() const;
    bool CreateAsyncPresenter();
    void DestroyAsyncPresenter();
    bool StartAsyncPresenter();
    void StopAsyncPresenter();
    void PresenterMain();
    bool AttachCompositionVisual();
    bool WaitForPacketDeadline(int packetIndex, double deadlineMs);
    HRESULT PresentCompositorFrame(UINT syncInterval, UINT flags, bool interpolated);
    double TargetRefreshHz();
    uint32_t WarpCountForFrame(double refreshHz) const;
    uint32_t WarpCountForPeriod(double realFrameMs, double refreshHz) const;
    void WaitUntil(double deadlineMs) const;
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

    OptiInput::RawMouseMotion _syncSourceMouse {};
    static constexpr int CALIBRATION_LAG_BINS = 26; // 0..50 ms in 2 ms steps
    CalibrationBin _calibration[CALIBRATION_LAG_BINS] {};
    double _calibrationMatrix[4] = {}; // yawX, yawY, pitchX, pitchY in radians/count
    double _lastCalibrationTimestamp = 0.0;
    float _calibrationConfidence = 0.0f;
    int _calibrationLagMs = 0;
    double _metricsTimestamp = 0.0;
    uint32_t _metricsRealFrames = 0;
    uint32_t _metricsWarpFrames = 0;
    uint32_t _metricsDroppedWarps = 0;
    uint32_t _metricsMaxWarpsPerReal = 0;
    double _metricsPoseAgeTotalMs = 0.0;
    uint32_t _metricsPoseSamples = 0;
    RuntimeMetrics _runtimeMetrics {};
    mutable std::mutex _metricsMutex;
    std::mutex _refreshMutex;
    double _cachedRefreshHz = 0.0;
    double _lastRefreshQueryMs = 0.0;

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
    RuntimeMetrics GetRuntimeMetrics() const;

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
