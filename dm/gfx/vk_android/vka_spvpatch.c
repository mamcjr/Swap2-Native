// SPIR-V rewrites the replay makes at pipeline time (plain C, no Vulkan: unit-tested on the Mac by dm/host/tests/t_android_spvpatch.c).
//
// vka_spv_vin_retype: vertex inputs fed integer formats. The translator declares every vertex input a vec4 of floats (the library's vin_types say float for all of
// Donkey Kong Bananza's 65,846 inputs) and a shader that uses one as integer bits bit-casts it. On Maxwell an attribute of an integer format (UINT8 … SINT32) puts the
// integer, widened by its own sign, into the 32-bit register the shader reads, so those bits are what the bit-cast must see (bone indices, packed data). Vulkan gives a float
// input only a float, normalized or scaled format, and a USCALED stand-in hands the shader the bits of 5.0 instead of 5. So for a pipeline whose integer attributes feed float
// inputs, the vertex module is rewritten: those inputs become uvec4 / ivec4 (Input variables of the integer vector type), and each load of one, or of a component through an
// access chain, is followed by an OpBitcast back to the float type the body expects. The bits are then Maxwell's, and the attribute keeps its own integer format.
// nvn_mtl.m reaches the same bits by converting such attributes on the CPU into float32 bits (vconv_int); the Apple chain's dmsh_vin is the reverse case (integer inputs fed float formats).
#include "vka_spvpatch.h"

int vka_spv_mesh_fragment(uint32_t *w, size_t *nw) {
    if (!w || !nw || *nw < 5 || w[0] != 0x07230203) return 0;
    // Validate before mutation; callers can safely reject a malformed module.
    for (size_t i = 5; i < *nw;) {
        uint32_t n = w[i] >> 16;
        if (!n || n > *nw - i) return 0;
        i += n;
    }
    // Integer inputs may already be Flat. Drop PerPrimitive in that case rather
    // than introducing a duplicate decoration (including member decorations).
    for (size_t i = 5; i < *nw; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        unsigned d = op == 71 && n >= 3 ? 2 : op == 72 && n >= 4 ? 3 : 0;
        if (!d || w[i+d] != 5271) continue;
        int flat = 0;
        for (size_t j = 5; j < *nw; j += w[j] >> 16) {
            if ((w[j] & 65535) != op || (w[j] >> 16) != n || w[j+d] != 14) continue;
            if (w[j+1] == w[i+1] && (d == 2 || w[j+2] == w[i+2])) { flat = 1; break; }
        }
        // An invalid sentinel is internal only, removed below.
        w[i+d] = flat ? UINT32_MAX : 14;
    }
    size_t dst = 5;
    for (size_t i = 5; i < *nw;) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || n > *nw - i) return 0;
        int drop = op == 17 && n == 2 && w[i + 1] == 5283;
        if ((op == 71 && n >= 3 && w[i+2] == UINT32_MAX) || (op == 72 && n >= 4 && w[i+3] == UINT32_MAX)) drop = 1;
        if (op == 10 && n == 6 && w[i + 1] == 0x5f565053 && w[i + 2] == 0x5f545845 &&
            w[i + 3] == 0x6873656d && w[i + 4] == 0x6168735f && w[i + 5] == 0x00726564) drop = 1;
        if (!drop) {
            // The output stage already duplicates each primitive's value on its corners.
            // Flat interpolation is also correct for integer fragment inputs.
            for (uint32_t k = 0; k < n; k++) w[dst + k] = w[i + k];
            dst += n;
        }
        i += n;
    }
    *nw = dst; return 1;
}
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { OpExtInst = 12, OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54,
       OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpBitcast = 124,
       OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633 };
enum { DecLocation = 30, SCInput = 1 };

typedef struct { uint32_t *w; size_t n, cap; } vec_t;
static int put(vec_t *v, uint32_t x) {
    if (v->n == v->cap) { size_t c = v->cap ? 2 * v->cap : 4096; uint32_t *p = realloc(v->w, c * 4); if (!p) return 0; v->w = p; v->cap = c; }
    v->w[v->n++] = x; return 1;
}
static int put_n(vec_t *v, const uint32_t *x, size_t n) { for (size_t i = 0; i < n; i++) if (!put(v, x[i])) return 0; return 1; }

uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL; vec_t o = {0};   // cls[id]: 0 untouched, 1 a retyped input or a chain into one, as uint, 2 as int (bit 4: the id is a pointer to a vec4)
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if (!(cls = calloc(bound, 4))) { snprintf(err, errn, "out of memory"); return NULL; }
    if ((umask | smask) == 0 || (umask & smask)) FAIL("no input, or an input both uint and int");
    // Pass 1: locations, the types, the Input variables
    uint32_t *loc = calloc(bound, 4); if (!loc) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_u32 = 0, t_i32 = 0, t_v4f = 0, t_v4u = 0, t_v4i = 0, p_in_v4f = 0;
    uint32_t p_in[2][2] = {{0}};   // [uint/int][scalar/vec4] Input pointers that exist
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(loc); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32) t_f32 = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4) {
            if (w[i + 2] == t_f32 && t_f32) t_v4f = w[i + 1]; else if (w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1]; else if (w[i + 2] == t_i32 && t_i32) t_v4i = w[i + 1]; }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCInput) {
            uint32_t t = w[i + 3];
            if (t == t_v4f && t) p_in_v4f = w[i + 1];
            for (int c = 0; c < 2; c++) { if (t == (c ? t_i32 : t_u32) && t) p_in[c][0] = w[i + 1]; if (t == (c ? t_v4i : t_v4u) && t) p_in[c][1] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!t_f32 || !t_v4f || !p_in_v4f) { free(loc); FAIL("no vec4 float input type"); }
    // New ids for the types the module lacks
    uint32_t nb = bound;
    int need_c[2] = {(umask != 0), (smask != 0)};
    uint32_t t_s[2] = {t_u32, t_i32}, t_v[2] = {t_v4u, t_v4i};
    uint32_t mk_s[2] = {0}, mk_v[2] = {0}, mk_p[2][2] = {{0}};
    for (int c = 0; c < 2; c++) if (need_c[c]) {
        if (!t_s[c]) t_s[c] = mk_s[c] = nb++;
        if (!t_v[c]) t_v[c] = mk_v[c] = nb++;
        if (!p_in[c][0]) p_in[c][0] = mk_p[c][0] = nb++;
        if (!p_in[c][1]) p_in[c][1] = mk_p[c][1] = nb++;
    }
    // The input variables to retype: Location in a mask, type pointer-to-vec4-float
    uint32_t moved[32]; int nmoved = 0, found = 0;
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCInput && w[i + 2] < bound && w[i + 1] == p_in_v4f) {
            uint32_t id = w[i + 2], l = loc[id];
            if (l < 32 && ((umask | smask) >> l & 1)) { cls[id] = (umask >> l & 1 ? 1 : 2) | 16; if (nmoved < 32) moved[nmoved++] = id; found |= 1u << l; }
        }
        i += len;
    }
    free(loc);
    if ((uint32_t)found != (umask | smask)) FAIL("inputs %#x are not vec4 float Input variables", (umask | smask) & ~(uint32_t)found);
    // Pass 2: copy, retyping. The globals up to the first function, minus the moved variables; then the new types and the variables; then the functions
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) if (need_c[c]) {
                if (mk_s[c]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_s[c], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_v[c]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v[c], t_s[c], 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][0]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][0], SCInput, t_s[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                if (mk_p[c][1]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][1], SCInput, t_v[c]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_in[(cls[moved[k]] & 3) - 1][1], moved[k], SCInput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t rt = w[i + 1], r = w[i + 2];
            // The chain's result: a component (pointer to float) or the vector itself (no index)
            if (r >= bound) FAIL("bad id");
            int vec = len == 4;
            if (!vec && len != 5) FAIL("an access chain of %u indexes into a retyped input", len - 4);
            (void)rt;
            cls[r] = (uint32_t)(c + 1) | (vec ? 16 : 0);
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_in[c][vec];
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1, vec = (cls[w[i + 3]] & 16) != 0;
            uint32_t ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = vec ? t_v[c] : t_s[c]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        // Any other use of a retyped variable or chain (a call argument, a copy): not understood, refused. Debug names, decorations and the entry point's interface are fine
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]] && op != OpVariable) FAIL("opcode %u uses a retyped input (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_subgroup_single: subgroup operations in a stage the device has none for. The Adreno 840 lists subgroup operations for the fragment and compute stages only
// (VkPhysicalDeviceSubgroupProperties::supportedStages 0xf0), and the translator emits them wherever the Maxwell code votes or shuffles: in Donkey Kong Bananza four geometry
// shaders (the terrain's culling shader 5d537f7630ec3671 among them) count each warp's surviving triangles with a ballot, let one lane add them to the chunk's indirect draw
// record and hand the base to the others with a shuffle. The driver takes the module and gives no meaningful lanes, so triangles were dropped or written over each other:
// black holes in the cave walls. Rewritten as subgroups of one invocation, which every warp-level algorithm must already handle (a warp with one active lane): the
// invocation's id is 0, the subgroup size 1, a ballot is its predicate in bit 0, the equal / less-or-equal / greater-or-equal masks are bit 0, the less / greater masks 0,
// a shuffle or broadcast returns its own value, all / any return the predicate, all-equal and elect true. Ballot counts, finds and inverse ballots are refused (none met)
enum { OpTypeBool = 20, OpConstantTrue = 41, OpConstant = 43, OpConstantComposite = 44, OpCompositeConstruct = 80, OpCopyObject = 83, OpSelect = 169,
       OpGNUElect = 333, OpGNUAll = 334, OpGNUAny = 335, OpGNUAllEqual = 336, OpGNUBroadcast = 337, OpGNUBroadcastFirst = 338, OpGNUBallot = 339,
       OpGNUShuffle = 345, OpGNUShuffleXor = 346, OpGNUShuffleUp = 347, OpGNUShuffleDown = 348 };
enum { DecBuiltIn = 11, BISubgroupSize = 36, BISubgroupLocalInvocationId = 41, BIEq = 4416, BIGe = 4417, BIGt = 4418, BILe = 4419, BILt = 4420 };
static int bi_kind(uint32_t b) {   // 1 = value 0 (id, lt, gt), 2 = value 1 (size, eq, le, ge), 0 = not a subgroup built-in
    switch (b) { case BISubgroupLocalInvocationId: case BILt: case BIGt: return 1; case BISubgroupSize: case BIEq: case BILe: case BIGe: return 2; default: return 0; }
}
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint8_t *bi = NULL, *chain = NULL; int32_t *cval = NULL; vec_t o = {0}; *changed = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3], nb = bound;
    bi = calloc(bound, 1); chain = calloc(bound, 1); cval = malloc((size_t)bound * 4);
    if (!bi || !chain || !cval) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) cval[i] = -1;
    uint32_t t_u32 = 0, t_v4u = 0, t_bool = 0; size_t first_fn = nw; int any = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 1] < bound && bi_kind(w[i + 3])) bi[w[i + 1]] = (uint8_t)bi_kind(w[i + 3]) | (w[i + 3] >= BIEq ? 4 : 0);   // bit 2: a vec4 mask
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32 && !w[i + 3]) t_u32 = w[i + 1];
        else if (op == OpTypeBool && len >= 2) t_bool = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] == 4 && w[i + 2] == t_u32 && t_u32) t_v4u = w[i + 1];
        else if (op == OpConstant && len >= 4 && w[i + 2] < bound) cval[w[i + 2]] = (int32_t)w[i + 3];
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        else if (op >= OpGNUElect && op <= OpGNUShuffleDown) any = 1;
        i += len;
    }
    for (uint32_t i = 0; i < bound && !any; i++) any = bi[i] != 0;
    if (!any) { free(bi); free(chain); free(cval); return NULL; }   // nothing to do
    if (!t_u32) FAIL("no 32-bit unsigned type");
    uint32_t mk_v4 = t_v4u ? 0 : nb++, mk_bool = t_bool ? 0 : nb++; if (!t_v4u) t_v4u = mk_v4; if (!t_bool) t_bool = mk_bool;
    uint32_t c0 = nb++, c1 = nb++, cv0 = nb++, cv1 = nb++, ctrue = nb++;   // uint 0, uint 1, uvec4(0), uvec4(1, 0, 0, 0), true
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[2] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(&o, x, 2)) FAIL("out of memory"); }
            if (mk_v4) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_v4, t_u32, 4}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1, 7u << 16 | OpConstantComposite, t_v4u, cv0, c0, c0, c0, c0,
                            7u << 16 | OpConstantComposite, t_v4u, cv1, c1, c0, c0, c0, 3u << 16 | OpConstantTrue, t_bool, ctrue};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (i >= first_fn) {
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && bi[w[i + 3]]) {   // a component of a mask
                if (len != 5 || w[i + 4] >= bound || cval[w[i + 4]] < 0) FAIL("an access chain into a subgroup mask with a non-constant index");
                int k = bi[w[i + 3]] & 3; chain[w[i + 2]] = (uint8_t)(k == 2 && cval[w[i + 4]] == 0 ? 2 : 1);   // bit 0 only for eq / le / ge
                i += len; continue;   // dropped: its loads become constants
            }
            if (op == OpLoad && len >= 4 && w[i + 3] < bound && (bi[w[i + 3]] || chain[w[i + 3]])) {
                uint32_t v = chain[w[i + 3]] ? (chain[w[i + 3]] == 2 ? c1 : c0) : (bi[w[i + 3]] & 4) ? ((bi[w[i + 3]] & 3) == 2 ? cv1 : cv0) : ((bi[w[i + 3]] & 3) == 2 ? c1 : c0);
                uint32_t x[4] = {4u << 16 | OpCopyObject, w[i + 1], w[i + 2], v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
            if (op >= OpGNUElect && op <= OpGNUShuffleDown) {
                uint32_t rt = w[i + 1], r = w[i + 2], v;
                switch (op) {
                case OpGNUElect: case OpGNUAllEqual: v = ctrue; break;
                case OpGNUAll: case OpGNUAny: case OpGNUBroadcast: case OpGNUBroadcastFirst: case OpGNUShuffle: case OpGNUShuffleXor: case OpGNUShuffleUp: case OpGNUShuffleDown:
                    if (len < 5) FAIL("short subgroup instruction %u", op);
                    v = w[i + 4]; break;
                case OpGNUBallot: {
                    if (len < 5 || rt != t_v4u) FAIL("a ballot of an unexpected type");
                    uint32_t tmp = nb++;
                    uint32_t x[] = {6u << 16 | OpSelect, t_u32, tmp, w[i + 4], c1, c0, 7u << 16 | OpCompositeConstruct, t_v4u, r, tmp, c0, c0, c0};
                    if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
                    i += len; *changed = 1; continue; }
                default: FAIL("subgroup instruction %u not rewritten", op);
                }
                uint32_t x[4] = {4u << 16 | OpCopyObject, rt, r, v}; if (!put_n(&o, x, 4)) FAIL("out of memory");
                i += len; *changed = 1; continue;
            }
        }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(bi); free(chain); free(cval);
    *out_nw = o.n; return o.w;
fail:
    free(bi); free(chain); free(cval); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_gs_claim: a geometry shader that writes memory, made to write it once per primitive. The Adreno 840 runs a geometry shader several times for each primitive (three
// times for a triangle in and three vertices out, counted with an atomic add per gl_PrimitiveIDIn), each run executing the whole body: whatever the shader writes to memory
// is written that many times. Donkey Kong Bananza's terrain culling shader 5d537f7630ec3671 added each surviving triangle 3 times to its chunk's indirect draw record and wrote
// its indices 3 times, past the chunk's index buffer into the next chunk's: black holes in the cave walls. Vulkan says nothing of how many times a geometry shader runs, so the
// module claims its primitive first: one word per primitive in a storage buffer the replay binds at set 0 binding `binding` (VKA_GS_CLAIM_BINDING, zeroed for each draw), set by an atomic exchange
// at gl_PrimitiveIDIn; only the run that finds it 0 writes memory. Every store and copy into Uniform / StorageBuffer / PhysicalStorageBuffer / Image memory, image write and
// atomic is put under that condition (its block split around it); the other runs see an atomic's result as the memory's value (a compare-exchange its comparator, so a
// compare-exchange loop ends). What the shader emits is left alone: every run emits as before. Returns the count of memory writes (0: nothing to do, *out NULL), -1 with err
// on failure; with out and a count above 0, the rewritten module (malloc'd, *out_nw words)
enum { OpUndef = 1, OpExecutionMode = 16, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypeForwardPointer = 39,
       OpFunctionEnd = 56, OpFunctionParameter = 55, OpImageTexelPointer = 60, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpPtrAccessChain = 67,
       OpInBoundsPtrAccessChain = 70, OpImageWrite = 99, OpConvertUToPtr = 120, OpIEqual = 170, OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248,
       OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255, OpTerminateInvocation = 4416,
       OpAtomicStore = 228, OpAtomicExchange = 229, OpAtomicCompareExchange = 230, OpAtomicCompareExchangeWeak = 231, OpAtomicXor = 242,
       OpAtomicFMinEXT = 5614, OpAtomicFMaxEXT = 5615, OpAtomicFAddEXT = 6035, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75 };
enum { SCUniform = 2, SCPrivate = 6, SCImage = 11, SCStorageBuffer = 12, SCPhysicalStorageBuffer = 5349, DecBufferBlock = 3, DecArrayStride = 6, DecOffset = 35,
       DecBinding = 33, DecDescriptorSet = 34, BIPrimitiveId = 7 };
static int sc_mem(uint32_t sc) { return sc == SCUniform || sc == SCImage || sc == SCStorageBuffer || sc == SCPhysicalStorageBuffer; }
static int is_term(uint32_t op) { return op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpKill || op == OpReturn || op == OpReturnValue || op == OpUnreachable || op == OpTerminateInvocation; }
static int is_annot(uint32_t op) { return op == OpDecorate || op == OpMemberDecorate || op == OpDecorationGroup || op == OpGroupDecorate || op == OpGroupMemberDecorate || op == OpDecorateId || op == OpDecorateString || op == OpMemberDecorateString; }
// One pass of the rewrite, run twice: the first records in rec where each split block of the input ends, the second gives that to the phis (last) that name such a block
// as their parent (the new ids are the same in both passes)
static int claim_pass(const uint32_t *w, size_t nw, uint32_t binding, const uint32_t *last, uint32_t *rec, vec_t *o, int *found, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    uint32_t *psc = calloc(bound, 4), *ty = calloc(bound, 4);   // psc[pointer type]: storage class + 1; ty[id]: the type of a pointer-valued id
    if (!psc || !ty) FAIL("out of memory");
    uint32_t t_bool = 0, t_u32 = 0, t_i32 = 0, p_in_i32 = 0, p_priv_bool = 0, p_uni_u32 = 0, main_fn = 0, prim_var = 0;
    size_t first_fn = nw, first_annot = nw, ep_at = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) psc[w[i + 1]] = w[i + 2] + 1;
        if (op == OpTypePointer && len >= 4) {
            if (w[i + 2] == SCInput && w[i + 3] && w[i + 3] == t_i32) p_in_i32 = w[i + 1];
            if (w[i + 2] == SCPrivate && w[i + 3] && w[i + 3] == t_bool) p_priv_bool = w[i + 1];
            if (w[i + 2] == SCUniform && w[i + 3] && w[i + 3] == t_u32) p_uni_u32 = w[i + 1];
        }
        else if ((op == OpVariable || op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain || op == OpConvertUToPtr ||
                  op == OpBitcast || op == OpCopyObject || op == OpImageTexelPointer || op == OpFunctionParameter) && len >= 3 && w[i + 2] < bound) ty[w[i + 2]] = w[i + 1];
        if (op == OpTypeBool) t_bool = w[i + 1];
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) { if (w[i + 3]) t_i32 = w[i + 1]; else t_u32 = w[i + 1]; }
        else if (op == OpEntryPoint && len >= 3) { if (main_fn) FAIL("more than one entry point"); main_fn = w[i + 2]; ep_at = i; }
        else if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == BIPrimitiveId) prim_var = w[i + 1];
        else if (op == OpExecutionMode && len >= 4 && w[i + 2] == 0 && w[i + 3] > 1) FAIL("%u invocations a primitive: they would share its claim", w[i + 3]);   // Invocations
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    if (!main_fn || first_fn == nw || !t_u32) FAIL("no entry point, function or 32-bit unsigned type");
    if (first_annot > first_fn) first_annot = first_fn;
    if (prim_var && (!p_in_i32 || ty[prim_var] != p_in_i32)) FAIL("a PrimitiveId input that is not a 32-bit int");
    // New ids: types missing, the claim buffer's types and variable, the constants, the guard variable
    uint32_t mk_bool = t_bool ? 0 : nb++, mk_i32 = t_i32 ? 0 : nb++; if (!t_bool) t_bool = mk_bool; if (!t_i32) t_i32 = mk_i32;
    uint32_t mk_pin = p_in_i32 && !mk_i32 ? 0 : nb++, mk_ppb = p_priv_bool && !mk_bool ? 0 : nb++, mk_pu = p_uni_u32 ? 0 : nb++;
    if (mk_pin) p_in_i32 = mk_pin; if (mk_ppb) p_priv_bool = mk_ppb; if (mk_pu) p_uni_u32 = mk_pu;
    uint32_t mk_prim = prim_var ? 0 : nb++; if (!prim_var) prim_var = mk_prim;
    uint32_t rta = nb++, st = nb++, pst = nb++, cvar = nb++, c0 = nb++, c1 = nb++, gvar = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0; int in_fn = 0, in_main_entry = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == ep_at && !mk_prim) { if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (i == ep_at) {   // the PrimitiveId input joins the interface
            if (!put(o, (len + 1) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1) || !put(o, prim_var)) FAIL("out of memory");
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, DecBufferBlock, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, cvar, DecDescriptorSet, 0, 4u << 16 | OpDecorate, cvar, DecBinding, binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (mk_prim) { uint32_t y[] = {4u << 16 | OpDecorate, mk_prim, DecBuiltIn, BIPrimitiveId}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
        }
        if (i == first_fn) {
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_i32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_i32, 32, 1}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pin) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pin, SCInput, t_i32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_ppb) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_ppb, SCPrivate, t_bool}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_pu) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pu, SCUniform, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_prim) { uint32_t x[] = {4u << 16 | OpVariable, p_in_i32, mk_prim, SCInput}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, SCUniform, st,
                            4u << 16 | OpVariable, pst, cvar, SCUniform, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpVariable, p_priv_bool, gvar, SCPrivate};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) { in_fn = 1; in_main_entry = w[i + 2] == main_fn ? 1 : 0; }
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpLabel) { cur = cur_orig = w[i + 1]; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (in_main_entry == 1 && op != OpVariable && cur) {   // the claim, after the entry block's variables
            uint32_t pid = nb++, ptr = nb++, old = nb++, first = nb++;
            uint32_t x[] = {4u << 16 | OpLoad, t_i32, pid, prim_var, 6u << 16 | OpAccessChain, p_uni_u32, ptr, cvar, c0, pid,
                            7u << 16 | OpAtomicExchange, t_u32, old, ptr, c1, c0, c1, 5u << 16 | OpIEqual, t_bool, first, old, c0, 3u << 16 | OpStore, gvar, first};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            in_main_entry = 2;
        }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        #define SC(id) ((id) < bound && ty[id] < bound && psc[ty[id]] ? psc[ty[id]] - 1 : 0u)
        int drop = 0, atomic = 0;
        if (in_fn && (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) && len >= 2 && sc_mem(SC(in[1]))) drop = 1;
        else if (in_fn && op == OpImageWrite) drop = 1;
        else if (in_fn && ((op >= OpAtomicExchange && op <= OpAtomicXor) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT) && len >= 4 && sc_mem(SC(in[3]))) atomic = 1;
        if (drop || atomic) {
            (*found)++;
            for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) FAIL("a memory write in a loop header block"); if (is_term(oj)) break; j += w[j] >> 16; }
            uint32_t g = nb++, a = nb++, l = nb++, fb = 0;
            if (atomic) {
                uint32_t sc = SC(in[3]);
                if ((op == OpAtomicCompareExchange || op == OpAtomicCompareExchangeWeak) && len >= 9) fb = in[8];
                else { fb = nb++;
                    if (sc == SCImage) { uint32_t x[] = {3u << 16 | OpUndef, in[1], fb}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
                    else if (sc == SCPhysicalStorageBuffer) { uint32_t x[] = {6u << 16 | OpLoad, in[1], fb, in[3], 2, 4}; if (!put_n(o, x, 6)) FAIL("out of memory"); }   // Aligned 4
                    else { uint32_t x[] = {4u << 16 | OpLoad, in[1], fb, in[3]}; if (!put_n(o, x, 4)) FAIL("out of memory"); } }
            }
            uint32_t x[] = {4u << 16 | OpLoad, t_bool, g, gvar, 3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, g, a, l, 2u << 16 | OpLabel, a};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
            if (atomic) { uint32_t r = in[2]; if (!put(o, in[0]) || !put(o, in[1]) || !put(o, nb) || !put_n(o, in + 3, len - 3)) FAIL("out of memory");
                uint32_t ra = nb++, y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l, 7u << 16 | OpPhi, in[1], r, ra, a, fb, cur};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory"); }
            else { if (!put_n(o, in, len)) FAIL("out of memory"); uint32_t y[] = {2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
            cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
            i += len; continue;
        }
        #undef SC
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    o->w[3] = nb;
    free(psc); free(ty);
    return 0;
fail:
    free(psc); free(ty);
    return -1;
#undef FAIL
}
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn) {
    if (out) *out = NULL;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return -1; }
    uint32_t *rec = calloc(w[3], 4); vec_t o = {0}; int found = 0;
    if (!rec) { snprintf(err, errn, "out of memory"); return -1; }
    if (claim_pass(w, nw, binding, NULL, rec, &o, &found, err, errn)) { free(rec); free(o.w); return -1; }
    if (found && out) {
        free(o.w); o = (vec_t){0}; int f2 = 0;
        if (claim_pass(w, nw, binding, rec, NULL, &o, &f2, err, errn)) { free(rec); free(o.w); return -1; }
        *out = o.w; *out_nw = o.n; o.w = NULL;
    }
    free(rec); free(o.w);
    return found;
}

// vka_spv_fout_retype: float colour outputs drawn into integer targets. On Maxwell / Ampere a colour output is a 32-bit register per component and the target's format decides
// how its bits are read: a pixel shader the translator types as float (its outputs computed with float instructions, or moved through float registers) still writes the bits
// it holds into a UINT / SINT target. Vulkan leaves a float output to an integer attachment undefined, and the library's MOD_UINT / MOD_UINT_MIXED variants exist only for
// shaders whose outputs come from integer bits or constants (vk_android_build.py ps_variants). So for a pipeline where a float output meets an integer attachment, the module's
// Output variables at those locations (a float or a float vector) become the unsigned / signed integer type of as many components, each store into one (whole, or a component
// through an access chain) stores the OpBitcast of its value, and each load of one is bit-cast back. The bits reach the target unchanged, as on the Switch.
// Mario Kart World's fragment dcb0504c844f1ecc (DrivePassShaderDefault, paired with task-less mesh programs: by their shape ShadingRateWrite) writes its UINT target 0 this way
enum { SCOutput = 3 };
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t *cls = NULL, *loc = NULL, *ty = NULL; vec_t o = {0};   // cls[id]: 1 uint / 2 int, bit 4 the whole variable (else a component chain); ty: for each float type id its components (1..4), 0 none
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3];
    if ((umask | smask) == 0 || (umask & smask)) { snprintf(err, errn, "no output, or an output both uint and int"); return NULL; }
    if (!(cls = calloc(bound, 4)) || !(loc = calloc(bound, 4)) || !(ty = calloc(bound, 4))) FAIL("out of memory");
    for (uint32_t i = 0; i < bound; i++) loc[i] = ~0u;
    uint32_t t_f32 = 0, t_int[2] = {0}, t_ivec[2][5] = {{0}}, p_out_int[2][5] = {{0}};   // [uint/int][components]; the existing types
    uint32_t *ptee = calloc(bound, 4); if (!ptee) FAIL("out of memory");                // Output pointer type → its pointee
    size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) { free(ptee); FAIL("bad instruction at word %zu", i); }
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecLocation && w[i + 1] < bound) loc[w[i + 1]] = w[i + 3];
        else if (op == OpTypeFloat && len >= 3 && w[i + 2] == 32 && w[i + 1] < bound) { t_f32 = w[i + 1]; ty[t_f32] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 2] == 32) t_int[w[i + 3] ? 1 : 0] = w[i + 1];
        else if (op == OpTypeVector && len >= 4 && w[i + 3] >= 2 && w[i + 3] <= 4 && w[i + 1] < bound) {
            if (w[i + 2] == t_f32 && t_f32) ty[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) if (w[i + 2] == t_int[c] && t_int[c]) t_ivec[c][w[i + 3]] = w[i + 1];
        }
        else if (op == OpTypePointer && len >= 4 && w[i + 2] == SCOutput && w[i + 1] < bound) {
            ptee[w[i + 1]] = w[i + 3];
            for (int c = 0; c < 2; c++) { if (w[i + 3] == t_int[c] && t_int[c]) p_out_int[c][1] = w[i + 1]; for (int n = 2; n <= 4; n++) if (w[i + 3] == t_ivec[c][n] && t_ivec[c][n]) p_out_int[c][n] = w[i + 1]; }
        }
        else if (op == OpFunction && first_fn == nw) first_fn = i;
        i += len;
    }
    for (int c = 0; c < 2; c++) t_ivec[c][1] = t_int[c];
    // The output variables to retype: Location in a mask, a pointer to a float or float vector
    uint32_t moved[32], mcomp[32]; int nmoved = 0; uint32_t found = 0, need[2] = {0};   // need[c]: bit n = the integer type of n components (and its pointers) is used
    for (size_t i = 5; i < first_fn;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OpVariable && len >= 4 && w[i + 3] == SCOutput && w[i + 1] < bound && w[i + 2] < bound) {
            uint32_t id = w[i + 2], l = loc[id], pt = ptee[w[i + 1]], n = pt < bound ? ty[pt] : 0;
            if (l < 32 && ((umask | smask) >> l & 1)) {
                if (!n) { free(ptee); FAIL("output %u is not a float or float vector", l); }
                if (len > 4) { free(ptee); FAIL("output %u has an initializer", l); }   // (the translator emits none; re-emitted without it, unwritten components would change)
                if (nmoved == 32) { free(ptee); FAIL("too many outputs"); }
                int c = umask >> l & 1 ? 0 : 1;
                cls[id] = (uint32_t)(c + 1) | 16; mcomp[nmoved] = n; moved[nmoved++] = id;
                found |= 1u << l; need[c] |= 1u << n | 2u;   // (the scalar for component chains)
            }
        }
        i += len;
    }
    free(ptee);
    if (found != (umask | smask)) FAIL("outputs %#x are not float Output variables", (umask | smask) & ~found);
    // New ids for the types the module lacks
    uint32_t nb = bound, mk_t[2][5] = {{0}}, mk_p[2][5] = {{0}};
    for (int c = 0; c < 2; c++) for (int n = 1; n <= 4; n++) if (need[c] >> n & 1) {
        if (!t_ivec[c][n]) t_ivec[c][n] = mk_t[c][n] = nb++;
        if (!p_out_int[c][n]) p_out_int[c][n] = mk_p[c][n] = nb++;
    }
    if ((need[0] && !t_ivec[0][1]) || (need[1] && !t_ivec[1][1])) FAIL("no scalar type");
    // Pass 2: copy, retyping (the new types and the moved variables go right before the first function, as in vka_spv_vin_retype)
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) {
                if (mk_t[c][1]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_t[c][1], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 2; n <= 4; n++) if (mk_t[c][n]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_t[c][n], t_ivec[c][1], (uint32_t)n}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 1; n <= 4; n++) if (mk_p[c][n]) { uint32_t x[4] = {4u << 16 | OpTypePointer, mk_p[c][n], SCOutput, t_ivec[c][n]}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nmoved; k++) { uint32_t x[4] = {4u << 16 | OpVariable, p_out_int[(cls[moved[k]] & 3) - 1][mcomp[k]], moved[k], SCOutput}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound && cls[w[i + 2]] && i < first_fn) { i += len; continue; }   // re-emitted above
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            int c = (cls[w[i + 3]] & 3) - 1; uint32_t r = w[i + 2];
            if (r >= bound) FAIL("bad id");
            if (len != 5 || !(cls[w[i + 3]] & 16)) FAIL("an access chain of %u indexes into a retyped output", len - 4);
            cls[r] = (uint32_t)(c + 1);   // a component: pointer to the integer scalar
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = p_out_int[c][1];
            i += len; continue;
        }
        if (op == OpStore && len >= 3 && w[i + 1] < bound && cls[w[i + 1]]) {
            uint32_t p = w[i + 1], c = (cls[p] & 3) - 1, n = 1;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            uint32_t tmp = nb++;
            uint32_t x[4] = {4u << 16 | OpBitcast, t_ivec[c][n], tmp, w[i + 2]}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 2] = tmp;
            i += len; continue;
        }
        if (op == OpLoad && len >= 4 && w[i + 3] < bound && cls[w[i + 3]]) {
            uint32_t p = w[i + 3], c = (cls[p] & 3) - 1, n = 1, ft = w[i + 1], r = w[i + 2], tmp = nb++;
            if (cls[p] & 16) { for (int k = 0; k < nmoved; k++) if (moved[k] == p) n = mcomp[k]; }
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = t_ivec[c][n]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, ft, r, tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            i += len; continue;
        }
        if (op != OpName && op != OpMemberName && op != OpEntryPoint && op != OpDecorate && op != OpMemberDecorate && op != OpDecorateId && op != OpDecorateString && op != OpMemberDecorateString
            && i >= first_fn)
            for (uint32_t k = 1; k < len; k++) if (!(op == OpExtInst && k == 4) && w[i + k] < bound && cls[w[i + k]]) FAIL("opcode %u uses a retyped output (id %u)", op, w[i + k]);
        if (!put_n(&o, in, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(cls); free(loc); free(ty);
    *out_nw = o.n; return o.w;
fail:
    free(cls); free(loc); free(ty); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_img_retype: images bound to integer textures. Ampere's formatted surface operations (SULD.P / SUST.P) convert each texel by the format of the descriptor bound
// at run time, so the numeric class of what they read and write is the bound texture's, not the instruction's: an R32_UINT image hands the register its integer bits.
// The translator declares their image a float one of format Unknown (ShaderProperties.GetTextureFormat answers Unknown when the host can load formatless images, and
// Unknown's component type is FP32); its binary forms (SULD.D / SUST.D) and atomics already get integer formats. Its texture reads (TEX / TLD / TLD4) are the same: the
// texture unit returns an integer texture's texels as their integer values whatever the instruction, while DeclareSamplers gives every sampled texture a float sampled
// type. Vulkan refuses a float image over a view of an integer format (VUID-vkCmdDispatch-format-07752 / 07753), so id_view (vka_tex.c) leaves such a binding unbound.
// For a dispatch that binds an integer texture there, the module is rewritten: those image variables (selected by descriptor set and binding) take an OpTypeImage of
// the same shape whose sampled type is the 32-bit unsigned or signed integer.
// - A storage image (Sampled = 2) takes the concrete format of the bound view when the caller knows one (else Unknown); each OpImageRead / OpImageSparseRead of it reads
//   the integer vector and bit-casts it back to the float type the body expects, each OpImageWrite bit-casts its texel to the integer vector. A texel pointer (an
//   atomic) on it is refused (the translator gives atomics integer formats).
// - A sampled image (Sampled = 1: a variable of the OpTypeSampledImage of one, the translator's combined form, or of the image itself, its separate form, which
//   OpSampledImage pairs with a sampler) keeps format Unknown; its OpTypeSampledImage is made again. Each fetch, sample and gather of it (and their sparse forms)
//   returns the integer vector and is bit-cast back to the float result. A depth comparison (the Dref operations, which Vulkan defines on float images only) is refused.
//   Vulkan has no linear filtering of integer formats: the replay binds the guest sampler's nearest twin over such a view (vka_tex.c sampler_of)
// Arrays of either are retyped as a whole. The bits are those the hardware's register holds for an integer descriptor. Size and level queries are left alone.
// Mario Kart World's DrivePassShaderDefault compute shaders 20cedaf80534819b (R32_UINT) and 2c04affed9c64d35 (R8_UINT) bind storage images this way every frame, and
// d57cc8d2d94e662d samples an R8_UINT texture
enum { OpCapability = 17, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeArray = 28, OpVectorShuffle = 79, OpCompositeExtract = 81, OpCompositeInsert = 82,
       OpSampledImage = 86, OpImageFetch = 95, OpImageRead = 98, OpImage = 100, OpImageQueryLod = 105, OpImageSparseFetch = 313, OpImageSparseRead = 320,
       OpLine = 8, OpControlBarrier = 224, OpMemoryBarrier = 225 };
enum { SCUniformConstant = 0, CapStorageImageExtendedFormats = 49 };
// The integer class of a SPIR-V Image Format: 1 unsigned (Rgba32ui 30 ... R8ui 39), 2 signed (Rgba32i 21 ... R8i 29), 0 a float one or Unknown, -1 the 64-bit ones (Int64ImageEXT)
static int img_fmt_class(uint32_t f) { return f >= 21 && f <= 29 ? 2 : f >= 30 && f <= 39 ? 1 : f == 40 || f == 41 ? -1 : 0; }
// Formats past the Shader capability's (Rgba32f ... Rgba8Snorm, Rgba32i ... R32i, Rgba32ui ... R32ui) need StorageImageExtendedFormats
static int img_fmt_ext(uint32_t f) { return f && !(f <= 5) && !(f >= 21 && f <= 24) && !(f >= 30 && f <= 33); }
// An image instruction whose image (or sampled image) is word 3: 1 its result is the texel (retyped: the integer vector, bit-cast back), 2 the same inside a sparse
// result struct, 3 a depth comparison (refused), 4 a query (left alone), 0 none of these. *simg: it takes a sampled image (else an image)
static int img_op(uint32_t op, int *simg) {
    *simg = 1;
    switch (op) {
    case 87: case 88: case 91: case 92: case 96: return 1;                      // OpImageSample{Implicit,Explicit}Lod, OpImageSampleProj{Implicit,Explicit}Lod, OpImageGather
    case 305: case 306: case 309: case 310: case 314: return 2;                 // their sparse forms (OpImageSparseSample..., OpImageSparseGather)
    case 89: case 90: case 93: case 94: case 97: case 307: case 308: case 311: case 312: case 315: return 3;   // the Dref ones
    case OpImageQueryLod: return 4;
    }
    *simg = 0;
    switch (op) {
    case OpImageFetch: case OpImageRead: return 1;
    case OpImageSparseFetch: case OpImageSparseRead: return 2;
    case 101: case 102: case 103: case 104: case 106: case 107: return 4;    // OpImageQueryFormat, Order, SizeLod, Size, Levels, Samples
    }
    return 0;
}
// Word k of a function-body instruction may be an id (the literal operands that could pass for one left out, as vk_android_spv.py maybe_ids)
static int maybe_id(const uint32_t *in, uint32_t k) {
    uint32_t op = in[0] & 0xffff, len = in[0] >> 16;
    if (op == OpExtInst) return k != 4;
    if (op == OpCompositeExtract) return k <= 3;
    if (op == OpCompositeInsert || op == OpVectorShuffle) return k <= 4;
    if (op == OpSwitch) return k <= 2 || (k >= 4 && k % 2 == 0);
    uint32_t mask_at = 0;   // image sample / fetch / gather / read / write (and their sparse forms): the image operands mask
    switch (op) {
    case 87: case 88: case 91: case 92: case 95: case 98: case 305: case 306: case 309: case 310: case 313: case 320: mask_at = 5; break;
    case 89: case 90: case 93: case 94: case 96: case 97: case 307: case 308: case 311: case 312: case 314: case 315: mask_at = 6; break;
    case 99: mask_at = 4; break;
    }
    return !(mask_at && k == mask_at && len > mask_at);
}
// Instructions with no result type whose word 2 may name an id defined further on (the value types are taken first come: a definition precedes its other uses)
static int no_result(uint32_t op) {
    return op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpImageWrite || op == OpAtomicStore || op == OpLine || op == OpControlBarrier || op == OpMemoryBarrier
        || op == OpLabel || op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpSelectionMerge || op == OpLoopMerge || op == OpReturnValue;
}
uint32_t *vka_spv_img_retype(const uint32_t *w, size_t nw, const vka_spv_imgsel *sel, int nsel, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    // tag[id]: selection + 1 | what the id is (a pointer to the array, to one element; an image; a sampled image)
    enum { MAXS = 8, TG_PARR = 1 << 8, TG_PIMG = 2 << 8, TG_IMG = 3 << 8, TG_SIMG = 4 << 8 };
    uint32_t *dset = NULL, *dbind = NULL, *def = NULL, *vty = NULL, *tag = NULL; uint8_t *tk = NULL, *tn = NULL; vec_t o = {0};
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    if (nsel <= 0 || nsel > MAXS) { snprintf(err, errn, "%d images to retype (1 to %d)", nsel, MAXS); return NULL; }
    uint32_t bound = w[3];
    dset = malloc((size_t)bound * 4); dbind = malloc((size_t)bound * 4); def = calloc(bound, 4); vty = calloc(bound, 4); tag = calloc(bound, 4); tk = calloc(bound, 1); tn = calloc(bound, 1);
    if (!dset || !dbind || !def || !vty || !tag || !tk || !tn) FAIL("out of memory");
    memset(dset, 0xff, (size_t)bound * 4); memset(dbind, 0xff, (size_t)bound * 4);
    // Pass 1: decorations, the 32-bit scalar and vector types (tk: 1 float, 2 uint, 3 int; tn: components), where each type and global variable is defined (def), each value's type (vty)
    uint32_t t_int[2] = {0}, t_ivec[2][5] = {{0}}; int ext_cap = 0; size_t first_fn = nw;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpFunction && first_fn == nw) first_fn = i;
        if (op == OpCapability && len >= 2 && w[i + 1] == CapStorageImageExtendedFormats) ext_cap = 1;
        else if (op == OpDecorate && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == DecDescriptorSet) dset[w[i + 1]] = w[i + 3]; else if (w[i + 2] == DecBinding) dbind[w[i + 1]] = w[i + 3]; }
        else if (op == OpTypeFloat && len >= 3 && w[i + 1] < bound && w[i + 2] == 32) { tk[w[i + 1]] = 1; tn[w[i + 1]] = 1; }
        else if (op == OpTypeInt && len >= 4 && w[i + 1] < bound && w[i + 2] == 32) { int c = w[i + 3] ? 1 : 0; tk[w[i + 1]] = (uint8_t)(2 + c); tn[w[i + 1]] = 1; t_int[c] = w[i + 1]; }
        else if (op == OpTypeVector && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && tk[w[i + 2]] && tn[w[i + 2]] == 1 && w[i + 3] >= 2 && w[i + 3] <= 4) {
            tk[w[i + 1]] = tk[w[i + 2]]; tn[w[i + 1]] = (uint8_t)w[i + 3];
            if (tk[w[i + 2]] >= 2) t_ivec[tk[w[i + 2]] - 2][w[i + 3]] = w[i + 1];
        }
        if (i < first_fn) {
            if (op >= 19 && op <= 39 && len >= 2 && w[i + 1] < bound) def[w[i + 1]] = (uint32_t)i;   // OpTypeVoid ... OpTypeForwardPointer
            else if (op == OpVariable && len >= 4 && w[i + 2] < bound) { def[w[i + 2]] = (uint32_t)i; vty[w[i + 2]] = w[i + 1]; }
            else if (((op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == OpUndef) && len >= 3 && w[i + 2] < bound && !vty[w[i + 2]]) vty[w[i + 2]] = w[i + 1];
        } else if (len >= 3 && !no_result(op) && w[i + 2] < bound && !vty[w[i + 2]]) vty[w[i + 2]] = w[i + 1];
        i += len;
    }
    for (int c = 0; c < 2; c++) t_ivec[c][1] = t_int[c];
    // The selected variables: UniformConstant pointers to a float storage image of format Unknown, a float sampled image (or the OpTypeSampledImage of one), or an array of them.
    // nimg / nsimg: the new image / sampled image type; elem: the type of one element (nsimg for the combined form, else nimg); pvar, pelem: pointers to the variable's type, to one element
    struct { uint32_t var, img, arr, cls, fmt, nimg, nsimg, elem, narr, pvar, pelem; int smp, comb, mk_img, mk_simg; } s[MAXS];
    memset(s, 0, sizeof s);
    for (int k = 0; k < nsel; k++) {
        uint32_t c = sel[k].cls, f = sel[k].fmt;
        if ((c != 1 && c != 2) || img_fmt_class(f) < 0 || (f && img_fmt_class(f) != (int)c)) FAIL("set %u binding %u: class %u with image format %u", sel[k].set, sel[k].binding, c, f);
        uint32_t v = 0;
        for (uint32_t id = 1; id < bound && !v; id++)
            if (dset[id] == sel[k].set && dbind[id] == sel[k].binding && def[id] && (w[def[id]] & 0xffff) == OpVariable && w[def[id] + 3] == SCUniformConstant) v = id;
        if (!v) FAIL("no image variable at set %u binding %u", sel[k].set, sel[k].binding);
        for (int j = 0; j < k; j++) if (s[j].var == v) FAIL("set %u binding %u named twice", sel[k].set, sel[k].binding);
        uint32_t pt = w[def[v] + 1];
        if (pt >= bound || !def[pt] || (w[def[pt]] & 0xffff) != OpTypePointer || (w[def[pt]] >> 16) < 4) FAIL("set %u binding %u: the variable's type is not a pointer", sel[k].set, sel[k].binding);
        uint32_t t = w[def[pt] + 3], arr = 0, comb = 0;
        if (t < bound && def[t] && ((w[def[t]] & 0xffff) == OpTypeArray || (w[def[t]] & 0xffff) == OpTypeRuntimeArray) && (w[def[t]] >> 16) >= 3) { arr = t; t = w[def[t] + 2]; }
        if (t < bound && def[t] && (w[def[t]] & 0xffff) == OpTypeSampledImage && (w[def[t]] >> 16) >= 3) { comb = 1; t = w[def[t] + 2]; }
        if (t >= bound || !def[t] || (w[def[t]] & 0xffff) != OpTypeImage || (w[def[t]] >> 16) < 9) FAIL("set %u binding %u is not an image", sel[k].set, sel[k].binding);
        const uint32_t *ti = w + def[t];
        int smp = ti[7] == 1;
        if (ti[2] >= bound || tk[ti[2]] != 1 || tn[ti[2]] != 1 || ti[8] != 0 || (comb && !smp) || (!smp && ti[7] != 2))
            FAIL("set %u binding %u is not a float storage or sampled image of format Unknown (sampled type %u, sampled %u, format %u)", sel[k].set, sel[k].binding, ti[2], ti[7], ti[8]);
        if (smp && f) FAIL("set %u binding %u: a sampled image keeps format Unknown (format %u asked)", sel[k].set, sel[k].binding, f);
        s[k].var = v; s[k].img = t; s[k].arr = arr; s[k].cls = c; s[k].fmt = f; s[k].smp = smp; s[k].comb = (int)comb;
        tag[v] = (uint32_t)(k + 1) | (arr ? TG_PARR : TG_PIMG);
    }
    // Pass 1b: the function bodies, following the variables through access chains, loads, copies, OpSampledImage and OpImage to the image operations; the integer types those need
    uint32_t need[2] = {0};                                                  // bit n: the integer type of n components
    struct { uint32_t orig, c, nid; } sp[MAXS]; int nsp = 0;                 // sparse results' structs, made again with an integer texel
    #define TAGGED(x) ((x) < bound && tag[x])
    #define KIND(x) (tag[x] & ~0xffu)
    for (size_t i = first_fn; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && TAGGED(in[3])) {
            if (KIND(in[3]) != TG_PARR || len != 5) FAIL("an access chain of %u indexes into a retyped image", len - 4);
            if (in[2] < bound) tag[in[2]] = (tag[in[3]] & 0xff) | TG_PIMG;
            continue;
        }
        if (op == OpLoad && len >= 4 && TAGGED(in[3])) {
            if (KIND(in[3]) != TG_PIMG) FAIL("a load of a whole array of retyped images");
            if (in[2] < bound) tag[in[2]] = (tag[in[3]] & 0xff) | (s[(tag[in[3]] & 0xff) - 1].comb ? TG_SIMG : TG_IMG);
            continue;
        }
        if (op == OpCopyObject && len == 4 && TAGGED(in[3])) { if (in[2] < bound) tag[in[2]] = tag[in[3]]; continue; }
        if (op == OpSampledImage && len == 5 && TAGGED(in[3])) {   // the separate form: the image with a sampler
            if (KIND(in[3]) != TG_IMG || !s[(tag[in[3]] & 0xff) - 1].smp || TAGGED(in[4])) FAIL("OpSampledImage of a retyped image that is not a sampled one");
            if (in[2] < bound) tag[in[2]] = (tag[in[3]] & 0xff) | TG_SIMG;
            continue;
        }
        if (op == OpImage && len == 4 && TAGGED(in[3])) {
            if (KIND(in[3]) != TG_SIMG) FAIL("OpImage of a retyped image");
            if (in[2] < bound) tag[in[2]] = (tag[in[3]] & 0xff) | TG_IMG;
            continue;
        }
        if (op == OpImageTexelPointer && len >= 4 && TAGGED(in[3])) FAIL("a texel pointer (an image atomic) on a retyped image");
        int simg = 0, kind = len >= 4 && TAGGED(in[3]) ? img_op(op, &simg) : 0;
        if (kind) {
            if (KIND(in[3]) != (simg ? TG_SIMG : TG_IMG)) FAIL("opcode %u takes a retyped image as %s", op, KIND(in[3]) == TG_SIMG || KIND(in[3]) == TG_IMG ? "the wrong kind of image" : "a pointer");
            if (kind == 3) FAIL("a depth comparison (opcode %u) on a retyped image", op);
            if (kind != 4) {
                uint32_t c = s[(tag[in[3]] & 0xff) - 1].cls - 1, rt = in[1], n;
                if (len < 5) FAIL("opcode %u of %u words", op, len);
                if (kind == 2) {
                    const uint32_t *st = rt < bound && def[rt] ? w + def[rt] : NULL;
                    if (!st || (st[0] & 0xffff) != OpTypeStruct || (st[0] >> 16) != 4 || st[3] >= bound || tk[st[3]] != 1) FAIL("a sparse image operation (opcode %u) of type %u", op, rt);
                    n = tn[st[3]];
                    int j; for (j = 0; j < nsp && !(sp[j].orig == rt && sp[j].c == c); j++) {}
                    if (j == nsp) { if (nsp == MAXS) FAIL("too many sparse result types"); sp[nsp].orig = rt; sp[nsp].c = c; nsp++; }
                } else {
                    if (rt >= bound || tk[rt] != 1) FAIL("an image operation (opcode %u) of type %u (not a float scalar or vector)", op, rt);
                    n = tn[rt];
                }
                need[c] |= 1u << n;
            }
            for (uint32_t k = 4; k < len; k++) if (maybe_id(in, k) && TAGGED(in[k])) FAIL("opcode %u takes a retyped image as another operand", op);
            continue;
        }
        if (op == OpImageWrite && len >= 4 && TAGGED(in[1])) {
            if (KIND(in[1]) != TG_IMG) FAIL("an image write through a pointer");
            uint32_t c = s[(tag[in[1]] & 0xff) - 1].cls - 1, tt = in[3] < bound ? vty[in[3]] : 0;
            if (!tt || tt >= bound || tk[tt] != 1) FAIL("an image write of a texel (id %u) not known as a float scalar or vector", in[3]);
            need[c] |= 1u << tn[tt];
            for (uint32_t k = 2; k < len; k++) if (maybe_id(in, k) && TAGGED(in[k])) FAIL("opcode %u takes a retyped image as another operand", op);
            continue;
        }
        for (uint32_t k = 1; k < len; k++) if (maybe_id(in, k) && TAGGED(in[k])) FAIL("opcode %u uses a retyped image (id %u)", op, in[k]);
    }
    #undef TAGGED
    // New ids: the integer types the module lacks, the images' new types, sampled image types, arrays, pointers and sparse result structs
    uint32_t nb = bound, mk_int[2] = {0}, mk_vec[2][5] = {{0}};
    for (int k = 0; k < nsel; k++) need[s[k].cls - 1] |= 2u;   // the scalar: the image's sampled type
    for (int c = 0; c < 2; c++) {
        if ((need[c] & 2u) && !t_int[c]) t_int[c] = t_ivec[c][1] = mk_int[c] = nb++;
        for (int n = 2; n <= 4; n++) if ((need[c] >> n & 1) && !t_ivec[c][n]) t_ivec[c][n] = mk_vec[c][n] = nb++;
    }
    for (int k = 0; k < nsel; k++) {
        const uint32_t *ti = w + def[s[k].img]; uint32_t tl = ti[0] >> 16, si = t_int[s[k].cls - 1];
        for (int j = 0; j < k && !s[k].nimg; j++) if (s[j].img == s[k].img && s[j].cls == s[k].cls && s[j].fmt == s[k].fmt) s[k].nimg = s[j].nimg;
        // An OpTypeImage of that shape (Dim, Depth, Arrayed, MS and Sampled: a storage image does not take a sampled texture's type, nor the reverse), class and format
        // already in the module (declaring a second one would be invalid)
        if (!s[k].nimg && !mk_int[s[k].cls - 1])
            for (size_t i = 5; i < first_fn && !s[k].nimg; i += w[i] >> 16)
                if ((w[i] & 0xffff) == OpTypeImage && (w[i] >> 16) == tl && w[i + 2] == si && w[i + 8] == s[k].fmt && !memcmp(w + i + 3, ti + 3, 5 * 4) && (tl <= 9 || !memcmp(w + i + 9, ti + 9, (tl - 9) * 4))) s[k].nimg = w[i + 1];
        if (!s[k].nimg) { s[k].nimg = nb++; s[k].mk_img = 1; }
        if (s[k].smp) {   // its OpTypeSampledImage: another selection's, the module's own (of an image type it had), or a new one
            for (int j = 0; j < k && !s[k].nsimg; j++) if (s[j].smp && s[j].nimg == s[k].nimg) s[k].nsimg = s[j].nsimg;
            if (!s[k].nsimg && s[k].nimg < bound)
                for (size_t i = 5; i < first_fn && !s[k].nsimg; i += w[i] >> 16) if ((w[i] & 0xffff) == OpTypeSampledImage && (w[i] >> 16) == 3 && w[i + 2] == s[k].nimg) s[k].nsimg = w[i + 1];
            if (!s[k].nsimg) { s[k].nsimg = nb++; s[k].mk_simg = 1; }
        }
        s[k].elem = s[k].comb ? s[k].nsimg : s[k].nimg;
        if (s[k].arr) { s[k].narr = nb++; s[k].pelem = nb++; }
        s[k].pvar = nb++;
    }
    for (int j = 0; j < nsp; j++) sp[j].nid = nb++;
    // Pass 2: copy, retyping. The capability first; the new types and the moved variables right before the first function (as in vka_spv_vin_retype); then the functions
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    int want_ext = 0; for (int k = 0; k < nsel; k++) if (img_fmt_ext(s[k].fmt)) want_ext = 1;
    if (want_ext && !ext_cap) { uint32_t x[2] = {2u << 16 | OpCapability, CapStorageImageExtendedFormats}; if (!put_n(&o, x, 2)) FAIL("out of memory"); }
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn) {
            for (int c = 0; c < 2; c++) {
                if (mk_int[c]) { uint32_t x[4] = {4u << 16 | OpTypeInt, mk_int[c], 32, (uint32_t)c}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
                for (int n = 2; n <= 4; n++) if (mk_vec[c][n]) { uint32_t x[4] = {4u << 16 | OpTypeVector, mk_vec[c][n], t_int[c], (uint32_t)n}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
            }
            for (int k = 0; k < nsel; k++) {
                if (s[k].mk_img) {
                    const uint32_t *ti = w + def[s[k].img];
                    if (!put_n(&o, ti, ti[0] >> 16)) FAIL("out of memory");
                    uint32_t *x = o.w + o.n - (ti[0] >> 16); x[1] = s[k].nimg; x[2] = t_int[s[k].cls - 1]; x[8] = s[k].fmt;
                }
                if (s[k].mk_simg) { uint32_t x[3] = {3u << 16 | OpTypeSampledImage, s[k].nsimg, s[k].nimg}; if (!put_n(&o, x, 3)) FAIL("out of memory"); }
                if (s[k].arr) {
                    const uint32_t *ta = w + def[s[k].arr];
                    if (!put_n(&o, ta, ta[0] >> 16)) FAIL("out of memory");   // OpTypeArray (its length) or OpTypeRuntimeArray, of the new element type
                    uint32_t *x = o.w + o.n - (ta[0] >> 16); x[1] = s[k].narr; x[2] = s[k].elem;
                    uint32_t y[8] = {4u << 16 | OpTypePointer, s[k].pvar, SCUniformConstant, s[k].narr, 4u << 16 | OpTypePointer, s[k].pelem, SCUniformConstant, s[k].elem};
                    if (!put_n(&o, y, 8)) FAIL("out of memory");
                } else { uint32_t y[4] = {4u << 16 | OpTypePointer, s[k].pvar, SCUniformConstant, s[k].elem}; if (!put_n(&o, y, 4)) FAIL("out of memory"); }
            }
            for (int j = 0; j < nsp; j++) {
                const uint32_t *st = w + def[sp[j].orig];
                uint32_t x[4] = {4u << 16 | OpTypeStruct, sp[j].nid, st[2], t_ivec[sp[j].c][tn[st[3]]]}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            }
            for (int k = 0; k < nsel; k++) { uint32_t x[4] = {4u << 16 | OpVariable, s[k].pvar, s[k].var, SCUniformConstant}; if (!put_n(&o, x, 4)) FAIL("out of memory"); }
        }
        if (i < first_fn) {
            if (op == OpVariable && len >= 4 && w[i + 2] < bound && tag[w[i + 2]] && s[(tag[w[i + 2]] & 0xff) - 1].var == w[i + 2]) { i += len; continue; }   // re-emitted above
            if (!put_n(&o, in, len)) FAIL("out of memory");
            i += len; continue;
        }
        #define SEL(x) (&s[(tag[x] & 0xff) - 1])
        int simg = 0, kind = len >= 5 && in[3] < bound && tag[in[3]] ? img_op(op, &simg) : 0;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && len == 5 && in[3] < bound && tag[in[3]]) {
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = SEL(in[3])->pelem;
        } else if (op == OpLoad && len >= 4 && in[3] < bound && tag[in[3]]) {
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = SEL(in[3])->elem;
        } else if (op == OpCopyObject && len == 4 && in[3] < bound && tag[in[3]]) {
            uint32_t g = KIND(in[3]);
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - 3] = g == TG_IMG ? SEL(in[3])->nimg : g == TG_SIMG ? SEL(in[3])->nsimg : g == TG_PARR || !SEL(in[3])->arr ? SEL(in[3])->pvar : SEL(in[3])->pelem;
        } else if ((op == OpSampledImage || op == OpImage) && in[3] < bound && tag[in[3]]) {
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = op == OpSampledImage ? SEL(in[3])->nsimg : SEL(in[3])->nimg;
        } else if (kind == 1) {   // the texel as the integer vector, bit-cast back to the result id the body uses
            uint32_t c = SEL(in[3])->cls - 1, tmp = nb++;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = t_ivec[c][tn[in[1]]]; o.w[o.n - len + 2] = tmp;
            uint32_t x[4] = {4u << 16 | OpBitcast, in[1], in[2], tmp}; if (!put_n(&o, x, 4)) FAIL("out of memory");
        } else if (kind == 2) {   // the same in a sparse result: the residency code and the bit-cast texel in the original struct
            uint32_t c = SEL(in[3])->cls - 1, rt = in[1], nid = 0; const uint32_t *st = w + def[rt];
            for (int j = 0; j < nsp; j++) if (sp[j].orig == rt && sp[j].c == c) nid = sp[j].nid;
            uint32_t tmp = nb++, code = nb++, itx = nb++, ftx = nb++;
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 1] = nid; o.w[o.n - len + 2] = tmp;
            uint32_t x[] = {5u << 16 | OpCompositeExtract, st[2], code, tmp, 0, 5u << 16 | OpCompositeExtract, t_ivec[c][tn[st[3]]], itx, tmp, 1,
                            4u << 16 | OpBitcast, st[3], ftx, itx, 5u << 16 | OpCompositeConstruct, rt, in[2], code, ftx};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
        } else if (op == OpImageWrite && len >= 4 && in[1] < bound && tag[in[1]]) {
            uint32_t c = SEL(in[1])->cls - 1, tmp = nb++;
            uint32_t x[4] = {4u << 16 | OpBitcast, t_ivec[c][tn[vty[in[3]]]], tmp, in[3]}; if (!put_n(&o, x, 4)) FAIL("out of memory");
            if (!put_n(&o, in, len)) FAIL("out of memory");
            o.w[o.n - len + 3] = tmp;
        } else if (!put_n(&o, in, len)) FAIL("out of memory");
        #undef SEL
        i += len;
    }
    #undef KIND
    o.w[3] = nb;
    free(dset); free(dbind); free(def); free(vty); free(tag); free(tk); free(tn);
    *out_nw = o.n; return o.w;
fail:
    free(dset); free(dbind); free(def); free(vty); free(tag); free(tk); free(tn); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_pool_fb: texture-use feedback for the texture pool. A shader whose texture handle the translator could not trace to a slot reads the NVN texture pool, the
// update-after-bind sampled-image array at set 1 binding 0 (dmsh_abi_vk.h DMSH_VK_POOL_TEX_BINDING), at the handle's texture ID: agl / gsys lit materials read their
// lightmaps that way, from a handle in a material record in a storage buffer. The host cannot see those IDs (no draw binds them), so it could only make an image for every
// registered texture (DM_VK_EAGER_TEX=1: Ocarina of Time's 6,000 images, issue #97) or for none (the lightmaps read 0: black walls). With this rewrite the GPU says which:
// before each access chain into the pool it loads word id >> 5 of a storage buffer (set `fb_set` binding `fb_binding`, a Block of uint words) and, when bit id & 31 is
// clear, sets it with an atomic OR; vka_tex.c reads the bits back after each submission and makes the images of the IDs read. The same index read again in the same block
// gets no second test. In a loop header block (whose first instructions must stay the header's) the atomic OR is done without the test. The idea and the bitmap's layout
// follow Anka Native's texture-use feedback (MIT, copyright 2026 seedds and 2026 EddizEge: vk_android_spv.py tex_feedback), which rewrites the library's modules at build time; here
// the replay rewrites them at pipeline time, so no library changes. A pixel shader with a store loses its early depth test (its stores must not happen for fragments the
// test would kill): VKA_PFB_EARLY adds EarlyFragmentTests where that gives the same result, which the module decides here (no discard, demote or terminate, no depth, sample
// mask or stencil export, no other memory write; or one discard, the alpha test's under the alpha_spec constant, which the pipeline turns off) except for alpha to coverage
// and the alpha test, which the pipeline decides (vka_shader.c vka_shader_pfb_pick gives a pipeline that needs late tests the module without the mode).
// Ported unchanged from Banana Native v0.3.11 (eden/v0.3.11 dm/gfx/vk_android/vka_spvpatch.c), with the opcode names this file lacked prefixed PF_ and upstream's wider
// no_result as pf_no_result (this file's no_result, which vka_spv_img_retype uses, leaves out OpReturn, OpKill, OpFunctionEnd, OpNoLine, OpEmitVertex ...: taken as
// result-bearing here, their word 2 would overwrite the type of another id)
enum { PF_OpConstantNull = 46, PF_OpSpecConstant = 50, PF_DecSpecId = 1 };
static int pf_no_result(uint32_t op) {
    return op == OpStore || op == OpBranch || op == OpBranchConditional || op == OpSwitch || op == OpReturn || op == OpReturnValue || op == OpKill || op == OpUnreachable
        || op == OpSelectionMerge || op == OpLoopMerge || op == OpLabel || op == OpCopyMemory || op == OpCopyMemorySized || op == OpImageWrite || op == OpAtomicStore
        || op == OpTerminateInvocation || op == OpFunctionEnd || op == 224 || op == 225 || op == 8 || op == 317 || op == 5380 || op == 218 || op == 219;
}
enum { PF_BitwiseAnd = 199, PF_ShiftRightLogical = 194, PF_ShiftLeftLogical = 196, PF_AtomicOr = 241, PF_Demote = 5380, PF_EarlyFragmentTests = 9, PF_DepthReplacing = 12,
       PF_BISampleMask = 20, PF_BIFragDepth = 22, PF_BIStencilRef = 5014, PF_Fragment = 4, PF_GLCompute = 5 };
// Word k of an instruction of opcode op (length len) is a literal, not an id, in the instructions a function body has that carry literals among ids (others: none)
static int pf_lit(uint32_t op, uint32_t k) {
    switch (op) {
    case OpExtInst: return k == 4;
    case OpLoad: return k >= 4;
    case OpStore: return k >= 3;
    case 79: return k >= 5;                         // OpVectorShuffle: its components
    case 81: return k >= 4;                         // OpCompositeExtract: its indices
    case 82: return k >= 5;                         // OpCompositeInsert
    case OpSelectionMerge: return k == 2;
    case OpLoopMerge: return k >= 3;
    case OpBranchConditional: return k >= 4;        // branch weights
    case OpSwitch: return k >= 3 && !((k - 3) & 1);
    case 87: case 88: case 91: case 92: case 95: case 98: return k == 5;   // image operands' mask (the operands after it are ids)
    case 89: case 90: case 93: case 94: case 96: case 97: return k == 6;
    default: return 0;
    }
}
static int pf_mem_write(uint32_t op) { return op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || (op >= 228 && op <= 242) || op == 5614 || op == 5615 || op == 6035; }
static int pf_pass(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, const uint32_t *last, uint32_t *rec, vec_t *o,
                   int *sites, int *early, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    uint32_t bound = w[3], nb = bound;
    uint32_t *psc = calloc(bound, 4), *rt = calloc(bound, 4), *dset = malloc(bound * 4), *dbind = malloc(bound * 4), *ity = calloc(bound, 4), *bi = calloc(bound, 4), *vsc = calloc(bound, 4);
    uint8_t *pool = calloc(bound, 1);
    if (!psc || !rt || !dset || !dbind || !ity || !bi || !vsc || !pool) FAIL("out of memory");
    memset(dset, 0xff, bound * 4); memset(dbind, 0xff, bound * 4);
    uint32_t t_u32 = 0, t_bool = 0, p_uni_u32 = 0, model = UINT32_MAX;
    const uint32_t scb = w[1] >= 0x10300 ? SCStorageBuffer : SCUniform, blk = w[1] >= 0x10300 ? 2u : DecBufferBlock;   // (1.3 on: StorageBuffer and Block; before: Uniform and BufferBlock)
    int vtx = 0, kill = 0, mem_writes = 0, has_early = 0, depth_mode = 0, nsites = 0, aspec = 0;
    size_t first_fn = nw, first_annot = nw, last_ep = nw;
    for (size_t i = 5; i < nw;) {   // what the rewrite needs: types, the pool's variables, the sites, and whether a pixel shader's tests can run early
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
        if (op == OpEntryPoint && len >= 3) { last_ep = i; if (model == UINT32_MAX) model = w[i + 1]; else if (model != w[i + 1]) FAIL("entry points of two execution models"); if (w[i + 1] >= 1 && w[i + 1] <= 3) vtx = 1; if (w[i + 1] == 0) vtx = 1; }
        else if (op == OpExecutionMode && len >= 3) { if (w[i + 2] == PF_EarlyFragmentTests) has_early = 1; if (w[i + 2] == PF_DepthReplacing || (w[i + 2] >= 14 && w[i + 2] <= 16)) depth_mode = 1; }
        else if (op == OpDecorate && len >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == DecDescriptorSet) dset[w[i + 1]] = w[i + 3];
            else if (w[i + 2] == DecBinding) dbind[w[i + 1]] = w[i + 3];
            else if (w[i + 2] == DecBuiltIn) bi[w[i + 1]] = w[i + 3] + 1;
            else if (w[i + 2] == PF_DecSpecId && w[i + 3] == alpha_spec) aspec = 1;
        }
        else if (op == OpTypeInt && len >= 4 && w[i + 1] < bound) { ity[w[i + 1]] = w[i + 2] | (w[i + 3] ? 0x100u : 0); if (w[i + 2] == 32 && !w[i + 3] && !t_u32) t_u32 = w[i + 1]; }
        else if (op == OpTypeBool && len >= 2 && !t_bool) t_bool = w[i + 1];
        else if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) {
            psc[w[i + 1]] = w[i + 2] + 1;
            if (op == OpTypePointer && len >= 4 && w[i + 2] == scb && w[i + 3] && w[i + 3] == t_u32 && !p_uni_u32) p_uni_u32 = w[i + 1];
        }
        else if (op == OpVariable && len >= 4 && w[i + 2] < bound) { rt[w[i + 2]] = w[i + 1]; vsc[w[i + 2]] = w[i + 3] + 1; if (dset[w[i + 2]] == set && dbind[w[i + 2]] == tex_binding) pool[w[i + 2]] = 1; }
        else if ((op == OpConstant || op == PF_OpSpecConstant || op == PF_OpConstantNull || op == OpUndef) && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
        if (is_annot(op) && first_annot == nw) first_annot = i;
        if (op == OpFunction && first_fn == nw) first_fn = i;
        if (first_fn < nw) {   // a function's body
            if (op == OpKill || op == OpTerminateInvocation || op == PF_Demote) kill++;
            if (op == OpImageWrite) mem_writes = 1;
            else if (pf_mem_write(op) && len >= 2) { uint32_t p = op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == 228 ? w[i + 1] : len >= 4 ? w[i + 3] : 0;
                uint32_t sc = p < bound && rt[p] < bound && psc[rt[p]] ? psc[rt[p]] - 1 : SCUniform;   // (a pointer of unknown class: counted as memory)
                if (sc_mem(sc)) mem_writes = 1; }
            if (!pf_no_result(op) && op != OpLabel && op != OpFunction && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
            else if (op == OpFunction && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
            int site = (op == OpAccessChain || op == OpInBoundsAccessChain) && len >= 4 && w[i + 3] < bound && pool[w[i + 3]];
            if (site) {
                if (len != 5) FAIL("a texture pool access chain with %u indices", len - 4);
                uint32_t x = w[i + 4], t = x < bound ? rt[x] : 0;
                if (!t || t >= bound || (ity[t] & 0xff) != 32) FAIL("a texture pool index that is not a 32-bit integer");
                nsites++;
            }
            for (uint32_t k = 1; k < len; k++) if (w[i + k] < bound && pool[w[i + k]] && !(site && k == 3) && !pf_lit(op, k)) FAIL("the texture pool used by opcode %u, not a one-index access chain", op);
        }
        i += len;
    }
    if (!nsites) { free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool); return 0; }
    if (model == UINT32_MAX || first_fn == nw) FAIL("no entry point or function");
    if (vtx && !(flags & VKA_PFB_VTX_STORES)) FAIL("a vertex-pipeline stage on a device without vertexPipelineStoresAndAtomics");
    int exports = depth_mode;
    for (uint32_t id = 1; id < bound; id++) if (bi[id] && vsc[id] == SCOutput + 1 && (bi[id] - 1 == PF_BIFragDepth || bi[id] - 1 == PF_BISampleMask || bi[id] - 1 == PF_BIStencilRef)) exports = 1;
    // Early tests give the result late ones do: kind 1 always, kind 2 when the alpha test is off (its one discard, under the alpha_spec constant, the only one), 0 never
    int kind = model != PF_Fragment || has_early || exports || mem_writes ? 0 : !kill ? 1 : kill == 1 && aspec ? 2 : 0;
    int add_early = (flags & VKA_PFB_EARLY) && kind;
    if (first_annot > first_fn) first_annot = first_fn;
    // New ids, in the same order in both passes: missing types, the buffer's types and variable, the constants; then each site's
    uint32_t mk_u32 = t_u32 ? 0 : nb++, mk_bool = t_bool ? 0 : nb++; if (!t_u32) t_u32 = mk_u32; if (!t_bool) t_bool = mk_bool;
    uint32_t mk_pu = p_uni_u32 && !mk_u32 ? 0 : nb++; if (mk_pu) p_uni_u32 = mk_pu;
    uint32_t rta = nb++, st = nb++, pst = nb++, fbv = nb++, c0 = nb++, c1 = nb++, c5 = nb++, c31 = nb++, cmask = nb++;
    if (!put_n(o, w, 5)) FAIL("out of memory");
    uint32_t cur = 0, cur_orig = 0, done[16]; int ndone = 0, in_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (op == OpEntryPoint && w[1] >= 0x10400) {   // SPIR-V 1.4 on: every global variable the entry point uses is in its interface
            if (!put(o, (len + 1) << 16 | OpEntryPoint) || !put_n(o, in + 1, len - 1) || !put(o, fbv)) FAIL("out of memory");
            if (i == last_ep && add_early) { uint32_t x[] = {3u << 16 | OpExecutionMode, in[2], PF_EarlyFragmentTests}; if (!put_n(o, x, 3)) FAIL("out of memory"); }
            i += len; continue;
        }
        if (i == first_annot) {
            uint32_t x[] = {4u << 16 | OpDecorate, rta, DecArrayStride, 4, 3u << 16 | OpDecorate, st, blk, 5u << 16 | OpMemberDecorate, st, 0, DecOffset, 0,
                            4u << 16 | OpDecorate, fbv, DecDescriptorSet, fb_set, 4u << 16 | OpDecorate, fbv, DecBinding, fb_binding};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (i == first_fn) {
            if (mk_u32) { uint32_t x[] = {4u << 16 | OpTypeInt, mk_u32, 32, 0}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            if (mk_bool) { uint32_t x[] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(o, x, 2)) FAIL("out of memory"); }
            if (mk_pu) { uint32_t x[] = {4u << 16 | OpTypePointer, mk_pu, scb, t_u32}; if (!put_n(o, x, 4)) FAIL("out of memory"); }
            uint32_t x[] = {3u << 16 | OpTypeRuntimeArray, rta, t_u32, 3u << 16 | OpTypeStruct, st, rta, 4u << 16 | OpTypePointer, pst, scb, st,
                            4u << 16 | OpVariable, pst, fbv, scb, 4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c1, 1,
                            4u << 16 | OpConstant, t_u32, c5, 5, 4u << 16 | OpConstant, t_u32, c31, 31, 4u << 16 | OpConstant, t_u32, cmask, 0xFFFFFu};
            if (!put_n(o, x, sizeof x / 4)) FAIL("out of memory");
        }
        if (op == OpFunction) in_fn = 1;
        else if (op == OpFunctionEnd) in_fn = 0;
        if (op == OpEntryPoint && i == last_ep && add_early) {   // (before 1.4: the mode after the last entry point)
            if (!put_n(o, in, len)) FAIL("out of memory");
            uint32_t x[] = {3u << 16 | OpExecutionMode, in[2], PF_EarlyFragmentTests}; if (!put_n(o, x, 3)) FAIL("out of memory");
            i += len; continue;
        }
        if (op == OpLabel) { cur = cur_orig = in[1]; ndone = 0; if (!put_n(o, in, len)) FAIL("out of memory"); i += len; continue; }
        if (op == OpPhi && last) {   // a parent block that was split ends in its last piece
            if (!put_n(o, in, 3)) FAIL("out of memory");
            for (uint32_t k = 3; k + 1 < len; k += 2) { uint32_t par = in[k + 1]; if (!put(o, in[k]) || !put(o, par < bound && last[par] ? last[par] : par)) FAIL("out of memory"); }
            i += len; continue;
        }
        if (in_fn && (op == OpAccessChain || op == OpInBoundsAccessChain) && len == 5 && in[3] < bound && pool[in[3]]) {
            uint32_t x = in[4]; int seen = 0;
            for (int k = 0; k < ndone; k++) if (done[k] == x) seen = 1;
            if (!seen) {
                if (ndone < 16) done[ndone++] = x;
                (*sites)++;
                int header = 0;
                for (size_t j = i; j < nw;) { uint32_t oj = w[j] & 0xffff; if (oj == OpLoopMerge) header = 1; if (is_term(oj)) break; j += w[j] >> 16; }
                uint32_t iu = x;
                if (rt[x] != t_u32) { iu = nb++; uint32_t y[] = {4u << 16 | OpBitcast, t_u32, iu, x}; if (!put_n(o, y, 4)) FAIL("out of memory"); }
                uint32_t m = nb++, wi = nb++, bt = nb++, bit = nb++, p = nb++;
                uint32_t y[] = {5u << 16 | PF_BitwiseAnd, t_u32, m, iu, cmask, 5u << 16 | PF_ShiftRightLogical, t_u32, wi, m, c5, 5u << 16 | PF_BitwiseAnd, t_u32, bt, m, c31,
                                5u << 16 | PF_ShiftLeftLogical, t_u32, bit, c1, bt, 6u << 16 | OpAccessChain, p_uni_u32, p, fbv, c0, wi};
                if (!put_n(o, y, sizeof y / 4)) FAIL("out of memory");
                if (header) { uint32_t r = nb++, z[] = {7u << 16 | PF_AtomicOr, t_u32, r, p, c1, c0, bit}; if (!put_n(o, z, 7)) FAIL("out of memory"); }   // (scope Device, semantics None)
                else {
                    uint32_t old = nb++, has = nb++, clr = nb++, a = nb++, l = nb++, r = nb++;
                    uint32_t z[] = {4u << 16 | OpLoad, t_u32, old, p, 5u << 16 | PF_BitwiseAnd, t_u32, has, old, bit, 5u << 16 | OpIEqual, t_bool, clr, has, c0,
                                    3u << 16 | OpSelectionMerge, l, 0, 4u << 16 | OpBranchConditional, clr, a, l, 2u << 16 | OpLabel, a,
                                    7u << 16 | PF_AtomicOr, t_u32, r, p, c1, c0, bit, 2u << 16 | OpBranch, l, 2u << 16 | OpLabel, l};
                    if (!put_n(o, z, sizeof z / 4)) FAIL("out of memory");
                    cur = l; if (rec && cur_orig < bound) rec[cur_orig] = l;
                }
            }
        }
        if (!put_n(o, in, len)) FAIL("out of memory");
        i += len;
    }
    (void)cur;
    o->w[3] = nb;
    if (early) *early = add_early ? kind : 0;
    free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool);
    return 1;
fail:
    free(psc); free(rt); free(dset); free(dbind); free(ity); free(bi); free(vsc); free(pool);
    return -1;
#undef FAIL
}
uint32_t *vka_spv_pool_fb(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, size_t *out_nw, int *sites, int *early, char *err, size_t errn) {
    int s0 = 0, e0 = 0; if (!sites) sites = &s0; if (!early) early = &e0;
    *sites = 0; *early = 0; if (errn) err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t *rec = calloc(w[3], 4); vec_t o = {0};
    if (!rec) { snprintf(err, errn, "out of memory"); return NULL; }
    int r = pf_pass(w, nw, set, tex_binding, fb_set, fb_binding, flags, alpha_spec, NULL, rec, &o, sites, early, err, errn);
    if (r <= 0) { free(rec); free(o.w); *sites = 0; return NULL; }   // (0: no pool read, err empty)
    free(o.w); o = (vec_t){0}; *sites = 0;
    r = pf_pass(w, nw, set, tex_binding, fb_set, fb_binding, flags, alpha_spec, rec, NULL, &o, sites, early, err, errn);
    free(rec);
    if (r <= 0) { free(o.w); *sites = 0; return NULL; }
    *out_nw = o.n;
    return o.w;
}
// Whether a pixel shader does nothing but write its colour outputs (vka_spvpatch.h vka_spv_fs_inert): the same reading as pf_pass's early-test kinds, without the pool
int vka_spv_fs_inert(const uint32_t *w, size_t nw, uint32_t alpha_spec) {
    if (!w || nw < 5 || w[0] != 0x07230203 || !w[3] || w[3] > (1u << 22)) return 0;
    uint32_t bound = w[3], model = UINT32_MAX;
    uint32_t *psc = calloc(bound, 4), *rt = calloc(bound, 4), *bi = calloc(bound, 4), *vsc = calloc(bound, 4);
    int r = 0, kill = 0, mem = 0, exports = 0, aspec = 0; size_t first_fn = nw;
    if (!psc || !rt || !bi || !vsc) goto out;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) goto out;
        if (op == OpEntryPoint && len >= 3) { if (model == UINT32_MAX) model = w[i + 1]; else if (model != w[i + 1]) goto out; }
        else if (op == OpExecutionMode && len >= 3 && (w[i + 2] == PF_DepthReplacing || (w[i + 2] >= 14 && w[i + 2] <= 16))) exports = 1;   // DepthReplacing, DepthGreater / Less / Unchanged
        else if (op == OpDecorate && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == DecBuiltIn) bi[w[i + 1]] = w[i + 3] + 1; else if (w[i + 2] == PF_DecSpecId && w[i + 3] == alpha_spec) aspec = 1; }
        else if ((op == OpTypePointer || op == OpTypeForwardPointer) && len >= 3 && w[i + 1] < bound) psc[w[i + 1]] = w[i + 2] + 1;
        else if (op == OpVariable && len >= 4 && w[i + 2] < bound) { rt[w[i + 2]] = w[i + 1]; vsc[w[i + 2]] = w[i + 3] + 1; }
        else if ((op == OpConstant || op == PF_OpSpecConstant || op == PF_OpConstantNull || op == OpUndef) && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
        if (op == OpFunction && first_fn == nw) first_fn = i;
        if (first_fn < nw) {   // a function's body: discards, and writes to memory (a pointer of unknown class counts as memory)
            if (op == OpKill || op == OpTerminateInvocation || op == PF_Demote) kill++;
            if (op == OpImageWrite) mem = 1;
            else if (pf_mem_write(op) && len >= 2) {
                uint32_t p = op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == 228 ? w[i + 1] : len >= 4 ? w[i + 3] : 0;
                uint32_t sc = p < bound && rt[p] < bound && psc[rt[p]] ? psc[rt[p]] - 1 : SCUniform;
                if (sc_mem(sc)) mem = 1;
            }
            if (!pf_no_result(op) && op != OpLabel && len >= 3 && w[i + 2] < bound) rt[w[i + 2]] = w[i + 1];
        }
        i += len;
    }
    for (uint32_t id = 1; id < bound; id++) if (bi[id] && vsc[id] == SCOutput + 1 && (bi[id] - 1 == PF_BIFragDepth || bi[id] - 1 == PF_BISampleMask || bi[id] - 1 == PF_BIStencilRef)) exports = 1;
    r = model != PF_Fragment || first_fn == nw || exports || mem ? 0 : !kill ? 1 : kill == 1 && aspec ? 2 : 0;
out:
    free(psc); free(rt); free(bi); free(vsc);
    return r;
}

// vka_spv_array_len: a descriptor array declared longer than the device's table. The translator declares the texture pool as an array of 2^20 sampled images (and the
// sampler and image pools likewise, DMSH_VK_POOL_*_COUNT), which is the table's size on an Adreno. A device whose update-after-bind limits are lower gets a shorter table
// (vka_tex.c bindless_init: 499,680 on a Mali-G720), and a pipeline whose shader declares more elements than its layout binding holds is invalid
// (VUID-VkGraphicsPipelineCreateInfo-layout-07991): a driver may refuse it or read past the table. So every variable at (set, binding) whose type is an array of more than
// len elements gets an array type of len: a new OpConstant right before the OpTypeArray, which takes it as its length (the old constant may have other uses). Texture
// IDs at or past len were never in the table (vka_tex.c says so in red). NULL: nothing to change (no such variable, or none longer than len), or not SPIR-V
uint32_t *vka_spv_array_len(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t len, size_t *out_nw) {
    if (nw < 5 || w[0] != 0x07230203u || !len) return NULL;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 24)) return NULL;
    uint32_t *dset = malloc((size_t)bound * 4), *dbind = malloc((size_t)bound * 4), *pointee = calloc(bound, 4), *alen = calloc(bound, 4), *cval = calloc(bound, 4), *ctype = calloc(bound, 4);
    uint8_t *mark = calloc(bound, 1); uint32_t *out = NULL; size_t extra = 0;
    if (!dset || !dbind || !pointee || !alen || !cval || !ctype || !mark) goto done;
    memset(dset, 0xff, (size_t)bound * 4); memset(dbind, 0xff, (size_t)bound * 4);
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) goto done;
        if (op == 71 && n == 4 && w[i + 1] < bound) { if (w[i + 2] == 34) dset[w[i + 1]] = w[i + 3]; else if (w[i + 2] == 33) dbind[w[i + 1]] = w[i + 3]; }   // OpDecorate DescriptorSet / Binding
        else if (op == 32 && n == 4 && w[i + 1] < bound) pointee[w[i + 1]] = w[i + 3];                                                                   // OpTypePointer
        else if (op == 28 && n == 4 && w[i + 1] < bound) alen[w[i + 1]] = w[i + 3];                                                                      // OpTypeArray
        else if (op == 43 && n == 4 && w[i + 2] < bound) { cval[w[i + 2]] = w[i + 3]; ctype[w[i + 2]] = w[i + 1]; }                                      // OpConstant (one word)
        i += n;
    }
    for (size_t i = 5; i < nw;) {   // OpVariable: its pointer's array type, when it is at (set, binding) and longer than len
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 59 && n >= 4 && w[i + 2] < bound && dset[w[i + 2]] == set && dbind[w[i + 2]] == binding && w[i + 1] < bound) {
            uint32_t arr = pointee[w[i + 1]];
            if (arr && arr < bound && alen[arr] && alen[arr] < bound && ctype[alen[arr]] && cval[alen[arr]] > len && !mark[arr]) { mark[arr] = 1; extra += 4; }
        }
        i += n;
    }
    if (!extra) goto done;
    out = malloc((nw + extra) * 4); if (!out) goto done;
    memcpy(out, w, 5 * 4); size_t o = 5; uint32_t next = bound;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == 28 && n == 4 && w[i + 1] < bound && mark[w[i + 1]]) {
            out[o++] = (4u << 16) | 43; out[o++] = ctype[w[i + 3]]; out[o++] = next; out[o++] = len;
            out[o++] = w[i]; out[o++] = w[i + 1]; out[o++] = w[i + 2]; out[o++] = next++;
        } else { memcpy(out + o, w + i, (size_t)n * 4); o += n; }
        i += n;
    }
    out[3] = next; *out_nw = o;
done:
    free(dset); free(dbind); free(pointee); free(alen); free(cval); free(ctype); free(mark);
    return out;
}

// vka_spv_gs_fill: a geometry shader that may end without emitting anything is made to emit one degenerate primitive instead (its vertices all at (0, 0, 0, -1), outside
// the clip volume: the primitive covers nothing), so every input primitive has an output. Adreno renders in tiles: a binning pass runs the vertex and geometry stages to
// sort the primitives into screen tiles, and the rendering pass runs them again. A geometry shader that decides by a float test whether to emit (Donkey Kong Bananza's
// terrain culling shader 5d537f7630ec3671 emits only front-facing, on-screen triangles that cover a pixel) can decide differently in the two passes for a triangle on the
// edge; with the camera inside the ground there are many, and Qualcomm's driver then draws stray triangles and stops the GPU for good (kgsl "possible gpu syncpoint
// deadlock"). With a degenerate primitive in place of none it does not (Banana Native, Odin 2 Portal, Adreno 740).
// Taken: one Geometry entry point, a points / line strip / triangle strip output whose OutputVertices holds one primitive, every OpEmitVertex / OpEndPrimitive in the entry
// function (no streams), gl_Position as member 0 of an Output block or a BuiltIn Position Output variable, a vec4 of 32-bit floats. A Private flag is cleared at the start of
// the entry function and set after each OpEmitVertex; before each OpReturn of the entry function, when it is still clear, the degenerate primitive is emitted (the other
// outputs as they are). NULL with err empty: nothing to do (not such a shader); with err: not rewritten.
// Adapted from Banana Native commit 0749171c (dm/gfx/vk_android/vka_spvpatch.c vka_spv_gs_fill, MIT), written against this file's helpers (it has no module table)
uint32_t *vka_spv_gs_fill(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    enum { GF_ModeOutputVertices = 26, GF_ModeOutputPoints = 27, GF_ModeOutputLineStrip = 28, GF_ModeOutputTriangleStrip = 29, GF_ExecGeometry = 3,
           GF_OpEmitVertex = 218, GF_OpEndPrimitive = 219, GF_OpEmitStreamVertex = 220, GF_OpEndStreamPrimitive = 221, GF_OpNoLine = 317, GF_OpTypeVoid = 19,
           GF_BIPosition = 0 };
    vec_t o = {0}, decl = {0}; uint8_t *posblk = NULL; uint32_t *dop = NULL, *da1 = NULL, *da2 = NULL;
    err[0] = 0;
    if (!w || nw < 5 || w[0] != 0x07230203u || !w[3] || w[3] > (1u << 24)) FAIL("not SPIR-V");
    uint32_t bound = w[3];
    // Per id, for the types and globals: the defining opcode and two operands (a type: its words 2 and 3; OpVariable: its result type and storage class).
    // posblk 1: a struct whose member 0 is BuiltIn Position; 2: an id decorated BuiltIn Position
    if (!(posblk = calloc(bound, 1)) || !(dop = calloc(bound, 4)) || !(da1 = calloc(bound, 4)) || !(da2 = calloc(bound, 4))) FAIL("out of memory");
    uint32_t ep = 0, ep_at = 0, neps = 0, nv = 0, maxv = 0, first_fn = 0, t_bool = 0, posvar_direct = 0;
    for (size_t i = 5; i < nw;) {   // (an OpExecutionMode comes after its OpEntryPoint: the entry point is known by then)
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction length");
        if (op == OpFunction) { first_fn = (uint32_t)i; break; }
        if (op == OpEntryPoint && len >= 3) { neps++; if (w[i + 1] == GF_ExecGeometry) { ep = w[i + 2]; ep_at = (uint32_t)i; } }
        if (op == OpExecutionMode && len >= 3 && ep && w[i + 1] == ep) {
            uint32_t m = w[i + 2];
            if (m == GF_ModeOutputVertices && len >= 4) maxv = w[i + 3];
            else if (m == GF_ModeOutputPoints) nv = 1; else if (m == GF_ModeOutputLineStrip) nv = 2; else if (m == GF_ModeOutputTriangleStrip) nv = 3;
        }
        if (op == OpMemberDecorate && len >= 5 && w[i + 2] == 0 && w[i + 3] == DecBuiltIn && w[i + 4] == GF_BIPosition && w[i + 1] < bound) posblk[w[i + 1]] = 1;
        if (op == OpDecorate && len >= 4 && w[i + 2] == DecBuiltIn && w[i + 3] == GF_BIPosition && w[i + 1] < bound) posblk[w[i + 1]] = 2;
        if (op == OpTypeBool && len >= 2) t_bool = w[i + 1];
        if (op >= GF_OpTypeVoid && op <= OpTypeForwardPointer && len >= 2 && w[i + 1] < bound) { uint32_t id = w[i + 1]; dop[id] = op; da1[id] = len >= 3 ? w[i + 2] : 0; da2[id] = len >= 4 ? w[i + 3] : 0; }
        if (op == OpVariable && len >= 4 && w[i + 2] < bound) { uint32_t id = w[i + 2]; dop[id] = op; da1[id] = w[i + 1]; da2[id] = w[i + 3]; }
        i += len;
    }
    if (!ep || neps != 1 || !nv || maxv < nv || !first_fn) goto none;
    // The emits: all in the entry function, none to a stream
    uint32_t fn = 0, nemit = 0;
    for (size_t i = first_fn; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > nw) FAIL("bad instruction length");
        if (op == OpFunction && len >= 3) fn = w[i + 2];
        if (op == GF_OpEmitStreamVertex || op == GF_OpEndStreamPrimitive) goto none;
        if (op == GF_OpEmitVertex || op == GF_OpEndPrimitive) { if (fn != ep) goto none; nemit += op == GF_OpEmitVertex; }
        i += len;
    }
    if (!nemit) goto none;
    // gl_Position: an Output variable of a block with member 0 Position (a chain to member 0), or one decorated Position itself
    uint32_t posvar = 0, v4 = 0;
    for (uint32_t v = 1; v < bound && !posvar; v++) {
        if (dop[v] != OpVariable || da2[v] != SCOutput || da1[v] >= bound || dop[da1[v]] != OpTypePointer) continue;
        uint32_t t = da2[da1[v]];
        if (t < bound && dop[t] == OpTypeStruct && posblk[t] == 1) { posvar = v; v4 = da1[t]; }
        else if (posblk[v] == 2) { posvar = v; posvar_direct = 1; v4 = t; }
    }
    if (!posvar || v4 >= bound || dop[v4] != OpTypeVector || da2[v4] != 4) goto none;
    uint32_t t_f = da1[v4];
    if (t_f >= bound || dop[t_f] != OpTypeFloat || da1[t_f] != 32) goto none;
    // Declarations, before the first function: the module's own types where it has them (a second integer, boolean or pointer type of the same kind is invalid)
    uint32_t nb = bound, t_u = 0, p_priv = 0, p_out = 0;
    for (uint32_t t = 1; t < bound && !t_u; t++) if (dop[t] == OpTypeInt && da1[t] == 32 && da2[t] == 0) t_u = t;
    if (!t_u) { t_u = nb++; uint32_t x[4] = {4u << 16 | OpTypeInt, t_u, 32, 0}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    if (!t_bool) { t_bool = nb++; uint32_t x[2] = {2u << 16 | OpTypeBool, t_bool}; if (!put_n(&decl, x, 2)) FAIL("out of memory"); }
    for (uint32_t t = 1; t < bound && !p_priv; t++) if (dop[t] == OpTypePointer && da1[t] == SCPrivate && da2[t] == t_u) p_priv = t;
    if (!p_priv) { p_priv = nb++; uint32_t x[4] = {4u << 16 | OpTypePointer, p_priv, SCPrivate, t_u}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    if (!posvar_direct) {
        for (uint32_t t = 1; t < bound && !p_out; t++) if (dop[t] == OpTypePointer && da1[t] == SCOutput && da2[t] == v4) p_out = t;
        if (!p_out) { p_out = nb++; uint32_t x[4] = {4u << 16 | OpTypePointer, p_out, SCOutput, v4}; if (!put_n(&decl, x, 4)) FAIL("out of memory"); }
    }
    uint32_t u0 = nb++, u1 = nb++, f0 = nb++, fm1 = nb++, dead = nb++, flag = nb++;
    { uint32_t x[] = {4u << 16 | OpConstant, t_u, u0, 0, 4u << 16 | OpConstant, t_u, u1, 1, 4u << 16 | OpConstant, t_f, f0, 0, 4u << 16 | OpConstant, t_f, fm1, 0xbf800000u,
                      7u << 16 | OpConstantComposite, v4, dead, f0, f0, f0, fm1, 4u << 16 | OpVariable, p_priv, flag, SCPrivate};
      if (!put_n(&decl, x, sizeof x / 4)) FAIL("out of memory"); }
    // Copy: the flag in the entry point's interface from SPIR-V 1.4 (every global there), the declarations before the first function, the entry function rewritten
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    int in_ep = 0, cleared = 0, nret = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *in = w + i;
        if (i == first_fn && !put_n(&o, decl.w, decl.n)) FAIL("out of memory");
        if (i == ep_at && w[1] >= 0x10400) {
            size_t at = o.n; if (!put_n(&o, in, len) || !put(&o, flag)) FAIL("out of memory");
            o.w[at] = (uint32_t)(o.n - at) << 16 | OpEntryPoint; i += len; continue;
        }
        if (op == OpFunction && len >= 3) { in_ep = w[i + 2] == ep; cleared = 0; }
        if (in_ep && !cleared && op != OpFunction && op != OpFunctionParameter && op != OpLabel && op != OpVariable && op != OpLine && op != GF_OpNoLine) {
            uint32_t x[3] = {3u << 16 | OpStore, flag, u0}; if (!put_n(&o, x, 3)) FAIL("out of memory"); cleared = 1;
        }
        if (in_ep && op == OpReturn) {
            uint32_t ld = nb++, z = nb++, fill = nb++, merge = nb++;
            uint32_t x[] = {4u << 16 | OpLoad, t_u, ld, flag, 5u << 16 | OpIEqual, t_bool, z, ld, u0, 3u << 16 | OpSelectionMerge, merge, 0,
                            4u << 16 | OpBranchConditional, z, fill, merge, 2u << 16 | OpLabel, fill};
            if (!put_n(&o, x, sizeof x / 4)) FAIL("out of memory");
            for (uint32_t k = 0; k < nv; k++) {
                uint32_t dst = posvar;
                if (!posvar_direct) { dst = nb++; uint32_t c[5] = {5u << 16 | OpAccessChain, p_out, dst, posvar, u0}; if (!put_n(&o, c, 5)) FAIL("out of memory"); }
                uint32_t st[4] = {3u << 16 | OpStore, dst, dead, 1u << 16 | GF_OpEmitVertex}; if (!put_n(&o, st, 4)) FAIL("out of memory");
            }
            uint32_t y[] = {1u << 16 | GF_OpEndPrimitive, 2u << 16 | OpBranch, merge, 2u << 16 | OpLabel, merge};
            if (!put_n(&o, y, sizeof y / 4)) FAIL("out of memory");
            nret++;
        }
        if (!put_n(&o, in, len)) FAIL("out of memory");
        if (in_ep && op == GF_OpEmitVertex) { uint32_t x[3] = {3u << 16 | OpStore, flag, u1}; if (!put_n(&o, x, 3)) FAIL("out of memory"); }
        i += len;
    }
    if (!nret) FAIL("no OpReturn in the entry function");
    o.w[3] = nb;
    free(decl.w); free(posblk); free(dop); free(da1); free(da2);
    *out_nw = o.n; return o.w;
none:
fail:
    free(decl.w); free(posblk); free(dop); free(da1); free(da2); free(o.w);
    return NULL;
#undef FAIL
}

// vka_spv_shuffle_xor (from DroidPlay Core 8115593e, MIT, spv_shx.h): every OpGroupNonUniformShuffle as OpGroupNonUniformShuffleXor by lane ^ index. Qualcomm's shader
// compiler in its V@07xx drivers dies on the translator's shuffle (SIGSEGV at fault address 0x48 in libllvm-qgl.so inside vkCreate*Pipelines: a Retroid Pocket 5's
// Adreno 650 at V@0746, a Retroid Pocket G2's Adreno 735 at V@0762), and builds the same module once each shuffle is a ShuffleXor. lane ^ (lane ^ index) = index, so the
// same lane is read whatever the index: OpLoad of the SubgroupLocalInvocationId built-in, OpBitwiseXor with the index (an integer of 32 bits, as Shuffle's Id must be
// unsigned), ShuffleXor by the result with the shuffle's own type, scope and value. Needs the built-in's Input variable and a 32-bit unsigned type (every translated
// module with a shuffle has both: the translator computes the shuffle's lane from it); without them nothing is rewritten and err says why
uint32_t *vka_spv_shuffle_xor(const uint32_t *w, size_t nw, size_t *out_nw, int *sites, char *err, size_t errn) {
    *sites = 0; err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t lane_dec = 0, lane = 0, tu = 0, bound = w[3]; int nsh = 0;
    for (int pass = 0; pass < 2; pass++)   // the decoration comes before the variable in a valid module, but a second pass costs nothing and holds for any order
        for (size_t i = 5; i < nw;) {
            uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
            if (!len || i + len > nw) { snprintf(err, errn, "bad instruction at word %zu", i); return NULL; }
            if (pass == 0) {
                if (op == OpDecorate && len >= 4 && x[2] == DecBuiltIn && x[3] == BISubgroupLocalInvocationId) lane_dec = x[1];
                else if (op == OpTypeInt && len == 4 && x[2] == 32 && x[3] == 0 && !tu) tu = x[1];
                else if (op == OpGNUShuffle) nsh++;
            } else if (op == 59 && len >= 4 && lane_dec && x[2] == lane_dec && x[3] == 1) lane = x[2];   // OpVariable %ptr %id Input
            i += len;
        }
    if (!nsh) return NULL;   // nothing to do
    if (!lane || !tu) { snprintf(err, errn, "%d shuffles but no %s", nsh, !lane ? "SubgroupLocalInvocationId Input variable" : "32-bit unsigned integer type"); return NULL; }
    if (bound + 2u * (uint32_t)nsh < bound) { snprintf(err, errn, "id bound overflow"); return NULL; }
    vec_t o = {0};
    if (!put_n(&o, w, 5)) goto oom;
    for (size_t i = 5; i < nw;) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; i += len;
        if (op == OpGNUShuffle && len == 6) {   // OpGroupNonUniformShuffle %type %r %scope %value %index
            uint32_t l = bound++, m = bound++;
            uint32_t y[] = {4u << 16 | OpLoad, tu, l, lane,                                       // OpLoad %uint %l %lane
                            5u << 16 | 198, tu, m, l, x[5],                                       // OpBitwiseXor %uint %m %l %index
                            6u << 16 | OpGNUShuffleXor, x[1], x[2], x[3], x[4], m};               // OpGroupNonUniformShuffleXor %type %r %scope %value %m
            if (!put_n(&o, y, sizeof y / 4)) goto oom;
            (*sites)++;
            continue;
        }
        if (!put_n(&o, x, len)) goto oom;
    }
    if (!*sites) { free(o.w); snprintf(err, errn, "%d shuffles of an unexpected length", nsh); return NULL; }
    o.w[3] = bound;
    *out_nw = o.n; return o.w;
oom:
    free(o.w); snprintf(err, errn, "out of memory"); return NULL;
}

// vka_spv_shuffle_halves: each OpGroupNonUniformShuffle whose lane is (SubgroupLocalInvocationId & -32) | k, k a constant from 0 to 31 (or without the | k), as two
// OpGroupNonUniformBroadcast from the constant lanes k and 32 + k, the second taken where bit 32 of the shuffle's lane is set: the value of lane k of the invocation's own
// 32-lane half, as before, on a subgroup of 32 or 64 lanes. The translator writes Maxwell's SHFL.IDX of lane k (a warp of 32) this way on the library's 64-lane subgroups;
// Turnip has no arbitrary-index shuffle instruction and ir3 makes each such shuffle a loop of rotates, one pass per distinct (lane - index): up to 32 for this pattern.
// Ocarina of Time's foliage mesh programs read all 32 lanes of their half in a row (32 such shuffles each, 36 of its 520 task and mesh programs): their compute fallback
// went from 51.8 to 20.0 ms of a Kokiri frame on the Adreno 830 with Swap2-Turnip, the picture on 891.14 unchanged (the module's meaning is kept). Only for a subgroup of
// 64 lanes (lane 32 + k must exist where it is read). A broadcast's lane is a constant (OpConstant), as SPIR-V before 1.5 asks; a vector result needs SPIR-V 1.4's OpSelect
// with a scalar condition, so before 1.4 only scalar results are rewritten. Adds the GroupNonUniformBallot capability a broadcast needs when the module lacks it.
// A malloc'd copy (*out_nw words, *sites shuffles rewritten); NULL with err empty when no shuffle has the pattern, NULL with the reason in err when it cannot be rewritten
enum { SH_OpCapability = 17, SH_OpBitwiseOr = 197, SH_OpBitwiseAnd = 199, SH_OpINotEqual = 171, SH_CapGroupNonUniformBallot = 64 };
typedef struct { const uint32_t *w; uint32_t bound, lane; const uint32_t *def, *kof; const uint8_t *isconst; } sh_ctx;
static uint32_t sh_strip(const sh_ctx *c, uint32_t id) {   // through OpBitcast
    for (int n = 0; n < 16 && id < c->bound && c->def[id] && (c->w[c->def[id]] & 0xffff) == OpBitcast; n++) id = c->w[c->def[id] + 3];
    return id;
}
static int sh_is_lane(const sh_ctx *c, uint32_t id) {   // a load of the SubgroupLocalInvocationId input
    id = sh_strip(c, id);
    return id < c->bound && c->def[id] && (c->w[c->def[id]] & 0xffff) == OpLoad && c->w[c->def[id] + 3] == c->lane;
}
static int sh_const(const sh_ctx *c, uint32_t id, uint32_t *v) {
    id = sh_strip(c, id);
    if (id >= c->bound || !c->isconst[id]) return 0;
    *v = c->kof[id]; return 1;
}
static int sh_half(const sh_ctx *c, uint32_t id) {   // lane & -32, either operand order
    id = sh_strip(c, id); uint32_t v = 0;
    if (id >= c->bound || !c->def[id] || (c->w[c->def[id]] & 0xffff) != SH_OpBitwiseAnd) return 0;
    const uint32_t *x = c->w + c->def[id];
    return (sh_is_lane(c, x[3]) && sh_const(c, x[4], &v) && v == 0xffffffe0u) || (sh_is_lane(c, x[4]) && sh_const(c, x[3], &v) && v == 0xffffffe0u);
}
static int sh_lane_k(const sh_ctx *c, uint32_t idx) {   // k when idx is (lane & -32) | k, else -1
    uint32_t id = sh_strip(c, idx), k = 0;
    if (sh_half(c, id)) return 0;
    if (id >= c->bound || !c->def[id] || (c->w[c->def[id]] & 0xffff) != SH_OpBitwiseOr) return -1;
    const uint32_t *x = c->w + c->def[id];
    if (sh_half(c, x[3]) && sh_const(c, x[4], &k) && k < 32) return (int)k;
    if (sh_half(c, x[4]) && sh_const(c, x[3], &k) && k < 32) return (int)k;
    return -1;
}
uint32_t *vka_spv_shuffle_halves(const uint32_t *w, size_t nw, size_t *out_nw, int *sites, char *err, size_t errn) {
#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto fail; } while (0)
    *sites = 0; err[0] = 0;
    if (nw < 5 || w[0] != 0x07230203) { snprintf(err, errn, "not SPIR-V"); return NULL; }
    uint32_t bound = w[3], nb = bound;
    uint32_t *def = NULL, *kof = NULL; uint8_t *isconst = NULL, *tyk = NULL; vec_t o = {0};
    def = calloc(bound, 4); kof = calloc(bound, 4); isconst = calloc(bound, 1); tyk = calloc(bound, 1);   // tyk: 1 a 32-bit scalar type, 2 a vector type, 3 another integer
    if (!def || !kof || !isconst || !tyk) FAIL("out of memory");
    uint32_t lane_dec = 0, lane = 0, t_u32 = 0, t_bool = 0; size_t first_fn = nw, last_cap = 0; int has_ballot = 0, nsh = 0;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 5; i < nw;) {
            uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
            if (!len || i + len > nw) FAIL("bad instruction at word %zu", i);
            if (pass == 0) {
                if (op == SH_OpCapability && len >= 2) { last_cap = i + len; if (x[1] == SH_CapGroupNonUniformBallot) has_ballot = 1; }
                else if (op == OpDecorate && len >= 4 && x[2] == DecBuiltIn && x[3] == BISubgroupLocalInvocationId) lane_dec = x[1];
                else if (op == OpTypeInt && len >= 4 && x[1] < bound) { tyk[x[1]] = x[2] == 32 ? 1 : 3; if (x[2] == 32 && !x[3] && !t_u32) t_u32 = x[1]; }
                else if (op == OpTypeFloat && len >= 3 && x[1] < bound) tyk[x[1]] = x[2] == 32 ? 1 : 3;
                else if (op == OpTypeVector && len >= 4 && x[1] < bound) tyk[x[1]] = 2;
                else if (op == OpTypeBool && len >= 2) t_bool = x[1];
                else if (op == OpFunction && first_fn == nw) first_fn = i;
                else if (op == OpGNUShuffle) nsh++;
            } else {
                if (op == OpVariable && len >= 4 && lane_dec && x[2] == lane_dec && x[3] == SCInput) lane = x[2];
                else if (op == OpConstant && len == 4 && x[2] < bound && x[1] < bound && tyk[x[1]] == 1) { isconst[x[2]] = 1; kof[x[2]] = x[3]; }   // a 32-bit scalar
                if ((op == OpLoad || op == OpBitcast || op == SH_OpBitwiseAnd || op == SH_OpBitwiseOr) && len >= 4 && x[2] < bound) def[x[2]] = (uint32_t)i;
            }
            i += len;
        }
    if (!nsh || !lane || !t_u32 || !last_cap) goto none;   // nothing of this shape
    sh_ctx c = {w, bound, lane, def, kof, isconst};
    int want[32] = {0};
    for (size_t i = first_fn; i < nw; i += w[i] >> 16) {
        if ((w[i] & 0xffff) != OpGNUShuffle || (w[i] >> 16) != 6 || w[i + 1] >= bound) continue;
        int k = sh_lane_k(&c, w[i + 5]);
        if (k < 0 || !(tyk[w[i + 1]] == 1 || (tyk[w[i + 1]] == 2 && w[1] >= 0x10400))) continue;
        want[k] = 1; (*sites)++;
    }
    if (!*sites) goto none;
    uint32_t mk_bool = t_bool ? 0 : nb++; if (!t_bool) t_bool = mk_bool;
    uint32_t c0 = nb++, c32 = nb++, ck[64] = {0};
    for (int k = 0; k < 32; k++) if (want[k]) { ck[k] = nb++; ck[k + 32] = nb++; }
    if (!put_n(&o, w, 5)) FAIL("out of memory");
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16; const uint32_t *x = w + i;
        if (i == last_cap && !has_ballot) { uint32_t y[2] = {2u << 16 | SH_OpCapability, SH_CapGroupNonUniformBallot}; if (!put_n(&o, y, 2)) FAIL("out of memory"); }
        if (i == first_fn) {
            if (mk_bool) { uint32_t y[2] = {2u << 16 | OpTypeBool, mk_bool}; if (!put_n(&o, y, 2)) FAIL("out of memory"); }
            uint32_t y[8] = {4u << 16 | OpConstant, t_u32, c0, 0, 4u << 16 | OpConstant, t_u32, c32, 32}; if (!put_n(&o, y, 8)) FAIL("out of memory");
            for (uint32_t k = 0; k < 64; k++) if (ck[k]) { uint32_t z[4] = {4u << 16 | OpConstant, t_u32, ck[k], k}; if (!put_n(&o, z, 4)) FAIL("out of memory"); }
        }
        if (i >= first_fn && op == OpGNUShuffle && len == 6 && x[1] < bound) {
            int k = sh_lane_k(&c, x[5]);
            if (k >= 0 && (tyk[x[1]] == 1 || (tyk[x[1]] == 2 && w[1] >= 0x10400))) {
                uint32_t lo = nb++, hi = nb++, bit = nb++, cond = nb++;
                uint32_t y[] = {6u << 16 | OpGNUBroadcast, x[1], lo, x[3], x[4], ck[k],          // OpGroupNonUniformBroadcast %type %lo %scope %value %k
                                6u << 16 | OpGNUBroadcast, x[1], hi, x[3], x[4], ck[k + 32],     // ... from lane 32 + k
                                5u << 16 | SH_OpBitwiseAnd, t_u32, bit, x[5], c32,               // the shuffle's lane & 32
                                5u << 16 | SH_OpINotEqual, t_bool, cond, bit, c0,
                                6u << 16 | OpSelect, x[1], x[2], cond, hi, lo};                   // under the shuffle's own result id
                if (!put_n(&o, y, sizeof y / 4)) FAIL("out of memory");
                i += len; continue;
            }
        }
        if (!put_n(&o, x, len)) FAIL("out of memory");
        i += len;
    }
    o.w[3] = nb;
    free(def); free(kof); free(isconst); free(tyk);
    *out_nw = o.n; return o.w;
none:
    free(def); free(kof); free(isconst); free(tyk); *sites = 0;
    return NULL;
fail:
    free(def); free(kof); free(isconst); free(tyk); free(o.w); *sites = 0;
    return NULL;
#undef FAIL
}
