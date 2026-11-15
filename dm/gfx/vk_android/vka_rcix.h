// Render conditions on the Vulkan replay: the index of the newest report in flight at each address (vka_exec.c "Render conditions", rc_known), as pure logic free of
// Vulkan so dm/host/tests/t_android_rcix.c can check it against the reverse scan it replaces.
// rc_known used to scan the whole list of reports in flight from its end for every condition, twice (the pair's two counters): Ocarina of Time evaluates over 1,000
// conditions a frame with up to about 7,000 reports in flight (Erick's logs), millions of compares a frame on the replay thread. The index is an open-addressed table
// from an address to the position of its newest entry in the list:
// - an entry appended to the list is put in the index, over the address's slot when it has one (the newest wins);
// - nothing is ever deleted: when the list is compacted (rc_prune drops the reports whose submissions are done, keeping the order) the index is rebuilt from the list in
//   order, which brings back the newest entry per address. A slot whose epoch is not the table's counts as empty, so a rebuild is one increment, not a clear (the table is
//   cleared only when the epoch wraps);
// - the caller keeps the list at most RCIX_CAP / 2 entries long (vka_exec.c asserts it against RC_PEND_MAX): the table is never more than half full, so a probe always
//   reaches an empty slot or the address.
// After 5998b42 in Erick's Ocarina fork (2026-11-10), with the rebuild only when the compaction removed entries.
#ifndef VKA_RCIX_H
#define VKA_RCIX_H
#include <stdint.h>
#include <string.h>

#define RCIX_LOG2 16
#define RCIX_CAP (1u << RCIX_LOG2)
typedef struct { uint64_t addr; uint32_t latest, epoch; } rcix_slot;
typedef struct { rcix_slot s[RCIX_CAP]; uint32_t epoch; uint64_t lookups, probes, found; } rcix;   // zero-initialised is valid (epoch 0 is taken to 1 on first use)

static inline uint32_t rcix_hash(uint64_t a) { return (uint32_t)(((a >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - RCIX_LOG2)); }   // (records are 16-byte aligned)
static inline void rcix_ready(rcix *ix) { if (!ix->epoch) ix->epoch = 1; }   // (every slot of a zeroed table has epoch 0: empty)
static inline void rcix_put(rcix *ix, uint64_t a, uint32_t i) {
    rcix_ready(ix);
    for (uint32_t h = rcix_hash(a);; h = (h + 1) & (RCIX_CAP - 1)) {
        rcix_slot *s = &ix->s[h];
        if (s->epoch != ix->epoch || s->addr == a) { s->addr = a; s->latest = i; s->epoch = ix->epoch; return; }
    }
}
// The position of the newest entry at a, or -1
static inline int rcix_find(rcix *ix, uint64_t a) {
    rcix_ready(ix); ix->lookups++;
    for (uint32_t h = rcix_hash(a);; h = (h + 1) & (RCIX_CAP - 1)) {
        const rcix_slot *s = &ix->s[h]; ix->probes++;
        if (s->epoch != ix->epoch) return -1;
        if (s->addr == a) { ix->found++; return (int)s->latest; }
    }
}
// After the list was compacted: n entries of stride bytes from base, each with its address as its first field, put again in order (the newest wins)
static inline void rcix_rebuild(rcix *ix, const void *base, size_t stride, int n) {
    rcix_ready(ix);
    if (!++ix->epoch) { memset(ix->s, 0, sizeof ix->s); ix->epoch = 1; }
    for (int i = 0; i < n; i++) { uint64_t a; memcpy(&a, (const char *)base + (size_t)i * stride, sizeof a); rcix_put(ix, a, (uint32_t)i); }
}
#endif
