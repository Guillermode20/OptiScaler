#pragma once
#include "SysUtils.h"

class FrameLimit
{
  public:
    static void sleep(bool fgActive);
    static void sleepForMs(double ms);
    // High-resolution sleep with the larger Proton spin tail required by the
    // latency-critical async presenter.
    static void sleepForPrecisePacingMs(double ms);

  private:
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns, int64_t busywaitThresholdNs = 2'000'000);
};
