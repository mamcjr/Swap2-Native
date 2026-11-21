// The compute shared memory the host uses (dm/gfx/vk_android/vka_shmcap.h; vka_dev.c after vkGetPhysicalDeviceProperties): a Qualcomm GPU's advertised
// limit at most 32 KiB (community Turnip builds advertise 64 KiB the Adreno does not have), every other vendor's as advertised
#include <stdio.h>
#include "../../gfx/vk_android/vka_shmcap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    CHECK(vka_shared_mem_limit(0x5143, 65536) == 32768, "Turnip Gen8 V39 on the Adreno 830 (65536 advertised): 32 KiB used");
    CHECK(vka_shared_mem_limit(0x5143, 32768) == 32768, "Qualcomm 891.x and upstream Turnip (32768): unchanged");
    CHECK(vka_shared_mem_limit(0x5143, 16384) == 16384, "a smaller Adreno (A610, 16 KiB): unchanged, never raised");
    CHECK(vka_shared_mem_limit(0x5143, 32769) == 32768, "anything past 32 KiB on Qualcomm: 32 KiB");
    CHECK(vka_shared_mem_limit(0x13B5, 65536) == 65536, "ARM Mali (0x13B5): as advertised");
    CHECK(vka_shared_mem_limit(0x144D, 65536) == 65536, "Samsung Xclipse (0x144D): as advertised");
    CHECK(vka_shared_mem_limit(0x10DE, 49152) == 49152, "NVIDIA: as advertised");
    CHECK(vka_shared_mem_limit(0x5143, 0) == 0, "zero stays zero");
    if (fails) { printf("t_android_shmcap: %d failed\n", fails); return 1; }
    printf("t_android_shmcap: Qualcomm shared memory at most 32 KiB, other vendors as advertised: passed\n");
    return 0;
}
