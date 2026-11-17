#!/usr/bin/env python3
"""Cached raw loads of one base grouped: a SPIR-V rewrite over vk_android_spv.py's cache_raw_loads output, kept outside the shader chain (DMSH_VK_CHAIN covers
vk_android_spv.py: this file is not part of it and no build runs it). Used by hand on library modules (as DM_VK_SPV_OVERRIDE files) and by
dm/gfx/tests/test_vk_raw_group.py."""
from vk_android_spv import *  # noqa: F401,F403
from vk_android_spv import _Numbers, _RUN_BREAKERS, _store_class  # noqa: F401


# ---------- Cached raw loads of one base grouped (group_cached_loads) ----------
# cache_raw_loads leaves one call of a generated function per run of up to four words, and drivers inline every call: each carries its own copy of the pool search
# loop and its own kept-entry check. Ocarina of Time's foliage mesh compute fed302d791a4ba43 has 59 such calls (all runs of four words, LDG.128 loads), ~12,700 of
# its ~27,000 instructions; Qualcomm's 891.14 puts it at 6 % ALU occupancy. Most of a block's runs read one structure through one base pointer (base + 0, 16, 32,
# ...). Here the calls of one block that read through one base, with nothing between them that writes or orders memory, ends the invocation or calls anything
# else, are one call of a generated function at the first one's place: one kept-entry check for the whole span, the words read at once on a hit, and one search
# loop (over the words, in order) on a miss. The idea is AnkaNative's raw loads grouped by base (41da426; GPL: the idea only), written here over this chain's cache.
# Not run by raw_global_memory (so not part of DMSH_VK_CHAIN's output): on Ocarina of Time it groups little (2026-11-16: 49 of the calls of the 48 task/mesh
# computes at Link's porch, 5 of fed302d791a4ba43's 59, and the code with every call expanded stays the same size), since nearly every load there reads through a base
# of its own (an address computed from an index, not base + constant). Kept, with dm/gfx/tests/test_vk_raw_group.py, for a game whose loads share bases
GMEM_GROUP_WORDS = 16      # the most words one group reads (its results live from the first call on: a bound on what the hoisting adds to the registers)
GMEM_GROUP_BYTES = 4096    # the most bytes from a group's first word to the end of its last one


def _cached_fn_facts(m, f):
    """cache_raw_loads' generated function f's facts, read from its code: (low, high, the six cache variables) → u32 or uvec4, whose accesses are the n words of
    its hit and the one of its miss. → {n, ret, lo, hi, table, tptr, ity, word, pptr, mem}, else None"""
    a, e = m.function_range(f)
    body = m.ins[a:e + 1]
    params = [x for x in body if x[0] & 0xffff == OP_FUNCTION_PARAMETER]
    if len(params) != 8: return None
    if any(m.op(x[1]) != OP_TYPE_POINTER or m.pointee(x[1]) != (SC_FUNCTION, m.find_type(OP_TYPE_INT, 32, 0)) for x in params[2:]): return None
    if (m.int_type(params[0][1]) or (0,))[0] != 32 or (m.int_type(params[1][1]) or (0,))[0] != 32: return None
    rty = body[0][1]
    cus = [x for x in body if x[0] & 0xffff == OP_CONVERT_U_TO_PTR]
    chains = [x for x in body if x[0] & 0xffff == OP_ACCESS_CHAIN and len(x) == 6]
    loops = sum(1 for x in body if x[0] & 0xffff == OP_LOOP_MERGE)
    if not cus or not chains or len({x[1] for x in cus}) != 1 or len({x[3] for x in chains}) != 1 or len({x[1] for x in chains}) != 1: return None
    phys = {x[2] for x in cus}
    loads = [x for x in body if x[0] & 0xffff == OP_LOAD and x[3] in phys]
    if len(loads) != len(cus) or len({tuple(x[4:]) for x in loads}) != 1 or len({x[1] for x in loads}) != 1: return None
    n = len(cus) - 1
    if not 1 <= n <= GMEM_RUN_WORDS or loops != (1 if n == 1 else 2): return None
    u32 = m.find_type(OP_TYPE_INT, 32, 0)
    if (n == 1) != (rty == u32): return None
    return {'n': n, 'ret': rty, 'lo': params[0][1], 'hi': params[1][1], 'table': chains[0][3], 'tptr': chains[0][1], 'ity': m.consts[chains[0][4]][0],
            'word': loads[0][1], 'pptr': cus[0][1], 'mem': loads[0][4:]}


def _group_load_fn(m, h, offs, T):
    """group_cached_loads' function for a group whose words are at the first word's address + offs (byte offsets, 0 first, each a multiple of 4 below
    GMEM_GROUP_BYTES): (low, high of the first word's address, the six cache variables) → uint[len(offs)]. What it computes is, word for word, what the cached
    function of each run computes (_cached_load_fn), so what the raw load helper reads:
      hit: start <= first address, last address < kept end, first <= last (no wrap): each word at its address + delta (the kept range holds every address
           between the first and the last, so the search answers each with the kept entry);
      else, for each word in order: the search loop, in a pool the entry kept (its end cut to the next entry's start) and the word read, in no pool 0 and nothing read.
    The offsets are a constant array in a Function variable, the words of a miss another one, indexed by the loop"""
    ins = []
    bty, u32, v2, u64, pfu, U = T['bool'], T['u32'], T['v2'], T['u64'], T['pfu'], T['U']
    ity, tptr, table = h['ity'], h['tptr'], h['table']
    tword = m.pointee(tptr)[1]
    first_step, last_entry = 1 << (GMEM_MAP_LOG2 - 1), (1 << GMEM_MAP_LOG2) - 1
    I = {v: m.add_const(ity, v) for v in (0, 1, 2, 3, 4, 5, GMEM_MAP_ENTRY_WORDS, first_step, last_entry)}
    W = len(offs)
    uw = m.add_const(u32, W)
    arr = m.add_type(OP_TYPE_ARRAY, u32, uw)
    parr = m.add_type(OP_TYPE_POINTER, SC_FUNCTION, arr)
    oc = [m.add_const(u32, o) for o in offs]
    key = ('arr', arr, tuple(oc))
    ocomp = T['comp'].get(key)
    if ocomp is None:
        ocomp = m.new_id(); m._insert_decl([(3 + W) << 16 | OP_CONSTANT_COMPOSITE, arr, ocomp, *oc]); T['comp'][key] = ocomp
    fty = m.add_type(OP_TYPE_FUNCTION_, arr, h['lo'], h['hi'], *[pfu] * 6)
    def op(o, *w): ins.append([(len(w) + 1) << 16 | o, *w])
    def val(o, ty, *w):
        r = m.new_id(); ins.append([(len(w) + 3) << 16 | o, ty, r, *w]); return r
    def cast(v, frm, to): return v if frm == to else val(OP_BITCAST, to, v)
    def word(idx, w):
        p = val(OP_ACCESS_CHAIN, tptr, table, I[0], val(OP_IADD, ity, val(OP_IMUL, ity, idx, I[GMEM_MAP_ENTRY_WORDS]), I[w]))
        return cast(val(OP_LOAD, tword, p), tword, u32)
    def lt(ahi, alo, bhi, blo, eq=False):
        return val(OP_LOGICAL_OR, bty, val(OP_ULESS_THAN, bty, ahi, bhi),
                   val(OP_LOGICAL_AND, bty, val(OP_IEQUAL, bty, ahi, bhi), val(OP_ULESS_THAN_EQUAL if eq else OP_ULESS_THAN, bty, alo, blo)))
    def add64(alo, ahi, blo, bhi):
        lo = val(OP_IADD, u32, alo, blo)
        return lo, val(OP_IADD, u32, val(OP_IADD, u32, ahi, bhi), val(OP_SELECT, u32, val(OP_ULESS_THAN, bty, lo, alo), U[1], U[0]))
    def access(alo, ahi):
        p = val(OP_CONVERT_U_TO_PTR, h['pptr'], val(OP_BITCAST, u64, val(OP_COMPOSITE_CONSTRUCT, v2, alo, ahi)))
        return cast(val(OP_LOAD, h['word'], p, *h['mem']), h['word'], u32)
    fid, plo, phi = m.new_id(), m.new_id(), m.new_id()
    pc = [m.new_id() for _ in range(6)]
    E, HOT, COLD, M, LH, LB, LC, LM, SH, SB, SC, SM, AT, AM = (m.new_id() for _ in range(14))
    ins += [[5 << 16 | OP_FUNCTION, arr, fid, 0, fty], [3 << 16 | OP_FUNCTION_PARAMETER, h['lo'], plo], [3 << 16 | OP_FUNCTION_PARAMETER, h['hi'], phi]]
    ins += [[3 << 16 | OP_FUNCTION_PARAMETER, pfu, p] for p in pc]
    op(OP_LABEL, E)
    ov, rv = m.new_id(), m.new_id()
    ins += [[4 << 16 | OP_VARIABLE, parr, ov, SC_FUNCTION], [4 << 16 | OP_VARIABLE, parr, rv, SC_FUNCTION]]
    lu, hu = cast(plo, h['lo'], u32), cast(phi, h['hi'], u32)
    c = [val(OP_LOAD, u32, p) for p in pc]
    last = add64(lu, hu, m.add_const(u32, max(offs)), U[0]) if max(offs) else (lu, hu)
    hit = val(OP_LOGICAL_AND, bty, val(OP_LOGICAL_AND, bty, lt(c[1], c[0], hu, lu, eq=True), lt(last[1], last[0], c[3], c[2])), lt(hu, lu, last[1], last[0], eq=True))
    op(OP_SELECTION_MERGE, M, 0); op(OP_BRANCH_CONDITIONAL, hit, HOT, COLD)
    op(OP_LABEL, HOT)
    hv = []
    for o, ov_ in zip(offs, oc):
        al, ah = add64(lu, hu, ov_, U[0]) if o else (lu, hu)
        hv.append(access(*add64(al, ah, c[4], c[5])))
    hr = val(OP_COMPOSITE_CONSTRUCT, arr, *hv)
    op(OP_BRANCH, M)
    op(OP_LABEL, COLD)
    op(OP_STORE, ov, ocomp)
    k0, k1, x0, s0, x1, s1 = (m.new_id() for _ in range(6))
    op(OP_BRANCH, LH)
    op(OP_LABEL, LH)
    ins.append([7 << 16 | OP_PHI, u32, k0, U[0], COLD, k1, LC])
    op(OP_LOOP_MERGE, LM, LC, LOOP_DONT_UNROLL); op(OP_BRANCH, LB)
    op(OP_LABEL, LB)
    x, y = add64(lu, hu, val(OP_LOAD, u32, val(OP_ACCESS_CHAIN, pfu, ov, k0)), U[0])
    op(OP_BRANCH, SH)
    op(OP_LABEL, SH)
    ins.append([7 << 16 | OP_PHI, ity, x0, I[0], LB, x1, SC]); ins.append([7 << 16 | OP_PHI, ity, s0, I[first_step], LB, s1, SC])
    op(OP_LOOP_MERGE, SM, SC, LOOP_DONT_UNROLL); op(OP_BRANCH, SB)
    op(OP_LABEL, SB)
    nx = val(OP_IADD, ity, x0, s0)
    ins.append([6 << 16 | OP_SELECT, ity, x1, lt(word(nx, 1), word(nx, 0), y, x, eq=True), nx, x0])
    ins.append([5 << 16 | OP_SHR_LOGICAL, ity, s1, s0, I[1]])
    more = val(OP_INOT_EQUAL, bty, s1, I[0])
    op(OP_BRANCH, SC); op(OP_LABEL, SC); op(OP_BRANCH_CONDITIONAL, more, SH, SM)
    op(OP_LABEL, SM)
    end = [word(x1, 2), word(x1, 3)]
    found = lt(y, x, end[1], end[0])
    op(OP_SELECTION_MERGE, AM, 0); op(OP_BRANCH_CONDITIONAL, found, AT, AM)
    op(OP_LABEL, AT)
    start, dl = [word(x1, 0), word(x1, 1)], [word(x1, 4), word(x1, 5)]
    has_next = val(OP_ULESS_THAN, bty, x1, I[last_entry])
    nxt = val(OP_SELECT, ity, has_next, val(OP_IADD, ity, x1, I[1]), x1)
    ns = [val(OP_SELECT, u32, has_next, word(nxt, w), U[0xffffffff]) for w in (0, 1)]
    cut = lt(ns[1], ns[0], end[1], end[0])
    kept = [val(OP_SELECT, u32, cut, ns[w], end[w]) for w in (0, 1)]
    for p, v in zip(pc, start + kept + dl): op(OP_STORE, p, v)
    got = access(*add64(x, y, dl[0], dl[1]))
    op(OP_BRANCH, AM)
    op(OP_LABEL, AM)
    g = val(OP_PHI, u32, got, AT, U[0], SM)
    op(OP_STORE, val(OP_ACCESS_CHAIN, pfu, rv, k0), g)
    ins.append([5 << 16 | OP_IADD, u32, k1, k0, U[1]])
    more2 = val(OP_ULESS_THAN, bty, k1, uw)
    op(OP_BRANCH, LC); op(OP_LABEL, LC); op(OP_BRANCH_CONDITIONAL, more2, LH, LM)
    op(OP_LABEL, LM)
    cr = val(OP_LOAD, arr, rv)
    op(OP_BRANCH, M)
    op(OP_LABEL, M)
    op(OP_RETURN_VALUE, val(OP_PHI, arr, hr, HOT, cr, LM))
    op(OP_FUNCTION_END)
    m.ins += ins
    return fid


def group_cached_loads(m):
    """The calls of cache_raw_loads' functions (_cached_fn_facts) grouped by base and replaced by calls of _group_load_fn (see "Cached raw loads of one base
    grouped"): in each function, the calls of one block through one base (the addresses (H:B) + c as cache_raw_loads numbers them, the same six cache variables),
    up to GMEM_GROUP_WORDS words within GMEM_GROUP_BYTES of the first call's address and none of them below it, with nothing between the first and the last that
    breaks a run (_RUN_BREAKERS, a store outside Function memory, a call of anything but these functions) or starts a block. Every word is read where the first
    call was; the later calls' results are taken from the group's array there (the reads moved up past nothing that writes memory). What is read is what the
    cached functions read, word for word; the order of the physical reads is the group's. → (groups, calls grouped); (0, 0) and the module unchanged when none"""
    m.index()
    fns = [x[2] for x in m.ins if x[0] & 0xffff == OP_FUNCTION]
    facts = {f: h for f in fns for h in [_cached_fn_facts(m, f)] if h}
    if not facts: return 0, 0
    same = lambda a, b: all(a[k] == b[k] for k in ('lo', 'hi', 'table', 'tptr', 'ity', 'word', 'pptr', 'mem'))
    c32 = lambda i: m.consts[i][1][0] if i > 0 and i in m.consts and len(m.consts[i][1]) == 1 and (m.int_type(m.consts[i][0]) or (0,))[0] == 32 else None
    groups = []   # (function, [(index in its body, the call, its offset from the group's first call)])
    for f in fns:
        if f in facts: continue
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        if not any(x[0] & 0xffff == OP_FUNCTION_CALL and x[3] in facts for x in body): continue
        defs = {x[2]: x for x in body if len(x) > 2 and (x[0] & 0xffff) not in NO_RESULT_OPS and x[0] & 0xffff != OP_LABEL}
        V = _Numbers()
        def addr(lo, hi, V=V, defs=defs):   # cache_raw_loads' addr: (B, H, c) for an address (H:B) + c, else (low, high, 0)
            vl, vh = V(lo), V(hi)
            sl, sh = V.adds.get(vl), V.adds.get(vh)
            k0, k1 = (c32(sl[0]), c32(sl[1])) if sl else (None, None)
            b, c = (sl[1], k0) if sl and k0 is not None and k1 is None else (sl[0], k1) if sl and k1 is not None and k0 is None else (None, None)
            if sh and c is not None:
                for hh, s in ((sh[0], sh[1]), (sh[1], sh[0])):
                    s = defs.get(s)
                    if s is None or s[0] & 0xffff != OP_SELECT or c32(s[4]) != 1 or c32(s[5]) != 0: continue
                    lt_ = defs.get(V(s[3]))
                    if lt_ is not None and lt_[0] & 0xffff == OP_ULESS_THAN and V(lt_[3]) == vl and V(lt_[4]) == b: return (b, hh, c)
            return (vl, vh, 0)
        open_ = {}   # (B, H, the cache variables) → {'c0', 'words', 'calls'}
        def flush():
            for g in open_.values():
                if len(g['calls']) > 1: groups.append((f, g['calls']))
            open_.clear()
        for k, x in enumerate(body):
            op = x[0] & 0xffff
            if op == OP_FUNCTION_CALL and x[3] in facts and len(x) == 12:
                B, H, c = addr(x[4], x[5]); n = facts[x[3]]['n']; key = (B, H, tuple(x[6:12]))
                g = open_.get(key)
                if g is not None and same(facts[g['calls'][0][1][3]], facts[x[3]]) and c >= g['c0'] and c - g['c0'] + 4 * n <= GMEM_GROUP_BYTES \
                        and g['words'] + n <= GMEM_GROUP_WORDS:
                    g['calls'].append((k, x, c - g['c0'])); g['words'] += n
                else:
                    if g is not None and len(g['calls']) > 1: groups.append((f, g['calls']))
                    open_[key] = {'c0': c, 'words': n, 'calls': [(k, x, 0)]}
            elif op == OP_LABEL or op in _RUN_BREAKERS or op == OP_FUNCTION_CALL or (op == OP_STORE and _store_class(m, defs, x[1]) != SC_FUNCTION):
                flush()
            V.see(m, x)
        flush()
    if not groups: return 0, 0
    u32 = m.add_type(OP_TYPE_INT, 32, 0)
    T: dict = dict(bool=m.add_type(OP_TYPE_BOOL), u32=u32, v2=m.add_type(OP_TYPE_VECTOR, u32, 2), v4=m.add_type(OP_TYPE_VECTOR, u32, 4), u64=m.add_type(OP_TYPE_INT, 64, 0),
             pfu=m.add_type(OP_TYPE_POINTER, SC_FUNCTION, u32), U={v: m.add_const(u32, v) for v in (0, 1, 0xffffffff)}, comp={})
    gfns, done = {}, 0
    plans = []
    for f, calls in groups:
        h = facts[calls[0][1][3]]
        offs = tuple(o + 4 * j for _, x, o in calls for j in range(facts[x[3]]['n']))
        if (id(h), offs) not in gfns: gfns[id(h), offs] = _group_load_fn(m, h, list(offs), T)
        plans.append((f, calls, gfns[id(h), offs], offs))
    arrs = {offs: m.add_type(OP_TYPE_ARRAY, u32, m.add_const(u32, len(offs))) for _, _, _, offs in plans}
    for f in fns:
        mine = [(c, g, o) for ff, c, g, o in plans if ff == f]
        if not mine: continue
        a, e = m.function_range(f)
        body = m.ins[a:e + 1]
        rep = {}
        for calls, g, offs in mine:
            k0, x0, _ = calls[0]; r = m.new_id()
            new = [[12 << 16 | OP_FUNCTION_CALL, arrs[offs], r, g, x0[4], x0[5], *x0[6:12]]]
            w = 0
            for _, x, _ in calls:
                n = facts[x[3]]['n']
                ws = []
                for j in range(n):
                    v = m.new_id(); new.append([5 << 16 | OP_COMPOSITE_EXTRACT, u32, v, r, w + j]); ws.append(v)
                if n == 1: new.append([4 << 16 | OP_COPY_OBJECT, x[1], x[2], ws[0]])
                else: new.append([(3 + 4) << 16 | OP_COMPOSITE_CONSTRUCT, x[1], x[2], *ws, *[T['U'][0]] * (4 - n)])
                w += n
            rep[k0] = new
            for k, _, _ in calls[1:]: rep[k] = []
            done += len(calls)
        out = []
        for k, x in enumerate(body): out += rep.get(k, [x])
        m.ins[a:e + 1] = out
    m.index()
    return len(plans), done
