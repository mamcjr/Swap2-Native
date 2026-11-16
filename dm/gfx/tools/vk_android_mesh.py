"""Lower the translator's TaskEXT/MeshEXT modules to compute plus a pulling vertex stage.

The native modules remain available. No game-specific keys or material substitutions are used.
Like Hokkaido/Eden's mesh emulation, task payloads and mesh outputs cross stages through
storage buffers. Unlike a truncated arena, the host consumes mesh output in bounded batches.
This module is part of DMSH_VK_CHAIN (vk_android_build.py).
"""
import struct
import vk_android_spv as spv

OUT, PAYLOAD, OFFSETS, INFO, SOURCE = range(384, 389)
COMPUTE, VERTEX = 5, 6
SC_PRIVATE, SC_WORKGROUP, SC_PAYLOAD = 6, 4, 5402


def ins(op, *args): return [((1 + len(args)) << 16) | op, *args]


class Build:
    def __init__(self, m):
        self.m, self.code = m, []
        self.u = m.add_type(21, 32, 0)
        self.b = m.add_type(20)
        self.f = m.add_type(22, 32)

    def c(self, n): return self.m.add_const(self.u, n)
    def emit(self, op, *a): self.code.append(ins(op, *a))
    def val(self, op, ty, *a):
        i = self.m.new_id(); self.emit(op, ty, i, *a); return i
    def add(self, a, b): return self.val(128, self.u, a, b)
    def mul(self, a, b): return self.val(132, self.u, a, b)
    def load(self, ty, p): return self.val(61, ty, p)
    def ac(self, ty, var, *idx): return self.val(65, self.m.add_type(32, 12, ty), var, *idx)
    def word(self, var, idx): return self.ac(self.u, var, self.c(0), idx)
    def info(self, var, n): return self.load(self.u, self.word(var, self.c(n)))
    def deco(self, ident, dec, *args):
        if ins(71, ident, dec, *args) in self.m.ins: return
        at = next(i for i, x in enumerate(self.m.ins) if 19 <= (x[0] & 65535) <= 39)
        self.m.ins.insert(at, ins(71, ident, dec, *args))
    def member(self, ident, n, dec, *args):
        if ins(72, ident, n, dec, *args) in self.m.ins: return
        at = next(i for i, x in enumerate(self.m.ins) if 19 <= (x[0] & 65535) <= 39)
        self.m.ins.insert(at, ins(72, ident, n, dec, *args))
    def global_(self, ty, sc):
        p = self.m.add_type(32, sc, ty); i = self.m.new_id()
        self.m._insert_decl(ins(59, p, i, sc)); return i
    def buffer(self, ty, binding):
        v = self.global_(ty, 12); self.deco(v, 34, 0); self.deco(v, 33, binding); return v
    def words(self, binding):
        ar = self.m.add_type(29, self.u); self.deco(ar, 6, 4)
        st = self.m.add_type(30, ar); self.deco(st, 2); self.member(st, 0, 35, 0)
        return self.buffer(st, binding)
    def input(self, ty, builtin, force=False):
        if not force:
            for v, (p, sc) in self.m.vars.items():
                if sc == 1 and self.m.decos.get(v, {}).get(11) == [builtin] and self.m.pointee(p)[1] == ty: return v
        v = self.global_(ty, 1); self.deco(v, 11, builtin); return v
    def guarded(self, cond, body):
        a, z = self.m.new_id(), self.m.new_id()
        self.emit(247, z, 0); self.emit(250, cond, a, z); self.emit(248, a)
        body(); self.emit(249, z); self.emit(248, z)


def _remove_mesh_interface(m, old_outputs, private_wg):
    keep = []
    for x in m.ins:
        op = x[0] & 65535
        if op == 17 and x[1] == 5283: continue
        if op == 10 and spv.string_at(x, 1)[0] == 'SPV_EXT_mesh_shader': continue
        if op == 71 and (x[1] in old_outputs or (x[1] in private_wg and x[2] == 11)): continue
        if op == 5 and x[1] in old_outputs: continue
        if op == 16 and x[2] in (26, 27, 5270, 5298): continue
        if op == 59 and x[2] in old_outputs: continue
        if op in (32, 59) and x[2 if op == 32 else 3] == SC_PAYLOAD:
            x = list(x); x[2 if op == 32 else 3] = SC_WORKGROUP
        keep.append(x)
    m.ins = keep


def _layout(m, b, ty, done):
    """std430 layout, adding only the decorations needed by our output structure."""
    if ty in done: return done[ty]
    t, op = m.types[ty], m.op(ty)
    if op in (21, 22): a, n = 4, 4
    elif op == 20: raise ValueError('Boolean storage must be retyped before mesh layout')
    elif op == 23:
        a = 8 if t[3] == 2 else 16; n = 4 * t[3]
    elif op == 28:
        a, n = _layout(m, b, t[2], done); stride = (n + a - 1) // a * a
        b.deco(ty, 6, stride); n = stride * m.consts[t[3]][1][0]
    elif op == 30:
        a, n = 1, 0
        for k, child in enumerate(t[2:]):
            ca, cn = _layout(m, b, child, done); a = max(a, ca)
            n = (n + ca - 1) // ca * ca; b.member(ty, k, 35, n); n += cn
        n = (n + a - 1) // a * a
    else: raise ValueError(f'Unsupported mesh output type {op}')
    done[ty] = a, n; return a, n


def promote_function_locals(m):
    """Forward direct, nonvolatile local loads within their basic block.

    The debug translator emits a Function variable for nearly every intermediate.
    Driver inlining then clones thousands of variables for each memory helper call:
    real mesh modules can exhaust Turnip's compiler memory before execution.
    Keep each load's result ID with CopyObject, so literal operands need no rewriting.
    Never forward across a label or through an escaping/indirect pointer. Remove a
    variable and its stores only when every read was forwarded; otherwise preserve
    its memory at block boundaries. Calls cannot alias these nonescaping locals.
    """
    candidates = {x[2] for x in m.ins if x[0] & 65535 == 59 and x[3] == 7}
    allowed = set(candidates)
    for x in m.ins:
        op = x[0] & 65535
        refs = candidates.intersection(x[1:])
        if op == 71:
            if x[2] == 21: allowed.discard(x[1])  # Volatile also applies without memory operands.
            continue
        if op in (5, 59): continue
        if op == 61 and len(x) == 4: refs.discard(x[3])
        if op == 62 and len(x) == 3: refs.discard(x[1])
        allowed -= refs
    known, out, forwarded, remaining = {}, [], 0, set()
    for x in m.ins:
        op = x[0] & 65535
        if op in (54, 56, 248): known.clear()
        if op == 62 and x[1] in allowed: known[x[1]] = x[2]
        elif op == 61 and x[3] in allowed:
            if x[3] in known:
                x = ins(83, x[1], x[2], known[x[3]]); forwarded += 1
            else: remaining.add(x[3])
        out.append(x)
    removed = allowed - remaining
    m.ins = [x for x in out if not (
        (x[0] & 65535 in (5, 71) and x[1] in removed) or
        (x[0] & 65535 == 59 and x[2] in removed) or
        (x[0] & 65535 == 62 and x[1] in removed))]
    m.index()
    return forwarded, len(removed)


def _localize_generated(m, variables, setup):
    """Materialize immutable invocation values in each function that reads them.

    Each value is derived only from builtins and immutable dispatch/mapping buffers.
    Qualcomm 891.11 intermittently mixes payload addresses with Private temporaries;
    function-local values avoid that driver failure without disabling geometry.
    Helpers receive the same invocation values by recomputation, never a shared
    mutable variable. Keep the entry's SSA setup for its cooperative task copy.
    """
    initial = {x[1]: x[2] for x in setup if x[0] & 65535 == 62}
    for var in variables:
        if var is None: continue
        m.index()
        old = next(x for x in m.ins if x[0] & 65535 == 59 and x[2] == var)
        value = initial[var]
        uses, fn = set(), None
        for x in m.ins:
            op = x[0] & 65535
            if op == 54: fn = x[2]
            elif op == 56: fn = None
            elif op == 62 and x[1] == var:
                if x[2] != value: raise ValueError('Mutable virtual invocation state')
            elif fn is not None and var in x[1:]: uses.add(fn)
        ty = m.pointee(old[1])[1]
        pointer = m.add_type(32, 7, ty)
        # Remove the one generated initializer, then introduce one local per reader.
        m.ins = [x for x in m.ins if not (x[0] & 65535 == 62 and x[1] == var)]
        for fid in sorted(uses):
            local = m.new_id()
            roots = {var}
            a, z = m.function_range(fid)
            for x in m.ins[a:z]:
                if x[0] & 65535 in (65, 66) and x[3] in roots: roots.add(x[2])
            for x in list(m.ins[a:z]):
                op = x[0] & 65535
                if op in (65, 66) and x[2] in roots:
                    x[1] = m.add_type(32, 7, m.pointee(x[1])[1])
                if op in (61, 65, 66) and x[3] == var: x[3] = local
            # The generated setup is straight-line, side-effect-free except for
            # initial stores. Clone its SSA IDs; all referenced globals are read-only.
            ids, code = {}, []
            for x in setup:
                if x[0] & 65535 == 62: continue
                y = [x[0], *(ids.get(v, v) for v in x[1:])]
                ids[x[2]] = m.new_id(); y[2] = ids[x[2]]; code.append(y)
            code.append(ins(62, local, ids.get(value, value)))
            a, _ = m.function_range(fid)
            at = next(k for k in range(a, len(m.ins)) if m.ins[k][0] & 65535 == 248) + 1
            m.ins.insert(at, ins(59, pointer, local, 7))
            at = spv.first_block_insert_point(m, fid); m.ins[at:at] = code
        m.ins = [x for x in m.ins if not ((x[0] & 65535 == 59 and x[2] == var) or
                                        (x[0] & 65535 in (5, 71) and x[1] == var))]
        for i, x in enumerate(m.ins):
            if x[0] & 65535 != 15: continue
            _, end = spv.string_at(x, 3)
            m.ins[i] = ins(15, *x[1:end], *(v for v in x[end:] if v != var))
        m.index()


def _count_store(m, b, out_var, v3, actual_wg, lid, member):
    """A helper function (uint primitives) storing a mesh group's primitive count in its output structure.

    SPV_EXT_mesh_shader takes OpSetMeshOutputsEXT's arguments from the first invocation of the
    workgroup: only LocalInvocationIndex 0 stores, so the one word has a single writer (no race;
    the same invocation's earlier store of 0 is ordered before it by program order).
    """
    void = m.add_type(19); fty = m.add_type(33, void, b.u)
    fid, param, l0, live, merge = (m.new_id() for _ in range(5))
    b.code = []
    first = b.val(170, b.b, b.load(b.u, lid), b.c(0))
    b.emit(247, merge, 0); b.emit(250, first, live, merge); b.emit(248, live)
    real = b.val(81, b.u, b.load(v3, actual_wg), 0)
    b.emit(62, b.ac(b.u, out_var, b.c(0), real, b.c(member)), param)
    b.emit(249, merge); b.emit(248, merge); b.emit(253)
    body = b.code; b.code = []
    return fid, [ins(54, void, fid, 0, fty), ins(55, b.u, param), ins(248, l0), *body, ins(56)]


def lower(data):
    """Return (compute bytes, pulling-vertex bytes or None, metadata). Validated by the caller.

    A mesh group's output structure ends with its primitive count (OpSetMeshOutputsEXT's
    Primitive Count, 0 until the group sets it: SPV_EXT_mesh_shader allows no output write
    before the instruction, so a group that never executes it emits nothing). The pulling
    vertex stage reads it, the CullPrimitiveEXT flag when there is one and a triangle's
    indices before anything else: a corner of an unemitted, culled or zero-area (three equal
    indices) primitive only writes a clipped position (_vertex).
    """
    m = spv.Module(data)
    if m.model not in ('ts', 'ms'): raise ValueError('Expected a task or mesh module')
    task = m.model == 'ts'; b = Build(m)
    local = next((x[1:] for x in m.modes if x[0] == 17), None)
    if not local: raise ValueError('Task/mesh module has no fixed workgroup size')
    threads = local[0] * local[1] * local[2]
    payload_var = next((v for v, (_, sc) in m.vars.items() if sc == SC_PAYLOAD), None)
    payload_words = 0
    if payload_var:
        ar = m.pointee(m.vars[payload_var][0])[1]
        payload_words = m.consts[m.types[ar][3]][1][0]
    outputs = {v: m.pointee(p)[1] for v, (p, sc) in m.vars.items() if sc == 3}
    output_decos = {v: dict(m.decos.get(v, {})) for v in outputs}
    member_decos = {}
    for x in m.ins:
        if x[0] & 65535 == 72: member_decos.setdefault((x[1], x[2]), {})[x[3]] = x[4:]
    maxpr = next((x[1] for x in m.modes if x[0] == 5270), 0)
    points = any(x[0] == 27 for x in m.modes)
    # Native point culling is Boolean Output; storage buffers require an integer representation.
    culls = {v for v, d in output_decos.items() if d.get(11) == [5299]}
    out_types = dict(outputs)
    for v in culls:
        t = m.types[outputs[v]]; out_types[v] = m.add_type(28, b.u, t[3])
    oldwg = {v for v, d in m.decos.items() if d.get(11) == [26] and v in m.vars}
    if len(oldwg) > 1: raise ValueError('Multiple WorkgroupId inputs')
    original_ptrs = dict(m.vars)
    v3 = m.add_type(23, b.u, 3)
    actual_wg = b.input(v3, 26, force=True)
    lid = b.input(b.u, 29)  # LocalInvocationIndex; handles non-1D local sizes too.
    info = b.words(INFO); payload = b.words(PAYLOAD); offsets = b.words(OFFSETS)
    out_var = None; group_type = stride = 0
    members = {v: k for k, v in enumerate(out_types)}
    count_member = len(out_types); count_fn = None
    if not task:
        group_type = m.add_type(30, *out_types.values(), b.u)  # (the trailing uint: the primitive count)
        # BuiltIn/Block/Invariant are interface decorations, not storage-structure layout.
        nested = set()
        def visit(t):
            if t in nested: return
            nested.add(t)
            if m.op(t) == 28: visit(m.types[t][2])
            elif m.op(t) == 30:
                for c in m.types[t][2:]: visit(c)
        for t in out_types.values(): visit(t)
        m.ins = [x for x in m.ins if not ((x[0] & 65535 == 71 and x[1] in nested and x[2] in (2, 18)) or
                    (x[0] & 65535 == 72 and x[1] in nested and x[3] in (11, 18)))]
        m.index()
        _, stride = _layout(m, b, group_type, {})
        rt = m.add_type(29, group_type); b.deco(rt, 6, stride)
        block = m.add_type(30, rt); b.deco(block, 2); b.member(block, 0, 35, 0)
        out_var = b.buffer(block, OUT)
        count_id, count_fn = _count_store(m, b, out_var, v3, actual_wg, lid, count_member)
    # The old workgroup variable becomes a private emulated NVN workgroup ID. The new
    # builtin remains the real compute index used to address the bounded output batch.
    wg_ptrs = {}
    for v in oldwg:
        p, _ = original_ptrs[v]; wg_ptrs[v] = m.add_type(32, SC_PRIVATE, m.pointee(p)[1])
    # A mesh shader only reads its task payload. Keep those reads in the immutable
    # storage buffer rather than staging another shared-memory copy in every workgroup.
    payload_base = b.global_(b.u, SC_PRIVATE) if not task and payload_var else None
    removed = dict(outputs)
    if not task and payload_var: removed[payload_var] = None
    _remove_mesh_interface(m, removed, oldwg)
    m.index()
    for x in m.ins:
        if x[0] & 65535 == 59 and x[2] in wg_ptrs: x[1], x[3] = wg_ptrs[x[2]], SC_PRIVATE
    # Set up payload and virtual workgroup ID before the original entry's first instruction.
    b.code = []
    wg = b.load(v3, actual_wg); gx = b.val(81, b.u, wg, 0)
    li = b.load(b.u, lid)
    stride_words = b.info(info, 11)
    if task:
        virtual_x = b.add(gx, b.info(info, 19)); task_id = gx
    else:
        global_x = b.add(gx, b.info(info, 5))
        has_task = b.val(171, b.b, b.info(info, 16), b.c(0))
        # The distribution pass writes an explicit (task, local mesh group) pair per
        # generated workgroup. Empty tasks consume no slots; mesh invocation code needs
        # only two reads instead of a dependent binary search for every invocation.
        map_word = b.val(169, b.u, has_task, b.mul(global_x, b.c(2)), b.c(0))
        task_id = b.load(b.u, b.word(offsets, map_word))
        task_id = b.val(169, b.u, has_task, task_id, b.c(0))
        local_x = b.load(b.u, b.word(offsets, b.add(map_word, b.c(1))))
        virtual_x = b.val(169, b.u, has_task, local_x, b.add(global_x, b.info(info, 19)))
        if payload_base: b.emit(62, payload_base, b.mul(task_id, stride_words))
    for v in oldwg:
        vid = b.val(82, v3, virtual_x, wg, 0); b.emit(62, v, vid)
    setup = [list(x) for x in b.code]
    # Every group starts with no primitives: the output buffer holds earlier batches' groups.
    # (Not part of setup, which _localize_generated clones into helper functions.)
    if count_fn: b.emit(57, m.add_type(19), m.new_id(), count_id, b.c(0))
    at = spv.first_block_insert_point(m, m.entry_function()); m.ins[at:at] = b.code
    # Redirect native output accesses into the corresponding element of the SSBO group.
    rewritten, roots = [], dict.fromkeys(oldwg, 'wg')
    for x in list(m.ins):
        op = x[0] & 65535
        if op in (65, 66) and not task and payload_var and x[3] == payload_var:
            if len(x) != 5: raise ValueError('Unexpected task payload access chain')
            b.code = []
            word = b.add(b.load(b.u, payload_base), x[4])
            rewritten += b.code
            rewritten.append(ins(op, m.add_type(32, 12, b.u), x[2], payload, b.c(0), word))
        elif op in (65, 66) and x[3] in outputs:
            root = x[3]; b.code = []
            real = b.val(81, b.u, b.load(v3, actual_wg), 0)
            old_ty = m.pointee(x[1])[1]
            ty = b.u if root in culls else old_ty
            ptr = m.add_type(32, 12, ty)
            rewritten += b.code
            rewritten.append(ins(op, ptr, x[2], out_var, b.c(0), real, b.c(members[root]), *x[4:]))
            roots[x[2]] = 'cull' if root in culls else 'out'
        elif op in (65, 66) and roots.get(x[3]) == 'wg':
            y = list(x); y[1] = m.add_type(32, SC_PRIVATE, m.pointee(x[1])[1]); rewritten.append(y); roots[x[2]] = 'wg'
        elif op == 62 and roots.get(x[1]) == 'cull':
            b.code = []; v = b.val(169, b.u, x[2], b.c(1), b.c(0)); rewritten += b.code; rewritten.append(ins(62, x[1], v))
        elif op == 5294:
            if not task: raise ValueError('EmitMeshTasks in mesh module')
            b.code = []
            # SPV_EXT_mesh_shader defines EmitMeshTasks itself as a Workgroup
            # AcquireRelease/WorkgroupMemory control barrier. Preserve it before
            # invocations cooperatively copy words written by other invocations.
            b.emit(224, b.c(2), b.c(2), b.c(0x108))
            for k in range((payload_words + threads - 1) // threads):
                w = b.add(li, b.c(k * threads))
                def cp(w=w):
                    p = b.val(65, m.add_type(32, 4, b.u), payload_var, w)
                    v = b.load(b.u, p)
                    b.emit(62, b.word(payload, b.add(b.mul(gx, stride_words), w)), v)
                if (k + 1) * threads <= payload_words: cp()
                else: b.guarded(b.val(176, b.b, w, b.c(payload_words)), cp)
            b.emit(253); rewritten += b.code
        elif op == 5295:
            if task: raise ValueError('SetMeshOutputsEXT in task module')
            rewritten.append(ins(57, m.add_type(19), m.new_id(), count_id, x[2]))
        elif op == 15: continue
        else: rewritten.append(x)
    # add_type/add_const can introduce declarations while rewriting. Keep their final list,
    # replacing only the original function section so declarations are never lost.
    decls = [x for x in m.ins[:next(i for i, x in enumerate(m.ins) if x[0] & 65535 == 54)] if x[0] & 65535 != 15]
    start = next(i for i, x in enumerate(rewritten) if x[0] & 65535 == 54)
    # Retyped globals may name newly declared pointers. All global variables must follow
    # those types; their initializers are constants and do not depend on other globals.
    m.ins = [x for x in decls if x[0] & 65535 != 59] + [x for x in decls if x[0] & 65535 == 59] + rewritten[start:] + (count_fn or [])
    m.index()
    interface = [v for v, (_, sc) in m.vars.items() if sc != 7]
    ent = ins(15, 5, m.entry_function() if m.entry else 0, *spv.lit_string('main'), *interface)
    # The original entry ID was removed; recover it from the local-size execution mode.
    ent[2] = next(x[1] for x in m.ins if x[0] & 65535 == 16 and x[2] == 17)
    at = next(i for i, x in enumerate(m.ins) if x[0] & 65535 in (16, 5, 19)); m.ins.insert(at, ent)
    m.index()
    meta = dict(payload_words=payload_words, stride=stride, max_primitives=maxpr, topology=1 if points else 3)
    vertex = None if task else _vertex(m, out_var, group_type, out_types, output_decos, member_decos, meta)
    _localize_generated(m, [*oldwg, payload_base], setup)
    promote_function_locals(m)
    cs = m.data()
    if not task: cs = direct_outputs(cs)   # (the outputs straight into the output buffer: direct_outputs below)
    return cs, vertex, meta


def _vertex(compute, out_var, group_type, outputs, decos, member_decos, meta):
    """A small conventional vertex stage pulling one primitive corner from compute output."""
    # Keep only types/constants and output-buffer layout decorations; there are no game resources
    # or translated instructions in this stage.
    keep_types = set()
    def visit(t):
        if t in keep_types: return
        keep_types.add(t); x = compute.types[t]; op = x[0] & 65535
        if op in (23, 28, 29): visit(x[2])
        elif op == 30:
            for c in x[2:]: visit(c)
        elif op == 32: visit(x[3])
    visit(compute.vars[out_var][0])
    decls = []
    for x in compute.ins:
        op = x[0] & 65535
        if 19 <= op <= 39 and x[1] in keep_types: decls.append(x)
        elif op in (43, 44): decls.append(x)
        elif op == 59 and x[2] == out_var: decls.append(x)
    # Constants can name types not in the output structure (including booleans and vectors).
    # Include their scalar/vector types, in original topological declaration order.
    for x in decls:
        if x[0] & 65535 in (43, 44): visit(x[1])
    decls = [x for x in compute.ins if (19 <= (x[0] & 65535) <= 39 and x[1] in keep_types) or
             (x[0] & 65535 in (43, 44)) or (x[0] & 65535 == 59 and x[2] == out_var)]
    anns = [x for x in compute.ins if (x[0] & 65535 == 71 and
             (x[1] in keep_types or x[1] == out_var)) or (x[0] & 65535 == 72 and x[1] in keep_types)]
    # Module.add_type needs a function insertion point during construction.
    void, fid, ft, label = range(compute.header[3], compute.header[3] + 4)
    base = [ins(17, 1), ins(14, 0, 1), ins(15, 0, fid, *spv.lit_string('main'), out_var), *anns,
            *decls, ins(19, void), ins(33, ft, void), ins(54, void, fid, 0, ft), ins(248, label), ins(253), ins(56)]
    head = list(compute.header); head[3] += 4
    m = spv.Module(struct.pack('<%dI' % (5 + sum(map(len, base))), *head, *(a for x in base for a in x)))
    b = Build(m); vid = b.input(b.u, 42)  # VertexIndex
    v = b.load(b.u, vid); n = meta['max_primitives'] * meta['topology']
    group = b.val(134, b.u, v, b.c(n))
    prim = b.val(137, b.u, b.val(134, b.u, v, b.c(meta['topology'])), b.c(meta['max_primitives']))
    corner = b.val(137, b.u, v, b.c(meta['topology']))
    idx_member = next(i for i, v0 in enumerate(outputs) if decos[v0].get(11) in ([5296], [5294]))
    cull_member = next((i for i, v0 in enumerate(outputs) if decos[v0].get(11) == [5299]), None)
    # A primitive that rasterizes nothing only writes a position outside the clip volume; every
    # other output load is behind the branch. That is one the group did not emit (at or past its
    # OpSetMeshOutputsEXT count, the trailing member), one it culled (CullPrimitiveEXT), and a
    # triangle whose three indices are equal (zero area: no fragment in fill mode). The translator
    # declares the maximum count and gives each primitive past the guest's real count the indices
    # (0, 0, 0) (its epilogue), so the last case is what makes those primitives cheap.
    count = b.load(b.u, b.ac(b.u, out_var, b.c(0), group, b.c(len(outputs))))
    live = b.val(176, b.b, prim, count)
    if cull_member is not None:
        cv = b.load(b.u, b.ac(b.u, out_var, b.c(0), group, b.c(cull_member), prim))
        live = b.val(167, b.b, live, b.val(170, b.b, cv, b.c(0)))
    if meta['topology'] != 1:
        v3 = m.add_type(23, b.u, 3)
        tri = b.load(v3, b.ac(v3, out_var, b.c(0), group, b.c(idx_member), prim))
        i0, i1, i2 = (b.val(81, b.u, tri, n) for n in range(3))
        area = b.val(166, b.b, b.val(171, b.b, i0, i1), b.val(171, b.b, i1, i2))
        live = b.val(167, b.b, live, area)
        index = b.val(77, b.u, tri, corner)
    on, off, merge = m.new_id(), m.new_id(), m.new_id()
    b.emit(247, merge, 0); b.emit(250, live, on, off); b.emit(248, on)
    if meta['topology'] == 1:  # (a point's index is needed only when it is drawn)
        index = b.load(b.u, b.ac(b.u, out_var, b.c(0), group, b.c(idx_member), prim))
    clipped = []  # (variable, value) stores of the dead branch
    for k, (orig, ty) in enumerate(outputs.items()):
        d = decos[orig]
        if d.get(11) in ([5296], [5294], [5299]): continue
        el = m.types[ty][2]
        perprim = 5271 in d; which = prim if perprim else index
        if m.op(el) == 30:
            for j, et in enumerate(m.types[el][2:]):
                built = member_decos.get((el, j), {}).get(11)
                if not built: raise ValueError('Mesh per-vertex block member without builtin')
                value = b.load(et, b.ac(et, out_var, b.c(0), group, b.c(k), which, b.c(j)))
                dest = b.global_(et, 3); b.deco(dest, 11, *built)
                if built == [0]:
                    b.deco(dest, 18)  # Preserve position invariance across depth/color passes.
                    f2, f1 = m.add_const(b.f, 0x40000000), m.add_const(b.f, 0x3f800000)
                    cc = m.new_id(); m._insert_decl(ins(44, et, cc, f2, f2, f2, f1))
                    clipped.append((dest, cc))
                elif built == [1] and et == b.f:
                    clipped.append((dest, m.add_const(b.f, 0x3f800000)))  # (PointSize 1: defined, not needed)
                b.emit(62, dest, value)
        else:
            if 30 not in d: raise ValueError('Mesh generic without location')
            dest = b.global_(el, 3); b.deco(dest, 30, *d[30])
            if perprim: b.deco(dest, 14)  # Flat values duplicated on all primitive corners.
            b.emit(62, dest, b.load(el, b.ac(el, out_var, b.c(0), group, b.c(k), which)))
    b.emit(249, merge); b.emit(248, off)
    for dest, value in clipped: b.emit(62, dest, value)
    b.emit(249, merge); b.emit(248, merge)
    at = spv.first_block_insert_point(m, fid); m.ins[at:at] = b.code
    m.index(); io = [v for v, (_, sc) in m.vars.items() if sc != 7]
    for i, x in enumerate(m.ins):
        if x[0] & 65535 == 15: m.ins[i] = ins(15, 0, fid, *spv.lit_string('main'), *io)
    m.index(); spv.gl_depth(m, 0)
    return m.data()


# ---------- Direct mesh outputs (direct_outputs) ----------
# The driver's statistics (DM_VK_PIPE_STATS, VK_KHR_pipeline_executable_properties) put Ocarina of Time's heavy mesh computes at 4-6 % ALU occupancy on the Adreno 830
# (Qualcomm 891.14), with 8-13 KB of workgroup memory per 32-thread group: almost all of it the Workgroup copy of the planes that vk_android_spv.defer_mesh_plane_stores
# adds (2026-11-12, so Turnip would not die compiling the store chain inlined at every call). On the compute form only (the native MeshEXT module keeps the deferral):
#   _undefer: the calls of mesh_plane_defer go back to mesh_plane_store, and the copy, its written mask and the clear / flush loops are removed;
#   _direct_stores: mesh_plane_store's chains of `if (plane == p) out.member[index]... = value` (one per plane, inlined at every call) become one lookup in two Private
#     tables (byte offset and stride of each plane, from the output block's Offset / ArrayStride decorations; ~0 = a plane with no output, not stored) and one store
#     into a uint[] view of the same output binding (after DroidPlay 2e802839, MIT: each mesh output stored straight into the output buffer). The GL depth copy of
#     planes 2 / 3 (mesh_position_zw) is kept. The pulling vertex stage reads the same layout as before.
# Ocarina of Time by Kokiri Forest's scarecrow on the POCO F7 Ultra (2026-11-15, as replay overrides): 18.7 / 18.5 fps → 22.7 / 22.5, GPU 47.7 / 48.0 → 39.1 / 38.9 ms;
# by Mido 30.0 (capped) → 41.6 fps, GPU 25.2 → 20.7 ms; the same picture; compile time no worse (docs/RESULTADOS-ETAPA1-2026-11-15.md, tenth step).
OP_NAME, OP_FUNCTION_CALL, OP_VARIABLE, OP_DECORATE, OP_ENTRY_POINT = 5, 57, 59, 71, 15


def _undefer(data):
    m = spv.Module(data)
    fid = {n: i for i, n in m.names.items() if n in ('mesh_plane_store', 'mesh_plane_defer', 'mesh_plane_flush', 'mesh_plane_clear')}
    if len(fid) != 4: return None, 'no deferral'
    store, defer, flush, clear = fid['mesh_plane_store'], fid['mesh_plane_defer'], fid['mesh_plane_flush'], fid['mesh_plane_clear']
    # the copy and the mask: the Workgroup variables the deferred store writes
    a, e = m.function_range(defer)
    used = {x[3] for x in m.ins[a:e + 1] if x[0] & 0xffff == 65}   # OpAccessChain bases
    copyvars = {v for v in used if m.vars.get(v, (0, 0))[1] == spv.SC_WORKGROUP}
    if len(copyvars) != 2: return None, f'copy variables {copyvars}'
    n = 0
    for x in m.ins:
        if x[0] & 0xffff == OP_FUNCTION_CALL and x[3] == defer: x[3] = store; n += 1
    # drop main's calls of clear / flush, then the three functions, the variables, their names and decorations, and the entry point's interface entries
    m.ins = [x for x in m.ins if not (x[0] & 0xffff == OP_FUNCTION_CALL and x[3] in (flush, clear))]
    m.index()
    for f in (defer, flush, clear):
        a, e = m.function_range(f); del m.ins[a:e + 1]; m.index()
    drop = copyvars | {defer, flush, clear}
    out = []
    for x in m.ins:
        o = x[0] & 0xffff
        if o == OP_VARIABLE and x[2] in copyvars: continue
        if o in (OP_NAME, OP_DECORATE) and x[1] in drop: continue
        if o == OP_ENTRY_POINT:
            name_end = 3
            while True:   # the name: a literal string, up to and with the word holding its 0 byte
                w = x[name_end]; name_end += 1
                if any(((w >> s) & 0xff) == 0 for s in (0, 8, 16, 24)): break
            x = x[:name_end] + [i for i in x[name_end:] if i not in copyvars]; x[0] = len(x) << 16 | OP_ENTRY_POINT
        out.append(x)
    m.ins = out; m.index()
    return m.data(), n



OP = dict(IEQ=170, SMERGE=247, BRC=250, LABEL=248, BR=249, AC=65, STORE=62, LOAD=61, CEXT=81, VAR=59, MDECO=72)
NONE = 0xFFFFFFFF


def layout(m):
    md = {}
    for x in m.ins:
        if x[0] & 0xffff == OP['MDECO']: md.setdefault((x[1], x[2]), {})[x[3]] = x[4:]
    return md


def const(m, i):
    c = m.consts.get(i)
    return c[1][0] if c and len(c[1]) == 1 else None


def offset_of(m, md, ty, idx):   # byte offset of constant indices idx inside type ty (Offset / ArrayStride decorations, 4-byte scalars)
    off = 0
    for i in idx:
        t = m.types[ty]; o = t[0] & 0xffff
        if o == 30: off += md[(ty, i)][35][0]; ty = t[2 + i]
        elif o == 23: off += 4 * i; ty = t[2]
        elif o in (28, 29): off += m.decos[ty][6][0] * i; ty = t[2]
        else: raise ValueError(f'type op {o}')
    return off


def _direct_stores(m):
    store = next((i for i, n in m.names.items() if n == 'mesh_plane_store'), None)
    if store is None: return 0
    md = layout(m)
    a, e = m.function_range(store)
    body = m.ins[a:e + 1]
    params = [x[2] for x in body if x[0] & 0xffff == 55]
    word_p, value_p = params
    # the chains: blocks `%c = IEqual %plane %const; SelectionMerge %M; BranchConditional %c %T %M; Label %T ... Branch %M; Label %M`, grouped by %plane
    chains = {}
    k = 0
    while k < len(body):
        x = body[k]
        if x[0] & 0xffff == OP['IEQ'] and const(m, x[4]) is not None and k + 3 < len(body) and body[k + 1][0] & 0xffff == OP['SMERGE'] \
                and body[k + 2][0] & 0xffff == OP['BRC'] and body[k + 2][1] == x[2]:
            M = body[k + 1][1]
            end = next(j for j in range(k + 3, len(body)) if body[j][0] & 0xffff == OP['LABEL'] and body[j][1] == M)
            blk = body[k + 3:end]
            chains.setdefault(x[3], []).append((k, end, const(m, x[4]), blk))
            k = end + 1; continue
        k += 1
    if not chains: return 0
    u = m.add_type(21, 32, 0); bty = m.add_type(20); v3 = m.add_type(23, u, 3)
    C = lambda n: m.add_const(u, n)
    b = Build(m)
    raw = b.words(OUT)
    # the output block: its runtime array's stride is the group stride
    out_var = None; zw_var = next((i for i, n in m.names.items() if n == 'mesh_position_zw'), None)
    new_body = list(body); shift = 0
    replaced = 0
    for plane_var, blocks in chains.items():
        # each block: an AccessChain into the output (base, 0, group, member, index, consts...), maybe a zw store
        table = {}; idx_var = None; wg_load_var = None; zw = {}; gstride = 0
        for (_, _, p, blk) in blocks:
            for y in blk:
                if y[0] & 0xffff == OP['AC'] and m.vars.get(y[3], (0, 0))[1] == 12 and len(y) >= 8:
                    out_var = y[3]
                    member = const(m, y[6]); idx_var = y[7]; rest = [const(m, z) for z in y[8:]]
                    if member is None: raise ValueError('mesh_plane_store: output member index is not a constant')
                    blockty = m.pointee(m.vars[out_var][0])[1]; rt = m.types[blockty][2]; group_ty = m.types[rt][2]
                    gstride = m.decos[rt][6][0]
                    mty = m.types[group_ty][2 + member]
                    table[p] = (md[(group_ty, member)][35][0] + offset_of(m, md, m.types[mty][2], rest), m.decos[mty][6][0])
                if y[0] & 0xffff == OP['LOAD'] and y[1] == v3: wg_load_var = y[3]
                if y[0] & 0xffff == OP['AC'] and zw_var is not None and y[3] == zw_var: zw[p] = 1
        if idx_var is None or not table: continue
        n = max(*table, *(zw or (0,))) + 1
        offs = [table.get(p, (NONE, 0))[0] for p in range(n)]; strides = [table.get(p, (0, 0))[1] for p in range(n)]
        arr = m.add_type(28, u, C(n)); pp = m.add_type(32, SC_PRIVATE, arr)
        tv = {}
        for name, vals in (('off', offs), ('str', strides)):
            cc = m.new_id(); m._insert_decl([(3 + n) << 16 | 44, arr, cc, *[C(v) for v in vals]])
            var = m.new_id(); m._insert_decl([5 << 16 | OP['VAR'], pp, var, SC_PRIVATE, cc]); tv[name] = var
        # the new code
        b.code = []
        inr = b.val(176, bty, plane_var, C(n))   # ULessThan
        pc = b.val(169, u, inr, plane_var, C(0))   # Select
        off = b.load(u, b.val(65, m.add_type(32, SC_PRIVATE, u), tv['off'], pc))
        st = b.load(u, b.val(65, m.add_type(32, SC_PRIVATE, u), tv['str'], pc))
        ok = b.val(167, bty, inr, b.val(171, bty, off, C(NONE)))   # LogicalAnd, INotEqual
        def put(wg_load_var=wg_load_var, gstride=gstride, off=off, idx_var=idx_var, st=st):   # (bound now: called below, in this iteration)
            g = b.val(OP['CEXT'], u, b.load(v3, wg_load_var), 0)
            byte = b.add(b.add(b.mul(g, C(gstride)), off), b.mul(idx_var, st))
            w = b.val(194, u, byte, C(2))   # ShiftRightLogical
            b.emit(OP['STORE'], b.word(raw, w), value_p)
        b.guarded(ok, put)
        if zw:
            zp = sorted(zw)
            isz = b.val(166, bty, b.val(170, bty, plane_var, C(zp[0])), b.val(170, bty, plane_var, C(zp[-1])))   # LogicalOr of IEquals
            def putz(plane_var=plane_var, zp=zp, idx_var=idx_var):
                hi = b.val(169, u, b.val(170, bty, plane_var, C(zp[-1])), C(1), C(0)) if len(zp) > 1 else C(0)
                p_ = b.val(65, m.add_type(32, SC_WORKGROUP, u), zw_var, b.add(b.mul(idx_var, C(2)), hi))
                b.emit(OP['STORE'], p_, value_p)
            b.guarded(isz, putz)
        first, last = blocks[0][0], blocks[-1][1]
        new_body[first + shift:last + 1 + shift] = b.code[:-1] if False else b.code
        # (b.code ends with the last guard's merge label, as the replaced span ended with the last block's merge label)
        shift += len(b.code) - (last + 1 - first)
        replaced += len(blocks)
    if not replaced: return 0
    # the ids used in the span must not be needed after it: the replaced blocks' own results only feed their own stores
    m.index(); a, e = m.function_range(store)   # (the declarations added above moved the function)
    m.ins[a:e + 1] = new_body
    # the entry point lists every global (SPIR-V 1.4)
    m.index()
    ent = m.entry
    for v in (raw, *[x[2] for x in m.ins if x[0] & 0xffff == OP['VAR'] and x[3] == SC_PRIVATE]):
        if v not in ent[3:]: ent.append(v)
    ent[0] = len(ent) << 16 | 15
    m.index()
    return replaced



def direct_outputs(data):
    """A lowered mesh compute module (bytes) → the same with direct outputs (bytes); unchanged when it has neither the deferral nor a plane store chain"""
    a, _ = _undefer(data)
    m = spv.Module(a if a is not None else data)
    r = _direct_stores(m)
    return m.data() if (r or a is not None) else data
