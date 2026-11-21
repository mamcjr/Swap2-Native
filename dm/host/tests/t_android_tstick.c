// From Banana Native 5e4212d1 (MIT), unchanged but for this line.
// The GPU timestamp tick measured against the host clock (dm/gfx/vk_android/vka_tstick.h): a driver whose timestampPeriod does not match its counter is found and
// the measured tick used; a driver whose period is right keeps it exactly; a window in which the counter stood still, samples out of order, a late completion and a
// counter that went back do not change the result
#include <stdio.h>
#include <math.h>
#include "../../gfx/vk_android/vka_tstick.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A frame every 16.7 ms on a counter of `hz`; each completion is processed `late` ns after the GPU's end, plus a jitter of up to 1 ms; from `pause_at` s the counter stands
// still for `pause` s (the GPU powered down) while the host clock goes on. Returns the measured tick (0: not in after `secs` s)
static double run(double hz, uint64_t late, double pause_at, double pause, int secs, int shuffle) {
    vktk k = {0}; double m = 0; uint64_t host = 1000000000000ull, stopped = 0; unsigned seed = 12345;
    for (int f = 0; f < secs * 60; f++) {
        host += 16700000;
        double t = f / 60.0;
        if (pause > 0 && t >= pause_at && t < pause_at + pause) { stopped += 16700000; continue; }   // no submission while the GPU is off
        seed = seed * 1103515245u + 12345u; uint64_t jit = (seed >> 8) % 1000000;
        uint64_t gpu_ns = host - 1000000000000ull - stopped, ticks = (uint64_t)(gpu_ns * hz / 1e9) + 777;
        if (shuffle && f % 3 == 2) { double x = vktk_add(&k, ticks - (uint64_t)(0.0167 * hz), host - 16700000 + late + jit); if (x > 0) m = x; }   // the frame before, seen done after this one
        double x = vktk_add(&k, ticks, host + late + jit); if (x > 0) m = x;
    }
    return m;
}

int main(void) {
    const double real = 1e9 / 19.2e6;   // 52.083 ns: an Adreno's always-on counter
    // Turnip T30 on an Adreno 740: the driver says 33.11 ns
    double m = run(19.2e6, 200000, 0, 0, 20, 0);
    CHECK(m > 0 && fabs(m / real - 1) < 0.001, "measured %.3f ns a tick against %.3f", m, real);
    CHECK(vktk_pick(33.11, m) == m, "a driver's period 36 %% off: the measured tick is used");
    CHECK(fabs(m / 33.11 - 1.573) < 0.003, "the factor its GPU times were low by: %.3f", m / 33.11);
    // a driver whose period is right keeps it to the digit
    CHECK(vktk_pick(52.08, m) == 52.08, "within the tolerance: the driver's period (%.4f measured)", m);
    CHECK(vktk_pick(52.08, 52.08 * 1.019) == 52.08 && vktk_pick(52.08, 52.08 * 1.021) != 52.08 && vktk_pick(52.08, 52.08 * 0.979) != 52.08, "the tolerance's edges");
    CHECK(vktk_pick(52.08, 0) == 52.08 && vktk_pick(0, 50) == 0, "nothing measured, or no period: the driver's");
    // not before VKTK_N windows are in
    CHECK(run(19.2e6, 200000, 0, 0, 9, 0) == 0 && run(19.2e6, 200000, 0, 0, 11, 0) > 0, "five windows of two seconds");
    // a completion processed 5 ms late every time: the same at both ends of a window
    m = run(19.2e6, 5000000, 0, 0, 20, 0);
    CHECK(fabs(m / real - 1) < 0.001, "late completions: %.3f", m);
    // the counter stands still for 1.5 s in one window (that window reads long): the median holds
    m = run(19.2e6, 200000, 3.0, 1.5, 30, 0);
    CHECK(fabs(m / real - 1) < 0.001, "one window with the counter stopped: %.3f", m);
    // samples out of order
    m = run(19.2e6, 200000, 0, 0, 20, 1);
    CHECK(fabs(m / real - 1) < 0.001, "samples out of order: %.3f", m);
    // a 1 ns counter (a driver that reports nanoseconds)
    m = run(1e9, 200000, 0, 0, 20, 0);
    CHECK(fabs(m - 1) < 0.001 && vktk_pick(1.0, m) == 1.0, "a nanosecond counter: %.4f", m);
    // a counter that went back (a reset): the window starts again once it is long past, and the measure still comes
    { vktk k = {0}; double x = 0; uint64_t host = 5000000000ull;
      for (int i = 0; i < 110; i++) { host += 100000000; x = vktk_add(&k, 1000000000ull + (uint64_t)i * 1920000, host); }   // 11 s: all five windows
      CHECK(k.n == VKTK_N && fabs(x / real - 1) < 0.001, "five windows in 11 s: %d, %.3f", k.n, x); x = 0;
      vktk j = {0}; host = 5000000000ull;
      for (int i = 0; i < 30; i++) { host += 100000000; vktk_add(&j, 1000000000ull + (uint64_t)i * 1920000, host); }     // 3 s: one window
      for (int i = 0; i < 300; i++) { host += 100000000; double y = vktk_add(&j, 5000 + (uint64_t)i * 1920000, host); if (y > 0) x = y; }   // the counter restarted from 5000
      CHECK(x > 0 && fabs(x / real - 1) < 0.001, "after a counter reset: %.3f", x); }
    if (!fails) printf("t_android_tstick: ok\n");
    return fails != 0;
}
