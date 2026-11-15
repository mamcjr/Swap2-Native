// Inline uniform updates on the Vulkan replay: the range arithmetic and the batch and pass-end rules, without Vulkan (vka_exec.c "Inline uniform updates (OP_UPDATE_UB)",
// vka_draw.c ub_hazard; docs/reference/rendering.md, "Inline uniform updates on the Vulkan replay"). Kept apart so dm/host/tests/t_android_ubq.c checks them against
// updates applied one by one, in NVN's order
#ifndef VKA_UBQ_H
#define VKA_UBQ_H
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t so, dof, size; } ubq_region;   // one region of a batched copy: VkBufferCopy's fields, in its order (vka_exec.c asserts the layout)
typedef struct { uint64_t addr, size; const uint8_t *p; } ubq_write;   // an update kept in the open pass: its guest range and its bytes

static inline uint64_t ubq_end(uint64_t a, uint64_t n) { return !n || a > UINT64_MAX - n ? UINT64_MAX : a + n; }   // (size 0: to the end of memory)
// Two ranges with sizes: empty ranges never overlap (a batch's regions)
static inline int ubq_overlap(uint64_t a, uint64_t na, uint64_t b, uint64_t nb) { return na && nb && (a <= b ? b - a < na : a - b < nb); }
// Two half-open ranges whose size 0 runs to the end of memory (what a draw reads: an index buffer of the GPU's count)
static inline int ubq_rng(uint64_t a, uint64_t na, uint64_t b, uint64_t nb) { return a < ubq_end(b, nb) && b < ubq_end(a, na); }

// A region joins the staged batch only when it shares the batch's buffers, the batch has room and its destination overlaps none staged: the regions of one
// vkCmdCopyBuffer run in no order, so an overlap (a later update over an earlier one) flushes the batch first, and the later write lands last
static inline int ubq_must_flush(const ubq_region *r, unsigned n, unsigned max, int same_buffers, uint64_t dof, uint64_t k) {
    if (!n) return 0;
    if (!same_buffers || n >= max) return 1;
    for (unsigned i = 0; i < n; i++) if (ubq_overlap(r[i].dof, r[i].size, dof, k)) return 1;
    return 0;
}

// The kept writes over [addr, addr + size) (size 0: to the end), the bounding range [lo, hi) checked first
static inline int ubq_any(const ubq_write *w, int n, uint64_t lo, uint64_t hi, uint64_t addr, uint64_t size) {
    if (!n) return 0;
    uint64_t e = ubq_end(addr, size);
    if (e <= lo || addr >= hi) return 0;
    for (int i = 0; i < n; i++) if (addr < ubq_end(w[i].addr, w[i].size) && w[i].addr < e) return 1;
    return 0;
}
static inline void ubq_bound(uint64_t *lo, uint64_t *hi, int first, uint64_t addr, uint64_t size) {
    uint64_t e = ubq_end(addr, size);
    if (first) { *lo = addr; *hi = e; } else { if (addr < *lo) *lo = addr; if (e > *hi) *hi = e; }
}
// The kept writes' bytes over out = the bytes of [addr, addr + size) before them, in list order: the last write wins
static inline void ubq_overlay(const ubq_write *w, int n, uint64_t addr, void *out, uint64_t size) {
    uint64_t e = ubq_end(addr, size);
    for (int i = 0; i < n; i++) {
        uint64_t we = ubq_end(w[i].addr, w[i].size);
        if (!(addr < we && w[i].addr < e)) continue;
        uint64_t lo = addr > w[i].addr ? addr : w[i].addr, hi = e < we ? e : we;
        memcpy((uint8_t *)out + (lo - addr), w[i].p + (lo - w[i].addr), (size_t)(hi - lo));
    }
}

// What a draw reads from its index buffer: count indices of itype's size (NVN's record has no first index) for a direct draw; to the end (0) for an indirect or
// indirect-count one, whose count is the GPU's, or a count that is not positive
static inline uint64_t ubq_index_len(int itype, int32_t count, int indirect) {
    uint64_t isz = itype == 0 ? 1 : itype == 1 ? 2 : 4;
    return !indirect && count > 0 ? (uint64_t)count * isz : 0;
}
// An indirect-count group, drawn whole at its first record (vkCmdDraw*IndirectCount): max records of stride, the last one rec bytes
static inline uint64_t ubq_mdi_len(uint32_t max, uint32_t stride, uint64_t rec) { return (uint64_t)(max ? max - 1 : 0) * stride + rec; }
#endif
