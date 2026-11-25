// A profile's low-RAM keys (dm/games/README.md low_ram_place_part_mb, low_ram_img_cache_max_mb; from Banana Native v0.3.52, MIT): defaults the Vulkan replay takes only
// on a device of VKLR_MEM_TOTAL_MAX or less, for a profile that sets them. One predicate for both (vka_tex.c vka_low_ram): the profile sets one of the keys and MemTotal is
// at most that. DM_VK_LOW_RAM=0 turns them off and =1 applies them on any device (an A/B; a profile without the keys still gets nothing). They sit beside the texture
// cache's limit (vka_texlimit.h), not in it: the image cache's key is a ceiling on the limit that rule measures, applied after it (vklr_ceiling), so the rule's RAM, budget
// and memory pressure parts still act below it. DM_VK_IMG_LIMIT_MB and DM_VK_PLACE_PART_MB still replace what they replace. Free of Vulkan: dm/host/tests/t_android_lowram.c
// checks it. (Upstream's third key, low_ram_app_pool_mb, a smaller TotalMemorySize, is not ported: no profile here can take it, Ocarina of Time's main sizes SetHeapSize
// from that total and needs all of its 9216 MiB)
#ifndef VKA_LOWRAM_H
#define VKA_LOWRAM_H
#include <stdint.h>
#include "vka_texlimit.h"
#ifndef DM_GAME_LOW_RAM_PLACE_PART_MB   // (a profile header from a game.py before the keys, a test: not set)
#define DM_GAME_LOW_RAM_PLACE_PART_MB 0
#endif
#ifndef DM_GAME_LOW_RAM_IMG_CACHE_MAX_MB
#define DM_GAME_LOW_RAM_IMG_CACHE_MAX_MB 0
#endif

// The gate: 12 GiB, as upstream (the class of vktl_reserve's eighth; a 12 GB phone reports about 11 GiB). A 16 GiB gate was tried here (2026-11-24) and lost:
// Ocarina of Time on a 16 GB POCO F7 Ultra (891.14, RAM saving on) in Castle Town ran at about 12 fps with the 1280 MiB image ceiling, the cache evicting and making again
// textures still in use (1651 prunes; the scene holds 2.2 GiB of images, 1.5 GiB GPU-written, which are never evicted) and the replay threads busy about 170 ms a frame, against
// about 14.5 fps GPU-bound and no prunes with DM_VK_LOW_RAM=0. The swap thrash that 16 GB phone had before (0.1-0.5 fps) is the general rule's to catch (vka_texlimit.h
// vktl_fault_psi, the process's major faults where PSI cannot be read), not this ceiling's
#define VKLR_MEM_TOTAL_MAX (12 * 1024 * VKTL_MIB)

// Whether the profile's low-RAM keys apply: has = the profile sets one of them; mem_total = MemTotal in bytes (0 = not known: they do not apply);
// force = DM_VK_LOW_RAM (-1 unset, 0 never, 1 on any device)
static inline int vklr_applies(int has, uint64_t mem_total, int force) {
    if (!has || !force) return 0;
    if (force > 0) return 1;
    return mem_total && mem_total <= VKLR_MEM_TOTAL_MAX;
}
// The image cache's limit under a ceiling (0 = none): the smaller of the two
static inline uint64_t vklr_ceiling(uint64_t limit, uint64_t ceiling) { return ceiling && limit > ceiling ? ceiling : limit; }

int vka_low_ram(void);                 // the predicate, decided once (vka_tex.c)
int vka_low_ram_place_part_mb(void);   // the profile's low_ram_place_part_mb where the keys apply, else 0
#endif
