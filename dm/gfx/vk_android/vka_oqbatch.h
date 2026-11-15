// Occlusion queries on the Vulkan replay: the batch rule for a completed submission's samples-passed reports (vka_exec.c "Occlusion queries", oq_batch_init), as pure
// logic free of Vulkan so dm/host/tests/t_android_rcix.c can check it against the reports read one by one.
// The completion thread used to read each report's queries (vkGetQueryPoolResults, up to OQ_RB at a time) and reset them (vkResetQueryPool) report by report, under a
// mutex: about two driver calls and a lock pair per report, and Ocarina of Time makes about 1,400 a frame. When the ranges of a submission's reports and given-back
// segments follow one another in sequence (each starts where the previous ended), the whole range is read in a few wide reads into one buffer before the first report,
// each report sums its own part of it in the original order, and the range is reset and given back once after the last. A gap or a range out of order (a carried list:
// vka_exec.c carry_take), or a range over the ring, keeps the report-by-report path. After a926b0c, 6b00c2a and 3af72a2 in Erick's Ocarina fork (2026-11-10 / 11)
#ifndef VKA_OQBATCH_H
#define VKA_OQBATCH_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t first, last; int found, bad; } oqb_plan;   // [first, last): the queries of the list's ranges, when they follow one another
// One report's (or given-back segment's) range [q0, q1), in the list's order; an empty one changes nothing
static inline void oqb_add(oqb_plan *p, uint64_t q0, uint64_t q1) {
    if (q1 <= q0 || p->bad) return;
    if (p->found && q0 != p->last) { p->bad = 1; return; }   // a gap or a range out of order: nothing outside the reported ranges is read or reset
    if (!p->found) { p->first = q0; p->found = 1; }
    p->last = q1;
}
static inline int oqb_ok(const oqb_plan *p, uint64_t ring) { return p->found && !p->bad && p->last > p->first && p->last - p->first <= ring; }
// The queries from sequence number q that one call may cover: up to last, not past the pool's end (q's slot is q % ring), at most cap
static inline uint32_t oqb_chunk(uint64_t q, uint64_t last, uint64_t ring, uint64_t cap) {
    uint64_t k = last - q, e = ring - q % ring;
    if (e < k) k = e;
    if (cap < k) k = cap;
    return (uint32_t)k;
}
// The samples of cnt queries read as {result, availability} pairs: one not available after its fence counts 1 sample (and adds to *unav), as the per-report path does
static inline uint64_t oqb_sum(const uint64_t *pairs, size_t cnt, uint64_t *unav) {
    uint64_t s = 0;
    for (size_t i = 0; i < cnt; i++) if (!pairs[2 * i + 1]) { s++; (*unav)++; } else s += pairs[2 * i];
    return s;
}
#endif
