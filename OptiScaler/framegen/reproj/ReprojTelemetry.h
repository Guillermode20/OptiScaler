#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <array>
#include <limits>

// Reprojection telemetry — see telemetry_plan.md.
// QPC is the canonical CPU time domain. No allocation per slot, no per-slot logging.

struct ReprojClock
{
    int64_t qpcFrequency = 0;

    int64_t NowQpc() const
    {
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    double QpcToMs(int64_t ticks) const
    {
        if (qpcFrequency <= 0)
            return 0.0;
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(qpcFrequency);
    }

    double DeltaMs(int64_t begin, int64_t end) const
    {
        if (qpcFrequency <= 0)
            return std::numeric_limits<double>::quiet_NaN();
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(qpcFrequency);
    }

    int64_t MsToQpc(double ms) const
    {
        if (qpcFrequency <= 0)
            return 0;
        return static_cast<int64_t>(ms * static_cast<double>(qpcFrequency) / 1000.0);
    }
};

enum class ReprojEffectiveMode : uint8_t
{
    Unwarped = 0,
    MotionVector = 1,
    DepthCamera = 2,
    RotationOnly = 3,
};

enum class ReprojTimestampOrigin : uint8_t
{
    None = 0,
    CameraCallback = 1,
    PacketCapture = 2,
    FrameIntervalFallback = 3,
};

enum class ReprojSlotOutcome : uint8_t
{
    Pending = 0,
    NoAnchor = 1,
    Presented = 2,
    SoftwareSkipped = 3,
    WaitableTimeout = 4,
    DispatchFailed = 5,
    FenceFailed = 6,
    PresentFailed = 7,
    PresenterStopped = 8,
};

enum class ReprojMissCause : uint8_t
{
    None = 0,
    CpuWakeLate = 1,
    WaitableLate = 2,
    CaptureNotReady = 3,
    PresentQueueBacklog = 4,
    WarpGpuSlow = 5,
    PresentSlip = 6,
    ClockCorrection = 7,
    Unknown = 8,
    // A presenter loop reached a scheduled slot after its deadline without a
    // specific waitable/GPU failure. This is distinct from a display-clock
    // correction so telemetry does not turn ordinary scheduling misses into
    // clock faults.
    SoftwareScheduleSkip = 9,
};

// Secondary cause flags (bitmask, fits in uint32_t)
enum ReprojSecondaryCause : uint32_t
{
    Secondary_None = 0,
    Secondary_CpuWakeLate = 1u << 0,
    Secondary_WaitableLate = 1u << 1,
    Secondary_CaptureNotReady = 1u << 2,
    Secondary_QueueBacklog = 1u << 3,
    Secondary_WarpGpuSlow = 1u << 4,
    Secondary_PresentSlip = 1u << 5,
    Secondary_ClockCorrection = 1u << 6,
};

struct ReprojSlotRecord
{
    uint64_t sequence = 0;
    bool occupied = false; // true when sequence is valid (distinguishes stale ring data)

    // Identity
    uint64_t anchorFrameId = 0;
    uint32_t outputIndex = UINT32_MAX;
    uint32_t representedSlots = 1;
    uint32_t skippedSlotsBeforeAttempt = 0;

    // Timing (QPC)
    int64_t loopBeginQpc = 0;
    int64_t softwareDeadlineQpc = 0;
    int64_t wakeTargetQpc = 0;
    int64_t wakeCompletedQpc = 0;
    int64_t waitableBeginQpc = 0;
    int64_t waitableEndQpc = 0;
    int64_t packetSelectionQpc = 0;
    int64_t commandRecordingBeginQpc = 0;
    int64_t commandRecordingEndQpc = 0;
    int64_t queueSubmitQpc = 0;
    int64_t lateLatchSignalQpc = 0;
    int64_t presentBeginQpc = 0;
    int64_t presentEndQpc = 0;
    int64_t latestDxgiSyncQpc = 0;
    uint64_t dxgiPresentCount = 0;
    uint64_t dxgiRefreshCount = 0;

    // GPU timing
    uint64_t gpuStartTimestamp = 0;
    uint64_t gpuEndTimestamp = 0;
    int64_t calibratedGpuStartQpc = 0;
    int64_t calibratedGpuEndQpc = 0;
    uint64_t scFenceValue = 0;
    uint32_t gpuQueryIndex = UINT32_MAX;
    bool gpuValid = false;

    // Display clock snapshot
    double targetRefreshHz = 0.0;
    double configuredPeriodMs = 0.0;
    double measuredPeriodMs = 0.0;
    double dispatchLeadMs = 0.0;
    double displayClockAnchorMs = 0.0;
    double totalEarlyCorrectionMs = 0.0;
    bool dxgiStatsValid = false;

    // Packet / prediction
    double packetRenderTimestampMs = 0.0;
    double sourcePoseTimestampMs = 0.0;
    float rawCaptureIntervalMs = std::numeric_limits<float>::quiet_NaN();
    float selectedFrameIntervalMs = std::numeric_limits<float>::quiet_NaN();
    float sourceProvidedFrameIntervalMs = std::numeric_limits<float>::quiet_NaN();
    float poseIntervalMs = std::numeric_limits<float>::quiet_NaN();
    float refreshPeriodMs = std::numeric_limits<float>::quiet_NaN();
    float anchorAgeMs = std::numeric_limits<float>::quiet_NaN();
    float unclampedTimeStep = std::numeric_limits<float>::quiet_NaN();
    float finalTimeStep = std::numeric_limits<float>::quiet_NaN();
    float maxTimeStep = std::numeric_limits<float>::quiet_NaN();
    float mvScaleX = std::numeric_limits<float>::quiet_NaN();
    float mvScaleY = std::numeric_limits<float>::quiet_NaN();
    float jitterX = std::numeric_limits<float>::quiet_NaN();
    float jitterY = std::numeric_limits<float>::quiet_NaN();
    float cameraNear = std::numeric_limits<float>::quiet_NaN();
    float cameraFar = std::numeric_limits<float>::quiet_NaN();
    float cameraVFov = std::numeric_limits<float>::quiet_NaN();
    float cameraAspect = std::numeric_limits<float>::quiet_NaN();
    ReprojEffectiveMode requestedMode = ReprojEffectiveMode::MotionVector;
    ReprojEffectiveMode effectiveMode = ReprojEffectiveMode::Unwarped;
    ReprojTimestampOrigin timestampOrigin = ReprojTimestampOrigin::None;
    bool newAnchor = false;
    bool repeatedAnchor = false;
    bool timestepClamped = false;
    bool velocityAvailable = false;
    bool depthAvailable = false;
    bool cameraBasisAvailable = false;
    bool cameraProjectionValid = false;
    bool depthConstantsValid = false;
    bool captureFenceReadyAtSelection = false;
    bool hudlessSource = false;
    bool displayClockCorrectionApplied = false;
    // Input-predicted timewarp: what the estimator applied for this slot.
    bool inputPredicted = false;
    float predictedYawRad = 0.0f;
    float predictedPitchRad = 0.0f;
    uint8_t contentKind = 0;
    uint8_t posePath = 0;
    float contentFraction = 1.0f;
    float contentAgeMs = std::numeric_limits<float>::quiet_NaN();
    double poseSampleTimestampMs = 0.0;
    double targetScanoutTimestampMs = 0.0;
    float residualPredictionIntervalMs = std::numeric_limits<float>::quiet_NaN();
    float yawConfidence = 0.0f;
    float pitchConfidence = 0.0f;
    float yawErrorDegrees = std::numeric_limits<float>::quiet_NaN();
    float pitchErrorDegrees = std::numeric_limits<float>::quiet_NaN();
    float fgDurationMs = std::numeric_limits<float>::quiet_NaN();

    // Result
    HRESULT waitableResult = S_OK;
    HRESULT presentResult = S_OK;
    ReprojSlotOutcome outcome = ReprojSlotOutcome::Pending;
    ReprojMissCause primaryMissCause = ReprojMissCause::None;
    uint32_t secondaryCauseFlags = Secondary_None;
    float wakeLatenessMs = std::numeric_limits<float>::quiet_NaN();
    float waitableDurationMs = std::numeric_limits<float>::quiet_NaN();
    float commandRecordingMs = std::numeric_limits<float>::quiet_NaN();
    float gpuQueueDelayMs = std::numeric_limits<float>::quiet_NaN();
    float lateLatchToGpuStartMs = std::numeric_limits<float>::quiet_NaN();
    bool lateLatchGameQueue = false;
    float lateLatchArrivalWaitMs = std::numeric_limits<float>::quiet_NaN();
    bool lateInputApplied = false;
    int64_t lateInputDeltaX = 0;
    int64_t lateInputDeltaY = 0;
    float lateInputYawRad = 0.0f;
    float lateInputPitchRad = 0.0f;
    float cameraLatencyEstimateMs = std::numeric_limits<float>::quiet_NaN();
    float gpuDurationMs = std::numeric_limits<float>::quiet_NaN();
    float gpuEndLatenessMs = std::numeric_limits<float>::quiet_NaN();
    float presentBlockMs = std::numeric_limits<float>::quiet_NaN();
    float presentIntervalMs = std::numeric_limits<float>::quiet_NaN();

    // Derived secondary diagnostics
    bool gpuFinishedBeforePresentCall = false;
    bool gpuFinishedBeforeEstimatedScanout = false;
};

// Aggregated snapshot published once per second for menu/logging.
struct ReprojTelemetrySnapshot
{
    uint64_t windowSequenceStart = 0;
    uint64_t windowSequenceEnd = 0;
    double windowDurationMs = 0.0;

    // Cadence
    uint32_t scheduledSlots = 0;
    uint32_t presented = 0;
    uint32_t classifiedMisses = 0;
    uint32_t legacyMisses = 0; // old _metricsMissedDisplaySlots for comparison
    uint32_t skippedRepresentedSlots = 0;
    uint32_t newAnchorOutputs = 0;
    uint32_t repeatedAnchorOutputs = 0;
    uint32_t slippedPresents = 0; // successful presents spanning >1.5 refresh periods
    double displayFps = 0.0;
    float presentIntervalP50 = std::numeric_limits<float>::quiet_NaN();
    float presentIntervalP95 = std::numeric_limits<float>::quiet_NaN();
    float presentIntervalP99 = std::numeric_limits<float>::quiet_NaN();
    float presentIntervalMax = std::numeric_limits<float>::quiet_NaN();

    // CPU
    float wakeP50 = std::numeric_limits<float>::quiet_NaN();
    float wakeP95 = std::numeric_limits<float>::quiet_NaN();
    float wakeP99 = std::numeric_limits<float>::quiet_NaN();
    float wakeMax = std::numeric_limits<float>::quiet_NaN();
    float waitP50 = std::numeric_limits<float>::quiet_NaN();
    float waitP95 = std::numeric_limits<float>::quiet_NaN();
    float waitP99 = std::numeric_limits<float>::quiet_NaN();
    float cmdP50 = std::numeric_limits<float>::quiet_NaN();
    float cmdP95 = std::numeric_limits<float>::quiet_NaN();
    uint32_t lateWakes = 0;

    // GPU
    float queueP50 = std::numeric_limits<float>::quiet_NaN();
    float queueP95 = std::numeric_limits<float>::quiet_NaN();
    float queueP99 = std::numeric_limits<float>::quiet_NaN();
    float queueMax = std::numeric_limits<float>::quiet_NaN();
    float gpuP50 = std::numeric_limits<float>::quiet_NaN();
    float gpuP95 = std::numeric_limits<float>::quiet_NaN();
    float gpuP99 = std::numeric_limits<float>::quiet_NaN();
    float gpuMax = std::numeric_limits<float>::quiet_NaN();
    float gpuMarginP50 = std::numeric_limits<float>::quiet_NaN(); // gpuEnd - deadline
    uint32_t gpuQuerySkipped = 0;
    uint32_t calibrationFailures = 0;
    bool calibrationValid = false;

    // DXGI
    float presentBlockP50 = std::numeric_limits<float>::quiet_NaN();
    float presentBlockP95 = std::numeric_limits<float>::quiet_NaN();
    float presentBlockP99 = std::numeric_limits<float>::quiet_NaN();
    float presentBlockMax = std::numeric_limits<float>::quiet_NaN();
    double frameStatisticsPeriodMs = 0.0;
    double configuredPeriodMs = 0.0;
    double measuredPeriodMs = 0.0;
    uint32_t dxgiRejectedSamples = 0;

    // Prediction
    uint32_t modeMv = 0;
    uint32_t modeDepth = 0;
    uint32_t modeRotation = 0;
    uint32_t modeUnwarped = 0;
    float sourceRawP50 = std::numeric_limits<float>::quiet_NaN();
    float sourceRawP95 = std::numeric_limits<float>::quiet_NaN();
    float sourceSelectedP50 = std::numeric_limits<float>::quiet_NaN();
    float sourceSelectedP95 = std::numeric_limits<float>::quiet_NaN();
    float sourceRatioP50 = std::numeric_limits<float>::quiet_NaN();
    float sourceRatioP95 = std::numeric_limits<float>::quiet_NaN();
    float sourceCapRequestedHz = 0.0f;
    bool sourceCapActive = false;
    float poseIntervalP50 = std::numeric_limits<float>::quiet_NaN();
    float poseIntervalP95 = std::numeric_limits<float>::quiet_NaN();
    float sourceCapHz = 0.0f;
    float sourceCapTimingErrorMs = std::numeric_limits<float>::quiet_NaN();
    float anchorAgeP50 = std::numeric_limits<float>::quiet_NaN();
    float anchorAgeP95 = std::numeric_limits<float>::quiet_NaN();
    float anchorAgeMax = std::numeric_limits<float>::quiet_NaN();
    float unclampedP50 = std::numeric_limits<float>::quiet_NaN();
    float unclampedP95 = std::numeric_limits<float>::quiet_NaN();
    float unclampedMax = std::numeric_limits<float>::quiet_NaN();
    float finalP50 = std::numeric_limits<float>::quiet_NaN();
    float finalP95 = std::numeric_limits<float>::quiet_NaN();
    float finalMax = std::numeric_limits<float>::quiet_NaN();
    uint32_t clampCount = 0;
    float lateLatchToGpuStartP95 = std::numeric_limits<float>::quiet_NaN();
    uint32_t lateLatchGameQueue = 0;
    float lateLatchArrivalWaitP95 = std::numeric_limits<float>::quiet_NaN();
    uint32_t lateInputApplied = 0;
    uint32_t lateInputNonzero = 0;
    float lateInputDeltaP95 = std::numeric_limits<float>::quiet_NaN();
    float lateInputRotationDegreesP95 = std::numeric_limits<float>::quiet_NaN();
    float targetCoverage = 0.0f;
    float targetErrorP95Degrees = std::numeric_limits<float>::quiet_NaN();
    uint32_t contentReal = 0;
    uint32_t contentGenerated = 0;

    // Shadow calculations (current vs alternative)
    float shadowRawDeltaP50 = std::numeric_limits<float>::quiet_NaN(); // current - rawIntervalStep
    float shadowSourceDeltaP50 = std::numeric_limits<float>::quiet_NaN();

    uint32_t cameraBasisAvailable = 0;
    uint32_t depthAvailable = 0;
    uint32_t depthConstantsValid = 0;
    uint32_t hudlessSource = 0;

    // Cause totals
    uint32_t causeCpu = 0;
    uint32_t causeWaitable = 0;
    uint32_t causeCapture = 0;
    uint32_t causeQueue = 0;
    uint32_t causeGpu = 0;
    uint32_t causePresent = 0;
    uint32_t causeClock = 0;
    uint32_t causeSchedule = 0;
    uint32_t causeUnknown = 0;

    bool targetResolverEnabled = false;
    uint32_t targetActiveSamples = 0;

    bool valid = false;
};

class ReprojTelemetry
{
  public:
    static constexpr uint32_t TRACE_SLOT_COUNT = 512;
    static constexpr uint32_t GPU_QUERY_COUNT = TRACE_SLOT_COUNT * 2;

    ReprojTelemetry();
    ~ReprojTelemetry() = default;

    void Initialize(ID3D12CommandQueue* presentQueue);
    void Shutdown();

    // Clock helpers
    int64_t NowQpc() const { return _clock.NowQpc(); }
    double DeltaMs(int64_t begin, int64_t end) const { return _clock.DeltaMs(begin, end); }
    double QpcToMs(int64_t ticks) const { return _clock.QpcToMs(ticks); }

    // Slot management — presenter thread is sole writer.
    ReprojSlotRecord* BeginSlot();
    void FinalizeSlot(ReprojSlotRecord* slot);
    ReprojSlotRecord* GetSlot(uint64_t sequence);
    const ReprojSlotRecord* GetSlot(uint64_t sequence) const;

    // GPU calibration
    void TryCalibrate(); // once per second, non-blocking
    bool CalibrateGpuTimestamp(uint64_t gpuTimestamp, int64_t& outCpuQpc) const;
    void SetPresentQueue(ID3D12CommandQueue* queue) { _presentQueue = queue; }

    // GPU query reservation (trace-sequence-indexed)
    // Returns query start index or UINT32_MAX if telemetry should be skipped for this slot.
    uint32_t ReserveGpuQueries(uint64_t sequence, uint64_t fenceValue);
    void OnGpuWorkSubmitted(uint64_t sequence, uint64_t fenceValue, uint32_t queryStart, int64_t submitQpc);
    void PollCompletedGpuWork(); // check SC fence, map readback — caller ensures fence valid
    // Latest completed submit-to-GPU-start delay. The presenter reads this
    // summary without a fence wait or readback of its own.
    float RecentGpuQueueDelayMs() const { return _recentGpuQueueDelayMs.load(std::memory_order_relaxed); }
    float RecentGpuDurationMs() const { return _recentGpuDurationMs.load(std::memory_order_relaxed); }
    void SetTimestampResources(ID3D12QueryHeap* heap, ID3D12Resource* readback, ID3D12Fence* scFence,
                               uint64_t timestampFrequency);

    // Aggregation
    bool ShouldPublish(int64_t nowQpc) const;
    ReprojTelemetrySnapshot Publish(int64_t nowQpc, uint32_t legacyMissed);
    ReprojTelemetrySnapshot GetSnapshot() const;

    // Classification
    void ClassifySlot(ReprojSlotRecord& slot, double refreshPeriodMs);

    // Logging
    void LogSnapshot(const ReprojTelemetrySnapshot& snap);
    bool ShouldDumpMiss(const ReprojTelemetrySnapshot& snap) const;
    void DumpMissWindow(uint64_t triggerSequence);

    // Overlay helpers
    void FillOverlayText(char* buffer, size_t size) const;

  private:
    ReprojClock _clock {};
    ID3D12CommandQueue* _presentQueue = nullptr;
    ID3D12QueryHeap* _timestampHeap = nullptr;
    ID3D12Resource* _timestampReadback = nullptr;
    ID3D12Fence* _scFence = nullptr;
    uint64_t _timestampFrequency = 0;

    // Calibration state
    uint64_t _calibratedGpuTimestamp = 0;
    int64_t _calibratedCpuQpc = 0;
    uint64_t _calibrationGeneration = 0;
    int64_t _calibrationQpc = 0;
    bool _calibrationValid = false;
    int64_t _lastCalibrationAttemptQpc = 0;
    uint32_t _calibrationFailures = 0;

    // Ring
    std::array<ReprojSlotRecord, TRACE_SLOT_COUNT> _slots {};
    std::atomic<uint64_t> _nextSequence { 0 };

    // GPU tracking per trace index
    struct GpuSlot
    {
        uint64_t sequence = UINT64_MAX;
        uint64_t fenceValue = 0;
        uint32_t queryStart = UINT32_MAX;
        int64_t submitQpc = 0;
        uint64_t generation = 0;
        bool pending = false;
    };
    std::array<GpuSlot, TRACE_SLOT_COUNT> _gpuSlots {};
    uint32_t _gpuQuerySkipped = 0;
    std::atomic<float> _recentGpuQueueDelayMs { 0.0f };
    std::atomic<float> _recentGpuDurationMs { 2.0f };

    // Aggregation window
    mutable std::mutex _snapshotMutex;
    ReprojTelemetrySnapshot _snapshot {};
    int64_t _windowStartQpc = 0;
    int64_t _lastPublishQpc = 0;
    uint64_t _windowStartSequence = 0;
    uint64_t _lastDumpQpc = 0;

    // Helpers
    static float Percentile(std::array<float, TRACE_SLOT_COUNT>& values, size_t count, double p);
    static float SafeDeltaMs(const ReprojClock& clock, int64_t a, int64_t b);
    void ResetWindow(int64_t nowQpc);
};
