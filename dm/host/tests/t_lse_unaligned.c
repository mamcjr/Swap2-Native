// Adapted from Banana Native (MIT), dm/host/tests/t_lse_unaligned.c of commit e6b6f0d0 (v0.3.40).
// Unaligned LSE atomics (dm/host/lse_unaligned.h): the instruction found in issue #343 (main+0x104e4c of Ocarina of Time, LDADD w9, w8, [x8] with x8 = ...5f1), each other form,
// the bytes beside the access left alone, an access that crosses a 16-byte block, and threads adding through it at once
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../lse_unaligned.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static _Alignas(64) uint8_t buf[256];
static void clear(void) { memset(buf, 0xA5, sizeof buf); }

// the register encodings: size 10 / 11 (w / x), A = 0, R = 0
#define LDOP(size, opc, rs, rn, rt) (((size) << 30) | 0x38200000u | ((rs) << 16) | ((opc) << 12) | ((rn) << 5) | (rt))
#define SWP(size, rs, rn, rt) (((size) << 30) | 0x38208000u | ((rs) << 16) | ((rn) << 5) | (rt))
#define CAS(size, rs, rn, rt) (((size) << 30) | 0x08A07C00u | ((rs) << 16) | ((rn) << 5) | (rt))

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

typedef struct { uint8_t *at; int n; } job;
static void *adder(void *a) {
    job *j = a; lse_regs r; memset(&r, 0, sizeof r);
    for (int i = 0; i < j->n; i++) { r.x[8] = (uint64_t)(uintptr_t)j->at; r.x[9] = 1; if (!lse_emulate(0xb8290108u, &r, r.x[8])) return (void *)1; }
    return NULL;
}
static void *aligned_adder(void *a) {   // an ordinary aligned atomic add on the bytes beside the unaligned access
    job *j = a; for (int i = 0; i < j->n; i++) __atomic_fetch_add((uint32_t *)j->at, 1, __ATOMIC_SEQ_CST); return NULL;
}

int main(void) {
    // the instruction of the crash: ldadd w9, w8, [x8] = 0xb8290108 (the bytes 08 01 29 b8 at main+0x104e4c), x8 odd, x9 = 1
    lse_insn d;
    CHECK(lse_decode(0xb8290108u, &d) && d.op == LSE_OP_ADD && d.bytes == 4 && d.rs == 9 && d.rt == 8 && d.rn == 8, "decode the crash's instruction");
    clear(); uint32_t v = 0x11223344; memcpy(buf + 0x51, &v, 4);
    lse_regs r; memset(&r, 0, sizeof r); r.x[8] = (uint64_t)(uintptr_t)(buf + 0x51); r.x[9] = 1;
    CHECK(lse_emulate(0xb8290108u, &r, r.x[8]), "emulated");
    CHECK(rd32(buf + 0x51) == 0x11223345 && r.x[8] == 0x11223344, "the add is made, the old value is in w8 (zero extended): mem %#x, x8 %#llx", rd32(buf + 0x51), (unsigned long long)r.x[8]);
    CHECK(buf[0x50] == 0xA5 && buf[0x55] == 0xA5, "the bytes beside it are as they were");
    // an aligned address is not for this code, an address that is not the fault's neither, and other instructions are not decoded
    memset(&r, 0, sizeof r); r.x[8] = (uint64_t)(uintptr_t)(buf + 0x50); r.x[9] = 1;
    CHECK(!lse_emulate(0xb8290108u, &r, r.x[8]) && !lse_emulate(0xb8290108u, &r, 1), "an aligned address, or another one than the fault's, is not emulated");
    CHECK(!lse_decode(0xb9400109u, &d) && !lse_decode(0x38bfc000u, &d) && !lse_decode(0x08a07c00u, &d), "ldr, ldapr and a byte cas are not decoded");

    // each operation, 32 bit, at every offset of a 16-byte block (also across the two 8-byte halves) and the four neighbours unchanged
    static const struct { int opc; uint32_t mem, rs, want; } ops[] = {
        {0, 0xFFFFFFFE, 3, 1}, {1, 0xF0F0F0F0, 0x0FF00FF0, 0xF000F000}, {2, 0xF0F0F0F0, 0xFFFF0000, 0x0F0FF0F0}, {3, 0x0000FFFF, 0xFF000000, 0xFF00FFFF},
        {4, 0xFFFFFFFF, 5, 5}, {4, 7, 5, 7}, {5, 0xFFFFFFFF, 5, 0xFFFFFFFF}, {5, 7, 5, 5}, {6, 0xFFFFFFFF, 5, 0xFFFFFFFF}, {6, 7, 5, 7}, {7, 0xFFFFFFFF, 5, 5}, {7, 7, 5, 5},
    };
    for (unsigned k = 0; k < sizeof ops / sizeof *ops; k++) for (unsigned off = 1; off < 16; off++) {
        if (!(off & 3)) continue;
        clear(); memcpy(buf + 32 + off, &ops[k].mem, 4);
        memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 32 + off); r.x[2] = ops[k].rs;
        int ok = lse_emulate(LDOP(2u, ops[k].opc, 2u, 1u, 3u), &r, r.x[1]);
        CHECK(ok && rd32(buf + 32 + off) == ops[k].want && r.x[3] == ops[k].mem && buf[31 + off] == 0xA5 && buf[36 + off] == 0xA5,
              "op %d mem %#x rs %#x at +%u: got %#x old %#llx", ops[k].opc, ops[k].mem, ops[k].rs, off, rd32(buf + 32 + off), (unsigned long long)r.x[3]);
    }
    // 64 bit: add at +1 inside a block, and across the 8-byte halves (+9, +12 crosses nothing at 16, +9 + 8 = 17 crosses the block: the lock)
    for (unsigned off = 1; off < 16; off += 2) {
        clear(); uint64_t m = 0x0102030405060708ull; memcpy(buf + 32 + off, &m, 8);
        memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 32 + off); r.x[2] = 0x10;
        int ok = lse_emulate(LDOP(3u, 0, 2u, 1u, 3u), &r, r.x[1]);
        CHECK(ok && rd64(buf + 32 + off) == m + 0x10 && r.x[3] == m && buf[31 + off] == 0xA5 && buf[40 + off] == 0xA5, "64 bit add at +%u: %#llx", off, (unsigned long long)rd64(buf + 32 + off));
    }
    // a 32 bit access across the block (+14 .. +17): the locked path
    clear(); v = 0xFFFFFFFF; memcpy(buf + 32 + 14, &v, 4);
    memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 32 + 14); r.x[2] = 2;
    CHECK(lse_emulate(LDOP(2u, 0, 2u, 1u, 3u), &r, r.x[1]) && rd32(buf + 32 + 14) == 1 && r.x[3] == 0xFFFFFFFF && buf[32 + 13] == 0xA5 && buf[32 + 18] == 0xA5, "across a 16-byte block");
    // swap, compare and swap (Rs expected and receives the old value, Rt the new one), and the result register XZR (stadd)
    clear(); v = 0xAABBCCDD; memcpy(buf + 33, &v, 4);
    memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 33); r.x[2] = 0x12345678;
    CHECK(lse_emulate(SWP(2u, 2u, 1u, 3u), &r, r.x[1]) && rd32(buf + 33) == 0x12345678 && r.x[3] == 0xAABBCCDD, "swp");
    memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 33); r.x[2] = 0x12345678; r.x[3] = 0x99;
    CHECK(lse_emulate(CAS(2u, 2u, 1u, 3u), &r, r.x[1]) && rd32(buf + 33) == 0x99 && r.x[2] == 0x12345678, "cas that matches stores Rt");
    memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 33); r.x[2] = 5; r.x[3] = 0x77;
    CHECK(lse_emulate(CAS(2u, 2u, 1u, 3u), &r, r.x[1]) && rd32(buf + 33) == 0x99 && r.x[2] == 0x99, "cas that does not match leaves memory and returns the old value in Rs");
    memset(&r, 0, sizeof r); r.x[1] = (uint64_t)(uintptr_t)(buf + 33); r.x[2] = 1;
    CHECK(lse_emulate(LDOP(2u, 0, 2u, 1u, 31u), &r, r.x[1]) && rd32(buf + 33) == 0x9A && !r.x[31], "stadd (result XZR) adds and returns nothing");

    // the same bytes added through the emulation by 4 threads and through an aligned atomic add by 2 more (the aligned word beside it, in the same 16-byte block)
    clear(); memset(buf + 64, 0, 32);
    enum { N = 20000 };
    job a = {buf + 65, N}, b = {buf + 69, N}, c = {buf + 76, N};   // 65..68 (unaligned), 69..72 (unaligned), 76..79 (aligned word)
    pthread_t t[6]; void *res[6];
    pthread_create(&t[0], 0, adder, &a); pthread_create(&t[1], 0, adder, &a); pthread_create(&t[2], 0, adder, &b); pthread_create(&t[3], 0, adder, &b);
    pthread_create(&t[4], 0, aligned_adder, &c); pthread_create(&t[5], 0, aligned_adder, &c);
    for (int i = 0; i < 6; i++) pthread_join(t[i], &res[i]);
    CHECK(!res[0] && !res[1] && !res[2] && !res[3], "no thread failed");
    CHECK(rd32(buf + 65) == 2 * N && rd32(buf + 69) == 2 * N && rd32(buf + 76) == 2 * N, "no add lost: %u %u %u of %u", rd32(buf + 65), rd32(buf + 69), rd32(buf + 76), 2 * N);

    printf(fails ? "t_lse_unaligned: %d FAILED\n" : "t_lse_unaligned: all passed\n", fails);
    return fails != 0;
}
