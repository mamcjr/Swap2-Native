"""The Vulkan library's raw global memory passes (vk_android_spv.py raw_global_memory: value_params, promote_locals, loop_pool_search, cache_raw_loads), adapted from
Banana Native's (eden/v0.3.11 test_vk_gmem_helpers.py, test_vk_gmem_cache.py) to the helper this translator emits (GlobalToStorage.cs GenerateRawAccess: a 12-step
search over the 4,096-entry pool table, the inPool branch, the hash-set write log, the access through a physical pointer).

The modules are written here word by word the way the translator writes its own (Asm): every intermediate in a Function variable stored once, table words kept in
float variables, helper arguments passed as pointers to temporaries, an address with an offset as low + c and high + (low + c < low ? 1 : 0). No compiler is needed;
spirv-val is used when it is on PATH. A small SPIR-V interpreter (Interp) runs a module's entry point, or one function, over a pool table, a write log and a
physical memory, and records every physical read and write: the passes must leave all of them as they were.

DMSH_VK_RAW_LIB=<dmsh_vk.lib> also runs the helpers of a real library's shaders before and after (RealLibrary; the differential over a whole library is
the same check, run from outside the repository because the library is game data)."""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import vk_android_spv as spv  # noqa: E402

M32, M64 = 0xffffffff, (1 << 64) - 1
ENTRIES, EW = 1 << spv.GMEM_MAP_LOG2, spv.GMEM_MAP_ENTRY_WORDS   # the Android host's table (vka_draw.c VKA_GMEM_MAP_ENTRIES)
XL_ENTRIES = 1 << spv.XL_GMEM_MAP_LOG2   # the translator's search covers this many (dmsh_abi.h DMSH_GMEM_MAP_ENTRIES)


def word_at(addr): return (addr * 0x9e3779b97f4a7c15 >> 17) & M32   # what the interpreter's physical memory holds at an address nothing wrote


def sg(v): return v - (1 << 32) if v & 0x80000000 else v


BIN = {128: lambda a, b: (a + b) & M32, 130: lambda a, b: (a - b) & M32, 132: lambda a, b: (a * b) & M32, 194: lambda a, b: a >> (b & 31), 196: lambda a, b: (a << (b & 31)) & M32,
       195: lambda a, b: (sg(a) >> (b & 31)) & M32, 197: lambda a, b: a | b, 198: lambda a, b: a ^ b, 199: lambda a, b: a & b, 170: lambda a, b: a == b, 171: lambda a, b: a != b,
       176: lambda a, b: a < b, 178: lambda a, b: a <= b, 177: lambda a, b: sg(a) < sg(b), 179: lambda a, b: sg(a) <= sg(b), 172: lambda a, b: a > b, 173: lambda a, b: sg(a) > sg(b),
       174: lambda a, b: a >= b, 175: lambda a, b: sg(a) >= sg(b), 166: lambda a, b: a or b, 167: lambda a, b: a and b, 164: lambda a, b: a == b, 165: lambda a, b: a != b}
ATOMIC = {229: lambda o, v: v, 234: lambda o, v: (o + v) & M32, 235: lambda o, v: (o - v) & M32, 236: lambda o, v: min(o, v, key=sg), 237: min, 238: lambda o, v: max(o, v, key=sg),
          239: max, 240: lambda o, v: o & v, 241: lambda o, v: o | v, 242: lambda o, v: o ^ v}


class Interp:
    """One invocation of a module's functions: 32-bit values as unsigned integers (a float is its bits: the helpers only move them), booleans, tuples for vectors;
    pointers ('tab' | 'log' | 'io', word), ('phys', address) and ('v', n) for a Function variable. table: the pool table's words (a read past it raises); log and
    phys: the words written (a physical word nothing wrote is word_at); reads / writes: every physical access in order. A buffer given a number instead of a
    name (a real shader's other buffers) holds a fixed word at each index until it is written (other)"""
    def __init__(self, m, table, bufs, io=None):
        self.m, self.table, self.bufs = m, table, bufs   # bufs: {variable id: 'tab' | 'log' | 'io' | a number}
        self.log, self.phys, self.io, self.mem, self.reads, self.writes, self.steps, self.nvar, self.other = {}, {}, dict(io or {}), {}, [], [], 0, 0, {}
        self.glob = {i: (v[0] if len(v) == 1 else v[0] | v[1] << 32) for i, (_, v) in m.consts.items()}
        for x in m.ins:
            op = x[0] & 0xffff
            if op in (41, 42): self.glob[x[2]] = op == 41
            elif op == spv.OP_CONSTANT_NULL: self.glob[x[2]] = (0,) * m.types[x[1]][3] if m.op(x[1]) == spv.OP_TYPE_VECTOR else 0
            elif op == spv.OP_CONSTANT_COMPOSITE: self.glob[x[2]] = tuple(self.glob[i] for i in x[3:])
            elif op == spv.OP_FUNCTION: break
        self.fns = {}

    def rd(self, p):
        if p[0] == 'tab': return self.table[p[1]]
        if p[0] == 'phys': self.reads.append(p[1]); return self.phys.get(p[1], word_at(p[1]))
        if p[0] == 'v':
            v = self.mem[p]
            return tuple(v[i] for i in range(len(v))) if isinstance(v, dict) else v   # (an array a loop filled element by element)
        if p[0] == 've': v = self.mem[('v', p[1])]; return v[p[2]]
        if p[0] in ('log', 'io'): return (self.log if p[0] == 'log' else self.io).get(p[1], 0)
        return self.other.get(p, word_at(p[0] << 24 ^ (p[1] if isinstance(p[1], int) else p[1][0] * 4 + p[1][1])))

    def wr(self, p, v):
        if p[0] == 'phys': self.writes.append((p[1], v)); self.phys[p[1]] = v
        elif p[0] == 'v': self.mem[p] = v
        elif p[0] == 've':
            a = self.mem[('v', p[1])]
            a = dict(enumerate(a)) if isinstance(a, tuple) else a if isinstance(a, dict) else {}
            a[p[2]] = v; self.mem[('v', p[1])] = a
        elif p[0] == 'tab': raise AssertionError('a store to the pool table')
        elif p[0] in ('log', 'io'): (self.log if p[0] == 'log' else self.io)[p[1]] = v
        else: self.other[p] = v

    def call(self, f, args):
        m = self.m
        if f not in self.fns:
            a, e = m.function_range(f); body = m.ins[a:e + 1]
            self.fns[f] = body, {x[1]: k for k, x in enumerate(body) if x[0] & 0xffff == spv.OP_LABEL}
        body, labels = self.fns[f]
        val = dict(self.glob)
        for x, v in zip([x for x in body if x[0] & 0xffff == spv.OP_FUNCTION_PARAMETER], args): val[x[2]] = v
        k = next(i for i, x in enumerate(body) if x[0] & 0xffff == spv.OP_LABEL)
        blk = prev = None
        while True:
            self.steps += 1
            assert self.steps < 5_000_000, 'interpreter: too many steps'
            x = body[k]; op = x[0] & 0xffff; k += 1
            if op == spv.OP_LABEL: prev, blk = blk, x[1]; continue
            if op in (spv.OP_SELECTION_MERGE, spv.OP_LOOP_MERGE): continue
            if op == spv.OP_PHI:   # every OpPhi of a block reads the values of the block before (not another phi's new value)
                j = k - 1; new = {}
                while body[j][0] & 0xffff == spv.OP_PHI:
                    y = body[j]; new[y[2]] = next(val[v] for v, p in zip(y[3::2], y[4::2]) if p == prev); j += 1
                val.update(new); k = j; continue
            if op == spv.OP_VARIABLE: self.nvar += 1; val[x[2]] = ('v', self.nvar); self.mem[val[x[2]]] = None; continue
            if op == spv.OP_BRANCH: k = labels[x[1]]; continue
            if op == spv.OP_BRANCH_CONDITIONAL: k = labels[x[2] if val[x[1]] else x[3]]; continue
            if op == spv.OP_RETURN_VALUE: return val[x[1]]
            if op == spv.OP_RETURN: return None
            if op == spv.OP_STORE: self.wr(val[x[1]], val[x[2]]); continue
            if op == 228: self.wr(val[x[1]], val[x[4]]); continue   # OpAtomicStore
            r = x[2]
            if op == spv.OP_LOAD: val[r] = self.rd(val[x[3]])
            elif op == spv.OP_ACCESS_CHAIN and x[3] not in self.bufs and isinstance(val.get(x[3]), tuple) and val[x[3]][0] == 'v' and len(x) == 5:
                val[r] = ('ve', val[x[3]][1], val[x[4]])   # an element of a Function array variable
            elif op == spv.OP_ACCESS_CHAIN:
                assert x[3] in self.bufs and len(x) in (6, 7) and val[x[4]] == 0, f'interpreter: access chain {x}'
                val[r] = (self.bufs[x[3]], val[x[5]] if len(x) == 6 else (val[x[5]], val[x[6]]))   # (a word of a word array, or a component of a row of a vec4 array)
            elif op == spv.OP_FUNCTION_CALL: val[r] = self.call(x[3], [val[i] for i in x[4:]])
            elif op in (spv.OP_BITCAST, spv.OP_COPY_OBJECT):
                v = val[x[3]]
                val[r] = v[0] | v[1] << 32 if isinstance(v, tuple) and m.op(x[1]) == spv.OP_TYPE_INT else v
            elif op == spv.OP_CONVERT_U_TO_PTR: val[r] = ('phys', val[x[3]])
            elif op == spv.OP_COMPOSITE_CONSTRUCT: val[r] = tuple(val[i] for i in x[3:])
            elif op == spv.OP_COMPOSITE_EXTRACT:
                v = val[x[3]]
                for i in x[4:]: v = v[i]
                val[r] = v
            elif op == spv.OP_VECTOR_INSERT_DYNAMIC:
                v = list(val[x[3]]); v[val[x[5]]] = val[x[4]]; val[r] = tuple(v)
            elif op == 168: val[r] = not val[x[3]]   # OpLogicalNot
            elif op == 200: val[r] = ~val[x[3]] & M32   # OpNot
            elif op == spv.OP_SELECT: val[r] = val[x[4]] if val[x[3]] else val[x[5]]
            elif op == 201:   # OpBitFieldInsert base insert offset count
                b, i, o, c = (val[y] for y in x[3:7]); mask = ((1 << c) - 1) << o & M32 if c < 32 else M32
                val[r] = (b & ~mask | (i << o) & mask) & M32
            elif op in (202, 203):   # OpBitFieldSExtract / UExtract base offset count
                b, o, c = (val[y] for y in x[3:6]); v = (b >> o) & ((1 << c) - 1)
                val[r] = (v - (1 << c)) & M32 if op == 202 and c and v >> (c - 1) else v
            elif op == 227: val[r] = self.rd(val[x[3]])   # OpAtomicLoad
            elif op == 230:   # OpAtomicCompareExchange pointer scope equal unequal value comparator
                p = val[x[3]]; old = self.rd(p)
                if old == val[x[8]]: self.wr(p, val[x[7]])
                val[r] = old
            elif op in ATOMIC:
                p = val[x[3]]; old = self.rd(p); self.wr(p, ATOMIC[op](old, val[x[6]])); val[r] = old
            elif op in BIN: val[r] = BIN[op](val[x[3]], val[x[4]])
            else: raise NotImplementedError(f'interpreter: opcode {op}')


# ---- modules as the translator writes them
class Asm:
    def __init__(self):
        self.n, self.types, self.cons, self.decl, self.deco, self.fns = 1, {}, {}, [], [], []
        self.void, self.bool, self.int, self.uint, self.float = self.ty(19), self.ty(20), self.ty(21, 32, 1), self.ty(21, 32, 0), self.ty(22, 32)
        self.u64 = self.ty(21, 64, 0); self.v2 = self.ty(23, self.uint, 2)
        self.pphys, self.ptu = self.ty(32, 5349, self.uint), self.ty(32, 12, self.uint)
        self.ra = self.ty(29, self.uint); self.deco.append([71, self.ra, 6, 4])

    def id(self): self.n += 1; return self.n - 1

    def ty(self, op, *a):
        if (op, *a) not in self.types: i = self.id(); self.types[(op, *a)] = i; self.decl.append([op, i, *a])
        return self.types[(op, *a)]

    def c(self, ty, v):
        if (ty, v) not in self.cons: i = self.id(); self.cons[(ty, v)] = i; self.decl.append([43, ty, i, v & M32])
        return self.cons[(ty, v)]

    def buffer(self, binding):
        st, var = self.id(), self.id()
        self.decl += [[30, st, self.ra], [32, self.id(), 12, st]]; self.decl.append([59, self.decl[-1][1], var, 12])
        self.deco += [[71, st, 2], [72, st, 0, 35, 0], [71, var, 34, 0], [71, var, 33, binding]]
        return var

    def data(self, main):
        lit = spv.lit_string
        ins = [[17, 1], [17, 5347], [17, 11], [10, *lit('SPV_KHR_physical_storage_buffer')], [14, 5348, 1], [15, 5, main.id, *lit('main')], [16, main.id, 17, 1, 1, 1],
               *self.deco, *self.decl]
        for f in self.fns: ins += f.words()
        words = [spv.MAGIC, 0x10300, 0, self.n, 0] + [w for x in ins for w in (len(x) << 16 | x[0], *x[1:])]
        return struct.pack('<%dI' % len(words), *words)


class Fn:
    """A function as the translator writes one: parameters are pointers to Function variables, local() stores a value into a variable of its own"""
    def __init__(self, a, ret, params):
        self.a, self.ret, self.id, self.first, self.vars, self.code = a, ret, a.id(), a.id(), [], []
        self.ptys = [a.ty(32, 7, t) for t in params]; self.params = [a.id() for _ in params]
        self.fty = a.ty(33, ret, *self.ptys); a.fns.append(self)

    def op(self, o, *w): self.code.append([o, *w])

    def val(self, o, ty, *w):
        r = self.a.id(); self.code.append([o, ty, r, *w]); return r

    def local(self, ty, value):
        v = self.a.id(); self.vars.append([59, self.a.ty(32, 7, ty), v, 7]); self.op(62, v, value); return v

    def ld(self, ty, v): return self.val(61, ty, v)

    def words(self):
        return [[54, self.ret, self.id, 0, self.fty]] + [[55, t, p] for t, p in zip(self.ptys, self.params)] + [[248, self.first]] + self.vars + self.code + [[56]]


def table_word(f, table, entry, w):
    """Word w of the table entry whose first word's index is in the local entry, as the translator reads it: into a float local"""
    a = f.a
    e = f.local(a.int, f.val(128, a.int, f.ld(a.int, entry), a.c(a.int, w)))
    return f.local(a.float, f.val(124, a.float, f.val(61, a.uint, f.val(65, a.ptu, table, a.c(a.int, 0), f.ld(a.int, e)))))


def search(f, table, plo, phi, steps=spv.XL_GMEM_MAP_LOG2, swap=False, odd=None):
    """GenerateRawAccess's search → the local holding the index. swap: the first comparison's operands exchanged (another function: not the reference);
    odd: a step whose entry is index * 16 (the steps are then not one step repeated)"""
    a = f.a; I, U, F, B = a.int, a.uint, a.float, a.bool
    idx = None
    for s in range(steps - 1, -1, -1):
        cand = f.local(I, f.val(128, I, f.ld(I, idx) if idx else a.c(I, 0), a.c(I, 1 << s)))
        ent = f.local(I, f.val(132, I, f.ld(I, cand), a.c(I, 16 if s == odd else EW)))
        sl, sh = table_word(f, table, ent, 0), table_word(f, table, ent, 1)
        x, y = f.val(124, U, f.ld(F, sh)), f.val(124, U, f.ld(I, phi))
        c1 = f.local(B, f.val(176, B, *((y, x) if swap else (x, y))))
        c2 = f.local(B, f.val(170, B, f.val(124, I, f.ld(F, sh)), f.ld(I, phi)))
        c3 = f.local(B, f.val(178, B, f.val(124, U, f.ld(F, sl)), f.val(124, U, f.ld(I, plo))))
        c4 = f.local(B, f.val(167, B, f.ld(B, c2), f.ld(B, c3)))
        c5 = f.local(B, f.val(166, B, f.ld(B, c1), f.ld(B, c4)))
        idx = f.local(I, f.val(169, I, f.ld(B, c5), f.ld(I, cand), f.ld(I, idx) if idx else a.c(I, 0)))
    return idx


def in_pool(f, table, plo, phi, idx):
    a = f.a; I, U, F, B = a.int, a.uint, a.float, a.bool
    fnd = f.local(I, f.val(132, I, f.ld(I, idx), a.c(I, EW)))
    el, eh = table_word(f, table, fnd, 2), table_word(f, table, fnd, 3)
    c1 = f.local(B, f.val(176, B, f.val(124, U, f.ld(I, phi)), f.val(124, U, f.ld(F, eh))))
    c2 = f.local(B, f.val(170, B, f.ld(I, phi), f.val(124, I, f.ld(F, eh))))
    c3 = f.local(B, f.val(176, B, f.val(124, U, f.ld(I, plo)), f.val(124, U, f.ld(F, el))))
    c4 = f.local(B, f.val(167, B, f.ld(B, c2), f.ld(B, c3)))
    c5 = f.local(B, f.val(166, B, f.ld(B, c1), f.ld(B, c4)))
    return fnd, f.ld(B, c5)


def host_pointer(f, table, plo, phi, fnd):
    a = f.a; I, U, F, B = a.int, a.uint, a.float, a.bool
    dl = table_word(f, table, fnd, 4)
    hl = f.local(I, f.val(128, I, f.ld(I, plo), f.val(124, I, f.ld(F, dl))))
    cy = f.local(B, f.val(176, B, f.val(124, U, f.ld(I, hl)), f.val(124, U, f.ld(I, plo))))
    carry = f.local(I, f.val(169, I, f.ld(B, cy), a.c(I, 1), a.c(I, 0)))
    dh = table_word(f, table, fnd, 5)
    h0 = f.local(I, f.val(128, I, f.ld(I, phi), f.val(124, I, f.ld(F, dh))))
    hh = f.local(I, f.val(128, I, f.ld(I, h0), f.ld(I, carry)))
    v = f.val(80, a.v2, f.val(124, U, f.ld(I, hl)), f.val(124, U, f.ld(I, hh)))
    return f.val(120, a.pphys, f.val(124, a.u64, v))


def load_helper(a, table, **kw):
    f = Fn(a, a.int, [a.int, a.int]); plo, phi = f.params
    fnd, c = in_pool(f, table, plo, phi, search(f, table, plo, phi, **kw))
    t, mg = a.id(), a.id()
    f.op(247, mg, 0); f.op(250, c, t, mg); f.op(248, t)
    w = f.local(a.float, f.val(124, a.float, f.val(61, a.uint, host_pointer(f, table, plo, phi, fnd), 2, 4)))
    f.op(254, f.val(124, a.int, f.ld(a.float, w)))
    f.op(248, mg); f.op(254, a.c(a.int, 0))
    return f


def store_helper(a, table, log):
    """The raw store: the search, the inPool branch, the page put in the log's hash set (8 probes, word 0 set when none is free), the store"""
    f = Fn(a, a.void, [a.int, a.int, a.int]); plo, phi, pv = f.params
    I, U, B = a.int, a.uint, a.bool
    fnd, c = in_pool(f, table, plo, phi, search(f, table, plo, phi))
    t, mg = a.id(), a.id()
    f.op(247, mg, 0); f.op(250, c, t, mg); f.op(248, t)
    page = f.local(I, f.val(197, I, f.val(196, I, f.ld(I, phi), a.c(I, 20)), f.val(194, I, f.ld(I, plo), a.c(I, 12))))
    hsh = f.local(I, f.val(194, I, f.val(132, I, f.ld(I, page), a.c(I, 0x9e3779b1)), a.c(I, 22)))
    joins = []
    for probe in range(8):
        slot = f.local(I, f.val(128, I, f.val(199, I, f.val(128, I, f.ld(I, hsh), a.c(I, probe)), a.c(I, 1023)), a.c(I, 1)))
        p = f.val(65, a.ptu, log, a.c(I, 0), f.ld(I, slot))
        old = f.local(I, f.val(124, I, f.val(230, U, p, a.c(U, 1), a.c(U, 0), a.c(U, 0), f.val(124, U, f.ld(I, page)), a.c(U, 0))))
        cond = f.local(B, f.val(166, B, f.val(170, B, f.ld(I, old), a.c(I, 0)), f.val(170, B, f.ld(I, old), f.ld(I, page))))
        j, n, logged = a.id(), a.id(), f.ld(B, cond); joins.append(j)
        f.op(247, j, 0); f.op(250, logged, j, n); f.op(248, n)
    f.op(62, f.val(65, a.ptu, log, a.c(I, 0), a.c(I, 0)), a.c(U, 1))
    for j in reversed(joins): f.op(249, j); f.op(248, j)
    f.op(62, host_pointer(f, table, plo, phi, fnd), f.val(124, U, f.ld(I, pv)), 2, 4)
    f.op(253); f.op(248, mg); f.op(253)
    return f


def shader(register=False, with_store=True, **kw):
    """→ (module bytes, {buffer variable: kind}). main reads a base address from io 0, 1 and a second one from io 2, 3; loads the words at base + 0, 4, 8, 12 (a run of
    four: the first address plain, the others with their offsets and carries), stores them to io 8..11; loads base + 16 (one word: the stores to io ended the run)
    into io 12; loads the second base + 8, + 12 (a run of two) into io 13, 14; stores 7 at the address in io 4, 5 through the store helper; loads the base word again
    into io 15 (after the store: it reads the stored word when the addresses are one). register: the base is kept in two variables stored twice (the first base,
    then the second), as the translator keeps a register: they are not promoted, and their loads are equal only between two stores"""
    a = Asm(); I, U, B = a.int, a.uint, a.bool
    table, log, io = a.buffer(0), a.buffer(1), a.buffer(2)
    hl = load_helper(a, table, **kw)
    hs = store_helper(a, table, log) if with_store else None
    f = Fn(a, a.void, [])
    def rd(k): return f.val(124, I, f.val(61, U, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, k))))
    def wr(k, v): f.op(62, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, k)), f.val(124, U, f.ld(I, v)))
    def offs(lo, hi, c):   # the translator's address + c
        lk = f.local(I, f.val(128, I, f.ld(I, lo), a.c(I, c)))
        ck = f.local(B, f.val(176, B, f.val(124, U, f.ld(I, lk)), f.val(124, U, f.ld(I, lo))))
        sk = f.local(I, f.val(169, I, f.ld(B, ck), a.c(I, 1), a.c(I, 0)))
        return lk, f.local(I, f.val(128, I, f.ld(I, hi), f.ld(I, sk)))
    def call(lo, hi):
        al, ah = f.local(I, f.ld(I, lo)), f.local(I, f.ld(I, hi))
        return f.local(I, f.val(57, I, hl.id, al, ah))
    lo, hi = f.local(I, rd(0)), f.local(I, rd(1))
    ads = [(lo, hi)] + [offs(lo, hi, c) for c in (4, 8, 12)]
    for k, r in enumerate([call(*ad) for ad in ads]): wr(8 + k, r)
    wr(12, call(*offs(lo, hi, 16)))
    if register: f.op(62, lo, rd(2)); f.op(62, hi, rd(3)); lo2, hi2 = lo, hi
    else: lo2, hi2 = f.local(I, rd(2)), f.local(I, rd(3))
    ads = [offs(lo2, hi2, c) for c in (8, 12)]
    for k, r in enumerate([call(*ad) for ad in ads]): wr(13 + k, r)
    if with_store:
        sl, sh, sv = f.local(I, rd(4)), f.local(I, rd(5)), f.local(I, a.c(I, 7))
        f.val(57, a.void, hs.id, sl, sh, sv)
    b0, b1 = f.local(I, rd(0)), f.local(I, rd(1))
    wr(15, call(b0, b1))
    f.op(253)
    return a.data(f), {table: 'tab', log: 'log', io: 'io'}


def pool_table(pools):
    """The GMEM_MAP table of pools [(start, end, delta)] sorted by start (entries 1.., as vka_draw.c gmem_map fills it: entry 0 zero, the rest all ones)"""
    w = [M32] * (ENTRIES * EW); w[:EW] = [0] * EW
    for p, (s, e, dl) in enumerate(pools, 1):
        w[p * EW:(p + 1) * EW] = [s & M32, s >> 32, e & M32, e >> 32, dl & M32, dl >> 32, 0, 0]
    return w


def some_pools(n, rnd=None):
    """n pools: 4.5 MiB every 6 MiB from 64 GiB, deltas up and down; pool 5 ends where pool 6 starts, pool 8 ends inside pool 9 (the search answers with 9 from its
    start on), and pool 12 lies across a 4 GiB boundary (addresses whose low word wraps)"""
    pools = []
    for p in range(1, n + 1):
        s = 0x10_0000_0000 + (p - 1) * 0x60_0000 + 0x8000; e = s + 0x48_0000
        dl = (0x3_0000_0000 + p * 0x10_0040) if p % 2 else (-(0x8_0000_0000 + p * 0x1004)) & M64
        if p == 12: s, e = 0x10_ffff_c000, 0x11_0000_4000
        pools.append([s, e, dl])
    pools.sort()
    if n >= 9: pools[4][1] = pools[5][0]; pools[7][1] = pools[8][0] + 0x1000
    return [tuple(p) for p in pools]


def model(table, a, steps=spv.GMEM_MAP_LOG2):
    """What the raw load reads for address a, by GlobalToStorage's rule with that many search steps (the Android table's 13; the translator's own 12):
    (the physical address, or None in no pool)"""
    idx = 0
    for s in range(steps - 1, -1, -1):
        c = idx + (1 << s)
        if (table[c * EW] | table[c * EW + 1] << 32) <= a: idx = c
    if a < (table[idx * EW + 2] | table[idx * EW + 3] << 32): return (a + (table[idx * EW + 4] | table[idx * EW + 5] << 32)) & M64
    return None


def addresses(pools, rnd):
    out = [0, 4, M64 - 3, 0x0f_ffff_fff0]
    for s, e, _ in pools:
        out += [s, s + 4, (s + e) // 2 & ~3, e - 16, e - 12, e - 8, e - 4, e, e + 0x100, s - 4, s - 16, s + rnd.randrange(0, e - s, 4)]
    out += [a for s, e, _ in pools if s >> 32 != (e - 1) >> 32 for a in (((e - 1) >> 32 << 32) - 16, ((e - 1) >> 32 << 32) - 8, ((e - 1) >> 32 << 32) - 4, (e - 1) >> 32 << 32)]
    return out


def spirv_val(data):
    exe = shutil.which('spirv-val')
    if not exe: return None
    with tempfile.TemporaryDirectory() as t:
        p = Path(t) / 'm.spv'; p.write_bytes(data)
        r = subprocess.run([exe, '--target-env', 'vulkan1.3', str(p)], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def run_main(data, bufs, table, io):
    m = spv.Module(data)
    it = Interp(m, table, bufs, io)
    it.call(m.entry_function(), [])
    return it.io, it.reads, it.writes, it.log


def passes(data):
    m = spv.Module(data)
    st = spv.raw_global_memory(m)
    return m.data(), st


class RawGlobal(unittest.TestCase):
    def test_abi(self):
        """the table's entry is dmsh_abi.h's and its size vka_draw.c's (the function made for the load helper and the search loop are written from them); the
        translator's search is dmsh_abi.h's size (the reference the load helper is compared with)"""
        abi = (HERE.parent / 'nvn' / 'dmsh_abi.h').read_text()
        self.assertEqual(int(re.search(r'#define DMSH_GMEM_MAP_ENTRIES (\d+)', abi).group(1)), 1 << spv.XL_GMEM_MAP_LOG2)
        draw = (HERE.parent / 'vk_android' / 'vka_draw.c').read_text()
        self.assertEqual(int(re.search(r'#define VKA_GMEM_MAP_ENTRIES (\d+)', draw).group(1)), 1 << spv.GMEM_MAP_LOG2)
        self.assertEqual(int(re.search(r'#define DMSH_GMEM_MAP_ENTRY (\d+)', abi).group(1)), 4 * spv.GMEM_MAP_ENTRY_WORDS)

    def test_no_helper_untouched(self):
        """a module without a raw helper is not touched: raw_global_memory returns None and the bytes are the same"""
        a = Asm(); io = a.buffer(2); f = Fn(a, a.void, [])
        v = f.local(a.int, f.val(124, a.int, f.val(61, a.uint, f.val(65, a.ptu, io, a.c(a.int, 0), a.c(a.int, 0)))))
        f.op(62, f.val(65, a.ptu, io, a.c(a.int, 0), a.c(a.int, 1)), f.val(124, a.uint, f.ld(a.int, v))); f.op(253)
        data = a.data(f)
        m = spv.Module(data)
        self.assertIsNone(spv.raw_global_memory(m))
        self.assertEqual(m.data(), data)

    def test_counts_and_valid(self):
        for register in (False, True):
            data, bufs = shader(register=register)
            v = spirv_val(data)
            if v: self.assertEqual(v[0], 0, v[1])
            new, st = passes(data)
            self.assertEqual((st['helpers'], st['by_value'], st['load_helpers'], st['loops'], st['unlooped'], st['untouched']), (2, 5, 1, 2, 0, 0), st)
            self.assertEqual((st['cached'], st['runs'], st['left']), (8, {'1': 2, '2': 1, '4': 1}, 1), st)   # left: the store helper's call
            v = spirv_val(new)
            if v: self.assertEqual(v[0], 0, v[1])
            m = spv.Module(new)
            ops = [x[0] & 0xffff for x in m.ins]
            self.assertEqual(ops.count(spv.OP_CONVERT_U_TO_PTR), 1 + 2 + 3 + 5, 'the store helper; in the functions for 1, 2 and 4 words, each word of a hit and the looked-up one')
            loops = [x for x in m.ins if x[0] & 0xffff == spv.OP_LOOP_MERGE]
            self.assertTrue(loops and all(x[3] == spv.LOOP_DONT_UNROLL for x in loops))
            self.assertFalse(any(x[0] & 0xffff == spv.OP_VARIABLE and x[3] == spv.SC_PRIVATE for x in m.ins), 'the cache is Function variables, not Private ones')
            again, st2 = passes(new)
            self.assertEqual((st2['loops'], st2['cached'], st2['untouched']), (0, 0, 1), 'nothing left to do: a second run leaves the module as it is')
            self.assertEqual(again, new)

    def test_same_words(self):
        """main before and after, over addresses in and out of the pools, runs that leave their pool or cross into the next one, low words that wrap, pools that touch
        or overlap, the last table entry: the same words into io, the same physical reads in the same order, the same stores, the same log"""
        rnd = random.Random(7)
        for register, pools in ((False, some_pools(40)), (True, some_pools(40)), (False, some_pools(XL_ENTRIES - 1))):
            table = pool_table(pools)
            data, bufs = shader(register=register)
            new, _ = passes(data)
            ads = addresses(pools if len(pools) < 100 else pools[:3] + pools[-3:], rnd)
            kinds = set()
            for i, a in enumerate(ads):
                for b in (ads[(i * 7 + 3) % len(ads)], a, (a + 8) & M64):
                    s = ads[(i * 5 + 1) % len(ads)] if i % 3 else a
                    io = {0: a & M32, 1: a >> 32, 2: b & M32, 3: b >> 32, 4: s & M32, 5: s >> 32}
                    want, got = run_main(data, bufs, table, io), run_main(new, bufs, table, io)
                    self.assertEqual(got, want, f'base {a:#x}, second {b:#x}, store {s:#x}')
                    where = [model(table, (a + 4 * k) & M64) for k in range(4)]
                    for k in range(4): self.assertEqual(got[0].get(8 + k, 0), 0 if where[k] is None else word_at(where[k]), f'word {k} of {a:#x}')
                    kinds.add('in' if None not in where else 'none' if set(where) == {None} else 'leaves')
                    if model(table, s) is not None and s == a: self.assertEqual(got[0][15], 7, 'the load after the store reads the stored word')
            self.assertEqual(kinds, {'in', 'none', 'leaves'})

    def test_android_table(self):
        """on a full Android table (8,191 pools), main after the passes reads every word the 13-step search finds, the pools past entry 4,095 too, where main
        before them (the translator's 12 steps) read 0"""
        rnd = random.Random(11)
        pools = some_pools(ENTRIES - 1); table = pool_table(pools)
        data, bufs = shader()
        new, _ = passes(data)
        found_past = 0
        for a in addresses(pools[4093:4097] + pools[-3:], rnd):
            io = {0: a & M32, 1: a >> 32, 2: a & M32, 3: a >> 32, 4: a & M32, 5: a >> 32}
            got, before = run_main(new, bufs, table, io), run_main(data, bufs, table, io)
            where = [model(table, (a + 4 * k) & M64) for k in range(4)]
            for k in range(4): self.assertEqual(got[0].get(8 + k, 0), 0 if where[k] is None else word_at(where[k]), f'word {k} of {a:#x}')
            old = model(table, a, spv.XL_GMEM_MAP_LOG2)
            if where[0] is not None and old is None:
                found_past += 1
                self.assertEqual(before[0].get(8, 0), 0, f'{a:#x}: the search of the translator finds no pool there')
        self.assertGreater(found_past, 0)

    def test_helper_search(self):
        """each helper alone, before and after the loop, against the search written out (model), on a full table: after it every entry is found, the last one
        too; before it (the translator's 12 steps) the entries up to 4,095, the rest in no pool"""
        pools = some_pools(ENTRIES - 1); table = pool_table(pools)
        data, bufs = shader()
        m0 = spv.Module(data)
        m1 = spv.Module(data)
        hs = spv.raw_helpers(m1)
        for f in hs: spv.value_params(m1, f)
        spv.promote_locals(m1)
        self.assertEqual([spv.loop_pool_search(m1, f) for f in hs], [spv.GMEM_MAP_LOG2] * 2)
        self.assertEqual([spv.loop_pool_search(m1, f) for f in hs], [0, 0], 'a search that is a loop is not made one again')
        rnd = random.Random(3)
        for p in [0, 1, 2, 63, 64, 65, 1000, 2047, 2048, 4093, 4094, 4095, 4096, 6000, 8189, 8190]:
            s, e, _ = pools[p]
            for a in (s, e - 4, e, s - 4, s + rnd.randrange(0, e - s, 4)):
                i0 = Interp(m0, table, bufs); lo, hi = ('v', -1), ('v', -2); i0.mem[lo], i0.mem[hi] = a & M32, a >> 32
                i1 = Interp(m1, table, bufs)
                r0, r1 = i0.call(hs[0], [lo, hi]), i1.call(hs[0], [a & M32, a >> 32])
                old, want = model(table, a, spv.XL_GMEM_MAP_LOG2), model(table, a)
                self.assertEqual((r0, i0.reads), (0 if old is None else word_at(old), [] if old is None else [old]), f'{a:#x}')
                self.assertEqual((r1, i1.reads), (0 if want is None else word_at(want), [] if want is None else [want]), f'{a:#x}')
                if p < XL_ENTRIES - 1: self.assertEqual((r1, i1.reads), (r0, i0.reads), f'{a:#x}')
                elif s <= a < e: self.assertIsNotNone(want, f'{a:#x}: past entry 4,095, found by the 13 steps')

    def test_other_shapes(self):
        """a helper that is not the reference keeps its calls: one whose first comparison is the other way round (another function: its search is still n equal
        steps, so it becomes a loop, and reads as before), one with 13 steps (the same); one whose steps differ is left whole, and so is its module"""
        pools = some_pools(40); table = pool_table(pools); rnd = random.Random(5)
        for kw, loops in (({'swap': True}, 1), ({'steps': 13}, 1), ({'odd': 5}, 0)):
            data, bufs = shader(with_store=False, **kw)
            new, st = passes(data)
            self.assertEqual((st['load_helpers'], st['cached'], st['loops'], st['untouched']), (0, 0, loops, int(not loops)), (kw, st))
            if not loops: self.assertEqual(new, data); continue
            v = spirv_val(new)
            if v: self.assertEqual(v[0], 0, v[1])
            big = table + [M32] * len(table) if kw.get('steps') == 13 else table   # (13 steps look at entries up to 8,191)
            for a in addresses(pools[:6], rnd)[:40]:
                io = {0: a & M32, 1: a >> 32, 2: (a + 64) & M32, 3: a >> 32}
                self.assertEqual(run_main(new, bufs, big, io), run_main(data, bufs, big, io), (kw, hex(a)))

    def test_run_breakers(self):
        """two loads at address + 0 and + 4 with a raw store between them are two calls, the store's between them; without it they are one run"""
        for store in (True, False):
            a = Asm(); I, U, B = a.int, a.uint, a.bool
            table, log, io = a.buffer(0), a.buffer(1), a.buffer(2)
            hl, hs = load_helper(a, table), store_helper(a, table, log)
            f = Fn(a, a.void, [])
            def rd(k): return f.val(124, I, f.val(61, U, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, k))))
            lo, hi = f.local(I, rd(0)), f.local(I, rd(1))
            lk = f.local(I, f.val(128, I, f.ld(I, lo), a.c(I, 4)))
            sk = f.local(I, f.val(169, I, f.val(176, B, f.val(124, U, f.ld(I, lk)), f.val(124, U, f.ld(I, lo))), a.c(I, 1), a.c(I, 0)))
            hk = f.local(I, f.val(128, I, f.ld(I, hi), f.ld(I, sk)))
            rs = []
            for n, (l, h) in enumerate(((lo, hi), (lk, hk), (lo, hi), (lk, hk))):
                al, ah = f.local(I, f.ld(I, l)), f.local(I, f.ld(I, h))
                rs.append(f.local(I, f.val(57, I, hl.id, al, ah)))
                if store and n == 0:
                    s1, s2, s3 = f.local(I, f.ld(I, lk)), f.local(I, f.ld(I, hk)), f.local(I, a.c(I, 99))
                    f.val(57, a.void, hs.id, s1, s2, s3)
            for k, r in enumerate(rs): f.op(62, f.val(65, a.ptu, io, a.c(I, 0), a.c(I, 8 + k)), f.val(124, U, f.ld(I, r)))
            f.op(253)
            data, bufs = a.data(f), {table: 'tab', log: 'log', io: 'io'}
            new, st = passes(data)
            self.assertEqual(st['runs'], {'1': 2, '2': 1} if store else {'2': 2}, st)
            pools = some_pools(20); tb = pool_table(pools)
            for s, e, _ in pools[:5]:
                for ad in (s, e - 4, e - 8, s - 4):
                    io_ = {0: ad & M32, 1: ad >> 32}
                    got = run_main(new, bufs, tb, io_)
                    self.assertEqual(got, run_main(data, bufs, tb, io_))
                    if store and model(tb, ad + 4) is not None: self.assertEqual(got[0][9], 99, 'the second word is read after the store wrote it')

    def test_literal_is_not_an_id(self):
        """promotion replaces a load's id only where it is an id: a component literal equal to that id stays"""
        a = Asm(); I, U = a.int, a.uint
        io = a.buffer(2)
        v4 = a.ty(23, U, 4)
        f = Fn(a, a.void, [])
        while a.n < 40: a.id()
        vec = f.val(80, v4, a.c(U, 10), a.c(U, 11), a.c(U, 12), a.c(U, 13))
        tmp = f.local(I, a.c(I, 5))
        ld = f.ld(I, tmp)   # this load's id is used below as a literal too: OpCompositeExtract's index would have to be < 4
        self.assertGreater(ld, 3)
        f.op(62, f.val(65, a.ptu, io, a.c(I, 0), f.val(128, I, ld, a.c(I, 0))), f.val(81, U, vec, 1))
        f.op(253)
        m = spv.Module(a.data(f))
        ext = next(x for x in m.ins if x[0] & 0xffff == spv.OP_COMPOSITE_EXTRACT)
        ext[4] = ld   # (not valid SPIR-V: only the literal's value matters here)
        self.assertEqual(spv.promote_locals(m), (1, 0))
        self.assertEqual(next(x for x in m.ins if x[0] & 0xffff == spv.OP_COMPOSITE_EXTRACT)[4], ld)
        self.assertEqual(next(x for x in m.ins if x[0] & 0xffff == spv.OP_IADD)[3], a.c(I, 5))

    def test_store_not_dominating(self):
        """a variable read on a path its one store is not on keeps its variable (a loop: the load at the top reads the turn before's value)"""
        a = Asm(); I, U, B = a.int, a.uint, a.bool
        io = a.buffer(2)
        f = Fn(a, a.void, [])
        h, body, cont, mg = (a.id() for _ in range(4))
        v = a.id(); f.vars.append([59, a.ty(32, 7, I), v, 7])
        n = a.id(); f.vars.append([59, a.ty(32, 7, I), n, 7])
        f.op(62, n, a.c(I, 0))
        f.op(249, h); f.op(248, h); f.op(246, mg, cont, 0); f.op(249, body); f.op(248, body)
        seen = f.ld(I, v)   # (before the store in the loop: the value stored a turn earlier)
        f.op(62, f.val(65, a.ptu, io, a.c(I, 0), f.ld(I, n)), f.val(124, U, seen))
        f.op(62, v, f.val(128, I, f.ld(I, n), a.c(I, 100)))
        f.op(62, n, f.val(128, I, f.ld(I, n), a.c(I, 1)))
        more = f.val(177, B, f.ld(I, n), a.c(I, 3))
        f.op(249, cont); f.op(248, cont); f.op(250, more, h, mg); f.op(248, mg); f.op(253)
        m = spv.Module(a.data(f))
        self.assertEqual(spv.promote_locals(m), (0, 0), 'v is stored once but read before the store in the loop; n is stored twice')
        self.assertEqual(sum(1 for x in m.ins if x[0] & 0xffff == spv.OP_VARIABLE and x[3] == spv.SC_FUNCTION), 2)

    def test_dominance(self):
        """_dominance answers as the set fixed point, on random graphs with unreachable cycles, self loops and branches back to the entry (Banana Native's test)"""
        def sets(blks, preds):
            dom = {b: set(blks) for b in blks}; dom[blks[0]] = {blks[0]}
            chg = True
            while chg:
                chg = False
                for b in blks[1:]:
                    d = ({b} | set.intersection(*(dom[p] for p in preds[b]))) if preds[b] else {b}
                    if d != dom[b]: dom[b] = d; chg = True
            return dom
        rnd = random.Random(1)
        for _ in range(3000):
            n = rnd.randint(1, 10); blks = rnd.sample(range(100, 200), n)
            preds = {b: set() for b in blks}
            for _ in range(rnd.randint(0, 3 * n)): preds[rnd.choice(blks)].add(rnd.choice(blks))
            d, f = sets(blks, preds), spv._dominance(blks, preds)
            for a in blks:
                for b in blks: self.assertEqual(a in d[b], f(a, b), (blks, preds, a, b))


@unittest.skipUnless(os.environ.get('DMSH_VK_RAW_LIB'), 'DMSH_VK_RAW_LIB names no library')
class RealLibrary(unittest.TestCase):
    def test_load_helpers(self):
        """the 32-bit raw load of a real library's shaders (every DMSH_VK_RAW_STEP-th one that binds the pool table, default 50): the translator's helper and the
        functions cache_raw_loads makes for it read the same words at the same physical addresses, over one sequence of addresses with the kept entry carried along"""
        import vk_android_delta as delta
        from compression import zstd
        L = delta.Library(os.environ['DMSH_VK_RAW_LIB'])
        keys = [k for k, i in sorted(L.index.items()) if any(b == 20 for b in L.entry(i)[2][::delta.VK['BIND_SIZE']])]
        pools = some_pools(60); table = pool_table(pools); rnd = random.Random(11)
        ads = addresses(pools[:14], rnd)
        n = 0
        for key in keys[::int(os.environ.get('DMSH_VK_RAW_STEP', '50'))]:
            _, _, _, _, mods = L.entry(L.index[key])
            m0 = spv.Module(delta.spirv(mods[0][2], mods[0][3], zstd))
            m1 = spv.Module(m0.data())
            st = spv.raw_global_memory(m1)
            if not st or not st['cached']: continue
            w = spv.Module(m0.data())
            for f in spv.raw_helpers(w): spv.value_params(w, f)
            spv.promote_locals(w)
            h = next(f for f in spv.raw_helpers(w) if spv._raw_load_helper(w, f))
            table_var = spv._raw_load_helper(w, h)['table']
            made = {x[3] for x in m1.ins if x[0] & 0xffff == spv.OP_FUNCTION_CALL and len(x) == 12}   # (two address words and the six cache variables)
            one = next((x[2] for x in m1.ins if x[0] & 0xffff == spv.OP_FUNCTION and x[2] in made and m1.op(x[1]) == spv.OP_TYPE_INT), None)   # the one-word function
            if one is None: continue
            i0, i1 = Interp(m0, table, {table_var: 'tab'}), Interp(m1, table, {table_var: 'tab'})
            cache = [('v', -k) for k in range(1, 7)]
            for c, v in zip(cache, (M32, M32, 0, 0, 0, 0)): i1.mem[c] = v
            lo, hi = ('v', -10), ('v', -11)
            for a in ads:
                i0.mem[lo], i0.mem[hi] = a & M32, a >> 32
                r0 = i0.call(h, [lo, hi]) & M32
                r1 = i1.call(one, [a & M32, a >> 32, *cache])
                self.assertEqual((r1, i1.reads), (r0, i0.reads), f'{key} {a:#x}')
            n += 1
        self.assertGreater(n, 0)


if __name__ == '__main__':
    unittest.main()
