#!/usr/bin/env python3
"""vk_android_spv.group_cached_loads: the cached raw loads of one base in one block (cache_raw_loads' calls) read as one group. A shader written the way the
translator writes one (test_vk_raw_global.py's Asm and Fn) reads four runs of four words through one base (+0, +16, +32, +48), one word at +64, a run through a
second base and a word of the first base again after a store; the module after raw_global_memory and after group_cached_loads too is run in the interpreter over
addresses in and out of the pools, groups that leave their pool or cross into the next one, low words that wrap and pools that touch or overlap: the same words,
the same stores and the same log. The physical reads may come in another order (the group reads its words where its first call was)"""
import random
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / 'tools'))
import vk_android_spv as spv  # noqa: E402
import vk_raw_group as grp  # noqa: E402
import test_vk_raw_global as rg  # noqa: E402

M32, M64 = rg.M32, (1 << 64) - 1


def shader(second_base_between=True):
    """→ (module bytes, buffers). main reads a base from io 0, 1 and a second from io 2, 3; reads base + 0..60 (four runs of four words, consecutive calls), then
    the second base + 8, + 12 (a run of two) when second_base_between, then base + 64 (one word): all of them stored to io 8..; stores 7 through the store helper at
    io 4, 5; reads base + 0 again into the last io word (after the store: a new group)"""
    a = rg.Asm(); I, U, B = a.int, a.uint, a.bool
    table, log, io = a.buffer(0), a.buffer(1), a.buffer(2)
    hl = rg.load_helper(a, table)
    hs = rg.store_helper(a, table, log)
    f = rg.Fn(a, a.void, [])
    def rd(k): return f.val(124, I, f.val(61, U, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, k))))
    def wr(k, v): f.op(62, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, k)), f.val(124, U, f.ld(I, v)))
    def offs(lo, hi, c):
        lk = f.local(I, f.val(128, I, f.ld(I, lo), a.c(I, c)))
        ck = f.local(B, f.val(176, B, f.val(124, U, f.ld(I, lk)), f.val(124, U, f.ld(I, lo))))
        sk = f.local(I, f.val(169, I, f.ld(B, ck), a.c(I, 1), a.c(I, 0)))
        return lk, f.local(I, f.val(128, I, f.ld(I, hi), f.ld(I, sk)))
    def call(lo, hi):
        al, ah = f.local(I, f.ld(I, lo)), f.local(I, f.ld(I, hi))
        return f.local(I, f.val(57, I, hl.id, al, ah))
    lo, hi = f.local(I, rd(0)), f.local(I, rd(1))
    lo2, hi2 = f.local(I, rd(2)), f.local(I, rd(3))
    res = [call(*((lo, hi) if c == 0 else offs(lo, hi, c))) for c in range(0, 64, 4)]
    if second_base_between: res += [call(*offs(lo2, hi2, c)) for c in (8, 12)]
    res.append(call(*offs(lo, hi, 64)))
    for k, r in enumerate(res): wr(8 + k, r)
    sl, sh, sv = f.local(I, rd(4)), f.local(I, rd(5)), f.local(I, a.c(I, 7))
    f.val(57, a.void, hs.id, sl, sh, sv)
    b0, b1 = f.local(I, rd(0)), f.local(I, rd(1))
    wr(8 + len(res), call(b0, b1))
    f.op(253)
    return a.data(f), {table: 'tab', log: 'log', io: 'io'}, len(res) + 1


def grouped(data):
    m = spv.Module(data)
    st = spv.raw_global_memory(m)
    cached = m.data()
    n = grp.group_cached_loads(m)
    return cached, m.data(), st, n


class Grouped(unittest.TestCase):
    def test_counts_and_valid(self):
        for between in (True, False):
            data, _, _ = shader(between)
            cached, new, st, (groups, calls) = grouped(data)
            self.assertEqual(st['runs'], {'1': 2, '2': 1, '4': 4} if between else {'1': 2, '4': 4}, st)
            self.assertEqual((groups, calls), (1, 4), 'the four runs of four through one base are one group of 16 words; the word at +64 would make 17 (GMEM_GROUP_WORDS)')
            for d in (cached, new):
                v = rg.spirv_val(d)
                if v: self.assertEqual(v[0], 0, v[1])
            m = spv.Module(new)
            self.assertFalse(any(x[0] & 0xffff == spv.OP_VARIABLE and x[3] == spv.SC_PRIVATE for x in m.ins))
            again = spv.Module(new)
            self.assertEqual(grp.group_cached_loads(again), (0, 0), 'a second run finds nothing to group')

    def test_same_words(self):
        rnd = random.Random(11)
        for between in (True, False):
            for pools in (rg.some_pools(40), rg.some_pools(rg.ENTRIES - 1)):
                table = rg.pool_table(pools)
                data, bufs, nout = shader(between)
                cached, new, _, _ = grouped(data)
                ads = rg.addresses(pools if len(pools) < 100 else pools[:3] + pools[-3:], rnd)
                ads += [e - d for _, e, _ in pools[:12] for d in (16, 32, 48, 60, 64, 68)]   # groups that leave their pool partway
                for i, a in enumerate(ads):
                    b = ads[(i * 7 + 3) % len(ads)]
                    s = ads[(i * 5 + 1) % len(ads)] if i % 3 else a
                    io = {0: a & M32, 1: a >> 32, 2: b & M32, 3: b >> 32, 4: s & M32, 5: s >> 32}
                    want, got = rg.run_main(cached, bufs, table, io), rg.run_main(new, bufs, table, io)
                    self.assertEqual((got[0], got[2], got[3]), (want[0], want[2], want[3]), f'base {a:#x}, second {b:#x}, store {s:#x}')
                    self.assertEqual(sorted(set(got[1])), sorted(set(want[1])), 'the same physical words read')
                    for k in range(16):
                        w = rg.model(table, (a + 4 * k) & M64)
                        self.assertEqual(got[0].get(8 + k, 0), 0 if w is None else rg.word_at(w), f'word {k} of {a:#x}')


if __name__ == '__main__':
    unittest.main()
