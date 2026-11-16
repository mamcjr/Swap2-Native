// nxhost_vk_warm: the game's pipeline cache filled with the game closed (vk_android.h). The launcher runs it in a process of its own (android WarmActivity, process
// ":warm") with the game's environment (the same DM_TEMP, driver and switches, so the same cache file and the same pipelines), after Prepare, after a GPU driver change,
// or when asked. It reads the pack's profile (as nxhost_main does), takes the pack folder as the module folder (where the shader library is: vka_shader.c lib_load), brings
// the Vulkan device up and runs the prewarm list through vka_pso_warm (vka_pso.c "Warming with the game closed"). No guest memory, no game code
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include <stdio.h>

__attribute__((visibility("default"))) int nxhost_vk_warm(const char *pack, unsigned *out, int n) {
    unsigned r[4] = {0, 0, 0, 0};
    if (!pack || !*pack) { VRED("pipeline warm: no pack folder"); return 2; }
#ifdef DM_GAME_RUNTIME
    { char p[4096]; snprintf(p, sizeof p, "%s/game_profile.tsv", pack); if (dm_game_load(p)) return 2; }
#endif
    dm_module_dir_set(pack);
    if (!vka_init()) { VRED("pipeline warm: no Vulkan device"); return 1; }
    int rc = vka_pso_warm(r) ? 3 : 0;
    for (int i = 0; out && i < n && i < 4; i++) out[i] = r[i];
    return rc;
}

__attribute__((visibility("default"))) void nxhost_vk_warm_progress(unsigned *done, unsigned *total) {
    unsigned p[2]; vka_pso_warm_progress(p);
    if (done) *done = p[0];
    if (total) *total = p[1];
}
__attribute__((visibility("default"))) void nxhost_vk_warm_stop(void) { vka_pso_warm_stop(); }
#endif
