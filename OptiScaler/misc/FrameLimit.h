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

    // Opt-in source cadence cap for async timewarp (60->120 A/B tests). Paces
    // the calling (game present) thread onto an absolute deadline grid while
    // [AsyncTimewarp] SourceFramerateLimit > 0; 0 = uncapped and resets the
    // grid without sleeping. Separate from sleep(): only the caller that
    // publishes virtualized anchors to the async presenter invokes it, and it
    // never falls back to the generic FramerateLimit.
    static void paceReprojectionSource(bool active);

  private:
    static uint64_t get_timestamp();
    static int timer_sleep(int64_t hundred_ns);
    static int busywait_sleep(int64_t ns);
    static int combined_sleep(int64_t ns, int64_t busywaitThresholdNs = 2'000'000);
};
