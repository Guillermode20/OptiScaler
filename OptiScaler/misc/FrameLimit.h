#pragma once
#include "SysUtils.h"

class FrameLimit
{
  public:
    struct SourcePacingStats
    {
        float capHz = 0.0f;
        float timingErrorMs = 0.0f;
    };

    static void sleep(bool fgActive);
    static void sleepForMs(double ms);
    // High-resolution sleep with the larger Proton spin tail required by the
    // latency-critical async presenter.
    static void sleepForPrecisePacingMs(double ms);
    // Source pacing uses a separate short spin tail so the game thread does
    // not compete with the presenter for a full millisecond every frame.
    static void sleepForReprojectionSourceMs(double ms);
    // Called by the virtualized async-reprojection game thread after an anchor
    // has been published. This deliberately never runs on the presenter thread.
    static void paceReprojectionSource(bool active);
    static SourcePacingStats reprojectionSourcePacingStats();

  private:
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns, int64_t busywaitThresholdNs = 2'000'000);
};
