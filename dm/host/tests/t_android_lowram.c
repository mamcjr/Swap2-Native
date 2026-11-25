// A profile's low-RAM keys (dm/gfx/vk_android/vka_lowram.h; from Banana Native v0.3.52): they apply only for a profile that sets them and a MemTotal of 12 GiB or less,
// not when MemTotal is unknown; DM_VK_LOW_RAM=0 turns them off and =1 applies them on any device, never for a profile without them; the image cache's ceiling only lowers
// the measured limit. And the texture cache's stand-in for memory pressure where PSI cannot be read (vka_texlimit.h vktl_fault_psi), at the fault rates measured on a
// 16 GB phone
#include <stdio.h>
#include "../../gfx/vk_android/vka_lowram.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define MIB VKTL_MIB
#define GIB (1024 * MIB)

static double run_faults(double rate, int secs) { double a = 0; for (int i = 0; i < secs; i++) a = vktl_fault_psi(a, (uint64_t)rate, 1.0); return a; }

int main(void) {
    CHECK(VKLR_MEM_TOTAL_MAX == 12 * GIB, "threshold %llu", (unsigned long long)VKLR_MEM_TOTAL_MAX);
    // Unset DM_VK_LOW_RAM: by MemTotal
    CHECK(vklr_applies(1, 11265 * MIB, -1), "an 11 GiB device (AYN Thor)");
    CHECK(vklr_applies(1, 12 * GIB, -1), "exactly 12 GiB");
    CHECK(!vklr_applies(1, 12 * GIB + 4096, -1), "just over 12 GiB");
    CHECK(!vklr_applies(1, 15114 * MIB, -1), "a 16 GB phone (POCO F7 Ultra: the 1280 MiB ceiling made Castle Town slower there)");
    CHECK(!vklr_applies(1, 22 * GIB, -1), "a 24 GB phone");
    CHECK(!vklr_applies(1, 0, -1), "MemTotal not known");
    CHECK(!vklr_applies(0, 8 * GIB, -1), "a profile without the keys");
    // DM_VK_LOW_RAM
    CHECK(!vklr_applies(1, 8 * GIB, 0), "=0 on a small device");
    CHECK(vklr_applies(1, 22 * GIB, 1), "=1 on a 24 GB device");
    CHECK(vklr_applies(1, 0, 1), "=1 with MemTotal not known");
    CHECK(!vklr_applies(0, 8 * GIB, 1), "=1 for a profile without the keys");
    // The ceiling
    CHECK(vklr_ceiling(4300 * MIB, 1280 * MIB) == 1280 * MIB, "a measured limit above the ceiling");
    CHECK(vklr_ceiling(700 * MIB, 1280 * MIB) == 700 * MIB, "a measured limit below it (memory pressure) stays");
    CHECK(vklr_ceiling(4300 * MIB, 0) == 4300 * MIB, "no ceiling");
    // The measured rule on that phone's thrash (images 2.3 GiB, MemAvailable 834 MiB): the limit is about the images, the ceiling takes it to 1280 MiB
    vktl_in in = {.mem_total = 15114 * MIB, .mem_avail = 834 * MIB, .img_bytes = 2300 * MIB};
    CHECK(vktl_limit(&in) > in.img_bytes && vklr_ceiling(vktl_limit(&in), 1280 * MIB) == 1280 * MIB, "the RAM rule follows the images (%llu MiB); the ceiling holds", (unsigned long long)(vktl_limit(&in) >> 20));
    // Major faults as memory pressure: a run that plays (400 a second) stays under the hold mark, a few seconds of a load's burst (5,000) too; the thrash (13,000 sustained)
    // passes the shrink mark, and the RAM rule then gives an eighth back
    CHECK(run_faults(400, 60) < VKTL_PSI_HOLD, "400 faults a second: %.1f %%", run_faults(400, 60));
    { double a = run_faults(300, 30); for (int i = 0; i < 3; i++) a = vktl_fault_psi(a, 5000, 1.0); CHECK(a < VKTL_PSI_HOLD, "a 3 s burst of 5,000 faults a second: %.1f %%", a); }
    CHECK(run_faults(13000, 30) >= VKTL_PSI_SHRINK, "13,000 faults a second: %.1f %%", run_faults(13000, 30));
    CHECK(run_faults(1e7, 5) <= 100.0, "capped at 100 %%");
    CHECK(vktl_fault_psi(7.0, 100, 0) == 7.0, "no time passed: unchanged");
    in.psi_some10 = run_faults(13000, 30);
    CHECK(vktl_limit(&in) == in.img_bytes - in.img_bytes / 8, "under the thrash the limit gives an eighth back (%llu MiB)", (unsigned long long)(vktl_limit(&in) >> 20));
    if (!fails) printf("t_android_lowram: ok\n");
    return fails != 0;
}
