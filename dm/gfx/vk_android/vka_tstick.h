// From Banana Native 5e4212d1 (MIT), unchanged but for this line: the tick measure, used by vka_exec.c tick_sample (DM_VK_TS_TICK).
// The length of a GPU timestamp tick, measured against the host clock (vka_exec.c tick_sample), free of Vulkan so the unit test (dm/host/tests/t_android_tstick.c)
// can check it. A driver reports the tick as VkPhysicalDeviceLimits::timestampPeriod, and every GPU time the host shows (the pace and [longframe] lines, the frame
// statistics, DM_VK_GPUPROF) and the segment timestamps a game reads are tick counts times that. A driver whose period does not match its counter makes all of them
// wrong by one factor: Turnip T30 on an Adreno 740 reports 33.11 ns for a counter that advances once in 52.08 ns (19.2 MHz), so its GPU times read 36 % low
// (docs/reference/frame-rate.md "GPU times off by one factor"). The host has two clocks for one stretch of time: a submission's last timestamp, in ticks, and the host
// time its completion was processed. Over a window of VKTK_WINDOW_NS the ratio of their differences is the tick's real length; how late a completion is processed
// (a fraction of a millisecond) is a few parts in ten thousand of the window. The median of VKTK_N windows is taken, so one in which the counter stood still (the
// GPU powered down) or a completion came late does not decide. The driver's period is kept while the measure is within VKTK_TOL of it
#ifndef VKA_TSTICK_H
#define VKA_TSTICK_H
#include <stdint.h>

#define VKTK_WINDOW_NS 2000000000ull
enum { VKTK_N = 5 };
#define VKTK_TOL 0.02

typedef struct { uint64_t t0, h0; double w[VKTK_N]; int n; } vktk;

static inline double vktk_median(const vktk *k) {
    double v[VKTK_N]; int n = k->n < VKTK_N ? k->n : VKTK_N;
    if (!n) return 0;
    for (int i = 0; i < n; i++) { double x = k->w[i]; int j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
    return v[n / 2];
}
// One sample: a submission's last timestamp (ticks) and the host time (ns) its completion was processed. Samples may come out of order (several submissions seen
// done at once): one at or before the window's start is left out. A counter that went back long ago (wrapped, reset) or a pause of several windows starts a new
// window. Returns the measured tick in ns once VKTK_N windows are in (and from then on), else 0
static inline double vktk_add(vktk *k, uint64_t ticks, uint64_t host_ns) {
    if (k->n >= VKTK_N) return vktk_median(k);
    if (!k->h0) { k->t0 = ticks; k->h0 = host_ns; return 0; }
    if (host_ns > k->h0 && host_ns - k->h0 > 5 * VKTK_WINDOW_NS) { k->t0 = ticks; k->h0 = host_ns; return 0; }
    if (ticks <= k->t0 || host_ns <= k->h0 || host_ns - k->h0 < VKTK_WINDOW_NS) return 0;
    k->w[k->n++] = (double)(host_ns - k->h0) / (double)(ticks - k->t0);
    k->t0 = ticks; k->h0 = host_ns;
    return k->n >= VKTK_N ? vktk_median(k) : 0;
}
// The tick to use: the driver's, unless the measure is off it by more than VKTK_TOL
static inline double vktk_pick(double driver_ns, double measured_ns) {
    if (!(measured_ns > 0) || !(driver_ns > 0)) return driver_ns;
    double r = measured_ns / driver_ns;
    return r > 1 + VKTK_TOL || r < 1 - VKTK_TOL ? measured_ns : driver_ns;
}
#endif
