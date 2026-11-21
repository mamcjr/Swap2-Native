// Adapted from Banana Native (MIT), dm/host/lse_unaligned.h of commit e6b6f0d0 (v0.3.40; unchanged in v0.3.42). "Issue #343" below is Banana Native's issue tracker.
// Unaligned LSE atomics for a core without FEAT_LSE2 (Armv8.4): what the game's atomic read-modify-write on an address that is not a multiple of its size does there.
// Before Armv8.4 such an LDADD / SWP / CAS raises an alignment fault (SIGBUS, BUS_ADRALN) whatever the address; with FEAT_LSE2 it is one atomic access while the bytes stay inside
// one 16-byte block, and the Switch 2's code relies on that: Ocarina of Time's intrusive reference count of an object embedded at +0xd1 of another (main+0x104e4c, LDADD w9, w8, [x8],
// x8 = 0x...5f1) is the case found. Snapdragon 888 (Cortex-X1, A78, A55: Armv8.2) has no FEAT_LSE2 (issue #343). crash.c decodes the faulting instruction here and does it again, atomically
// on the aligned 16-byte block that holds the bytes (LDAXP / STLXP), so a neighbour's aligned atomic access to the same bytes is ordered with it as on the console. Bytes that cross a
// 16-byte block (the console's access would not be one atomic access either) are done under a lock, atomic only against another unaligned access of this kind.
// Done: LDADD LDCLR LDEOR LDSET LDSMAX LDSMIN LDUMAX LDUMIN, SWP and CAS, 32 and 64 bits, every acquire / release form (all done with acquire and release, which is stronger), and STADD..STUMIN
// (the same with the result register XZR). The 8 and 16 bit forms cannot be unaligned. Pure: the registers go in and out through lse_regs, so a test runs it without a signal.
#pragma once
#include <stdint.h>
#include <string.h>
#include <pthread.h>

typedef struct { uint64_t x[32]; uint64_t sp; } lse_regs;   // x[31] is not used: a register number 31 is XZR, or SP as the base

enum { LSE_OP_ADD, LSE_OP_CLR, LSE_OP_EOR, LSE_OP_SET, LSE_OP_SMAX, LSE_OP_SMIN, LSE_OP_UMAX, LSE_OP_UMIN, LSE_OP_SWP, LSE_OP_CAS };
typedef struct { int op; unsigned bytes, rs, rt, rn; } lse_insn;

// Decode an LSE atomic memory instruction; 1 when it is one of those above (32 or 64 bit), else 0
static inline int lse_decode(uint32_t insn, lse_insn *d) {
    unsigned size = insn >> 30;
    if (size < 2) return 0;
    d->bytes = 1u << size; d->rs = (insn >> 16) & 31; d->rt = insn & 31; d->rn = (insn >> 5) & 31;
    if ((insn & 0x3F200C00u) == 0x38200000u) {   // LD<OP>{A}{L} / SWP{A}{L}: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt
        unsigned o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        if (o3) { if (opc) return 0; d->op = LSE_OP_SWP; return 1; }   // o3 = 1 with another opc is LDAPR or a later extension
        d->op = (int)opc; return 1;
    }
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) { d->op = LSE_OP_CAS; return 1; }   // CAS{A}{L}: size 0010001 L 1 Rs o0 11111 Rn Rt
    return 0;
}

// The new memory value for the old one: *store is 0 when the instruction leaves the memory as it is (a CAS whose compare failed)
static inline __attribute__((always_inline)) uint64_t lse_apply(const lse_insn *d, uint64_t old, uint64_t rs, uint64_t rt, int *store) {
    uint64_t m = d->bytes == 8 ? ~0ull : 0xFFFFFFFFull, s = rs & m, n = 0;
    int64_t so = d->bytes == 8 ? (int64_t)old : (int32_t)old, ss = d->bytes == 8 ? (int64_t)s : (int32_t)s;
    *store = 1;
    switch (d->op) {
    case LSE_OP_ADD: n = old + s; break;
    case LSE_OP_CLR: n = old & ~s; break;
    case LSE_OP_EOR: n = old ^ s; break;
    case LSE_OP_SET: n = old | s; break;
    case LSE_OP_SMAX: n = so > ss ? old : s; break;
    case LSE_OP_SMIN: n = so < ss ? old : s; break;
    case LSE_OP_UMAX: n = old > s ? old : s; break;
    case LSE_OP_UMIN: n = old < s ? old : s; break;
    case LSE_OP_SWP: n = s; break;
    default: if (old == s) n = rt & m; else *store = 0; break;   // CAS: Rs is the expected value, Rt the new one
    }
    return n & m;
}

static pthread_mutex_t g_lse_mu = PTHREAD_MUTEX_INITIALIZER;

// Do the instruction on the bytes at addr: the old value comes back in *old
static inline int lse_do(const lse_insn *d, uint64_t addr, uint64_t rs, uint64_t rt, uint64_t *old) {
    unsigned off = (unsigned)(addr & 15), n = d->bytes;
    if (off + n <= 16) {
        uint64_t *p = (uint64_t *)(uintptr_t)(addr & ~15ull);
        uint64_t lo, hi, nlo, nhi, ov = 0; uint32_t fail;
#if defined(__aarch64__)
        do {   // the new block is made from registers alone between the two exclusive accesses
            __asm__ volatile("ldaxp %0, %1, [%2]" : "=&r"(lo), "=&r"(hi) : "r"(p) : "memory");
            unsigned __int128 blk = ((unsigned __int128)hi << 64) | lo;   // little endian: the byte at addr + k is bits 8 * (off + k)
            unsigned sh = off * 8;
            uint64_t m = n == 8 ? ~0ull : 0xFFFFFFFFull;
            int st;
            ov = (uint64_t)(blk >> sh) & m;
            uint64_t nv = lse_apply(d, ov, rs, rt, &st);
            if (!st) { __asm__ volatile("clrex" ::: "memory"); fail = 0; continue; }
            blk = (blk & ~((unsigned __int128)m << sh)) | ((unsigned __int128)nv << sh);
            nlo = (uint64_t)blk; nhi = (uint64_t)(blk >> 64);
            __asm__ volatile("stlxp %w0, %1, %2, [%3]" : "=&r"(fail) : "r"(nlo), "r"(nhi), "r"(p) : "memory");
        } while (fail);
        *old = ov;
        return 1;
#else
        (void)lo; (void)hi; (void)nlo; (void)nhi; (void)ov; (void)fail; (void)p;
#endif
    }
    pthread_mutex_lock(&g_lse_mu);   // across a 16-byte block
    uint64_t ov2 = 0; memcpy(&ov2, (void *)(uintptr_t)addr, n);
    int st; uint64_t nv = lse_apply(d, ov2, rs, rt, &st);
    if (st) memcpy((void *)(uintptr_t)addr, &nv, n);
    pthread_mutex_unlock(&g_lse_mu);
    *old = ov2;
    return 1;
}

// Emulate the instruction for the registers r (a SIGBUS at addr): 1 when done (the caller moves the pc on by 4), 0 when it is not one of these or the fault address is not its address
static inline int lse_emulate(uint32_t insn, lse_regs *r, uint64_t fault_addr) {
    lse_insn d;
    if (!lse_decode(insn, &d)) return 0;
    uint64_t addr = d.rn == 31 ? r->sp : r->x[d.rn];
    if (addr != fault_addr || !(addr & (d.bytes - 1))) return 0;   // an aligned address does not fault here
    uint64_t rs = d.rs == 31 ? 0 : r->x[d.rs], rt = d.rt == 31 ? 0 : r->x[d.rt], old;
    lse_do(&d, addr, rs, rt, &old);
    if (d.op == LSE_OP_CAS) { if (d.rs != 31) r->x[d.rs] = old; }   // CAS returns the old value in Rs
    else if (d.rt != 31) r->x[d.rt] = old;                            // the others in Rt; with Rt = XZR (STADD...) it is dropped
    return 1;
}
