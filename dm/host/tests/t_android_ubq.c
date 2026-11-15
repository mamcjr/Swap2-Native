// Inline uniform updates on the Vulkan replay (dm/gfx/vk_android/vka_ubq.h; vka_exec.c "Inline uniform updates (OP_UPDATE_UB)", vka_draw.c ub_hazard): random update
// sequences through the batch rule (ubq_must_flush, each copy's regions applied in reverse to show they never depend on order) and through the kept list (ubq_any,
// ubq_overlay) against the same updates applied one by one, in NVN's order: the last write wins. Then the ranges' edges and a draw's index and indirect-count ranges
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_ubq.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { MEM = 4096, RUN = 1024, BMAX = 8 };
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

// The batch as vka_exec.c ub_add stages it: guest RAM placed in runs of RUN bytes, each run its own buffer (the destination buffer changes at a run's end), the source
// scratch in one buffer that sometimes changes (a new scratch block)
static ubq_region g_r[BMAX]; static unsigned g_n; static int g_src, g_dst; static uint8_t g_scr[8][MEM]; static uint8_t g_gpu[MEM];
static unsigned g_flushes;
static void flush(void) {   // one vkCmdCopyBuffer: its regions run in no order (here: the last first)
    for (unsigned i = g_n; i-- > 0;) memcpy(g_gpu + (size_t)g_dst * RUN + g_r[i].dof, g_scr[g_src] + g_r[i].so, (size_t)g_r[i].size);
    if (g_n) g_flushes++;
    g_n = 0;
}
static void add(int sb, uint64_t so, int db, uint64_t dof, uint64_t k) {
    if (ubq_must_flush(g_r, g_n, BMAX, g_src == sb && g_dst == db, dof, k)) flush();
    if (!g_n) { g_src = sb; g_dst = db; }
    CHECK(g_n < BMAX, "batch overrun");
    g_r[g_n++] = (ubq_region){so, dof, k};
}

static void batch_random(void) {
    for (int round = 0; round < 2000; round++) {
        uint8_t ref[MEM]; for (int i = 0; i < MEM; i++) ref[i] = g_gpu[i] = (uint8_t)rnd();
        // (scratch is never reused before the copies that read it ran: each block only grows in a round, 40 updates of at most 96 bytes fit one)
        int nu = 1 + (int)(rnd() % 40); uint64_t sob[8] = {0}, last = 0; int sb = (int)(rnd() % 8);
        g_n = 0;
        for (int u = 0; u < nu; u++) {
            uint64_t n = 1 + rnd() % 96, d = rnd() % (MEM - n);
            if ((rnd() & 3) == 0 && u && last + n <= MEM) d = last;   // often the same place again (a constant block rewritten draw after draw)
            last = d;
            if ((rnd() % 16) == 0) sb = (int)(rnd() % 8);
            uint64_t so = sob[sb];
            for (uint64_t i = 0; i < n; i++) ref[d + i] = g_scr[sb][so + i] = (uint8_t)rnd();
            for (uint64_t done = 0; done < n;) {   // ub_copy: run by placed run
                uint64_t a = d + done, k = RUN - a % RUN; if (k > n - done) k = n - done;
                add(sb, so + done, (int)(a / RUN), a % RUN, k); done += k;
            }
            sob[sb] += n;
        }
        flush();
        CHECK(!memcmp(ref, g_gpu, MEM), "round %d: the batched copies differ from the updates one by one", round);
        if (fails) return;
    }
    CHECK(g_flushes > 2000, "overlapping updates flushed the batch (%u copies for 2000 rounds)", g_flushes);
}

static void kept_random(void) {
    static uint8_t bytes[64][128];
    for (int round = 0; round < 4000; round++) {
        uint8_t base[MEM], ref[MEM]; for (int i = 0; i < MEM; i++) ref[i] = base[i] = (uint8_t)rnd();
        ubq_write w[64]; int n = (int)(rnd() % 64); uint64_t lo = 0, hi = 0;
        for (int i = 0; i < n; i++) {
            uint64_t sz = 1 + rnd() % 128, a = rnd() % (MEM - sz);
            for (uint64_t k = 0; k < sz; k++) ref[a + k] = bytes[i][k] = (uint8_t)rnd();
            w[i] = (ubq_write){a, sz, bytes[i]};
            ubq_bound(&lo, &hi, i == 0, a, sz);
        }
        for (int q = 0; q < 8; q++) {
            uint64_t sz = 1 + rnd() % 512, a = rnd() % (MEM - sz);
            int any = 0; for (int i = 0; i < n; i++) any |= a < w[i].addr + w[i].size && w[i].addr < a + sz;
            CHECK(ubq_any(w, n, lo, hi, a, sz) == any, "round %d: overlap of %#llx+%llu", round, (unsigned long long)a, (unsigned long long)sz);
            uint8_t out[512]; memcpy(out, base + a, (size_t)sz);
            ubq_overlay(w, n, a, out, sz);
            CHECK(!memcmp(out, ref + a, (size_t)sz), "round %d: the shadow of %#llx+%llu is not the bytes after the updates", round, (unsigned long long)a, (unsigned long long)sz);
            if (fails) return;
        }
        for (int i = 0; i < n; i++) CHECK(lo <= w[i].addr && w[i].addr + w[i].size <= hi, "bounding range");
    }
}

int main(void) {
    // ranges
    CHECK(!ubq_overlap(0, 0, 0, 16) && !ubq_overlap(0, 16, 8, 0), "an empty region never overlaps");
    CHECK(ubq_overlap(0, 16, 15, 1) && !ubq_overlap(0, 16, 16, 1) && ubq_overlap(16, 1, 0, 17), "edges");
    CHECK(ubq_overlap(UINT64_MAX - 8, 8, UINT64_MAX - 1, 1), "near the top, no wrap");
    CHECK(ubq_end(UINT64_MAX - 4, 8) == UINT64_MAX && ubq_end(16, 0) == UINT64_MAX && ubq_end(16, 16) == 32, "ends");
    CHECK(ubq_rng(0x1000, 0, 0xFFFF0000ull, 4) && !ubq_rng(0x1000, 16, 0x1010, 16) && ubq_rng(0x1000, 17, 0x1010, 16), "size 0 runs to the end; half-open");
    // the batch rule
    ubq_region r[2] = {{0, 0x100, 0x40}, {0x40, 0x200, 0x10}};
    CHECK(!ubq_must_flush(r, 0, 4, 0, 0x100, 4), "an empty batch takes anything");
    CHECK(!ubq_must_flush(r, 2, 4, 1, 0x140, 0xC0), "disjoint, same buffers: joins");
    CHECK(ubq_must_flush(r, 2, 4, 1, 0x13F, 1), "over an earlier region: flushes (the later write lands last)");
    CHECK(ubq_must_flush(r, 2, 4, 0, 0x400, 4), "another buffer pair: flushes");
    CHECK(ubq_must_flush(r, 2, 2, 1, 0x400, 4), "full: flushes");
    // the kept list's bounding range: a range outside it is a miss before the list is read
    ubq_write w1 = {0x1000, 0x10, NULL};
    CHECK(!ubq_any(&w1, 1, 0x1000, 0x1010, 0x1010, 0x10) && ubq_any(&w1, 1, 0x1000, 0x1010, 0x0F00, 0) && !ubq_any(&w1, 0, 0, UINT64_MAX, 0, 0), "bounds, size 0, empty list");
    // a draw's reads
    CHECK(ubq_index_len(0, 30, 0) == 30 && ubq_index_len(1, 30, 0) == 60 && ubq_index_len(2, 30, 0) == 120, "direct: count indices of the type's size");
    CHECK(ubq_index_len(2, 30, 1) == 0 && ubq_index_len(1, 0, 0) == 0 && ubq_index_len(1, -1, 0) == 0, "indirect or no count: to the end");
    CHECK(ubq_mdi_len(1, 32, 20) == 20 && ubq_mdi_len(4, 32, 20) == 3 * 32 + 20 && ubq_mdi_len(0, 32, 16) == 16, "indirect-count: max records of stride");
    CHECK(ubq_mdi_len(0x10000, 0xFFFFFFFCu, 20) == 0xFFFFull * 0xFFFFFFFCull + 20, "no 32-bit overflow");
    batch_random();
    kept_random();
    if (fails) { printf("t_android_ubq: %d failed\n", fails); return 1; }
    printf("t_android_ubq: ok\n");
    return 0;
}
