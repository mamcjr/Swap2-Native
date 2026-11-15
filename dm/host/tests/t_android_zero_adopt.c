// Production allocator bookkeeping without a GPU. Build with NDK and section GC.
#include "../../gfx/vk_android/vka_imgmem.c"
#include <assert.h>
vka_dev V; vka_fns vk;
static int freed;
static void VKAPI_CALL release_memory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; freed++; }
int main(void) {
    uint64_t key = 42, bytes = 299ull << 20;
    assert(vkz_add(&Z, &key, sizeof key, 123, bytes) >= 0);
    vka_imem a = {.dm=(VkDeviceMemory)(uintptr_t)123, .size=bytes, .z=1};
    assert(vkz_ref(&Z, &key, sizeof key) >= 0);
    assert(!vka_imem_adopt_zero(&a) && a.z && Z.n == 1 && !S.ded);
    assert(!vkz_unref(&Z, 123));
    assert(vka_imem_adopt_zero(&a) && !a.z && (uintptr_t)a.dm == 123 && a.size == bytes);
    assert(Z.n == 0 && Z.owned == 1 && S.ded == 1 && S.ded_bytes == bytes);
    assert(!vka_imem_adopt_zero(&a) && S.ded == 1);
    vka_imem missing = {.dm=(VkDeviceMemory)(uintptr_t)999, .z=1};
    assert(!vka_imem_adopt_zero(&missing));
    vk.vkFreeMemory = release_memory;
    vka_imem_free(&a); assert(!S.ded && !S.ded_bytes && freed == 1);
    iblock *occupied = calloc(1, sizeof *occupied), *idle = calloc(1, sizeof *idle);
    assert(vke_init(&occupied->free, BLOCK / UNIT) && vke_init(&idle->free, BLOCK / UNIT));
    occupied->dm = (VkDeviceMemory)(uintptr_t)200; idle->dm = (VkDeviceMemory)(uintptr_t)201;
    assert(vke_alloc(&occupied->free, (16ull << 20) / UNIT, 1) == 0);
    assert(vke_alloc(&idle->free, (1ull << 20) / UNIT, 1) == 0);
    idle->next = occupied; g_blocks[0] = idle; S.blocks = 2; S.runs = 2; S.run_bytes = S.req_bytes = 17ull << 20;
    vka_imem small = {.dm=idle->dm, .size=1ull << 20, .units=(1ull << 20) / UNIT, .blk=idle};
    vka_imem_free(&small); assert(S.blocks == 1 && freed == 2 && g_blocks[0] == occupied && occupied->free.used);
    vka_imem large = {.dm=occupied->dm, .size=16ull << 20, .units=(16ull << 20) / UNIT, .blk=occupied};
    vka_imem_free(&large); assert(S.blocks == 1 && freed == 2 && vke_empty(&occupied->free));
    vke_fini(&occupied->free); free(occupied); g_blocks[0] = NULL;
    puts("zero adoption: shared rejection, sole transfer, unchanged handle and balanced accounting passed");
    return 0;
}
