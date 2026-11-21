// The compute shared memory a Qualcomm Adreno really has, whatever its driver advertises (vka_dev.c, after vkGetPhysicalDeviceProperties), free of
// Vulkan so the unit test (dm/host/tests/t_android_shmcap.c) can check it.
// Adreno 6xx-8xx give a workgroup at most 32 KiB of shared memory: SP_CS_CNTL_1.SHARED_SIZE is 5 bits, (SHARED_SIZE + 1) x 1 KiB, and the ldl/stl offset past
// the programmed size is rewritten to 0 (Mesa src/freedreno/registers/adreno/a6xx.xml). Qualcomm's driver (891.x, Adreno 830) advertises 32768 bytes.
// Community Turnip builds (Turnip Gen8 V39, from whitebelyash/mesa-unified turnip/gen8 commit 9797598e1e9 "increase shared mem size") advertise 65536 on
// A7xx/A8xx without changing how the size is programmed. Measured on the POCO F7 Ultra (Adreno 830, 2026-11-20, turnip-swap2 tools/shm_probe.comp): one
// workgroup filling 64 KiB reads back every word of the upper 32 KiB wrong, and a word at the start is overwritten. The host sizes the task/mesh packing
// (vka_mesh.inc), the program refusal (vka_pso.c) and the shuffle path (vka_shader.c) from this limit, so it takes the real one.
// A workaround of a driver bug (the driver's limit is wrong; its code is not ours): docs/reference/shaders.md "Adreno shared memory advertised past 32 KiB".
#ifndef VKA_SHMCAP_H
#define VKA_SHMCAP_H
#include <stdint.h>

#define VKA_QCOM_VENDOR_ID 0x5143u
#define VKA_ADRENO_SHARED_MAX (32u * 1024u)

// The limit the host uses: the advertised one, but at most 32 KiB on a Qualcomm GPU
static inline uint32_t vka_shared_mem_limit(uint32_t vendor_id, uint32_t advertised) {
    return vendor_id == VKA_QCOM_VENDOR_ID && advertised > VKA_ADRENO_SHARED_MAX ? VKA_ADRENO_SHARED_MAX : advertised;
}
#endif
