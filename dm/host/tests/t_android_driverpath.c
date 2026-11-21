// From Banana Native v0.3.43 (af1ed596, community PR 409; MIT), unchanged but for this line.
// Whether a custom GPU driver is what runs (dm/gfx/vk_android/vka_driverpath.h; vka_driver.c vka_driver_mapped, vka_dev.c's [RED] line): the lines of /proc/self/maps
// that map the custom driver's library, and the ones that must not count (the system's driver of the same file name, another driver folder, a name that only ends alike)
#include <stdio.h>
#include <string.h>
#include "../../gfx/vk_android/vka_driverpath.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int maps(const char *line, const char *folder, const char *name) { return vkdp_line_maps(line, strlen(line), folder, name); }

int main(void) {
    char f[64];
    vkdp_last("/data/user/0/dev.bananative.android/files/gpu_drivers/Qualcomm_Adreno_842.8/", f, sizeof f);
    CHECK(!strcmp(f, "Qualcomm_Adreno_842.8"), "the folder's last component, trailing slash ignored: %s", f);
    vkdp_last("/data/x/v891.7", f, sizeof f); CHECK(!strcmp(f, "v891.7"), "without a trailing slash: %s", f);
    vkdp_last("", f, sizeof f); CHECK(!f[0], "nothing from nothing");
    vkdp_last("/a/abcdefghij/", f, 4); CHECK(!strcmp(f, "abc"), "cut to the buffer: %s", f);

    const char *fo = "Qualcomm_Adreno_842.8", *nm = "vulkan.adreno.so";
    // The custom driver mapped: the kernel writes the resolved path (/data/data, not the launcher's /data/user/0)
    CHECK(maps("7a1b200000-7a1b5c0000 r-xp 00000000 fe:31 123456                     /data/data/dev.bananative.android/files/gpu_drivers/Qualcomm_Adreno_842.8/vulkan.adreno.so", fo, nm),
          "the custom library's code map counts");
    CHECK(maps("7a1b200000-7a1b5c0000 r--p 00000000 fe:31 123456 /data/data/dev.bananative.android/files/gpu_drivers/Qualcomm_Adreno_842.8/vulkan.adreno.so (deleted)", fo, nm),
          "a replaced file still counts");
    // The fallback: the system's driver of the same name, from the vendor partition (AYN Thor Max, Android 13: the 12/27/23 build ran)
    CHECK(!maps("7b00000000-7b003c0000 r-xp 00000000 fd:05 2211 /vendor/lib64/hw/vulkan.adreno.so", fo, nm), "the system's vulkan.adreno.so does not count");
    CHECK(!maps("7b00000000-7b003c0000 r-xp 00000000 fe:31 99 /data/data/dev.bananative.android/files/gpu_drivers/v891.7/vulkan.adreno.so", fo, nm),
          "another installed driver's folder does not count");
    CHECK(!maps("7b00000000-7b003c0000 r-xp 00000000 fe:31 99 /data/data/x/gpu_drivers/XQualcomm_Adreno_842.8/vulkan.adreno.so", fo, nm), "a folder that only ends alike does not");
    CHECK(!maps("7b00000000-7b003c0000 r-xp 00000000 fe:31 99 /data/data/x/gpu_drivers/Qualcomm_Adreno_842.8/libvulkan.adreno.so", fo, nm), "a file that only ends alike does not");
    CHECK(!maps("7b00000000-7b003c0000 rw-p 00000000 00:00 0                          [anon:scudo:secondary]", fo, nm), "a named anonymous map does not");
    CHECK(!maps("7b00000000-7b003c0000 rw-p 00000000 00:00 0", fo, nm), "a line without a path does not");
    CHECK(!maps("/data/data/x/Qualcomm_Adreno_842.8/vulkan.adreno.so", fo, ""), "no library name: nothing");
    // Turnip (Mesa): its own library name
    CHECK(maps("70f0000000-70f0800000 r-xp 00000000 fe:31 7 /data/data/a/files/gpu_drivers/turnip_v39/vulkan.ad07XX.so", "turnip_v39", "vulkan.ad07XX.so"), "a Turnip build counts");
    // DM_VK_DRIVER=<path>.so with an empty folder part: the file name alone
    CHECK(maps("70f0000000-70f0800000 r-xp 00000000 fe:31 7 /data/data/a/drv/vulkan.ad07XX.so", "", "vulkan.ad07XX.so"), "no folder: the file name decides");
    if (fails) { printf("t_android_driverpath: %d failures\n", fails); return 1; }
    printf("t_android_driverpath: ok\n");
    return 0;
}
