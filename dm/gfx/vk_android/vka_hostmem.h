// Who holds the process's heap blocks and small driver maps: the driver's host memory, counted through VkAllocationCallbacks, and the replay's live Vulkan objects
// (diagnostic; docs/reference/memory.md "Heap blocks and driver maps that grow at each area change (Android)"). Free of Vulkan types, so the unit test builds it on any host
// (dm/host/tests/t_android_hostmem.c); vka_hostmem.c adapts it to VkAllocationCallbacks and the device's entry points.
//
// The memory-maps logs of the map-limit crashes (#236, #357: Ocarina of Time, Qualcomm's driver) leave the owner of their largest group open: `[anon:scudo:secondary]`
// blocks (Android's allocator gives every block of 64 KiB or more a map of its own, with guard maps beside it, so 3 maps a block at worst) and `/kgsl-3d0` maps of 64 KiB
// or less. Both stay flat while thousands of pipelines are made and jump at an area change (#236: 2,470 → 13,006 blocks and 3,346 → 8,773 kgsl maps). Two counts answer it:
//  - vkhm: every host allocation the driver makes through the allocator the instance and the device were made with (DM_VK_HOSTMEM=1; Vulkan picks the most specific
//    allocator, and the replay passes NULL for its objects, so the device's), live, by allocation scope and by the size class that decides scudo's maps (under 64 KiB:
//    its primary, no map of its own; 64 KiB to 1 MiB; 1 MiB and more). What the driver takes with malloc directly (its shader compiler) is not seen here: the difference
//    to the maps line's scudo count is that, or the host's own
//  - vko: the replay's live objects by kind, from the device's create and destroy entry points (always on: one atomic add a call)
// Each allocation carries a 16-byte header (its size, scope and offset) in front of the pointer the driver gets; the size class is the block's whole size, header included.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VKHM_SCOPES = 5, VKHM_CLASSES = 3 };   // scopes: VkSystemAllocationScope (command, object, cache, device, instance)
static const char *const vkhm_scope_name[VKHM_SCOPES] = {"command", "object", "cache", "device", "instance"};
typedef struct { _Atomic int64_t n[VKHM_SCOPES][VKHM_CLASSES], bytes[VKHM_SCOPES][VKHM_CLASSES]; _Atomic uint64_t calls, fails; } vkhm_stats;
typedef struct { uint64_t size; uint32_t scope, off; } vkhm_hdr;
_Static_assert(sizeof(vkhm_hdr) == 16, "the header is 16 bytes");

#define VKHM_AP(...) (l += (size_t)snprintf(o + (l < n ? l : n), l < n ? n - l : 0, __VA_ARGS__))   // append to o (n bytes), cut at its end
static inline int vkhm_class(size_t block) { return block < (64u << 10) ? 0 : block < (1u << 20) ? 1 : 2; }
static inline int vkhm_scope(int scope) { return scope < 0 || scope >= VKHM_SCOPES ? VKHM_SCOPES - 1 : scope; }
static inline void vkhm_count(vkhm_stats *s, const vkhm_hdr *h, int d) {
    int sc = vkhm_scope((int)h->scope), c = vkhm_class((size_t)h->off + (size_t)h->size);
    atomic_fetch_add_explicit(&s->n[sc][c], d, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes[sc][c], d * (int64_t)h->size, memory_order_relaxed);
}
// PFN_vkAllocationFunction: size bytes at alignment (a power of two, Vulkan's rule; anything else is refused), NULL when out of memory
static inline void *vkhm_alloc(vkhm_stats *s, size_t size, size_t align, int scope) {
    size_t a = align < 16 ? 16 : align;
    if (a & (a - 1)) { atomic_fetch_add_explicit(&s->fails, 1, memory_order_relaxed); return NULL; }
    void *raw = NULL;
    if (size > SIZE_MAX - a || posix_memalign(&raw, a, a + size)) { atomic_fetch_add_explicit(&s->fails, 1, memory_order_relaxed); return NULL; }
    uint8_t *p = (uint8_t *)raw + a;
    vkhm_hdr h = {size, (uint32_t)vkhm_scope(scope), (uint32_t)a};
    memcpy(p - sizeof h, &h, sizeof h);
    vkhm_count(s, &h, 1); atomic_fetch_add_explicit(&s->calls, 1, memory_order_relaxed);
    return p;
}
static inline void vkhm_free(vkhm_stats *s, void *p) {   // PFN_vkFreeFunction (NULL: nothing)
    if (!p) return;
    vkhm_hdr h; memcpy(&h, (uint8_t *)p - sizeof h, sizeof h);
    vkhm_count(s, &h, -1);
    free((uint8_t *)p - h.off);
}
// PFN_vkReallocationFunction: NULL original = allocate; size 0 = free and return NULL; else a new block with the original's bytes (as many as fit), the original freed.
// On failure NULL and the original is left as it was
static inline void *vkhm_realloc(vkhm_stats *s, void *orig, size_t size, size_t align, int scope) {
    if (!orig) return vkhm_alloc(s, size, align, scope);
    if (!size) { vkhm_free(s, orig); return NULL; }
    vkhm_hdr h; memcpy(&h, (uint8_t *)orig - sizeof h, sizeof h);
    void *p = vkhm_alloc(s, size, align, scope);
    if (!p) return NULL;
    memcpy(p, orig, (size_t)(h.size < size ? h.size : size));
    vkhm_free(s, orig);
    return p;
}
// One line: live allocations of 64 KiB and more (scudo's own maps) by scope, then all live ones. Returns the count of the large ones
static inline int64_t vkhm_line(const vkhm_stats *s, char *o, size_t n) {
    int64_t big = 0, bigb = 0, all = 0, allb = 0; size_t l = 0;
    VKHM_AP("driver host memory through the allocator: blocks of 64 KiB or more");
    for (int sc = 0; sc < VKHM_SCOPES; sc++) {
        int64_t bn = atomic_load(&s->n[sc][1]) + atomic_load(&s->n[sc][2]), bb = atomic_load(&s->bytes[sc][1]) + atomic_load(&s->bytes[sc][2]);
        big += bn; bigb += bb;
        for (int c = 0; c < VKHM_CLASSES; c++) { all += atomic_load(&s->n[sc][c]); allb += atomic_load(&s->bytes[sc][c]); }
        if (bn) VKHM_AP(" %s %lld (%.1f MiB, %lld of 1 MiB or more)", vkhm_scope_name[sc], (long long)bn, bb / 1048576.0,
                                      (long long)atomic_load(&s->n[sc][2]));
    }
    VKHM_AP("%s: %lld blocks, %.1f MiB in all; every live allocation %lld, %.1f MiB; %llu allocations so far, %llu refused",
                          big ? "" : " none", (long long)big, bigb / 1048576.0, (long long)all, allb / 1048576.0,
                          (unsigned long long)atomic_load(&s->calls), (unsigned long long)atomic_load(&s->fails));
    return big;
}

// The replay's live Vulkan objects (made by the device's entry points, less those destroyed). Command buffers and descriptor pools are never freed by the replay: their
// count is how many it ever made
enum { VKO_IMAGE, VKO_VIEW, VKO_BUFFER, VKO_BVIEW, VKO_SAMPLER, VKO_PIPELINE, VKO_SHADER, VKO_DPOOL, VKO_CMDBUF, VKO_MEMORY, VKO_N };
static const char *const vko_name[VKO_N] = {"images", "image views", "buffers", "buffer views", "samplers", "pipelines", "shader modules", "descriptor pools",
                                            "command buffers", "memory allocations"};
typedef struct { _Atomic int64_t live[VKO_N]; _Atomic uint64_t made[VKO_N]; } vko_stats;
static inline void vko_made(vko_stats *s, int k, int64_t n) {
    if (k < 0 || k >= VKO_N || n <= 0) return;
    atomic_fetch_add_explicit(&s->live[k], n, memory_order_relaxed); atomic_fetch_add_explicit(&s->made[k], (uint64_t)n, memory_order_relaxed);
}
static inline void vko_gone(vko_stats *s, int k) { if (k >= 0 && k < VKO_N) atomic_fetch_sub_explicit(&s->live[k], 1, memory_order_relaxed); }
static inline void vko_line(const vko_stats *s, char *o, size_t n) {
    size_t l = 0; VKHM_AP("Vulkan objects live (made in all):");
    for (int k = 0; k < VKO_N; k++)
        VKHM_AP("%s %s %lld (%llu)", k ? "," : "", vko_name[k], (long long)atomic_load(&s->live[k]), (unsigned long long)atomic_load(&s->made[k]));
}
