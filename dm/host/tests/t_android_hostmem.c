// The driver host memory and Vulkan object counts (dm/gfx/vk_android/vka_hostmem.h; vka_hostmem.c): the allocator's header, alignment, size classes at scudo's 64 KiB
// and 1 MiB, reallocation as Vulkan defines it, the live counts by scope, and the two log lines
#include <stdio.h>
#include <stdint.h>
#include "../../gfx/vk_android/vka_hostmem.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    static vkhm_stats s;
    // alignment: Vulkan's (any power of two) kept, a non-power of two refused
    size_t al[] = {1, 8, 16, 64, 256, 4096};
    for (size_t i = 0; i < sizeof al / sizeof *al; i++) {
        uint8_t *p = vkhm_alloc(&s, 100, al[i], 1);
        CHECK(p && ((uintptr_t)p % (al[i] < 16 ? 16 : al[i])) == 0, "alignment %zu: %p", al[i], (void *)p);
        memset(p, 0xab, 100);
        vkhm_free(&s, p);
    }
    CHECK(!vkhm_alloc(&s, 100, 48, 1) && atomic_load(&s.fails) == 1, "alignment 48 refused");
    CHECK(atomic_load(&s.n[1][0]) == 0 && atomic_load(&s.bytes[1][0]) == 0, "all freed: %lld", (long long)atomic_load(&s.n[1][0]));

    // size classes by the whole block (16-byte header at alignment 16): under 64 KiB, 64 KiB to 1 MiB, 1 MiB and more
    void *a = vkhm_alloc(&s, (64u << 10) - 17, 16, 0), *b = vkhm_alloc(&s, (64u << 10) - 16, 16, 0), *c = vkhm_alloc(&s, (1u << 20), 16, 1), *d = vkhm_alloc(&s, 73000, 16, 1);
    CHECK(atomic_load(&s.n[0][0]) == 1 && atomic_load(&s.n[0][1]) == 1, "command: one under 64 KiB, one at it (%lld, %lld)", (long long)atomic_load(&s.n[0][0]), (long long)atomic_load(&s.n[0][1]));
    CHECK(atomic_load(&s.n[1][2]) == 1 && atomic_load(&s.n[1][1]) == 1, "object: one of 1 MiB or more, one of 73,000 bytes");
    CHECK(atomic_load(&s.bytes[1][1]) == 73000, "bytes are the driver's size: %lld", (long long)atomic_load(&s.bytes[1][1]));
    // a scope out of range counts as the last (instance)
    void *e = vkhm_alloc(&s, 10, 16, 99);
    CHECK(atomic_load(&s.n[4][0]) == 1, "scope 99 counted as instance");

    char line[1600];
    int64_t big = vkhm_line(&s, line, sizeof line);
    CHECK(big == 3, "3 blocks of 64 KiB or more: %lld (%s)", (long long)big, line);
    CHECK(strstr(line, " command 1 (") && strstr(line, " object 2 (") && strstr(line, "1 of 1 MiB or more"), "line: %s", line);
    CHECK(strstr(line, "every live allocation 5,") || strstr(line, "every live allocation 5 "), "line: %s", line);
    // a short buffer is cut, never overrun (ASan)
    char tiny[24]; vkhm_line(&s, tiny, sizeof tiny); CHECK(strlen(tiny) == sizeof tiny - 1, "cut at the end: %zu", strlen(tiny));

    // reallocation: grow keeps the bytes and moves the count to the new size and scope; size 0 frees; NULL original allocates
    uint8_t *r = vkhm_alloc(&s, 32, 16, 1); for (int i = 0; i < 32; i++) r[i] = (uint8_t)i;
    r = vkhm_realloc(&s, r, 200000, 64, 1);
    int same = r != NULL; for (int i = 0; same && i < 32; i++) same = r[i] == (uint8_t)i;
    CHECK(same && (uintptr_t)r % 64 == 0, "grown with its bytes, aligned");
    CHECK(atomic_load(&s.n[1][1]) == 2 && atomic_load(&s.bytes[1][1]) == 273000, "object 64 KiB-1 MiB: 2, %lld bytes", (long long)atomic_load(&s.bytes[1][1]));
    r = vkhm_realloc(&s, r, 8, 16, 1);
    CHECK(r && r[7] == 7 && atomic_load(&s.n[1][1]) == 1, "shrunk to 8 bytes, the large count back to 1");
    CHECK(!vkhm_realloc(&s, r, 0, 16, 1), "size 0 frees");
    void *z = vkhm_realloc(&s, NULL, 40, 16, 2); CHECK(z && atomic_load(&s.n[2][0]) == 1, "NULL original allocates (cache scope)");
    vkhm_free(&s, NULL);
    vkhm_free(&s, a); vkhm_free(&s, b); vkhm_free(&s, c); vkhm_free(&s, d); vkhm_free(&s, e); vkhm_free(&s, z);
    int64_t left = 0; for (int i = 0; i < VKHM_SCOPES; i++) for (int k = 0; k < VKHM_CLASSES; k++) left += atomic_load(&s.n[i][k]) + atomic_load(&s.bytes[i][k]);
    CHECK(left == 0, "every count back to 0: %lld", (long long)left);

    // objects: made and gone, a count of several, nothing for a bad kind or a count of 0
    static vko_stats o;
    vko_made(&o, VKO_IMAGE, 1); vko_made(&o, VKO_IMAGE, 1); vko_gone(&o, VKO_IMAGE); vko_made(&o, VKO_CMDBUF, 33); vko_made(&o, VKO_PIPELINE, 0); vko_made(&o, VKO_N, 5); vko_gone(&o, -1);
    vko_line(&o, line, sizeof line);
    CHECK(strstr(line, "images 1 (2)") && strstr(line, "command buffers 33 (33)") && strstr(line, "pipelines 0 (0)"), "objects: %s", line);
    vko_line(&o, tiny, sizeof tiny); CHECK(strlen(tiny) == sizeof tiny - 1, "objects line cut at the end");

    if (!fails) printf("t_android_hostmem: ok\n");
    return fails != 0;
}
