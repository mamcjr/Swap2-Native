// Two task / mesh compute workgroups in one 64-lane subgroup (vka_mesh.inc, DM_VK_MESH_PACK). Self-contained (libc only), so that dm/host/tests/t_android_meshpack.c
// builds it alone.
//
// Why: every task and mesh program of an NVN2 game is a 32-invocation workgroup, one Ampere warp (Ocarina of Time: 500 mesh and 18 of its 20 task programs, local size
// 32x1x1). Their compute forms (DMSH_VK_MOD_MESH_COMPUTE) keep that size, and a GPU whose subgroups are 64 lanes wide (Qualcomm's Adreno driver: compute fixed at 64;
// Turnip pinned to 64) runs each workgroup as one wave with lanes 32-63 switched off: half of every ALU instruction, register file slot and latency-hiding wave of the
// fallback's heaviest kernels is spent on nothing. This rewrite makes the workgroup 64 invocations, two of the program's workgroups side by side, one per 32-lane half of
// the subgroup, and the host dispatches half as many workgroups (rounded up; the controller writes that record at info[20..22], selftest/mesh_control.comp).
//
// Why it is exact. The library is built for 64-lane subgroups (vka_shader.c sg_plan: "a module built for 64 lanes splits ballots and shuffles into 32-lane halves"):
// the translator (Ryujinx's, for a host subgroup over 32) already makes the lane ID lane & 31, a shuffle's source index | (lane & 96), and a ballot's or a lane mask's
// word the half's (lane & 32 ? .y : .x); votes are ballots. So each half's warp instructions only ever see that half. What the rewrite changes:
// - LocalSize 32 -> 64 (a constant decorated WorkgroupSize loses the decoration, which would override it: its uses keep the program's 32);
// - every Workgroup array of L elements becomes 2L, and each access chain into it adds h * L to its first index, h = the half (lane >> 5): each program workgroup keeps
//   shared memory of its own;
// - a load of LocalInvocationIndex gives lane & 31, of LocalInvocationId (lane & 31, 0, 0);
// - a load of WorkgroupId gives x = min(2 * id.x + h, n - 1), n = info[0], the program's workgroup count in this dispatch (the controller's unpacked record: the task
//   count before the task dispatch, the batch's mesh groups before a mesh dispatch); NumWorkgroups gives x = n. With an odd n the second half of the last workgroup
//   runs the program's last workgroup a second time, in the same wave, on the same data: the same values to the same addresses in the same instructions. A module with
//   an atomic on memory outside the workgroup (a guest counter) would count twice there and is left as it is.
// "lane" is SubgroupLocalInvocationId (the translator's own lane ID; consistent with the ballots and shuffles by construction). A module without it, which then has no
// subgroup operation that could tell the halves apart, takes LocalInvocationIndex instead. The pipeline must run the 64 invocations as one 64-lane subgroup: the host
// packs only where compute subgroups are 64 lanes (fixed, or pinned with VkPipelineShaderStageRequiredSubgroupSizeCreateInfo, full subgroups required where the
// device has computeFullSubgroups). A control barrier is then within one wave: the halves' different control flow can never wait on each other.
// Left as it is (the caller dispatches the library's module): another local size, Workgroup memory other than arrays of 32-bit scalars or vectors accessed only through
// access chains, a builtin used other than by a plain load (or GlobalInvocationId at all), group operations with another scope than the subgroup, explicit workgroup
// layouts, and atomics on memory outside the workgroup other than a 32-bit add. An add there (a guest counter) is the one place the repeated workgroup would act
// twice: in its half it adds 0 instead and takes the result its twin got, the same lane & 31 of the same subgroup (a shuffle), so both halves go on alike.
// Ocarina of Time's library (chain 45cedbd878f7): 500 of 500 mesh and 18 of 20 task computes are rewritten (the 2 others are 1 x 1 x 1); every rewritten module passes
// spirv-val (t_android_meshpack.c with DM_MESHPACK_DIR).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { VKA_MESH_PACK_LANES = 64, VKA_MESH_PACK_INFO_DISPATCH = 20 };   // the packed dispatch record's first word in the fallback's info block (selftest/mesh_control.comp)
typedef struct { uint32_t wg_bytes, chains, loads, made_lane, atomics; } vka_mesh_pack_stats;   // the packed workgroup's shared memory; access chains and builtin loads rewritten

// Instructions with no result (word 2 is not a result id) among those a module can hold; OpLabel and the declarations are told apart by the caller
static int vmp_no_result(uint32_t op) {
    switch (op) {
    case 0: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 14: case 15: case 16: case 17: case 56: case 62: case 63: case 64: case 71: case 72: case 74:
    case 75: case 99: case 218: case 219: case 224: case 225: case 228: case 246: case 247: case 248: case 249: case 250: case 251: case 252: case 253: case 254: case 255:
    case 256: case 257: case 317: case 330: case 331: case 332: case 4416: case 5380: case 5294: case 5295: case 5632: case 5633: return 1;
    default: return 0;
    }
}
// Word k of a function-body instruction that is a literal, not an id (others: none). A literal mistaken for an id only refuses a module, never changes one
static int vmp_lit(uint32_t op, uint32_t k) {
    switch (op) {
    case 12: return k == 4;                         // OpExtInst: the instruction number
    case 61: return k >= 4;                         // OpLoad: memory operands
    case 62: return k >= 3;                         // OpStore
    case 79: return k >= 5;                         // OpVectorShuffle
    case 81: return k >= 4;                         // OpCompositeExtract
    case 82: return k >= 5;                         // OpCompositeInsert
    case 247: return k == 2;                        // OpSelectionMerge
    case 246: return k >= 3;                        // OpLoopMerge
    case 250: return k >= 4;                        // OpBranchConditional: weights
    case 251: return k >= 3 && !((k - 3) & 1);      // OpSwitch: the case literals
    case 8: return k >= 2;                          // OpLine
    case 87: case 88: case 91: case 92: case 95: case 98: return k == 5;
    case 89: case 90: case 93: case 94: case 96: case 97: return k == 6;
    case 99: return k == 4;                         // OpImageWrite: image operands' mask
    default: return 0;
    }
}
static int vmp_atomic(uint32_t op) { return (op >= 227 && op <= 242) || op == 6035 || op == 5614 || op == 5615 || op == 4445 || op == 4446; }

typedef struct { uint32_t *w; size_t n, cap; int oom; } vmp_buf;
static void vmp_put(vmp_buf *b, const uint32_t *x, size_t k) {
    if (b->oom) return;
    if (b->n + k > b->cap) {
        size_t c = b->cap ? b->cap : 1024; while (c < b->n + k) c *= 2;
        uint32_t *p = realloc(b->w, c * 4); if (!p) { b->oom = 1; return; }
        b->w = p; b->cap = c;
    }
    memcpy(b->w + b->n, x, k * 4); b->n += k;
}
#define VMP(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; uint32_t h_ = (uint32_t)(sizeof a_ / 4 + 1) << 16 | (op); vmp_put(&B, &h_, 1); vmp_put(&B, a_, sizeof a_ / 4); } while (0)

// w[n]: a task or mesh program's compute module; info_binding: the fallback's info block binding (DMSH_VK_MESH_INFO, set 0); guest_adds: 0 leaves a module with an
// add to guest memory as it is. -> the packed module (malloc'd, *on words)
// or NULL with why: left as it is (why says what), or out of memory
static uint32_t *vka_mesh_pack(const uint32_t *w, size_t n, uint32_t info_binding, int guest_adds, size_t *on, vka_mesh_pack_stats *st, char *why, size_t whyn) {
#define REFUSE(...) do { snprintf(why, whyn, __VA_ARGS__); goto fail; } while (0)
    *on = 0; if (why && whyn) why[0] = 0;
    vka_mesh_pack_stats s0 = {0}; if (st) *st = s0;
    uint32_t *rt = NULL, *def = NULL, *bi = NULL, *bind = NULL, *set = NULL, *bcv = NULL, *bcc = NULL; uint8_t *iswg = NULL;
    vmp_buf B = {0};
    if (n < 5 || w[0] != 0x07230203u) { snprintf(why, whyn, "not a SPIR-V module"); return NULL; }
    uint32_t bound = w[3], version = w[1];
    if (bound < 2 || bound > (1u << 22)) { snprintf(why, whyn, "id bound %u", bound); return NULL; }
    rt = calloc(bound, 4); def = calloc(bound, sizeof *def); bi = calloc(bound, 4); bind = calloc(bound, 4); set = calloc(bound, 4); iswg = calloc(bound, 1); bcv = calloc(bound, 4); bcc = calloc(bound, 4);
    if (!rt || !def || !bi || !bind || !set || !iswg || !bcv || !bcc) REFUSE("out of memory");
    for (uint32_t i = 0; i < bound; i++) bi[i] = bind[i] = set[i] = UINT32_MAX;
    // ---- scan ----
    int cap_shuffle = 0; uint32_t atoms = 0;
    size_t lsz = 0, ep = 0, first_fn = 0, first_ann = 0, first_type = 0; int neps = 0, in_fn = 0;
    uint32_t tu = 0, ti = 0, tbool = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) REFUSE("malformed instruction at word %zu", i);
        const uint32_t *x = w + i;
        if (op == 17 && len >= 2) { if (x[1] >= 4428 && x[1] <= 4430) REFUSE("explicit workgroup layout"); if (x[1] == 65) cap_shuffle = 1; }
        else if (op == 15) { neps++; ep = i; if (len < 3 || x[1] != 5) REFUSE("not a GLCompute entry point"); }
        else if (op == 16 && len >= 3 && x[2] == 17) { if (len != 6 || x[3] != 32 || x[4] != 1 || x[5] != 1) REFUSE("local size %u x %u x %u, not 32 x 1 x 1", len > 3 ? x[3] : 0, len > 4 ? x[4] : 0, len > 5 ? x[5] : 0); lsz = i; }
        else if (op == 331) REFUSE("an execution mode by id (LocalSizeId)");
        else if ((op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633) {
            if (!first_ann) first_ann = i;
            if (op == 71 && len >= 4 && x[1] < bound) { if (x[2] == 11) bi[x[1]] = x[3]; else if (x[2] == 33) bind[x[1]] = x[3]; else if (x[2] == 34) set[x[1]] = x[3]; }
            if (op == 72 && len >= 4 && x[3] == 11) REFUSE("a builtin block member");
        } else if (op >= 19 && op <= 39 && len >= 2 && x[1] < bound) {
            if (!first_type) first_type = i;
            def[x[1]] = i;
            if (op == 21 && len == 4 && x[2] == 32) { if (!x[3] && !tu) tu = x[1]; if (x[3] && !ti) ti = x[1]; }
            if (op == 20 && !tbool) tbool = x[1];
        } else if (op == 54) { in_fn = 1; if (!first_fn) first_fn = i; if (len >= 3 && x[2] < bound) { def[x[2]] = i; rt[x[2]] = x[1]; } }
        else if (op == 56) in_fn = 0;
        else if (op == 59 && len >= 4 && x[2] < bound) {
            def[x[2]] = i; rt[x[2]] = x[1];
            if (x[3] == 4 && !in_fn) { iswg[x[2]] = 1; if (len > 4) REFUSE("a workgroup variable with an initializer"); }
        } else if (op != 248 && !vmp_no_result(op) && len >= 3 && x[2] < bound && op != 11 && op != 73) { def[x[2]] = i; rt[x[2]] = x[1]; }
        i += len;
    }
    if (neps != 1) REFUSE("%d entry points", neps);
    if (!lsz) REFUSE("no LocalSize");
    if (!tu) REFUSE("no 32-bit unsigned integer type");
    if (!first_fn || !first_type) REFUSE("no function");
    // the builtin and info variables (Input / StorageBuffer OpVariables with those decorations)
    uint32_t v_lid = 0, v_liid = 0, v_wg = 0, v_nwg = 0, v_lane = 0, v_info = 0;
    for (uint32_t id = 1; id < bound; id++) {
        if (!def[id] || (w[def[id]] & 0xffff) != 59) continue;
        switch (bi[id]) {
        case 29: v_lid = id; break; case 27: v_liid = id; break; case 26: v_wg = id; break; case 24: v_nwg = id; break; case 41: v_lane = id; break;
        case 28: REFUSE("GlobalInvocationId"); case UINT32_MAX: break; default: break;
        }
        if (bind[id] == info_binding && (set[id] == 0 || set[id] == UINT32_MAX)) v_info = id;
    }
    // a pointer type's storage class and pointee
#define PTR_SC(t) ((t) < bound && def[t] && (w[def[t]] & 0xffff) == 32 ? w[def[t] + 2] : UINT32_MAX)
#define PTR_TO(t) ((t) < bound && def[t] && (w[def[t]] & 0xffff) == 32 ? w[def[t] + 3] : 0)
#define TOP(t) ((t) < bound && def[t] ? (w[def[t]] & 0xffff) : 0)
    uint32_t v3 = 0;
    const uint32_t vec_vars[3] = {v_liid, v_wg, v_nwg};
    for (int k = 0; k < 3; k++) {
        if (!vec_vars[k]) continue;
        uint32_t t = PTR_TO(rt[vec_vars[k]]);
        if (TOP(t) != 23 || w[def[t] + 2] != tu || w[def[t] + 3] != 3) REFUSE("a builtin vector not uvec3");
        if (!v3) v3 = t;
    }
    if (v_lid && PTR_TO(rt[v_lid]) != tu) REFUSE("LocalInvocationIndex not a uint");
    if (v_lane && PTR_TO(rt[v_lane]) != tu) REFUSE("SubgroupLocalInvocationId not a uint");
    if ((v_wg || v_nwg)) {
        if (!v_info) REFUSE("no info block at binding %u", info_binding);
        uint32_t S = PTR_TO(rt[v_info]);
        if (PTR_SC(rt[v_info]) != 12 || TOP(S) != 30 || (w[def[S]] >> 16) < 3) REFUSE("info block not a storage buffer struct");
        uint32_t RA = w[def[S] + 2];
        if (TOP(RA) != 29 || w[def[RA] + 2] != tu) REFUSE("info block not a uint array");
    }
    // the workgroup arrays: length and element size
    uint32_t nwgv = 0, wg_bytes = 0;
    for (uint32_t id = 1; id < bound; id++) {
        if (!iswg[id]) continue;
        uint32_t A = PTR_TO(rt[id]);
        if (TOP(A) != 28) REFUSE("workgroup variable %u not an array", id);
        uint32_t E = w[def[A] + 2], L = w[def[A] + 3];
        if (TOP(L) != 43 || (w[def[L]] >> 16) != 4 || TOP(w[def[L] + 1]) != 21) REFUSE("workgroup array length not a 32-bit constant");
        uint32_t len = w[def[L] + 3], es = 0;
        if ((TOP(E) == 21 || TOP(E) == 22) && w[def[E] + 2] == 32) es = 4;
        else if (TOP(E) == 23) { uint32_t C = w[def[E] + 2]; if ((TOP(C) == 21 || TOP(C) == 22) && w[def[C] + 2] == 32) es = 4 * w[def[E] + 3]; }
        if (!es) REFUSE("workgroup array of another element type");
        if (len > (1u << 20)) REFUSE("workgroup array too long");
        wg_bytes += 2 * len * es; nwgv++;
    }
    // every use of the special variables in the functions, the group operations' scopes and the atomics' memory
    uint32_t chains = 0, loads = 0;
    for (size_t i = first_fn; i < n;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
        for (uint32_t k = 1; k < len; k++) {
            uint32_t id = x[k];
            if (id >= bound || vmp_lit(op, k)) continue;
            int vecb = id && (id == v_liid || id == v_wg || id == v_nwg);
            int special = iswg[id] || bcv[id] || vecb || (id && id == v_lid);
            if (!special) continue;
            if (op == 61 && k == 3 && !iswg[id] && PTR_TO(rt[id]) == x[1]) { loads++; continue; }
            // a component of a builtin vector (gl_WorkGroupID.x): its chain stays, its loads are rewritten
            if ((op == 65 || op == 66) && k == 3 && vecb && len == 5 && x[4] < bound && TOP(x[4]) == 43 && w[def[x[4]] + 3] < 3 && PTR_TO(x[1]) == tu && x[2] < bound) {
                bcv[x[2]] = id; bcc[x[2]] = w[def[x[4]] + 3]; continue;
            }
            if ((op == 65 || op == 66) && k == 3 && iswg[id] && len >= 5) {
                uint32_t it = rt[x[4]];
                if (TOP(it) != 21 || w[def[it] + 2] != 32) REFUSE("a workgroup index not a 32-bit integer");
                if (w[def[it] + 3] && !ti) REFUSE("a signed workgroup index without the int type");
                chains++; continue;
            }
            REFUSE("a %s used by instruction %u", iswg[id] ? "workgroup variable" : "builtin", op);
        }
        if (op >= 333 && op <= 366 && len >= 4) {
            uint32_t sc = x[3];
            if (TOP(sc) != 43 || w[def[sc] + 3] != 3) REFUSE("a group operation (%u) whose scope is not the subgroup", op);
        }
        if (op >= 261 && op <= 269) REFUSE("an old group operation (%u)", op);
        if (vmp_atomic(op) && len >= 4) {
            uint32_t p = op == 228 ? x[1] : x[3];
            uint32_t psc = p < bound ? PTR_SC(rt[p]) : UINT32_MAX;
            if (psc != 4 && psc != 7) {
                // an add to guest memory: the repeated workgroup adds 0 and takes its twin's result (below); any other atomic there would act twice
                uint32_t at = rt[x[2]];
                int ok = op == 234 && len == 7 && TOP(at) == 21 && w[def[at] + 2] == 32 && (!w[def[at] + 3] || ti) && v_lane && v_wg && v_info && cap_shuffle && guest_adds;
                if (!ok) REFUSE("an atomic (%u) on memory outside the workgroup (storage class %d)", op, (int)psc);
                atoms++;
            }
        }
        i += len;
    }
    // ---- new ids ----
    uint32_t next = bound;
#define NEW() (next++)
    uint32_t c0 = NEW(), c1 = NEW(), c5 = NEW(), c31 = NEW(), c3 = atoms ? NEW() : 0, c0i = atoms && ti ? NEW() : 0;
    uint32_t new_bool = 0, new_psbu = 0, new_piu = 0, new_var = 0, src = v_lane ? v_lane : v_lid;
    if (!tbool && (v_wg || atoms)) tbool = new_bool = NEW();
    uint32_t psbu = 0;
    if (v_wg || v_nwg || atoms) {
        for (uint32_t t = 1; t < bound && !psbu; t++) if (TOP(t) == 32 && w[def[t] + 2] == 12 && w[def[t] + 3] == tu) psbu = t;
        if (!psbu) psbu = new_psbu = NEW();
    }
    if (!src) {   // no lane and no local index: one of our own
        uint32_t piu = 0;
        for (uint32_t t = 1; t < bound && !piu; t++) if (TOP(t) == 32 && w[def[t] + 2] == 1 && w[def[t] + 3] == tu) piu = t;
        if (!piu) piu = new_piu = NEW();
        src = new_var = NEW();
        rt[0] = piu;   // (unused slot: the new variable's type, read below)
    }
    // per workgroup array: the doubled length, its array and pointer types, and its length (a half's offset, h * L; bit-cast for a signed index)
    uint32_t *wg_c2 = calloc(bound, 4), *wg_a2 = calloc(bound, 4), *wg_p2 = calloc(bound, 4), *wg_lu = calloc(bound, 4);
    if (!wg_c2 || !wg_a2 || !wg_p2 || !wg_lu) { free(wg_c2); free(wg_a2); free(wg_p2); free(wg_lu); REFUSE("out of memory"); }
    for (uint32_t id = 1; id < bound; id++) if (iswg[id]) { wg_c2[id] = NEW(); wg_a2[id] = NEW(); wg_p2[id] = NEW(); wg_lu[id] = NEW(); }
    // a SPIR-V 1.4 module lists every global its entry point uses: the info block, which the rewrite may read where the module did not
    uint32_t add_info = 0;
    if (version >= 0x10400 && v_info && (v_wg || v_nwg || atoms)) {
        add_info = 1;
        const uint32_t *e = w + ep; uint32_t el = e[0] >> 16, k = 3;
        while (k < el && (e[k] & 0xff000000u) && (e[k] & 0xff0000u) && (e[k] & 0xff00u) && (e[k] & 0xffu)) k++;   // (the name's words: the last holds its NUL)
        for (k++; k < el; k++) if (e[k] == v_info) add_info = 0;
    }
    // ---- emit ----
    {
        uint32_t head[5] = {w[0], version, w[2], 0, w[4]}; vmp_put(&B, head, 5);
        int ann_done = 0, glob_done = 0;
        for (size_t i = 5; i < n;) {
            uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
            if (!ann_done && new_var && (i == first_ann || i == first_type)) { ann_done = 1; VMP(71, new_var, 11, 29); }
            if (!glob_done && i == first_fn) {
                glob_done = 1;
                if (new_bool) VMP(20, new_bool);
                if (new_psbu) VMP(32, new_psbu, 12, tu);
                if (new_piu) VMP(32, new_piu, 1, tu);
                VMP(43, tu, c0, 0); VMP(43, tu, c1, 1); VMP(43, tu, c5, 5); VMP(43, tu, c31, 31);
                if (c3) VMP(43, tu, c3, 3);
                if (c0i) VMP(43, ti, c0i, 0);
                for (uint32_t id = 1; id < bound; id++) if (iswg[id]) {
                    uint32_t L = w[def[w[def[PTR_TO(rt[id])] + 3]] + 3];
                    VMP(43, tu, wg_lu[id], L);
                }
                if (new_var) VMP(59, rt[0], new_var, 1);
            }
            if (op == 15 && (new_var || add_info)) {   // the entry point's interface gains the new input (and the info block a 1.4 module now reads)
                uint32_t h = (len + (new_var != 0) + add_info) << 16 | 15; vmp_put(&B, &h, 1); vmp_put(&B, x + 1, len - 1);
                if (new_var) vmp_put(&B, &new_var, 1);
                if (add_info) vmp_put(&B, &v_info, 1);
            } else if (i == lsz) { VMP(16, x[1], 17, 64, 1, 1); }
            else if (op == 71 && len == 4 && x[2] == 11 && x[3] == 25) {}   // (BuiltIn WorkgroupSize on a constant would override LocalSize: dropped, the constant's uses keep 32)
            else if (op == 59 && iswg[x[2]]) {
                uint32_t id = x[2], A = PTR_TO(rt[id]), L = w[def[w[def[A] + 3]] + 3];
                VMP(43, tu, wg_c2[id], 2 * L);
                VMP(28, wg_a2[id], w[def[A] + 2], wg_c2[id]);
                VMP(32, wg_p2[id], 4, wg_a2[id]);
                VMP(59, wg_p2[id], id, 4);
            } else if (i > first_fn && op == 61 && len >= 4 && x[3] && (x[3] == v_lid || x[3] == v_liid || x[3] == v_wg || x[3] == v_nwg || (x[3] < bound && bcv[x[3]]))) {
                uint32_t R = x[2], v = x[3], comp = UINT32_MAX;
                if (v < bound && bcv[v]) { comp = bcc[v]; v = bcv[v]; R = NEW(); }   // (the vector, then its component)
                if (v == v_lid || v == v_liid) {
                    uint32_t t = NEW(); VMP(61, tu, t, src);
                    if (v == v_lid) VMP(199, tu, R, t, c31);
                    else { uint32_t a = NEW(); VMP(199, tu, a, t, c31); VMP(80, v3, R, a, c0, c0); }
                } else {
                    uint32_t g = NEW(), p = NEW(), cnt = NEW(); VMP(61, v3, g, v);
                    VMP(65, psbu, p, v_info, c0, c0); VMP(61, tu, cnt, p);
                    if (v == v_nwg) VMP(82, v3, R, cnt, g, 0);
                    else {
                        uint32_t gx = NEW(), x2 = NEW(), t = NEW(), h = NEW(), vx = NEW(), m = NEW(), lt = NEW(), c = NEW();
                        VMP(81, tu, gx, g, 0); VMP(196, tu, x2, gx, c1); VMP(61, tu, t, src); VMP(194, tu, h, t, c5); VMP(128, tu, vx, x2, h);
                        VMP(130, tu, m, cnt, c1); VMP(176, tbool, lt, vx, cnt); VMP(169, tu, c, lt, vx, m); VMP(82, v3, R, c, g, 0);
                    }
                }
                if (comp != UINT32_MAX) VMP(81, tu, x[2], R, comp);
            } else if ((op == 65 || op == 66) && len >= 5 && x[3] < bound && iswg[x[3]] && i > first_fn) {
                uint32_t id = x[3], idx = x[4], it = rt[idx], sgn = w[def[it] + 3];
                uint32_t t = NEW(), h = NEW(), o = NEW(), j = NEW();
                VMP(61, tu, t, src); VMP(194, tu, h, t, c5); VMP(132, tu, o, h, wg_lu[id]);
                if (sgn) { uint32_t oi = NEW(); VMP(124, ti, oi, o); VMP(128, it, j, idx, oi); }
                else VMP(128, it, j, idx, o);
                uint32_t hd = len << 16 | op; vmp_put(&B, &hd, 1); vmp_put(&B, x + 1, 3); vmp_put(&B, &j, 1); vmp_put(&B, x + 5, len - 5);
            } else if (i > first_fn && op == 234 && len == 7 && atoms && PTR_SC(rt[x[3]]) != 4 && PTR_SC(rt[x[3]]) != 7) {
                // the repeated workgroup (an odd count's last second half: 2 * id.x + h >= n) adds 0, then takes the result its twin, lane & 31 of the same subgroup, got
                uint32_t T = x[1], R = x[2], z = w[def[T] + 3] ? c0i : c0;
                uint32_t g = NEW(), gx = NEW(), x2 = NEW(), t = NEW(), h = NEW(), vx = NEW(), pi = NEW(), cnt = NEW(), ph = NEW(), v2 = NEW(), r0 = NEW(), li = NEW(), sh = NEW();
                VMP(61, v3, g, v_wg); VMP(81, tu, gx, g, 0); VMP(196, tu, x2, gx, c1); VMP(61, tu, t, v_lane); VMP(194, tu, h, t, c5); VMP(128, tu, vx, x2, h);
                VMP(65, psbu, pi, v_info, c0, c0); VMP(61, tu, cnt, pi); VMP(174, tbool, ph, vx, cnt);
                VMP(169, T, v2, ph, z, x[6]); VMP(234, T, r0, x[3], x[4], x[5], v2);
                VMP(199, tu, li, t, c31); VMP(345, T, sh, c3, r0, li); VMP(169, T, R, ph, sh, r0);
            } else vmp_put(&B, x, len);
            i += len;
        }
        if (B.oom) { free(wg_c2); free(wg_a2); free(wg_p2); free(wg_lu); REFUSE("out of memory"); }
        B.w[3] = next;
    }
    free(wg_c2); free(wg_a2); free(wg_p2); free(wg_lu);
    free(rt); free(def); free(bi); free(bind); free(set); free(iswg); free(bcv); free(bcc);
    if (st) { st->wg_bytes = wg_bytes; st->chains = chains; st->loads = loads; st->made_lane = new_var != 0; st->atomics = atoms; }
    (void)nwgv;
    *on = B.n; return B.w;
fail:
    free(rt); free(def); free(bi); free(bind); free(set); free(iswg); free(bcv); free(bcc); free(B.w);
    return NULL;
#undef REFUSE
#undef NEW
#undef PTR_SC
#undef PTR_TO
#undef TOP
}
#undef VMP
