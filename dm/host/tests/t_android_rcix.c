// Render conditions and occlusion queries on the Vulkan replay (dm/gfx/vk_android/vka_rcix.h, vka_oqbatch.h; vka_exec.c "Render conditions", "Occlusion queries"):
// - the index of the newest report by address against the reverse scan it replaced (vka_exec.c rc_known before 2026-11-12), over random lists with repeated addresses,
//   compactions by fence with and without a rebuild, the epoch's wrap and a list of RC_PEND_MAX distinct addresses;
// - the batch rule of a completed list's occlusion queries: one plan for ranges that follow one another, none for a gap, a reversed order or a span over the ring, chunks
//   that never cross the pool's end, and the same sums and unavailable counts as the report-by-report reads
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_rcix.h"
#include "../../gfx/vk_android/vka_oqbatch.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rs = 777;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

// ---------- The index ----------
enum { RC_PEND_MAX = 32768 };   // (vka_exec.c's)
typedef struct { uint64_t addr, fence; int kind; } rc_rep;   // (vka_exec.c's layout: the address first)
static rc_rep L[RC_PEND_MAX]; static int nl;
static rcix IX;
static int scan(uint64_t a) { for (int i = nl - 1; i >= 0; i--) if (L[i].addr == a) return i; return -1; }   // the old rc_known's newest entry
static void add(uint64_t a, int kind) {   // vka_exec.c rc_note: the merge over the newest entries of the submission being recorded, else an append put in the index
    for (int i = nl - 1, k = 0; i >= 0 && !L[i].fence && k < 64; i--, k++) if (L[i].addr == a) { L[i].kind = kind; return; }
    if (nl >= RC_PEND_MAX) return;
    L[nl++] = (rc_rep){a, 0, kind}; rcix_put(&IX, a, (uint32_t)(nl - 1));
}
static void prune(uint64_t done, int rebuild_always) {   // vka_exec.c rc_prune
    int j = 0, n0 = nl;
    for (int i = 0; i < nl; i++) if (!L[i].fence || L[i].fence > done) L[j++] = L[i];
    nl = j;
    if (j != n0 || rebuild_always) rcix_rebuild(&IX, L, sizeof *L, nl);
}
static void compare(int naddr, const char *what) {
    for (int a = 0; a <= naddr; a++) {   // (one past: misses)
        uint64_t ad = 0x80000000ull + 16ull * (uint64_t)a;
        int s = scan(ad), f = rcix_find(&IX, ad);
        CHECK(s == f, "%s: address %#llx: index %d, scan %d", what, (unsigned long long)ad, f, s);
        if (s != f) return;
    }
    CHECK(rcix_find(&IX, 0x1234567ull) == scan(0x1234567ull), "%s: an address never reported", what);
}
static void index_random(void) {
    memset(&IX, 0, sizeof IX); nl = 0;
    uint64_t fence = 1;
    for (int round = 0; round < 60; round++) {
        int naddr = 1 + (int)(rnd() % 3500);
        for (int sub = 0; sub < 1 + (int)(rnd() % 6); sub++) {   // submissions: reports, then their fence (rc_commit)
            int nrep = (int)(rnd() % 1200);
            for (int r = 0; r < nrep; r++) add(0x80000000ull + 16ull * (rnd() % (unsigned)naddr), 1 + (int)(rnd() % 3));
            for (int i = nl - 1; i >= 0 && !L[i].fence; i--) L[i].fence = fence;
            fence++;
        }
        compare(3500, "after appends");
        uint64_t done = fence - 1 - rnd() % 4;   // some submissions done
        if (rnd() & 1) { prune(done, 0); compare(3500, "after a prune"); }
        else { int n0 = nl; prune(0, 0); CHECK(nl == n0, "a prune with nothing done removed entries"); compare(3500, "after a prune that removed nothing (no rebuild)"); }
    }
}
static void index_wrap(void) {   // the epoch wraps: the table is cleared, still the newest wins
    memset(&IX, 0, sizeof IX); nl = 0;
    for (int r = 0; r < 5000; r++) { add(0x80000000ull + 16ull * (rnd() % 2000), 1); if ((r & 255) == 255) for (int i = nl - 1; i >= 0 && !L[i].fence; i--) L[i].fence = 1 + (uint64_t)r; }
    IX.epoch = UINT32_MAX;
    rcix_rebuild(&IX, L, sizeof *L, nl);
    CHECK(IX.epoch == 1, "the epoch's wrap: epoch %u", IX.epoch);
    compare(2000, "after the epoch's wrap");
    prune(0, 1); compare(2000, "a rebuild with nothing removed");
}
static void index_full(void) {   // RC_PEND_MAX distinct addresses: the table at half full, every probe ends
    memset(&IX, 0, sizeof IX); nl = 0;
    for (int i = 0; i < RC_PEND_MAX; i++) { L[i] = (rc_rep){0x80000000ull + 16ull * (uint64_t)i * 37u, 1, 1}; rcix_put(&IX, L[i].addr, (uint32_t)i); }
    nl = RC_PEND_MAX;
    IX.lookups = IX.probes = IX.found = 0;
    for (int i = 0; i < RC_PEND_MAX; i++) CHECK(rcix_find(&IX, L[i].addr) == i, "full table: entry %d", i);
    for (int i = 0; i < 4096; i++) CHECK(rcix_find(&IX, 0x10ull + 16ull * (uint64_t)i) == -1, "full table: a miss found");
    CHECK(RCIX_CAP >= 2 * RC_PEND_MAX, "the table must stay half full");
    printf("render condition index at %d entries: %.2f probes a lookup\n", RC_PEND_MAX, (double)IX.probes / (double)IX.lookups);
}

// ---------- The occlusion batch ----------
enum { RING = 32768, RB = 256, BATCH = 1024 };
static void batch_plan(void) {
    oqb_plan p = {0};
    oqb_add(&p, 10, 20); oqb_add(&p, 20, 20); oqb_add(&p, 20, 3000); oqb_add(&p, 3000, 3010);
    CHECK(oqb_ok(&p, RING) && p.first == 10 && p.last == 3010, "a chain: [%llu, %llu)", (unsigned long long)p.first, (unsigned long long)p.last);
    int reads = 0; for (uint64_t q = p.first; q < p.last; q += oqb_chunk(q, p.last, RING, BATCH)) reads++;
    CHECK(reads == 3, "a chain of 3000 read in %d reads", reads);
    oqb_plan g = {0}; oqb_add(&g, 10, 20); oqb_add(&g, 21, 30); CHECK(!oqb_ok(&g, RING), "a gap planned");
    oqb_plan r = {0}; oqb_add(&r, 20, 30); oqb_add(&r, 10, 20); CHECK(!oqb_ok(&r, RING), "a reversed order planned");
    oqb_plan w = {0}; oqb_add(&w, 0, RING); oqb_add(&w, RING, RING + 1); CHECK(!oqb_ok(&w, RING), "a span over the ring planned");
    oqb_plan e = {0}; oqb_add(&e, 5, 5); CHECK(!oqb_ok(&e, RING), "only empty ranges planned");
    oqb_plan x = {0}; oqb_add(&x, 0, RING); CHECK(oqb_ok(&x, RING), "the whole ring not planned");
    // a list across the pool's end: no chunk crosses it, resets in at most 2 calls
    uint64_t f = 5ull * RING - 8, l = f + 3000; int rd = 0, rr = 0;
    for (uint64_t q = f; q < l;) { uint32_t k = oqb_chunk(q, l, RING, BATCH); CHECK(k && q % RING + k <= RING && k <= BATCH, "read chunk %u at %llu", k, (unsigned long long)q); q += k; rd++; }
    for (uint64_t q = f; q < l;) { uint32_t k = oqb_chunk(q, l, RING, RING); CHECK(k && q % RING + k <= RING, "reset chunk %u at %llu", k, (unsigned long long)q); q += k; rr++; }
    CHECK(rd <= (3000 + BATCH - 1) / BATCH + 1 && rr == 2, "across the end: %d reads, %d resets", rd, rr);
}
static uint64_t pool[2 * RING];   // the pool's {result, availability} per slot
static uint64_t per_report(uint64_t q0, uint64_t q1, uint64_t *unav) {   // vka_exec.c oq_done's report-by-report reads (OQ_RB at a time, not past the pool's end)
    uint64_t s = 0;
    for (uint64_t q = q0; q < q1;) {
        uint32_t sl = (uint32_t)(q % RING), k = (uint32_t)(q1 - q < RING - sl ? q1 - q : RING - sl); if (k > RB) k = RB;
        for (uint32_t i = 0; i < k; i++) { const uint64_t *r = &pool[2 * (sl + i)]; if (!r[1]) { s++; (*unav)++; } else s += r[0]; }
        q += k;
    }
    return s;
}
static void batch_sums(void) {
    static uint64_t pairs[2 * RING];
    for (int round = 0; round < 300; round++) {
        for (int i = 0; i < RING; i++) { pool[2 * i] = rnd() % 5000; pool[2 * i + 1] = (rnd() % 17) != 0; }
        uint64_t q = (uint64_t)round * 977u + rnd() % RING; int nr = 1 + (int)(rnd() % 60);
        uint64_t q0s[64], q1s[64]; int kind[64]; uint32_t v[64], mode[64];
        oqb_plan p = {0};
        for (int i = 0; i < nr; i++) { uint64_t len = rnd() % 4 ? rnd() % 200 : 0; q0s[i] = q; q1s[i] = q + len; q += len; kind[i] = rnd() % 5 ? 4 : 5; v[i] = rnd() % 3; mode[i] = (rnd() % 7) == 0; oqb_add(&p, q0s[i], q1s[i]); }
        if (!p.found) continue;
        CHECK(oqb_ok(&p, RING), "round %d: a chain not planned", round);
        for (uint64_t s = p.first; s < p.last;) {   // oq_batch_init's reads
            uint32_t k = oqb_chunk(s, p.last, RING, BATCH), sl = (uint32_t)(s % RING);
            memcpy(&pairs[2 * (s - p.first)], &pool[2 * sl], 16 * (size_t)k); s += k;
        }
        uint64_t acc_a = 0, acc_b = 0, un_a = 0, un_b = 0;
        for (int i = 0; i < nr; i++) {
            if (kind[i] != 4) continue;
            uint64_t sa = v[i] + per_report(q0s[i], q1s[i], &un_a);
            uint64_t sb = v[i] + (q1s[i] > q0s[i] ? oqb_sum(&pairs[2 * (q0s[i] - p.first)], (size_t)(q1s[i] - q0s[i]), &un_b) : 0);
            if (mode[i]) acc_a = acc_b = 0;
            acc_a += sa; acc_b += sb;
            CHECK(acc_a == acc_b, "round %d report %d: by report %llu, by list %llu", round, i, (unsigned long long)acc_a, (unsigned long long)acc_b);
        }
        CHECK(un_a == un_b, "round %d: unavailable by report %llu, by list %llu", round, (unsigned long long)un_a, (unsigned long long)un_b);
    }
}

int main(void) {
    index_random(); index_wrap(); index_full();
    batch_plan(); batch_sums();
    if (fails) { printf("t_android_rcix: %d failures\n", fails); return 1; }
    printf("t_android_rcix: ok\n");
    return 0;
}
