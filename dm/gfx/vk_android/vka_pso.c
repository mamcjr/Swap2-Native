// Pipelines of the Vulkan replay. The device has no extended dynamic state 3 and no shader objects (android/spike results), so a graphics pipeline is keyed on the state those
// would have made dynamic: attachment formats and sample count, blending and write masks per attachment, logic operation, polygon mode, depth clamp, alpha to coverage, the
// topology class and the depth convention (SpecId 0 on the last pre-raster stage), with vertex input in the key only on a device without VK_EXT_vertex_input_dynamic_state.
// Everything else is dynamic (EDS1 / EDS2): viewport and scissor (one of each), cull mode, front face, topology, depth and stencil tests, depth bias, rasterizer discard,
// primitive restart, blend constants. Pipelines and set layouts live as long as the process; the VkPipelineCache goes to the app's files (DM_TEMP, the game's own temp folder)
// so the next start builds from it. Layouts follow dmsh_abi_vk.h: set 0 the per-draw records of every stage (push descriptors when they fit), set 1 the pools (vka_tex.c)
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_pwlist.h"
#include "vka_compile_gate.h"
#include "vka_psogate.h"
#include "vka_deadly.h"
#include "vka_cachewrite.h"
#include "vk_prewarm_seed.h"   // the profile's seed list (dm/games/<id>/vk_prewarm.bin; dm/games/game.py seed): dm_vk_prewarm_seed, dm_vk_prewarm_seed_len (0: none)
#include "../nvn/nvn_mtl.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vkc_gate g_warm_gate = VKC_GATE_INIT;
static _Atomic int g_demand_waiters;
// Suspend (mtl_drain, the app going to the background): the prewarm and optimizer threads stop between pipelines until the game submits again (vka_pso_pause(0) at the next
// submission), so a backgrounded app does not keep compiling on the CPU for minutes
static _Atomic int g_pso_paused;
void vka_pso_pause(int on) { atomic_store(&g_pso_paused, on != 0); }
static int g_warm_only;   // the process only warms the pipeline cache (vka_pso_warm, nxhost_vk_warm): no game, so the prewarm takes what the game's share would be
static void pause_point(void) { while (atomic_load_explicit(&g_pso_paused, memory_order_relaxed)) usleep(100000); }
// The keys' hash, 8 bytes a step: the draw path hashes a pipeline key (about 1 KB) per draw, and FNV a byte at a time took 1 us of it on the Adreno 840 phone's cores
static inline uint64_t fnv(const void *p, size_t n, uint64_t h) { return vka_pw_fnv(p, n, h); }
_Static_assert(VKA_PW_TOPO_MAX == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && VKA_PW_POLY_MAX == VK_POLYGON_MODE_POINT && VKA_PW_LOGIC_MAX == VK_LOGIC_OP_SET
               && VKA_PW_BLENDF_MAX == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA && VKA_PW_BLENDOP_MAX == VK_BLEND_OP_MAX && VKA_PW_FS_VARIANT_MAX == VKA_FS_RETYPE, "vka_pwlist.h bounds");

// A small map from a byte key to a value (layouts, pipelines): open addressing on the key's hash, keys copied
typedef struct { uint64_t h; void *key; void *val; } ment;
typedef struct { ment *e; size_t cap, n, klen; } kmap;
static void *km_get(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return m->e[i].val; }
}
static void km_put(kmap *m, const void *key, uint64_t h, void *val) {
    if (2 * (m->n + 1) > m->cap) {
        size_t nc = m->cap ? 2 * m->cap : 256; ment *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < m->cap; i++) if (m->e[i].key) { size_t j = m->e[i].h & (nc - 1); while (n[j].key) j = (j + 1) & (nc - 1); n[j] = m->e[i]; }
        free(m->e); m->e = n; m->cap = nc;
    }
    size_t j = h & (m->cap - 1); while (m->e[j].key) j = (j + 1) & (m->cap - 1);
    void *k = malloc(m->klen); memcpy(k, key, m->klen);
    m->e[j] = (ment){h, k, val}; m->n++;
}
// The value slot of key, to change an entry in place (NULL: not in the map)
static void **km_slot(kmap *m, const void *key, uint64_t h) {
    if (!m->cap) return NULL;
    for (size_t i = h & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) { if (!m->e[i].key) return NULL; if (m->e[i].h == h && !memcmp(m->e[i].key, key, m->klen)) return &m->e[i].val; }
}

// ---------- Layouts ----------
static VkShaderStageFlags stage_bit(int vks) {
    if (!V.mesh && vks >= DMSH_VK_STAGE_TS) return VK_SHADER_STAGE_COMPUTE_BIT;
    static const VkShaderStageFlags b[8] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                            VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT};
    return vks >= 0 && vks < 8 ? b[vks] : 0;
}
enum { MAXB = 6 * DMSH_VK_STAGE_BINDINGS + 4 };   // guest resources plus the emulated mesh buffers
typedef struct { uint32_t n; struct { uint16_t binding; uint8_t desc, pad; uint16_t count; uint16_t stages; } b[MAXB]; } lkey;
static kmap g_layouts = {.klen = sizeof(lkey)};
static uint64_t g_nlayouts;

// The layout of a set of shaders, remembered by the shaders themselves: building the bindings key (3 KB) and hashing it was a third of the replay's CPU time at 5,500 draws a frame
typedef struct { vka_shader *sh[VKA_NSTAGE]; uint64_t compute; } skey;
static kmap g_lshort = {.klen = sizeof(skey)};
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out);
int vka_layout_of(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    skey sk; memset(&sk, 0, sizeof sk);
    for (int st = 0; st < VKA_NSTAGE; st++) if ((st == 5) == (compute != 0)) sk.sh[st] = sh[st];
    sk.compute = compute != 0;
    uint64_t h = fnv(&sk, sizeof sk, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_lshort, &sk, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    pthread_mutex_unlock(&g_mu);
    memset(out, 0, sizeof *out);
    int ok = layout_slow(sh, compute, out);
    L = malloc(sizeof *L); *L = *out;
    pthread_mutex_lock(&g_mu);
    if (!km_get(&g_lshort, &sk, h)) km_put(&g_lshort, &sk, h, L); else free(L);
    pthread_mutex_unlock(&g_mu);
    return ok;
}
static int layout_slow(vka_shader *const sh[VKA_NSTAGE], int compute, vka_layout *out) {
    lkey k; memset(&k, 0, sizeof k);
    uint32_t ndesc = 0; int mesh_fallback = 0;
    for (int st = 0; st < VKA_NSTAGE; st++) {
        const vka_shader *s = sh[st];
        if (!s || (st == 5) != (compute != 0)) continue;
        mesh_fallback |= !V.mesh && (s->e->flags & DMSH_VK_F_MESH_FALLBACK);
        for (unsigned i = 0; i < s->e->bind_count; i++) {
            const dmsh_vk_bind *b = &s->b[i];
            if (b->set != DMSH_VK_SET_DRAW || b->kind == DMSH_VK_KIND_TEX_POOL || b->kind == DMSH_VK_KIND_SAMP_POOL) continue;
            if (k.n == MAXB) { VRED_ONCE("more than %d set-0 bindings in one pipeline", MAXB); return 0; }
            k.b[k.n].binding = b->binding; k.b[k.n].desc = b->desc; k.b[k.n].count = b->count ? b->count : 1; k.b[k.n].stages = (uint16_t)stage_bit(s->e->stage);
            ndesc += k.b[k.n].count; k.n++;
        }
        if (s->gs_fx > 0 && k.n < MAXB) {   // the claim words of a geometry shader that writes memory (vka_spv_gs_claim)
            k.b[k.n].binding = VKA_GS_CLAIM_BINDING; k.b[k.n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; k.b[k.n].count = 1; k.b[k.n].stages = (uint16_t)stage_bit(s->e->stage); ndesc++; k.n++;
        }
    }
    if (mesh_fallback) {
        if (k.n + 4 > MAXB) return 0;
        uint32_t storage = 4;
        for (uint32_t i = 0; i < k.n; i++) if (k.b[i].desc == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) storage += k.b[i].count;
        if (storage > V.props.limits.maxDescriptorSetStorageBuffers || storage > V.props.limits.maxPerStageDescriptorStorageBuffers) {
            VRED_ONCE("task/mesh fallback storage descriptors exceed the device limit"); return 0;
        }
        for (uint32_t i = 0; i < 4; i++) {
            k.b[k.n].binding = DMSH_VK_MESH_OUT + i; k.b[k.n].desc = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            k.b[k.n].count = 1; k.b[k.n].stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT; k.n++; ndesc++;
        }
    }
    // Sorted by binding, so the same set of records gives the same key whatever the order of stages
    for (uint32_t i = 1; i < k.n; i++) for (uint32_t j = i; j > 0 && k.b[j - 1].binding > k.b[j].binding; j--) { typeof(k.b[0]) t = k.b[j]; k.b[j] = k.b[j - 1]; k.b[j - 1] = t; }
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    vka_layout *L = km_get(&g_layouts, &k, h);
    if (L) { *out = *L; pthread_mutex_unlock(&g_mu); return out->layout != VK_NULL_HANDLE; }
    L = calloc(1, sizeof *L);
    L->ndesc = ndesc; L->push = V.push_desc && ndesc <= V.max_push;
    VkDescriptorSetLayoutBinding *lb = calloc(k.n ? k.n : 1, sizeof *lb);
    for (uint32_t i = 0; i < k.n; i++) lb[i] = (VkDescriptorSetLayoutBinding){k.b[i].binding, (VkDescriptorType)k.b[i].desc, k.b[i].count, k.b[i].stages, NULL};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = L->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0, .bindingCount = k.n, .pBindings = lb};
    VkResult r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &L->set0);
    free(lb);
    if (!r) {
        // set 1 the pool (bindless tables), set 2 the pool reads' bitmap when they are reported (vka_tex.c "Texture pool reads"; DM_VK_POOL_FB=0: two sets, as before)
        VkDescriptorSetLayout sets[3] = {L->set0, vka_bindless_layout(), vka_pfb_layout()};
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = !sets[1] ? 1 : sets[2] ? 3 : 2, .pSetLayouts = sets};
        r = vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &L->layout);
    }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pipeline layout of %u bindings (%u descriptors): %d", k.n, ndesc, r); L->layout = VK_NULL_HANDLE; }
    km_put(&g_layouts, &k, h, L); g_nlayouts++;
    *out = *L;
    pthread_mutex_unlock(&g_mu);
    return out->layout != VK_NULL_HANDLE;
}


// ---------- The pipeline cache ----------
// DM_VK_PCACHE (restart): unset = $DM_TEMP/vk_pipelines[-<driver>]-fp0[-pfb].bin (VKA_PCACHE_GEN; -pfb, VKA_PCACHE_PFB, with the texture pool reads reported); a path = that file, as named; 0 = no file (the run starts empty and keeps nothing: a cold run). Before 2026-11-03 "0"
// was taken as a file named 0 in the working folder, so a second "cold" run read the first one's pipelines back (stutter-agent cold2gpl: 10 MB read, warm)
static VkPipelineCache g_pc; static char g_pc_path[1024], g_pw_path[1100], g_pw_import[1100]; static uint64_t g_made; static _Atomic uint64_t g_saved_at, g_save_ns, g_last_compile_ns; static double g_create_ms;
static uint64_t g_ngfx, g_ncs, g_nfail, g_ncs_over, g_create_ns, g_frame_made;   // all pipelines made (frame path, background and prewarm); g_frame_made / g_create_ns: on the frame path only
static uint64_t g_probe_hit, g_probe_miss, g_probe_miss_ns;          // GPL: the cache-only try before a link (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)
static _Atomic uint64_t g_save_ns_total, g_hy_draws, g_pw_wait_ns;
static void pw_load(void); static void pw_start(void); static int pw_save(void); static void heavy_load(void); static void deadly_load(void); static void bb_open(void);
static void *saver_main(void *a);
static pthread_mutex_t g_save_mu = PTHREAD_MUTEX_INITIALIZER;   // one save at a time (the saver thread, a forced save)
static pthread_mutex_t g_sv_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sv_cv = PTHREAD_COND_INITIALIZER; static int g_sv_want, g_sv_up;

// The file is kept for one shader library at a time (from upstream Banana Native v0.3.11, vka_pckeep.h, here inline). Its stamp <file>.lib names the library it was
// saved with and lists the shaders whose pipelines it holds with the hash of their library entry. Vulkan's cache finds a pipeline by the driver's hash of the module and
// state, so an entry of an older library is never used for a changed shader: it only takes space, and memory when a driver deserializes every object of a cache it opens
// (Mesa's vk_pipeline_cache_load: Turnip uploads each shader with its private memory; upstream: a 72 MB Turnip cache of 1,791 shaders committed 5.2 GB, lmkd then killed
// Ocarina of Time at its first 3D frames). Ours kept every library ever played: 0.1.6's live start read 36 MB holding four earlier library chains of Ocarina of Time.
// A file of another library is read when the shaders it lists that this library has byte for byte the same (live) outnumber the stale ones (changed or dropped, counted
// over every update the file was carried across) by DM_VK_PCACHE_KEEP (restart; percent of the live ones, default 50; 0 = a file of another library is never read);
// otherwise, and with no stamp (every file written before this), the run starts empty and its first save replaces the file. DM_VK_PCACHE_STAMP=0 (restart): no stamp,
// the file read whatever library made it (as before). The stamp's text: "<library id, 16 hex>\nkeep 1 dead <stale> shaders <n>\n" then n lines "<key> <entry hash>"
typedef struct { uint64_t key, hash; } vka_pck_ent;
typedef struct { uint64_t lib; int table; uint64_t dead; vka_pck_ent *ent; size_t n; } vka_pck_stamp;   // table 0: a stamp of the library only; ent by key (malloc'd)
enum { VKA_PCK_SAME, VKA_PCK_CARRIED, VKA_PCK_UNVERIFIED, VKA_PCK_NO_STAMP, VKA_PCK_STALE, VKA_PCK_OFF };
static inline int vka_pck_reads(int d) { return d == VKA_PCK_SAME || d == VKA_PCK_CARRIED || d == VKA_PCK_UNVERIFIED; }
static inline void vka_pck_free(vka_pck_stamp *s) { free(s->ent); memset(s, 0, sizeof *s); }
static inline int vka_pck_cmp_(const void *a, const void *b) { uint64_t x = ((const vka_pck_ent *)a)->key, y = ((const vka_pck_ent *)b)->key; return x < y ? -1 : x > y; }
// The stamp's text (n bytes, not terminated) -> *s. A first line that is no library id: lib 0; a list cut short or malformed: the library only (table 0)
static void vka_pck_parse(const char *t, size_t n, vka_pck_stamp *s) {
    memset(s, 0, sizeof *s);
    char *b = malloc(n + 1); if (!b) return;
    memcpy(b, t, n); b[n] = 0;
    char *p = b, *nl = strchr(p, '\n'); if (nl) *nl = 0;
    char *end; unsigned long long lib = strtoull(p, &end, 16);
    if (end == p || end - p > 16) { free(b); return; }
    s->lib = lib;
    unsigned long long dead, cnt;
    if (!nl || sscanf(nl + 1, "keep 1 dead %llu shaders %llu", &dead, &cnt) != 2 || cnt > (1u << 24)) { free(b); return; }
    p = strchr(nl + 1, '\n');
    vka_pck_ent *e = malloc((cnt ? cnt : 1) * sizeof *e); size_t k = 0;
    while (e && p && k < cnt) {
        unsigned long long key, h;
        if (sscanf(p + 1, "%16llx %16llx", &key, &h) != 2 || !h) break;
        e[k++] = (vka_pck_ent){key, h};
        p = strchr(p + 1, '\n');
    }
    if (!e || k != cnt) { free(e); free(b); return; }
    qsort(e, k, sizeof *e, vka_pck_cmp_);
    size_t u = 0; for (size_t i = 0; i < k; i++) if (!u || e[u - 1].key != e[i].key) e[u++] = e[i];
    s->table = 1; s->dead = dead; s->ent = e; s->n = u;
    free(b);
}
// What a run of library cur does with a file whose stamp is old: hash_of(ctx, key) is key's entry hash in cur (0 = not in it); *out the shaders the file keeps (read
// only), *live / *changed of old's list the same in cur / changed or dropped, *dead the stale estimate carried on
static int vka_pck_decide(const vka_pck_stamp *old, uint64_t cur, int keep_pct, uint64_t (*hash_of)(void *, uint64_t), void *ctx,
                          vka_pck_stamp *out, size_t *live, size_t *changed, uint64_t *dead) {
    memset(out, 0, sizeof *out); out->lib = cur; out->table = 1; *live = *changed = 0; *dead = 0;
    if (!old->lib) return VKA_PCK_NO_STAMP;
    if (old->lib == cur) {
        if (old->n && (out->ent = malloc(old->n * sizeof *out->ent))) { memcpy(out->ent, old->ent, old->n * sizeof *out->ent); out->n = old->n; }
        *live = out->n; *dead = out->dead = old->dead;
        return VKA_PCK_SAME;
    }
    if (keep_pct <= 0) return VKA_PCK_OFF;
    if (!old->table) return VKA_PCK_UNVERIFIED;
    vka_pck_ent *e = malloc((old->n ? old->n : 1) * sizeof *e); size_t k = 0;
    if (!e) return VKA_PCK_STALE;
    for (size_t i = 0; i < old->n; i++) {
        uint64_t h = hash_of(ctx, old->ent[i].key);
        if (h && h == old->ent[i].hash) e[k++] = old->ent[i]; else (*changed)++;
    }
    *live = k;
    uint64_t d = old->dead + *changed;
    if (!k || d * 100 > (uint64_t)k * (uint64_t)keep_pct) { free(e); return VKA_PCK_STALE; }
    out->ent = e; out->n = k; out->dead = *dead = d;
    return VKA_PCK_CARRIED;
}
// The stamp a save writes: lib, the stale estimate, the shaders the file kept (keep) and those this run made (run: their hashes win). malloc'd text, *len
static char *vka_pck_format(uint64_t lib, uint64_t dead, const vka_pck_ent *keep, size_t nk, const vka_pck_ent *run, size_t nr, size_t *len) {
    vka_pck_ent *k = malloc((nk ? nk : 1) * sizeof *k), *r = malloc((nr ? nr : 1) * sizeof *r), *all = malloc((nk + nr ? nk + nr : 1) * sizeof *all);
    if (!k || !r || !all) { free(k); free(r); free(all); return NULL; }
    if (nk) memcpy(k, keep, nk * sizeof *k);
    if (nr) memcpy(r, run, nr * sizeof *r);
    qsort(k, nk, sizeof *k, vka_pck_cmp_); qsort(r, nr, sizeof *r, vka_pck_cmp_);
    size_t i = 0, j = 0, u = 0;
    while (i < nk || j < nr) {
        vka_pck_ent e;
        if (j < nr && (i >= nk || r[j].key <= k[i].key)) { e = r[j++]; while (i < nk && k[i].key == e.key) i++; }   // (a key of both: this run's hash)
        else e = k[i++];
        if (!e.hash) continue;
        if (!u || all[u - 1].key != e.key) all[u++] = e;
    }
    free(k); free(r);
    size_t cap = 96 + u * 34; char *t = malloc(cap); if (!t) { free(all); return NULL; }
    size_t o = (size_t)snprintf(t, cap, "%016llx\nkeep 1 dead %llu shaders %zu\n", (unsigned long long)lib, (unsigned long long)dead, u);
    for (size_t q = 0; q < u; q++) o += (size_t)snprintf(t + o, cap - o, "%016llx %016llx\n", (unsigned long long)all[q].key, (unsigned long long)all[q].hash);
    free(all);
    *len = o;
    return t;
}
// The game's library (the first pipeline's shaders' library; a selftest's own, unmapped, is not taken), its id (FNV-1a of its header: chains, game, profile inputs,
// table sizes; every rebuild or update changes it) and an entry's hash of its bytes (its record, bindings, c1 bytes and stored modules, offsets left out)
static const vka_shlib *_Atomic g_pc_shlib;
static uint64_t g_pc_lib;   // the library this run's cache file is stamped with (0: none; no stamp read or written)
static vka_pck_stamp g_pck;   // the shaders the file was read with that this library has the same, and the stale estimate (written again with this run's)
static uint64_t pck_mix(const void *p, size_t n, uint64_t h) {   // 8 bytes a step (a game's stored modules are tens of MB)
    const uint8_t *b = p; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static uint64_t pck_entry_hash(const vka_shlib *L, const dmsh_vk_entry *e) {
    dmsh_vk_entry z = *e; z.mod = 0; z.bind = 0; z.c1_off = 0; z.c1_off_hi = 0;
    uint64_t h = pck_mix("DMVKPC1", 8, 1469598103934665603ull);
    h = pck_mix(&z, sizeof z, h);
    h = pck_mix(L->bind + e->bind, (size_t)e->bind_count * sizeof(dmsh_vk_bind), h);
    if (e->c1_len) h = pck_mix(vka_shlib_c1(L, e), e->c1_len, h);
    for (unsigned i = 0; i < e->mod_count; i++) {
        dmsh_vk_module m = L->mod[e->mod + i]; const uint8_t *src = L->blob + vka_shlib_mod_off(&m);
        m.off = 0; m.off_hi = 0;
        h = pck_mix(&m, sizeof m, h); h = pck_mix(src, m.stored, h);
    }
    h ^= h >> 32;
    return h ? h : 1;
}
static uint64_t pck_hash_of(void *ctx, uint64_t key) { const vka_shlib *L = ctx; const dmsh_vk_entry *e = L ? vka_shlib_find(L, key) : NULL; return e ? pck_entry_hash(L, e) : 0; }
static uint64_t pck_lib_id(const vka_shlib *L) {
    if (!L || !L->h) return 0;
    const uint8_t *p = (const uint8_t *)L->h; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *L->h; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}
// The library's shaders whose pipelines this run made (pck_note from pstate_fill and cs_obtain: frame path, asynchronous, optimizer, prewarm), each hashed once when first
// met, outside the lock; the stamp lists them with the kept ones at each save
static vka_pck_ent *g_pck_run; static size_t g_pck_runcap, g_pck_runn; static pthread_mutex_t g_pck_mu = PTHREAD_MUTEX_INITIALIZER;
static int pck_run_has(uint64_t key) {   // caller holds g_pck_mu
    if (!g_pck_runcap) return 0;
    for (size_t j = key & (g_pck_runcap - 1); g_pck_run[j].key; j = (j + 1) & (g_pck_runcap - 1)) if (g_pck_run[j].key == key) return 1;
    return 0;
}
static void pck_note(const vka_shader *s) {
    const vka_shlib *L = atomic_load_explicit(&g_pc_shlib, memory_order_relaxed);
    if (!g_pc_lib || !s || !s->key || !s->e || s->lib != L) return;
    pthread_mutex_lock(&g_pck_mu); int had = pck_run_has(s->key); pthread_mutex_unlock(&g_pck_mu);
    if (had || vka_shlib_find(L, s->key) != s->e) return;   // (a mesh shader's pulling vertex stage has its key, not its entry: the mesh shader is listed)
    uint64_t h = pck_entry_hash(L, s->e);
    pthread_mutex_lock(&g_pck_mu);
    if (!pck_run_has(s->key)) {
        if (2 * (g_pck_runn + 1) > g_pck_runcap) {
            size_t nc = g_pck_runcap ? 2 * g_pck_runcap : 1024; vka_pck_ent *n = calloc(nc, sizeof *n);
            if (!n) { pthread_mutex_unlock(&g_pck_mu); return; }
            for (size_t i = 0; i < g_pck_runcap; i++) if (g_pck_run[i].key) { size_t j = g_pck_run[i].key & (nc - 1); while (n[j].key) j = (j + 1) & (nc - 1); n[j] = g_pck_run[i]; }
            free(g_pck_run); g_pck_run = n; g_pck_runcap = nc;
        }
        size_t j = s->key & (g_pck_runcap - 1); while (g_pck_run[j].key) j = (j + 1) & (g_pck_runcap - 1);
        g_pck_run[j] = (vka_pck_ent){s->key, h}; g_pck_runn++;
    }
    pthread_mutex_unlock(&g_pck_mu);
}
static vka_pck_ent *pck_made(size_t *n) {   // malloc'd copy, *n of them
    *n = 0;
    pthread_mutex_lock(&g_pck_mu);
    vka_pck_ent *out = g_pck_runn ? malloc(g_pck_runn * sizeof *out) : NULL;
    for (size_t i = 0; out && i < g_pck_runcap; i++) if (g_pck_run[i].key) out[(*n)++] = g_pck_run[i];
    pthread_mutex_unlock(&g_pck_mu);
    return out;
}
static void pc_open_once(void) {
    atomic_store(&g_save_ns, now_ns());
    const char *e = getenv("DM_VK_PCACHE"), *t = getenv("DM_TEMP"), *home = getenv("HOME");
    if (vka_guest_fp() || (e && !strcmp(e, "0"))) g_pc_path[0] = 0;   // (DM_VK_GUEST_FPCR=1: pipelines compiled with the game's FPCR are kept in no file)
    else if (e && *e) snprintf(g_pc_path, sizeof g_pc_path, "%s", e);
    else if ((t && *t) || (home && *home)) {   // a custom driver (DM_VK_DRIVER) keeps its own file, so switching drivers leaves the system driver's cache alone
        const char *dt = vka_driver_tag();
        // (with the texture pool reads reported, three-set layouts: a file of their own, vka.h VKA_PCACHE_PFB; the two-set file of DM_VK_POOL_FB=0 is left as it is)
        snprintf(g_pc_path, sizeof g_pc_path, "%s/vk_pipelines%s%s" VKA_PCACHE_GEN "%s.bin", t && *t ? t : home, dt ? "-" : "", dt ? dt : "", vka_pfb_layout() ? VKA_PCACHE_PFB : "");
        // Before 2026-11-07 the file had no generation: its pipelines were compiled with the game's FPCR (flush-to-zero; vka_dev.c vka_host_fp), and Qualcomm's compiler
        // made other code from it (Ocarina of Time's overexposed scene). Such a file is removed, not read. An explicit DM_VK_PCACHE file is the caller's to replace
        char old[1024]; snprintf(old, sizeof old, "%s/vk_pipelines%s%s.bin", t && *t ? t : home, dt ? "-" : "", dt ? dt : "");
        if (!unlink(old)) VLOG("pipeline cache %s removed: its pipelines were compiled with the game's floating-point environment (now %s)", old, g_pc_path);
    }
    // The prewarm list (pw_*): the pipeline keys of earlier runs, whatever the driver (a key names shaders and state, not driver code). DM_VK_PREWARM_FILE overrides
    const char *pf = getenv("DM_VK_PREWARM_FILE");
    if (pf && *pf) snprintf(g_pw_path, sizeof g_pw_path, "%s", pf);
    else if (e && *e && strcmp(e, "0")) snprintf(g_pw_path, sizeof g_pw_path, "%s.keys", e);
    else if ((t && *t) || (home && *home)) {
        snprintf(g_pw_path, sizeof g_pw_path, "%s/vk_pipelines.keys", t && *t ? t : home);
        // Another device's list, which the Android launcher's Import shaders... puts beside it (a shaders zip's prewarm.keys: android/SHADERS.md "Shader zips"): merged
        // into this one at load, removed once a save has written them (pw_load, pw_save). Only beside the default list of the game's own folder (DM_TEMP)
        if (t && *t) snprintf(g_pw_import, sizeof g_pw_import, "%s/vk_pipelines.import.keys", t);
    }
    void *data = NULL; size_t n = 0;
    FILE *f = g_pc_path[0] ? fopen(g_pc_path, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 32 && (data = malloc((size_t)sz)) && fread(data, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
        fclose(f);
        // The header names the device: a cache of another driver is dropped here (the driver would ignore it too)
        uint32_t hl, ver, vendor, devid; uint8_t uuid[VK_UUID_SIZE];
        if (n) { memcpy(&hl, data, 4); memcpy(&ver, (uint8_t *)data + 4, 4); memcpy(&vendor, (uint8_t *)data + 8, 4); memcpy(&devid, (uint8_t *)data + 12, 4); memcpy(uuid, (uint8_t *)data + 16, VK_UUID_SIZE);
                 if (hl < 32 || ver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || vendor != V.props.vendorID || devid != V.props.deviceID || memcmp(uuid, V.props.pipelineCacheUUID, VK_UUID_SIZE)) { VLOG("pipeline cache %s is another driver's: started empty", g_pc_path); n = 0; } }
    }
    const vka_shlib *L = atomic_load(&g_pc_shlib);
    g_pc_lib = g_pc_path[0] && VKA_ENV("DM_VK_PCACHE_STAMP", 1) ? pck_lib_id(L) : 0;
    memset(&g_pck, 0, sizeof g_pck);
    if (n && g_pc_lib) {   // the stamp: the library the file was saved with and the shaders it holds
        char sp[1100]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path);
        char *sb = NULL; long sl = 0; FILE *sf = fopen(sp, "rb");
        if (sf) { fseek(sf, 0, SEEK_END); sl = ftell(sf); fseek(sf, 0, SEEK_SET); if (sl > 0 && sl < (64l << 20) && (sb = malloc((size_t)sl)) && fread(sb, 1, (size_t)sl, sf) != (size_t)sl) sl = 0; fclose(sf); }
        vka_pck_stamp old; vka_pck_parse(sb ? sb : "", sb && sl > 0 ? (size_t)sl : 0, &old); free(sb);
        int keep = VKA_ENV("DM_VK_PCACHE_KEEP", 50); size_t live, changed; uint64_t dead; uint64_t t0 = now_ns();
        int d = vka_pck_decide(&old, g_pc_lib, keep, pck_hash_of, (void *)L, &g_pck, &live, &changed, &dead);
        double ms = (now_ns() - t0) / 1e6;
        switch (d) {
        case VKA_PCK_CARRIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, %zu of the %zu shaders it lists are byte for byte the same in this one (%zu changed or dropped: "
                 "their pipelines are made again), %llu stale shaders in the file in all, at most %d%% of the kept ones allowed (DM_VK_PCACHE_KEEP; %.0f ms)", g_pc_path, n, live, old.n, changed,
                 (unsigned long long)dead, keep, ms); break;
        case VKA_PCK_UNVERIFIED:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: kept, its stamp names one library but lists no shaders; they are counted from this run on", g_pc_path, n); break;
        case VKA_PCK_STALE:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty, %zu of the %zu shaders it lists are the same in this one, %llu stale (changed or "
                 "dropped by this and earlier updates), over DM_VK_PCACHE_KEEP=%d%% of them; the next save replaces it (%.0f ms)", g_pc_path, n, live, old.n,
                 (unsigned long long)(old.dead + changed), keep, ms); n = 0; break;
        case VKA_PCK_OFF:
            VLOG("pipeline cache %s (%zu bytes) is of another shader library: started empty (DM_VK_PCACHE_KEEP=0; the next save replaces it)", g_pc_path, n); n = 0; break;
        case VKA_PCK_NO_STAMP:
            VLOG("pipeline cache %s (%zu bytes) is not stamped with its shader library: started empty (it may hold every library played on this driver, which would all be loaded; "
                 "the next save replaces it; DM_VK_PCACHE_STAMP=0 reads it)", g_pc_path, n); n = 0; break;
        default: break;
        }
        if (!vka_pck_reads(d)) vka_pck_free(&g_pck);
        vka_pck_free(&old);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = n, .pInitialData = n ? data : NULL};
    VkResult r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc);
    if (r && n) { ci.initialDataSize = 0; ci.pInitialData = NULL; r = vk.vkCreatePipelineCache(V.dev, &ci, NULL, &g_pc); vka_pck_free(&g_pck); n = 0; }   // (started empty after all)
    if (r) { g_pc = VK_NULL_HANDLE; VRED("vkCreatePipelineCache %d: pipelines are built without a cache", r); }
    else if (g_pc_lib) VLOG("pipeline cache %s: %zu bytes read (stamped with shader library %016llx: %zu shaders kept from the file, %llu stale in it)", g_pc_path, n,
                            (unsigned long long)g_pc_lib, g_pck.n, (unsigned long long)g_pck.dead);
    else VLOG("pipeline cache %s: %zu bytes read%s", g_pc_path[0] ? g_pc_path : vka_guest_fp() ? "(none: DM_VK_GUEST_FPCR=1)" : e && !strcmp(e, "0") ? "(none: DM_VK_PCACHE=0)" : "(not kept: no DM_TEMP or HOME)", n,
              !g_pc_path[0] ? "" : L ? " (not stamped with its shader library: DM_VK_PCACHE_STAMP=0)" : " (not stamped: no game shader library known)");
    free(data);
    deadly_load();   // the pipelines that killed this driver in earlier runs: not compiled
    heavy_load();   // the shaders earlier runs found long on this driver (build_end): the background compiles' gate runs them alone
    pw_load();
    pthread_t th;
    if (!pthread_create(&th, NULL, saver_main, NULL)) { pthread_detach(th); g_sv_up = 1; }
    pw_start();
}
void vka_pcache_path(char *out, size_t n) { snprintf(out, n, "%s", g_pc_path); }   // (written once, in pc_open_once)
// (the prewarm threads it starts call it too: they wait for it). sh: the pipeline's shaders, whose library stamps the file (the first game library one is taken)
static void pc_open(vka_shader *const *sh, int n) {
    if (!atomic_load_explicit(&g_pc_shlib, memory_order_relaxed))
        for (int i = 0; i < n; i++) if (sh[i] && sh[i]->lib && sh[i]->lib->mapped) { const vka_shlib *z = NULL; atomic_compare_exchange_strong(&g_pc_shlib, &z, sh[i]->lib); break; }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, pc_open_once);
}
// The cache and the prewarm list to their files. vkGetPipelineCacheData runs without g_mu (the cache is internally synchronized): holding it there kept the replay thread's
// pipeline lookups waiting for a 17 MB copy once a minute
static void save_now(void) {
    uint64_t t = now_ns();
    uint64_t made = __atomic_load_n(&g_made, __ATOMIC_RELAXED);
    size_t n = 0; void *d = NULL;
    if (g_pc && g_pc_path[0] && made != g_saved_at) {
        // The cache grows between the size query and the copy while prewarm and optimizer threads add pipelines: the copy then answers VK_INCOMPLETE with a partial
        // blob. Tried up to 3 times with a quarter more room; only a complete copy (VK_SUCCESS) is written
        VkResult gr = VK_INCOMPLETE;
        for (int tr = 0; tr < 3 && gr == VK_INCOMPLETE; tr++) {
            size_t q = 0; if (vk.vkGetPipelineCacheData(V.dev, g_pc, &q, NULL) || !q) { gr = VK_ERROR_UNKNOWN; break; }
            n = q + q / 4; free(d); d = malloc(n); if (!d) { gr = VK_ERROR_OUT_OF_HOST_MEMORY; break; }
            gr = vk.vkGetPipelineCacheData(V.dev, g_pc, &n, d);
        }
        if (gr != VK_SUCCESS) { free(d); d = NULL; n = 0; static _Atomic int k; if (atomic_fetch_add(&k, 1) < 4) VRED("pipeline cache not saved: vkGetPipelineCacheData %d after 3 tries (pipelines being added meanwhile); the next save tries again", (int)gr); }
        if (d) {
            if (vkc_write_file(g_pc_path, d, n)) {
                g_saved_at = made;
                // Its stamp after it: the shaders it was read with that this library keeps, and those this run made. Written whole or not at all (a stamp that cannot
                // be written is removed, and the next run starts empty)
                if (g_pc_lib) {
                    char sp[1100]; snprintf(sp, sizeof sp, "%s.lib", g_pc_path);
                    size_t nr = 0, tl = 0; vka_pck_ent *run = pck_made(&nr);
                    char *txt = vka_pck_format(g_pc_lib, g_pck.dead, g_pck.ent, g_pck.n, run, nr, &tl); free(run);
                    if (!txt || !vkc_write_file(sp, txt, tl)) unlink(sp);
                    free(txt);
                }
            }
            else { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("pipeline cache not written to %s", g_pc_path); free(d); d = NULL; n = 0; }
        }
    }
    int keys = pw_save();
    uint64_t dt = now_ns() - t; atomic_fetch_add(&g_save_ns_total, dt);
    if (d || keys > 0) VLOG("pipeline cache: %zu bytes written to %s (%llu pipelines made this run), prewarm list %d keys, %.0f ms (on the saver thread)", n, g_pc_path[0] ? g_pc_path : "(none)",
                            (unsigned long long)made, keys, dt / 1e6);
    free(d);
}
static void *saver_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-pcache"); vka_host_fp("vk-pcache");
    for (;;) {
        pthread_mutex_lock(&g_sv_mu); while (!g_sv_want) pthread_cond_wait(&g_sv_cv, &g_sv_mu); g_sv_want = 0; pthread_mutex_unlock(&g_sv_mu);
        pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
    }
    return NULL;
}
// From the replay thread at each submission: a save is due DM_VK_PCACHE_SAVE_S seconds (default 20) after the last one when pipelines were made since; it runs on the saver
// thread. force (the end of the run, the shader library check): now, on the caller's thread
void vka_pso_save(int force) {
    if (!g_pc) return;
    uint64_t t = now_ns();
    if (!force) {
        if (__atomic_load_n(&g_made, __ATOMIC_RELAXED) == g_saved_at) return;
        int s = VKA_ENV("DM_VK_PCACHE_SAVE_S", 20); if (s < 1) s = 1;
        int minimum = VKA_ENV("DM_VK_PCACHE_MIN_S", 120); if (minimum > s) s = minimum;
        if (t - g_save_ns < (uint64_t)s * 1000000000ull) return;
        int quiet = VKA_ENV("DM_VK_PCACHE_QUIET_S", 10);
        if (quiet > 0 && t - atomic_load(&g_last_compile_ns) < (uint64_t)quiet * 1000000000ull && t - g_save_ns < 4ull * (uint64_t)s * 1000000000ull) return;
        g_save_ns = t;
        if (g_sv_up) { pthread_mutex_lock(&g_sv_mu); g_sv_want = 1; pthread_cond_signal(&g_sv_cv); pthread_mutex_unlock(&g_sv_mu); return; }
    }
    g_save_ns = t;
    // force 2 (a compute pipeline that took over a second on the frame path, cs_obtain): saved now but on the saver thread, so the frame that already waited for the compile
    // does not also wait for the cache file's write (tens of MB). The end of a run (force 1) still writes on the caller's thread, since the process may end right after
    if (force == 2 && g_sv_up) { pthread_mutex_lock(&g_sv_mu); g_sv_want = 1; pthread_cond_signal(&g_sv_cv); pthread_mutex_unlock(&g_sv_mu); return; }
    pthread_mutex_lock(&g_save_mu); save_now(); pthread_mutex_unlock(&g_save_mu);
}
void vka_gpl_line(char *out, size_t n);
void vka_pso_async_line(char *out, size_t n);
void mtl_pso_line(char *out, size_t n) {
    if (!n) return;
    int k = snprintf(out, n, "%llu graphics + %llu compute pipelines (%llu failed, %llu compute (program, layout) pairs not made for shared memory, %llu on the frame path), %llu set layouts, %.0f ms creating", (unsigned long long)g_ngfx, (unsigned long long)g_ncs,
                     (unsigned long long)g_nfail, (unsigned long long)g_ncs_over, (unsigned long long)g_frame_made, (unsigned long long)g_nlayouts, g_create_ms);
    if (V.gpl && k > 0 && (size_t)k + 3 < n) { memcpy(out + k, "; ", 3); vka_gpl_line(out + k + 2, n - (size_t)k - 2); }
    size_t o = strlen(out); char a[640]; vka_pso_async_line(a, sizeof a);
    if (a[0] && o + 2 < n) snprintf(out + o, n - o, "; %s", a);
}

// The long-frame log's counters (hle_nvn.c): pipelines made on the frame path and the time it took, cumulative (it takes each frame's difference). Background and prewarm
// pipelines are not the frame's. Shader modules are made with the shader (vka_shader_make), not counted here
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { pthread_mutex_lock(&g_mu); if (lib) *lib = 0; if (pso) *pso = g_frame_made; pthread_mutex_unlock(&g_mu); }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { pthread_mutex_lock(&g_mu); if (lib_ns) *lib_ns = 0; if (pso_ns) *pso_ns = g_create_ns; pthread_mutex_unlock(&g_mu); }
// "draws with a hybrid pipeline" of the [longframe] line: on Android, draws recorded with a fast-linked GPL pipeline (before its optimized one took its place); made = pipelines
// linked. (The Mac's hybrid pipelines are its pulling stand-ins; this stub returned 0 on Android until 2026-11-03, whatever GPL did)
static uint64_t g_nlinked_pub;
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { if (draws) *draws = atomic_load(&g_hy_draws); if (made) *made = __atomic_load_n(&g_nlinked_pub, __ATOMIC_RELAXED); if (none) *none = 0; }
// Pipeline-side attribution for the [longframe] line (vka_extra.c): the saver's time, the frame path's waits for a prewarm thread making the same pipeline
void vka_pso_waits(uint64_t *save_ns, uint64_t *pw_wait_ns) { if (save_ns) *save_ns = atomic_load(&g_save_ns_total); if (pw_wait_ns) *pw_wait_ns = atomic_load(&g_pw_wait_ns); }

// ---------- Graphics pipelines ----------
// The map's value: the pipeline (VK_NULL_HANDLE = failed, its draws skipped), whether it is a fast-linked one, and whether a thread is still making it (busy: the frame path
// waits for that thread instead of making it a second time). p and linked change in place when the optimized pipeline replaces the linked one
typedef struct gval { VkPipeline p; uint8_t linked, busy, pw; pid_t tid; } gval;   // tid: a prewarm/async worker whose priority a waiting draw can raise
typedef struct { vka_shader *sh[VKA_NSTAGE]; VkPipelineLayout layout; vka_gkey k; } gkey_full;   // (sh[5], compute, stays NULL)
static kmap g_gfx = {.klen = sizeof(gkey_full)};
static pthread_cond_t g_busy_cv = PTHREAD_COND_INITIALIZER;
// The create info of a graphics pipeline, built in place (its members point at one another): the whole pipeline and each stage library take their parts from it
typedef struct {
    VkPipelineShaderStageCreateInfo ss[5]; uint32_t ns; VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg[5];
    VkBool32 gl_true; uint32_t afunc; VkSpecializationMapEntry me, ae; VkSpecializationInfo spec, aspec;
    VkVertexInputBindingDescription vb[33]; VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDivisorDescription vd[33];
    VkPipelineVertexInputDivisorStateCreateInfo vds; VkPipelineVertexInputStateCreateInfo vi; VkPipelineInputAssemblyStateCreateInfo ia; VkPipelineTessellationStateCreateInfo ts;
    VkPipelineViewportStateCreateInfo vp; VkPipelineRasterizationStateCreateInfo rs; VkPipelineMultisampleStateCreateInfo ms; VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cb[8]; VkPipelineColorBlendStateCreateInfo bs; VkDynamicState dyn[24]; VkPipelineDynamicStateCreateInfo dy;
    VkFormat cf[8]; VkPipelineRenderingCreateInfo ri; VkGraphicsPipelineCreateInfo ci; int tess, mesh;   // mesh: a task / mesh pipeline (pstate_fill)
    VkDescriptorSetLayout set0;   // the layout's set 0 (the library probe's independent-sets layout)
    uint64_t vs_key, fs_key;      // the pre-rasterization (vertex or mesh) and fragment shaders' keys: the build list's names for the parts (build_begin, the .heavy list)
} pstate;

// A VkFormat's class for colour outputs: 1 UINT, 2 SINT, 0 the rest (float, normalized, scaled)
static int vkf_int_class(uint32_t f) {
    static const uint16_t u[] = {13, 20, 27, 34, 41, 48, 55, 62, 68, 74, 81, 88, 95, 98, 101, 104, 107, 110, 113, 116, 119};
    for (size_t i = 0; i < sizeof u / sizeof *u; i++) { if (f == u[i]) return 1; if (f == u[i] + 1u) return 2; }
    return 0;
}
// VKA_FS_RETYPE: the pixel shader's float outputs (fout_types) that it writes (fout_mask) into UINT / SINT attachments of the key: retyped in its module (vka_shader_fout_module)
static void fout_masks(const vka_shader *fs, const vka_gkey *k, uint32_t *um, uint32_t *sm) {
    *um = *sm = 0;
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        int c = vkf_int_class(k->cfmt[i]);
        if (c && (fs->e->fout_mask >> i & 1) && (fs->e->fout_types >> (2 * i) & 3) == DMSH_VK_TYPE_FLOAT) { if (c == 1) *um |= 1u << i; else *sm |= 1u << i; }
    }
}
// ---------- Pixel shaders with nothing to do ----------
// A draw whose colour writes are all masked (or that has no colour target) and whose pixel shader has no other effect (vka_shader_fs_inert: no discard, no depth, sample
// mask or stencil export, no memory write) gets a pipeline without the pixel shader: depth-only passes, shadow maps and depth pre-passes, which NVN games draw with a
// pixel shader bound and the colour masked. Vulkan then tests and writes depth, and counts occlusion samples, from the rasterized depth alone, as it did with the shader;
// the tiler's binning and fragment work for the shader are saved. Not with alpha to coverage, and not with the alpha test on (a test other than ALWAYS) when its discard
// is the shader's only one. After Erick's Ocarina fork (f2b70e5, a52a663: the idea; the reading here is pf_pass's early-test rule, vka_spvpatch.c), for every game.
// DM_VK_FS_ELIDE=0 (read when a pipeline is made; default 1): the pixel shader always, as before
static _Atomic uint64_t g_fs_elided;
static int fs_elide(vka_shader *fs, const vka_gkey *k) {
    if (!fs || k->a2c || !VKA_ENV("DM_VK_FS_ELIDE", 1)) return 0;
    for (int i = 0; i < k->ncolor && i < 8; i++) if (k->att[i].mask) return 0;
    int kind = vka_shader_fs_inert(fs);
    if (kind != 1 && !(kind == 2 && (!k->alpha_func || k->alpha_func == NVN_ALPHA_FUNC_ALWAYS))) return 0;
    uint64_t n = atomic_fetch_add(&g_fs_elided, 1) + 1;
    if (n == 1 || !(n & (n - 1))) VLOG("pixel shaders left out: %llu pipelines whose colour writes are masked and whose pixel shader does nothing else (DM_VK_FS_ELIDE=0: kept)", (unsigned long long)n);
    return 1;
}
// 0 = a stage's module is missing (logged by vka_shader_module)
static int pstate_fill(pstate *P, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    memset(P, 0, sizeof *P);
    // Stages: vertex, tess control, tess evaluation, geometry, fragment; the GL depth constant on the last of the first four. A mesh pipeline (NVN2 task and mesh programs,
    // nvnCommandBufferDrawMeshTasks): task (optional), mesh, fragment, the GL depth constant on the mesh stage; it has no vertex input or input assembly state
    static const int ovtx[5] = {0, 3, 4, 2, 1}, omesh[5] = {6, 7, 1, -1, -1};
    const int mesh = sh[7] != NULL, *order = mesh ? omesh : ovtx, nord = mesh ? 3 : 5;
    int last_pre = -1;
    for (int i = 0; i < nord - 1; i++) if (sh[order[i]]) last_pre = order[i];
    P->gl_true = VK_TRUE; P->me = (VkSpecializationMapEntry){DMSH_VK_SPEC_GL_DEPTH, 0, sizeof(VkBool32)}; P->spec = (VkSpecializationInfo){1, &P->me, sizeof P->gl_true, &P->gl_true};
    P->afunc = k->alpha_func; P->ae = (VkSpecializationMapEntry){DMSH_VK_SPEC_ALPHA_FUNC, 0, sizeof(uint32_t)};   // the alpha test's function on an F_ALPHA_TEST pixel shader
    P->aspec = (VkSpecializationInfo){1, &P->ae, sizeof P->afunc, &P->afunc};
    int no_fs = fs_elide(sh[1], k);
    for (int i = 0; i < nord; i++) {
        vka_shader *s = sh[order[i]]; if (!s || (no_fs && order[i] == 1)) continue;
        if (s->e->stage != vka_vk_stage(order[i]) || (mesh && !V.mesh)) {
            VRED("graphics pipeline slot %d has shader stage %u (key %016llx); refusing an invalid Vulkan pipeline", order[i], s->e->stage, (unsigned long long)s->key);
            return 0;
        }
        int fs = order[i] == 1, gl = k->gl_depth && order[i] == last_pre && (s->e->flags & DMSH_VK_F_GL_DEPTH);
        uint32_t fu = 0, fsm = 0; if (fs && k->fs_variant == VKA_FS_RETYPE) fout_masks(s, k, &fu, &fsm);
        VkShaderModule mod = order[i] == 0 && (k->vin_u | k->vin_s) ? vka_shader_vin_module(s, k->vin_u, k->vin_s) : order[i] == 2 && k->gs_fx ? vka_shader_gs_module(s)
                           : fs && k->fs_variant == VKA_FS_RETYPE ? (fu | fsm ? vka_shader_fout_module(s, fu, fsm) : vka_shader_module(s, DMSH_VK_MOD_BASE))
                           : vka_shader_module(s, fs ? k->fs_variant : DMSH_VK_MOD_BASE);
        if (!mod && order[i] == 2 && k->gs_fx) mod = vka_shader_module(s, DMSH_VK_MOD_BASE);   // no module with the claim (logged): the shader as it is
        if (!mod) return 0;
        // A pixel module that reports its pool reads was given early depth tests where they give the late ones' result; alpha to coverage, or its alpha test on, takes the
        // twin without them (vka_shader.c "Texture pool reads")
        if (fs) mod = vka_shader_pfb_pick(mod, k->a2c, k->alpha_func);
        uint32_t n = P->ns++;
        P->ss[n] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = (VkShaderStageFlagBits)stage_bit(s->e->stage), .module = mod, .pName = "main",
                                                    .pSpecializationInfo = gl ? &P->spec : fs && P->afunc && (s->e->flags & DMSH_VK_F_ALPHA_TEST) ? &P->aspec : NULL};
        uint32_t pin = (s->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(s->e->stage) : 0;   // subgroup shaders of a stage whose size the device varies: pinned to the library's (vka_shader_sg_pin)
        if (pin) { P->rsg[n] = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
                   P->ss[n].pNext = &P->rsg[n]; }
    }
    uint32_t nvb = 0, nva = 0, nvd = 0;
    if (k->vin_static) {
        for (uint32_t i = 0; i < 32; i++) if (k->va[i].used) P->va[nva++] = (VkVertexInputAttributeDescription){i, k->va[i].binding, (VkFormat)k->va[i].fmt, k->va[i].offset};
        for (uint32_t i = 0; i < 33; i++) if (k->vb[i].used) {
            P->vb[nvb++] = (VkVertexInputBindingDescription){i, k->vb[i].stride, k->vb[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (k->vb[i].divisor > 1) P->vd[nvd++] = (VkVertexInputBindingDivisorDescription){i, k->vb[i].divisor};
        }
    }
    P->vds = (VkPipelineVertexInputDivisorStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = P->vd};
    P->vi = (VkPipelineVertexInputStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &P->vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = P->vb,
                                                   .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = P->va};
    P->ia = (VkPipelineInputAssemblyStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = (VkPrimitiveTopology)k->topo};
    P->ts = (VkPipelineTessellationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .patchControlPoints = 3};
    P->vp = (VkPipelineViewportStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};   // one viewport, as nvn_mtl.m draws
    P->rs = (VkPipelineRasterizationStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp && V.depth_clamp,
        .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    P->ms = (VkPipelineMultisampleStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1), .alphaToCoverageEnable = k->a2c};
    P->ds = (VkPipelineDepthStencilStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .maxDepthBounds = 1.0f};
    for (int i = 0; i < k->ncolor; i++)
        P->cb[i] = (VkPipelineColorBlendAttachmentState){k->att[i].en, (VkBlendFactor)k->att[i].src, (VkBlendFactor)k->att[i].dst, (VkBlendOp)k->att[i].op,
                                                          (VkBlendFactor)k->att[i].srca, (VkBlendFactor)k->att[i].dsta, (VkBlendOp)k->att[i].opa, k->att[i].mask};
    P->bs = (VkPipelineColorBlendStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = k->logic_en, .logicOp = (VkLogicOp)k->logic_op, .attachmentCount = k->ncolor, .pAttachments = P->cb};
    static const VkDynamicState base[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_LINE_WIDTH};
    uint32_t nd = 0;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)   // (a mesh pipeline may not name the primitive topology or restart: it has no input assembly)
        if (!mesh || (base[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && base[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)) P->dyn[nd++] = base[i];
    if (!k->vin_static && !mesh) P->dyn[nd++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
    if (V.depth_bounds) P->dyn[nd++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS;   // NVN's SetDepthBounds, set at the draw (vka_draw.c); the pipeline's own (0, 1) otherwise (a fragment shader library's: dyn_part)
    P->dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = nd, .pDynamicStates = P->dyn};
    for (int i = 0; i < k->ncolor; i++) P->cf[i] = (VkFormat)k->cfmt[i];
    P->ri = (VkPipelineRenderingCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = P->cf,
                                            .depthAttachmentFormat = (VkFormat)k->dfmt, .stencilAttachmentFormat = (VkFormat)k->sfmt};
    P->tess = !mesh && (sh[3] || sh[4]); P->mesh = mesh;
    P->ci = (VkGraphicsPipelineCreateInfo){VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &P->ri, .stageCount = P->ns, .pStages = P->ss, .pVertexInputState = mesh ? NULL : &P->vi,
        .pInputAssemblyState = mesh ? NULL : &P->ia,
        .pTessellationState = P->tess ? &P->ts : NULL, .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pColorBlendState = &P->bs,
        .pDynamicState = &P->dy, .layout = lay->layout};
    P->set0 = lay->set0;
    P->vs_key = sh[mesh ? 7 : 0] ? sh[mesh ? 7 : 0]->key : 0; P->fs_key = sh[1] ? sh[1]->key : 0;
    for (int i = 0; i < VKA_NSTAGE; i++) pck_note(sh[i]);   // (the cache stamp's list: a shader whose pipeline the cache may now hold)
    return 1;
}
static const char *stage_name(VkShaderStageFlagBits b) { return b == VK_SHADER_STAGE_VERTEX_BIT ? "vertex" : b == VK_SHADER_STAGE_FRAGMENT_BIT ? "fragment" : b == VK_SHADER_STAGE_GEOMETRY_BIT ? "geometry"
                                                              : b == VK_SHADER_STAGE_TASK_BIT_EXT ? "task" : b == VK_SHADER_STAGE_MESH_BIT_EXT ? "mesh" : "tessellation"; }
// Pipelines being made now (whole pipelines, any thread): the watchdog's dump names them (vka_pso_wait_dump), so a stop inside the driver's compiler says which shaders
// (a frame-pointer backtrace ends in the stripped driver). Slots are claimed with a compare-and-swap; a build that finds none full is simply not listed
enum { NBUILD = 16 };
static struct { _Atomic pid_t tid; _Atomic uint64_t t0, vs, fs; const char *_Atomic what; _Atomic int spec; } g_build[NBUILD];
// The calling thread makes speculative pipelines (vk-psopt, vk-prewarm: no draw waits for them); its builds are marked so (spec), for the frame jobs' hold (gate_hold)
static _Thread_local int t_spec;
static void vka_pso_wait_dump(void) {
    uint64_t t = now_ns(); int n = 0;
    for (int i = 0; i < NBUILD; i++) { pid_t id = atomic_load(&g_build[i].tid); if (!id) continue; n++;
        fprintf(stderr, "[dm] [vk] %s being made by thread %d for %.1f s: shaders %016llx %016llx\n", atomic_load(&g_build[i].what), (int)id, (t - atomic_load(&g_build[i].t0)) / 1e9,
                (unsigned long long)atomic_load(&g_build[i].vs), (unsigned long long)atomic_load(&g_build[i].fs)); }
    if (!n) fprintf(stderr, "[dm] [vk] no pipeline being made\n");
}
void crash_set_wait_dump(void (*fn)(void));
void crash_set_fatal_hook(void (*fn)(int sig));
// Pipelines that killed the process on this driver (<cache file>.deadly, beside the .heavy list): found 2026-11-12 on Ocarina of Time with Turnip V39 and V39-patched on the
// Adreno 830, which both died inside libvulkan_freedreno compiling mesh shader 73dc0b90c4082e43 (V39: its compute fallback, 8.7 s; V39-patched: the native mesh pipeline,
// 6.5 s), with the process's memory maps at 100 % of vm.max_map_count, on every run from the same place, so the game never got past it. The crash handler (crash.c,
// crash_set_fatal_hook, on the crashing thread) writes the build that thread was in, whatever its length, and each other build that had been running DM_VK_DEADLY_MS
// (default 2000) or more when the process died (vka_deadly.h: which record each leaves, how records match); later runs on that driver, host build and shader library do
// not compile it again: its draws or dispatches are skipped with a [RED] line, a missing object instead of a dead game. The crashing thread's build is recorded at any
// length since DroidPlay Core 1ae945d2 (MIT; a Retroid Pocket G2, Adreno 735, Qualcomm V@0762, crashed compiling Ocarina of Time's first compute pipeline
// a039399e6336de1b at every start, a short build the 2 s rule never kept). Nothing is written while builds run (the slots in g_build are what build_begin keeps anyway):
// the crash handler writes, so a build costs nothing more. This is a workaround for a driver bug (the cause is inside the driver's compiler, which we cannot fix); the
// file is stamped with the shader library, the driver (vendor, device, version, pipeline cache UUID) and the host build (DM_BUILD_COMMIT), so an update of any of them
// tries its pipelines again, and DM_VK_DEADLY=0 ignores it (an A/B). DM_VK_PIPE_CRASH=<16-hex key> crashes the builds of that shader on purpose (abort inside the build),
// to try the guard on a phone whose driver compiles everything
enum { NDEADLY = 256 };
static vkdl_rec g_deadly[NDEADLY]; static uint64_t g_deadly_min_ns = 2000000000ull, g_deadly_stamp; static _Atomic int g_deadly_n; static char g_deadly_path[1100];
static uint64_t g_pipe_crash;   // DM_VK_PIPE_CRASH (build_reg)
static int deadly_has(uint64_t pre, uint64_t fs) { return vkdl_match(g_deadly, atomic_load(&g_deadly_n), pre, fs); }
static int deadly_has_rec(vkdl_rec r) { int n = atomic_load(&g_deadly_n); for (int i = 0; i < n; i++) if (g_deadly[i].pre == r.pre && g_deadly[i].fs == r.fs) return 1; return 0; }
static void deadly_note(int sig) {   // signal handler context (crash.c): no locks, no allocation, open / write only
    (void)sig;
    if (!g_deadly_path[0]) return;
    uint64_t t = now_ns(), min = g_deadly_min_ns; pid_t me = gettid();
    int fd = -1;
    for (int i = 0; i < NBUILD; i++) {
        pid_t id = atomic_load(&g_build[i].tid); if (id <= 0) continue;
        uint64_t t0 = atomic_load(&g_build[i].t0); vkdl_rec r;
        if (!vkdl_choose(id == me, t > t0 ? t - t0 : 0, min, atomic_load(&g_build[i].vs), atomic_load(&g_build[i].fs), &r) || deadly_has_rec(r)) continue;
        if (fd < 0 && (fd = open(g_deadly_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)) < 0) return;
        char b[34]; vkdl_line(&r, b);
        if (write(fd, b, sizeof b) < 0) break;
        static const char ms[] = "[dm] [vk] [RED] the process crashed in this thread's pipeline build: its shaders are left out on this driver from the next run (DM_VK_DEADLY)\n";
        static const char ml[] = "[dm] [vk] [RED] the process died while the driver had been compiling this pipeline for long: its shader is left out on this driver from the next run (DM_VK_DEADLY)\n";
        if (id == me ? write(2, ms, sizeof ms - 1) : write(2, ml, sizeof ml - 1)) {}
    }
    if (fd >= 0) close(fd);
}
static void deadly_load(void) {   // (pc_open_once, after the cache stamp: g_pc_lib)
    if (!g_pc_path[0]) return;
    g_deadly_min_ns = (uint64_t)VKA_ENV("DM_VK_DEADLY_MS", 2000) * 1000000ull;
    const char *host = getenv("DM_BUILD_COMMIT");
    g_deadly_stamp = vkdl_stamp(g_pc_lib, V.props.vendorID, V.props.deviceID, V.props.driverVersion, V.props.pipelineCacheUUID, host);
    snprintf(g_deadly_path, sizeof g_deadly_path, "%s.deadly", g_pc_path);
    FILE *f = fopen(g_deadly_path, "r");
    char ln[256]; uint64_t st = 0; int stamped = 0, n = 0;
    if (f && fgets(ln, sizeof ln, f) && !strncmp(ln, "stamp ", 6)) { st = strtoull(ln + 6, NULL, 16); stamped = 1; }
    if (f && (!stamped || st != g_deadly_stamp)) {   // (before 2026-11-17 the stamp was the library's alone, "lib <id>", with one key a line: such a file is dropped too)
        fclose(f); f = NULL; unlink(g_deadly_path);
        VLOG("pipelines that killed the driver before: %s was made under %s %016llx, now %016llx (shader library %016llx, driver %08x, host build %s): removed, they are compiled again",
             g_deadly_path, stamped ? "stamp" : "an older format,", (unsigned long long)st, (unsigned long long)g_deadly_stamp, (unsigned long long)g_pc_lib, V.props.driverVersion, host && *host ? host : "(unknown)");
    }
    if (f) { vkdl_rec r; while (fgets(ln, sizeof ln, f)) if (vkdl_parse(ln, &r) && n < NDEADLY && !deadly_has_rec(r)) { g_deadly[n++] = r; atomic_store(&g_deadly_n, n); } fclose(f); }
    else {   // a new file starts with the stamp, and a line naming what it is made of
        FILE *w = fopen(g_deadly_path, "w");
        if (w) { fprintf(w, "stamp %016llx\n# shader library %016llx, driver %04x:%04x version %08x, host build %s\n", (unsigned long long)g_deadly_stamp, (unsigned long long)g_pc_lib,
                         V.props.vendorID, V.props.deviceID, V.props.driverVersion, host && *host ? host : "(unknown)"); fclose(w); }
    }
    bb_open();   // (the run before, killed from outside while it compiled: its long builds join the list; then this run's own record)
    if (!VKA_ENV("DM_VK_DEADLY", 1)) { if (n) VLOG("pipelines that killed the driver before: %d in %s, compiled anyway (DM_VK_DEADLY=0)", n, g_deadly_path); atomic_store(&g_deadly_n, 0); return; }
    for (int i = 0; i < n; i++) VRED("shaders %016llx %016llx (0: any) crashed or killed this driver while their pipeline was compiled in an earlier run (%s): not compiled, their draws and dispatches are skipped (DM_VK_DEADLY=0 tries them)",
                                     (unsigned long long)g_deadly[i].pre, (unsigned long long)g_deadly[i].fs, g_deadly_path);
}
// ---------- Builds in flight, kept where a kill from outside leaves them (<cache file>.building) ----------
// deadly_note runs in the crash handler, so it sees a death the process can catch (a fault inside the driver's compiler). A SIGKILL runs no handler: Android's low-memory
// killer, or the kernel's, ending the game while a driver's compiler takes gigabytes for one shader leaves no trace, the log just stops (two runs of Ocarina of Time on a
// Redmagic 10 Pro, Adreno 830 on 891.14, 2026-11-13: killed by signal 9 at 170-184 s, as the background compiles reached the task/mesh fallback's programs, with 5 GiB
// available ten seconds before), and every start dies at the same place. So the builds in flight are also kept in a small file mapped MAP_SHARED, whose pages the
// kernel keeps when the process is killed: each slot's shader keys and start time, and a heartbeat every 500 ms while any build runs, with MemAvailable and this
// process's resident size. The next start reads it (bb_open, from deadly_load) and says in the log what was being made and what the memory was; when the launcher
// reports that run as killed by a signal from outside (NX_PREV_EXIT: ApplicationExitInfo's reason 2 with status 9, or reason 3, low memory) at the heartbeat's time,
// every build that had been running DM_VK_DEADLY_MS or more goes onto the .deadly list, as deadly_note's do. A run the player stopped, or one whose end is not known,
// puts nothing on the list (a long compile under the player's own swipe is not the driver's fault). DM_VK_BUILDING=0: no file
enum { BB_MAGIC = 0x62626c64 };
typedef struct { uint64_t magic, stamp, now_ns, wall_ms, avail_kb, rss_kb, low_avail_kb, ticks; struct { uint64_t vs, fs, t0; } b[NBUILD]; } bb_file;
static bb_file *g_bb; static pthread_mutex_t g_bb_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_bb_cv = PTHREAD_COND_INITIALIZER;
static uint64_t bb_wall_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }
static void bb_mem(uint64_t *avail_kb, uint64_t *rss_kb) {
    char b[2048]; int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC); *avail_kb = 0; *rss_kb = 0;
    if (fd >= 0) { ssize_t n = read(fd, b, sizeof b - 1); close(fd); if (n > 0) { b[n] = 0; const char *p = strstr(b, "MemAvailable:"); if (p) *avail_kb = strtoull(p + 13, NULL, 10); } }
    fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ssize_t n = read(fd, b, 127); close(fd); if (n > 0) { b[n] = 0; char *e; strtoull(b, &e, 10); *rss_kb = strtoull(e, NULL, 10) * (uint64_t)(sysconf(_SC_PAGESIZE) / 1024); } }
}
static int bb_busy(void) { for (int i = 0; i < NBUILD; i++) if (atomic_load(&g_build[i].tid) > 0) return 1; return 0; }
static void bb_tick(void) {
    bb_file *f = g_bb; if (!f) return;
    uint64_t a, r; bb_mem(&a, &r);
    f->avail_kb = a; f->rss_kb = r; if (a && (!f->low_avail_kb || a < f->low_avail_kb)) f->low_avail_kb = a;
    f->wall_ms = bb_wall_ms(); f->ticks++; f->now_ns = now_ns();
}
static void *bb_main(void *p) {
    (void)p; pthread_setname_np(pthread_self(), "vk-building");
    for (;;) {
        pthread_mutex_lock(&g_bb_mu); while (!bb_busy()) pthread_cond_wait(&g_bb_cv, &g_bb_mu); pthread_mutex_unlock(&g_bb_mu);   // (asleep while nothing compiles: no wakeups in a running level)
        bb_tick();
        struct timespec ts = {0, 500000000}; nanosleep(&ts, NULL);
    }
    return NULL;
}
static void bb_begin(int i, uint64_t vs, uint64_t fs, uint64_t t0) {
    bb_file *f = g_bb; if (!f || i < 0) return;
    f->b[i].fs = fs; f->b[i].t0 = t0; f->b[i].vs = vs; f->now_ns = t0;
    pthread_mutex_lock(&g_bb_mu); pthread_cond_signal(&g_bb_cv); pthread_mutex_unlock(&g_bb_mu);
}
static void bb_end(int i) { bb_file *f = g_bb; if (f && i >= 0) { f->b[i].vs = 0; f->b[i].fs = 0; f->now_ns = now_ns(); } }
static void deadly_add_file(vkdl_rec r) {   // (deadly_load's thread: the list is not read by others yet)
    int n = atomic_load(&g_deadly_n); if ((!r.pre && !r.fs) || deadly_has_rec(r) || n >= NDEADLY) return;
    g_deadly[n] = r; atomic_store(&g_deadly_n, n + 1);
    FILE *w = fopen(g_deadly_path, "a"); if (w) { char b[34]; vkdl_line(&r, b); fwrite(b, 1, sizeof b, w); fclose(w); }
}
// The file of the run before (read, judged, logged), then this run's: called once, from deadly_load, with the cache's path and stamp known
static void bb_open(void) {
    if (!g_pc_path[0] || !VKA_ENV("DM_VK_BUILDING", 1)) return;
    // In the app's own files folder (where the launcher keeps the run's exit record), under the cache file's name: the cache itself is in shared storage, a FUSE mount,
    // and a shared writable map is surest on the app's own file system. Beside the cache when the launcher gave no such folder
    char path[2300]; const char *xr = getenv("NX_EXIT_RECORD"), *sl = xr ? strrchr(xr, '/') : NULL, *base = strrchr(g_pc_path, '/');
    if (sl && base) snprintf(path, sizeof path, "%.*s%s.building", (int)(sl - xr), xr, base);
    else snprintf(path, sizeof path, "%s.building", g_pc_path);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644); if (fd < 0) return;
    bb_file old; ssize_t got = read(fd, &old, sizeof old);
    if (got == (ssize_t)sizeof old && old.magic == BB_MAGIC && old.stamp == g_deadly_stamp) {
        int nb = 0, nlong = 0; char list[600] = ""; size_t ln = 0;
        for (int i = 0; i < NBUILD; i++) {
            uint64_t k = old.b[i].vs ? old.b[i].vs : old.b[i].fs; if (!k) continue;
            double s = old.now_ns > old.b[i].t0 ? (old.now_ns - old.b[i].t0) / 1e9 : 0; nb++;
            if (old.now_ns > old.b[i].t0 && old.now_ns - old.b[i].t0 >= g_deadly_min_ns) nlong++;
            if (ln < sizeof list - 40) ln += (size_t)snprintf(list + ln, sizeof list - ln, "%s%016llx for %.1f s", nb > 1 ? ", " : "", (unsigned long long)k, s);
        }
        if (nb) {
            // How that run ended, by the launcher (GameActivity: "reason,status,time in ms" of the newest earlier game process); the heartbeat must be that run's last seconds
            const char *pe = getenv("NX_PREV_EXIT"); int reason = -1, status = -1; long long ts = 0;
            if (pe) sscanf(pe, "%d,%d,%lld", &reason, &status, &ts);
            long long gap = ts && old.wall_ms ? ts - (long long)old.wall_ms : 0; if (gap < 0) gap = -gap;
            int same = ts && old.wall_ms && gap <= 30000, killed = same && ((reason == 2 && status == 9) || reason == 3);
            VLOG("the run before ended with %d pipeline%s being made on this driver: shader%s %s; memory at its last sample: %llu MiB available (lowest while compiling %llu MiB), this process %llu MiB resident; "
                 "that run's end by the launcher: %s", nb, nb > 1 ? "s" : "", nb > 1 ? "s" : "", list, (unsigned long long)(old.avail_kb >> 10), (unsigned long long)(old.low_avail_kb >> 10),
                 (unsigned long long)(old.rss_kb >> 10), !pe || reason < 0 ? "not known" : !same ? "another run's (times apart)" : killed ? "killed by a signal from outside" : "not a kill from outside");
            if (killed && nlong)
                for (int i = 0; i < NBUILD; i++) {
                    uint64_t k = old.b[i].vs ? old.b[i].vs : old.b[i].fs; vkdl_rec r;
                    if (old.now_ns <= old.b[i].t0 || !vkdl_choose(0, old.now_ns - old.b[i].t0, g_deadly_min_ns, old.b[i].vs, old.b[i].fs, &r) || deadly_has_rec(r)) continue;
                    deadly_add_file(r);
                    VRED("shader %016llx: the process was killed from outside while the driver had been compiling its pipeline for %.1f s (%llu MiB available at the last sample): left out on this driver from this run on (%s; DM_VK_DEADLY=0 tries it)",
                         (unsigned long long)k, (old.now_ns - old.b[i].t0) / 1e9, (unsigned long long)(old.avail_kb >> 10), g_deadly_path);
                }
        }
    }
    bb_file z = {.magic = BB_MAGIC, .stamp = g_deadly_stamp};
    if (ftruncate(fd, 0) || pwrite(fd, &z, sizeof z, 0) != (ssize_t)sizeof z) { close(fd); return; }
    void *m = mmap(NULL, sizeof z, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd);
    if (m == MAP_FAILED) return;
    g_bb = m;
    pthread_t th; if (!pthread_create(&th, NULL, bb_main, NULL)) pthread_detach(th);
}
static void build_reg(void) {
    crash_set_wait_dump(vka_pso_wait_dump); crash_set_fatal_hook(deadly_note);
    const char *pc = getenv("DM_VK_PIPE_CRASH"); g_pipe_crash = pc && *pc ? strtoull(pc, NULL, 16) : 0;
    if (g_pipe_crash) VRED("DM_VK_PIPE_CRASH=%016llx: a build of that shader's pipelines crashes the process on purpose (a test of the .deadly list)", (unsigned long long)g_pipe_crash);
}
// A pipeline or part whose shaders are on the list: not handed to the driver (logged the first 16 times)
static int deadly_skip(const char *what, uint64_t pre, uint64_t fs) {
    if (!atomic_load(&g_deadly_n) || !deadly_has(pre, fs)) return 0;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("%s of shaders %016llx %016llx not compiled: its build crashed or killed this driver in an earlier run (%s); its work is skipped",
                                                                 what, (unsigned long long)pre, (unsigned long long)fs, g_deadly_path);
    return 1;
}
static int build_begin(const char *what, uint64_t vs, uint64_t fs) {
    static pthread_once_t o = PTHREAD_ONCE_INIT; pthread_once(&o, build_reg);
    for (int i = 0; i < NBUILD; i++) { pid_t z = 0; if (atomic_compare_exchange_strong(&g_build[i].tid, &z, -1)) {
        uint64_t t0 = now_ns();
        atomic_store(&g_build[i].what, what); atomic_store(&g_build[i].vs, vs); atomic_store(&g_build[i].fs, fs); atomic_store(&g_build[i].spec, t_spec); atomic_store(&g_build[i].t0, t0);
        atomic_store(&g_build[i].tid, gettid()); bb_begin(i, vs, fs, t0);
        if (g_pipe_crash && (vs == g_pipe_crash || fs == g_pipe_crash)) { VRED("%s of shaders %016llx %016llx: crashing on purpose (DM_VK_PIPE_CRASH)", what, (unsigned long long)vs, (unsigned long long)fs); abort(); }
        return i; } }
    return -1;
}
// Shaders (pre-rasterization, compute or, for a fragment library part, fragment keys) whose build took DM_VK_PREWARM_HEAVY_MS (default 1000; 0 = none remembered) or
// more on this driver, kept beside its pipeline cache (<cache file>.heavy, a hex key a line, appended as they are found; from upstream Banana Native v0.3.1 / v0.3.11).
// Turnip compiles some of them again every run, cache hit or not (upstream, Ocarina of Time: vertex b1f06157308e3b22 8.6-18 s in each of four runs), and long ones that
// overlap take the process's memory maps (three at once took 58,619 of 65,530). The background compiles' gate (gate_enter) runs a known one alone, from its start
enum { NHEAVY = 1024 };
static uint64_t g_heavy[NHEAVY]; static _Atomic int g_heavy_n; static char g_heavy_path[1100]; static pthread_mutex_t g_heavy_mu = PTHREAD_MUTEX_INITIALIZER;
static int heavy_has(uint64_t k) { int n = atomic_load(&g_heavy_n); for (int i = 0; i < n; i++) if (g_heavy[i] == k) return 1; return 0; }
static void heavy_add(uint64_t k, int save) {
    pthread_mutex_lock(&g_heavy_mu);
    int n = atomic_load(&g_heavy_n);
    if (k && n < NHEAVY && !heavy_has(k)) {
        g_heavy[n] = k; atomic_store(&g_heavy_n, n + 1);
        if (!g_heavy_path[0] && g_pc_path[0]) snprintf(g_heavy_path, sizeof g_heavy_path, "%s.heavy", g_pc_path);
        FILE *f = save && g_heavy_path[0] ? fopen(g_heavy_path, "a") : NULL;
        if (f) { fprintf(f, "%016llx\n", (unsigned long long)k); fclose(f); }
    }
    pthread_mutex_unlock(&g_heavy_mu);
}
static void heavy_load(void) {
    if (!g_pc_path[0]) return;
    pthread_mutex_lock(&g_heavy_mu); snprintf(g_heavy_path, sizeof g_heavy_path, "%s.heavy", g_pc_path); pthread_mutex_unlock(&g_heavy_mu);
    FILE *f = fopen(g_heavy_path, "r"); if (!f) return;
    char ln[64]; while (fgets(ln, sizeof ln, f)) { uint64_t k = strtoull(ln, NULL, 16); if (k) heavy_add(k, 0); }
    fclose(f);
    VLOG("long compiles: %d shaders of earlier runs on this driver (%s): the background compiles' gate runs each alone (DM_VK_PREWARM_HEAVY_MS)", atomic_load(&g_heavy_n), g_heavy_path);
}
// DM_VK_PIPE_STATS (vka_dev.c V.pexec): the driver's statistics of a pipeline it made, one line per executable (VK_KHR_pipeline_executable_properties). For finding what
// limits a program on the GPU: its registers (and so how many waves fit), its instruction count, its spills
static void pipe_stats(VkPipeline p, const char *what, uint64_t key, uint32_t lx, uint64_t shared) {
    static PFN_vkGetPipelineExecutablePropertiesKHR props; static PFN_vkGetPipelineExecutableStatisticsKHR stats;
    if (!props) { props = (PFN_vkGetPipelineExecutablePropertiesKHR)vk.vkGetDeviceProcAddr(V.dev, "vkGetPipelineExecutablePropertiesKHR");
                  stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vk.vkGetDeviceProcAddr(V.dev, "vkGetPipelineExecutableStatisticsKHR"); }
    if (!props || !stats || !p) return;
    VkPipelineInfoKHR pi = {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = p};
    VkPipelineExecutablePropertiesKHR ep[4]; uint32_t ne = 4;
    for (uint32_t i = 0; i < 4; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    if (props(V.dev, &pi, &ne, ep) < 0) return;
    for (uint32_t e = 0; e < ne; e++) {
        VkPipelineExecutableInfoKHR ei = {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = p, .executableIndex = e};
        VkPipelineExecutableStatisticKHR st[48]; uint32_t ns = 48;
        for (uint32_t i = 0; i < 48; i++) st[i] = (VkPipelineExecutableStatisticKHR){VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        if (stats(V.dev, &ei, &ns, st) < 0) continue;
        char b[1600]; size_t o = 0; b[0] = 0;
        for (uint32_t i = 0; i < ns && o < sizeof b - 64; i++) {
            const VkPipelineExecutableStatisticKHR *t = &st[i];
            if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR) o += (size_t)snprintf(b + o, sizeof b - o, "; %s %u", t->name, t->value.b32);
            else if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) o += (size_t)snprintf(b + o, sizeof b - o, "; %s %lld", t->name, (long long)t->value.i64);
            else if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) o += (size_t)snprintf(b + o, sizeof b - o, "; %s %llu", t->name, (unsigned long long)t->value.u64);
            else if (t->format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) o += (size_t)snprintf(b + o, sizeof b - o, "; %s %.3g", t->name, t->value.f64);
        }
        VLOG("pipeline stats: %s %016llx (local size %u, shared %llu bytes) executable %u %s, subgroup %u%s", what, (unsigned long long)key, lx, (unsigned long long)shared, e, ep[e].name, ep[e].subgroupSize, b);
    }
}
static void build_end(int i) {
    atomic_store(&g_last_compile_ns, now_ns());
    if (i < 0) return;
    uint64_t vs = atomic_load(&g_build[i].vs), fs = atomic_load(&g_build[i].fs), age = now_ns() - atomic_load(&g_build[i].t0);
    const char *what = atomic_load(&g_build[i].what);
    atomic_store(&g_build[i].tid, 0); bb_end(i);
    int ms = VKA_ENV("DM_VK_PREWARM_HEAVY_MS", 1000);
    uint64_t k = vs ? vs : fs;
    if (k && ms > 0 && age >= (uint64_t)ms * 1000000ull && !heavy_has(k)) {
        heavy_add(k, 1);
        VLOG("long compile: %s of shader %016llx took %.1f s: remembered (%s), background compiles of it run alone", what, (unsigned long long)k, age / 1e9, g_heavy_path[0] ? g_heavy_path : "this run only");
    }
}
// The whole pipeline (optimized), with creation feedback (core in 1.3) for the line of a slow one: whether the pipeline cache had it and each stage's share. how: NULL = on the
// frame path (counted as the frame's), else where it was made (" in the background", " (prewarm)")
static VkPipeline full_pipeline(pstate *P, vka_shader *const sh[VKA_NSTAGE], const char *how) {
    VkPipelineCreationFeedback fb_all = {0}, fb_st[5]; memset(fb_st, 0, sizeof fb_st);
    VkPipelineCreationFeedbackCreateInfo fbi = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, .pNext = P->ci.pNext, .pPipelineCreationFeedback = &fb_all,
                                                .pipelineStageCreationFeedbackCount = P->ns, .pPipelineStageCreationFeedbacks = fb_st};
    P->ci.pNext = &fbi;
    uint64_t t0 = now_ns();
    VkPipeline p = VK_NULL_HANDLE;
    const vka_shader *pre0 = P->mesh ? sh[7] : sh[0];
    VkResult r;
    VkPipelineCreateFlags flags0 = P->ci.flags;
    // DM_VK_PIPE_STATS: the graphics pipelines' statistics too (each stage's registers and occupancy); DM_VK_PIPE_STATS_KEYS=<key>,<key>... only those with these fragment shaders (and the compute pipelines of those keys)
    // (a pipeline asked for statistics is compiled again, not taken from the cache: the list keeps a run from recompiling every pipeline)
    int stats = V.pexec;
    if (stats) { const char *only = getenv("DM_VK_PIPE_STATS_KEYS"); char hex[20];
                 if (only && *only) { snprintf(hex, sizeof hex, "%016llx", sh[1] ? (unsigned long long)sh[1]->key : 0ull); stats = strstr(only, hex) != NULL; } }
    if (stats) P->ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    // DM_VK_VRS_TEST=<w>x<h> with DM_VK_VRS_KEYS=<fragment key>+<key>... (diagnostic): those fragment shaders' whole pipelines shade at that pipeline rate (vka_dev.c)
    VkPipelineFragmentShadingRateStateCreateInfoKHR fsr = {VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, .fragmentSize = {1, 1},
        .combinerOps = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR}};
    int vrs = 0;
    if (V.fsr_pipe && sh[1]) { const char *keys = getenv("DM_VK_VRS_KEYS"), *t = getenv("DM_VK_VRS_TEST"); char hex[20]; unsigned w = 1, h = 1;
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)sh[1]->key);
        if (t && sscanf(t, "%ux%u", &w, &h) == 2 && keys && strstr(keys, hex) && w >= 1 && w <= 4 && h >= 1 && h <= 4) {
            fsr.fragmentSize = (VkExtent2D){w, h}; fsr.pNext = P->ci.pNext; P->ci.pNext = &fsr; vrs = 1;
            static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VLOG("fragment shading rate test: fragment %s at %ux%u", hex, w, h); } }
    if (deadly_skip("whole graphics pipeline", pre0 ? pre0->key : 0, sh[1] ? sh[1]->key : 0)) r = VK_ERROR_INITIALIZATION_FAILED;
    else { int bslot = build_begin("whole graphics pipeline", pre0 ? pre0->key : 0, sh[1] ? sh[1]->key : 0);
           r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
           build_end(bslot); }
    P->ci.flags = flags0;
    if (vrs) P->ci.pNext = fsr.pNext;
    if (stats && !r && p) pipe_stats(p, sh[1] ? "graphics (fragment key)" : "graphics (no fragment; vertex key)", sh[1] ? sh[1]->key : pre0 ? pre0->key : 0, 0, 0);
    P->ci.pNext = fbi.pNext;
    uint64_t dns = now_ns() - t0; double ms_ = dns / 1e6;
    pthread_mutex_lock(&g_mu);
    if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ngfx++; __atomic_fetch_add(&g_made, 1, __ATOMIC_RELAXED); if (!how) g_frame_made++; }
    g_create_ms += ms_; if (!how) g_create_ns += dns;   // made in the background or by prewarm: not the frame's time
    pthread_mutex_unlock(&g_mu);
    const vka_shader *pre = P->mesh ? sh[7] : sh[0];   // the vertex shader, or a mesh pipeline's mesh shader
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("graphics pipeline (%s %016llx fragment %016llx, %u colour targets, topology %u): %d; its draws are skipped", P->mesh ? "mesh" : "vertex",
                                                                         pre ? (unsigned long long)pre->key : 0ull, sh[1] ? (unsigned long long)sh[1]->key : 0ull, P->ri.colorAttachmentCount, P->mesh ? 0u : P->ia.topology, r); }
    else if (P->mesh) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) VLOG("mesh pipeline: task %016llx mesh %016llx fragment %016llx, %.1f ms%s", sh[6] ? (unsigned long long)sh[6]->key : 0ull,
                                                                                      (unsigned long long)sh[7]->key, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : ""); }
    else if (ms_ > 50) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 200) {
            char st[160]; size_t o = 0; st[0] = 0;
            for (uint32_t i = 0; i < P->ns && o < sizeof st; i++)
                o += (size_t)snprintf(st + o, sizeof st - o, "%s%s %.0f ms%s", i ? ", " : "", stage_name(P->ss[i].stage), (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? fb_st[i].duration / 1e6 : -1.0,
                                      (fb_st[i].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? " (cached)" : "");
            VLOG("graphics pipeline (%s %016llx fragment %016llx) took %.0f ms%s (pipeline cache %s; %s)", P->mesh ? "mesh" : "vertex", pre ? (unsigned long long)pre->key : 0ull, sh[1] ? (unsigned long long)sh[1]->key : 0ull, ms_, how ? how : "",
                 !(fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? "?" : (fb_all.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) ? "hit" : "miss", st[0] ? st : "no stage times");
        }
    }
    return p;
}

// ---------- Graphics pipeline libraries (VK_EXT_graphics_pipeline_library, V.gpl) ----------
// Making a pipeline the cache lacks took 60-160 ms on the Adreno 840's own driver (500 ms at the phone's 1 GHz thermal cap), on the replay thread in the middle of a frame:
// the frame's GPU work waited for the rest of its recording, and the game's frame-rate controller, which times frame start to GPU end, dropped to 20 fps for a moment.
// With libraries the pipeline is linked without optimization from four parts, each kept by its own key: vertex input, the pre-rasterization shaders, the fragment shader
// and the fragment output. Parts recur far more than whole pipelines (a fragment shader with many vertex shaders, the same targets and blending), so a miss usually compiles
// one stage, or none. The optimized whole pipeline is then made on a background thread (into the pipeline cache) and takes the linked one's place in the map; draws recorded
// before that keep the linked one, which stays alive with the process like every pipeline here.
// Found 2026-11-03 (stutter-agent cold2gpl / cold3gpl / gpl-2335 logs): Qualcomm's Adreno 840 driver (build 718a07726a) refused EVERY pre-rasterization library with
// VK_ERROR_UNKNOWN (-13), 0 made in any run: no pipeline was ever linked, each miss paid a vertex-input library, a refused library and then the whole pipeline on the frame
// path (cold3gpl 70 ms a pipeline against 59 without GPL). The first refusal now runs a probe (pre_probe): the same library made with one thing changed at a time, each
// result logged; a change that makes it is adopted for the run (g_pre_adapt), and with none, GPL turns itself off after DM_VK_GPL_GIVEUP refusals (default 3) with no
// pre-rasterization library made, so a miss costs the whole pipeline only
enum { L_VI, L_PRE, L_FS, L_FO, L_PREFS, NLIB };
typedef struct { uint8_t topo; uint32_t vin_static; uint8_t va[sizeof(((vka_gkey *)0)->va)], vb[sizeof(((vka_gkey *)0)->vb)]; } vikey;
typedef struct { VkShaderModule mod[4]; VkShaderStageFlagBits stage[4]; uint32_t pin[4]; uint8_t gl[4]; VkPipelineLayout layout; uint8_t depth_clamp, poly_mode, tess, vin_static; } prekey;
typedef struct { VkShaderModule mod; uint32_t pin, afunc; VkPipelineLayout layout; uint8_t samples, a2c, vin_static; } fskey;
typedef struct { uint32_t cfmt[8], dfmt, sfmt; uint8_t ncolor, samples, a2c, logic_en, logic_op, vin_static, att[8][8]; } fokey;
typedef struct { prekey pre; fskey fs; } prefskey;
static kmap g_lib[NLIB] = {{.klen = sizeof(vikey)}, {.klen = sizeof(prekey)}, {.klen = sizeof(fskey)}, {.klen = sizeof(fokey)}, {.klen = sizeof(prefskey)}};
static uint64_t g_nlib[NLIB], g_nlinked, g_nopt; static double g_lib_ms, g_link_ms;
static const char *const g_lib_name[NLIB] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output", "pre-rasterization + fragment shader"};
enum { A_NOCACHE = 1, A_RETAIN = 2, A_NORI = 4, A_COMBINED = 8 };
static unsigned g_pre_adapt; static int g_pre_probed, g_pre_refused; static _Atomic int g_gpl_off;
static int gpl_on(void) { return V.gpl && !atomic_load(&g_gpl_off); }
// A library's map value: the library, LIB_REFUSED (the driver refused it: the whole pipeline is made instead) or LIB_BUSY (a thread is making it). Several threads link
// (the replay thread and the DM_VK_PSO_ASYNC threads; prewarm and the optimizer make whole pipelines, not parts) and often miss the same part at once (one fragment shader
// under many vertex shaders). Before 2026-11-08 lib_make looked the key up, unlocked and made it: two threads that missed the same part both compiled it, and the second
// map entry hid the first library, which leaked. Now the first claims the key with LIB_BUSY and makes it, the others wait for it on g_lib_cv. A thread holds at most one
// claim and gives it up before it asks for its next part, so the waits cannot form a cycle. (From upstream Banana Native v0.1.2, 59f46bb)
#define LIB_REFUSED ((void *)(uintptr_t)1)
#define LIB_BUSY ((void *)(uintptr_t)2)
static pthread_cond_t g_lib_cv = PTHREAD_COND_INITIALIZER;
// Each map's generation (g_mu): bumped when an adaptation (gpl_link, DM_VK_GPL_PROBE) empties it. A part made across that is not stored: its key may have been claimed
// again, by a thread making it with the adaptation, whose LIB_BUSY it would overwrite (its waiters then taking the unadapted, usually refused, result as final)
static unsigned g_lib_gen[NLIB];
static VkPipeline lib_make(int kind, const void *key, size_t klen, VkGraphicsPipelineCreateInfo *ci, VkGraphicsPipelineLibraryFlagsEXT part, int *refused, uint64_t vs, uint64_t fs) {
    uint64_t h = fnv(key, klen, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    void *hit;
    while ((hit = km_get(&g_lib[kind], key, h)) == LIB_BUSY) pthread_cond_wait(&g_lib_cv, &g_mu);   // (looked up again: an adaptation may have emptied the map meanwhile)
    if (!hit) km_put(&g_lib[kind], key, h, LIB_BUSY);
    unsigned gen = g_lib_gen[kind], adapt = g_pre_adapt;   // (the adaptation this part is made with: read with the claim)
    pthread_mutex_unlock(&g_mu);
    if (hit) return hit == LIB_REFUSED ? VK_NULL_HANDLE : (VkPipeline)hit;
    int shp = kind == L_PRE || kind == L_PREFS;
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = shp && (adapt & A_NORI) ? NULL : ci->pNext, .flags = part};
    ci->pNext = &li; ci->flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (adapt & A_RETAIN ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
    uint64_t t0 = now_ns();
    VkPipeline p = VK_NULL_HANDLE; VkResult r;
    if (deadly_skip(g_lib_name[kind], vs, fs)) r = VK_ERROR_INITIALIZATION_FAILED;   // (refused: the whole pipeline, which skips it too)
    else { int bs = build_begin(g_lib_name[kind], vs, fs);   // (the shaders it compiles: a part of a long one goes to the .heavy list, a fragment shader's too)
           r = vk.vkCreateGraphicsPipelines(V.dev, shp && (adapt & A_NOCACHE) ? VK_NULL_HANDLE : g_pc, 1, ci, NULL, &p);
           build_end(bs); }
    ci->pNext = li.pNext;
    double ms_ = (now_ns() - t0) / 1e6;
    pthread_mutex_lock(&g_mu);
    if (r) p = VK_NULL_HANDLE; else g_nlib[kind]++;
    g_lib_ms += ms_;
    void **slot = gen == g_lib_gen[kind] ? km_slot(&g_lib[kind], key, h) : NULL;   // (an adaptation emptied the map while it was made: not kept, as it was made without it)
    if (slot) *slot = p ? (void *)p : LIB_REFUSED;
    pthread_cond_broadcast(&g_lib_cv);
    pthread_mutex_unlock(&g_mu);
    if (r == VK_ERROR_INITIALIZATION_FAILED && deadly_has(vs, fs)) {}   // (left out on purpose: not a refusal, which would start the probe and count toward turning GPL off)
    else if (r) { if (refused) *refused = r; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline library (%s): %d; the whole pipeline is made instead", g_lib_name[kind], r); }
    else if (ms_ > 50) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 100) VLOG("graphics pipeline library (%s) took %.0f ms", g_lib_name[kind], ms_); }
    return p;
}
// Each library names only the dynamic states of its own part (the Adreno 840's driver refused a pre-rasterization library given the whole list, VK_ERROR_UNKNOWN);
// the link takes them from the libraries
static int dyn_part(VkDynamicState d) {
    switch (d) {
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: return L_VI;
    case VK_DYNAMIC_STATE_VIEWPORT: case VK_DYNAMIC_STATE_SCISSOR: case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS: case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
    case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE: case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: return L_PRE;
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: return L_FO;
    default: return L_FS;   // depth and stencil tests, depth bounds
    }
}
static void dyn_of(const pstate *P, int part, VkDynamicState *out, VkPipelineDynamicStateCreateInfo *dy) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < P->dy.dynamicStateCount; i++) { int d = dyn_part(P->dyn[i]); if (d == part || (part == L_PREFS && (d == L_PRE || d == L_FS))) out[n++] = P->dyn[i]; }
    *dy = (VkPipelineDynamicStateCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = n, .pDynamicStates = out};
}
// One try of the probe: the library made (and destroyed), its result
static VkResult try_lib(VkGraphicsPipelineCreateInfo ci, VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCache pc, VkPipelineCreateFlags extra) {
    VkGraphicsPipelineLibraryCreateInfoEXT li = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = ci.pNext, .flags = part};
    ci.pNext = &li; ci.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | extra;
    int bs = build_begin("linked graphics pipeline", 0, 0);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, pc, 1, &ci, NULL, &p);
    build_end(bs);
    if (!r && p) vk.vkDestroyPipeline(V.dev, p, NULL);
    return r;
}
// The probe at the first refused pre-rasterization library (once a run): the same library with one thing changed at a time, then all the simplifications together. Logged as one
// line; returns the adaptation to use from now on (0 = none of the adoptable ones made it)
static unsigned pre_probe(const VkGraphicsPipelineCreateInfo *ci0, const pstate *P, const VkPipelineShaderStageCreateInfo *pre, uint32_t npre, const VkPipelineShaderStageCreateInfo *fs, VkResult first) {
    char line[900]; size_t o = 0; line[0] = 0;
#define NOTE(name, r) do { if (o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, "%s%s %s", o ? ", " : "", name, (r) ? "refused" : "made"); \
                           if ((r) && o < sizeof line) o += (size_t)snprintf(line + o, sizeof line - o, " (%d)", (int)(r)); } while (0)
    VkPipelineCreateFlags RET = VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    VkResult r_same = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("again as made", r_same);
    VkResult r_nc = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, 0); NOTE("no pipeline cache", r_nc);
    VkResult r_ret = try_lib(*ci0, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, RET); NOTE("RETAIN_LINK_TIME_OPTIMIZATION_INFO", r_ret);
    VkGraphicsPipelineCreateInfo c = *ci0; c.pNext = NULL;
    VkResult r_nori = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no VkPipelineRenderingCreateInfo", r_nori);
    VkViewport vp0 = {0, 0, 64, 64, 0, 1}; VkRect2D sc0 = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vps = P->vp; vps.pViewports = &vp0; vps.pScissors = &sc0;
    c = *ci0; c.pViewportState = &vps; c.pDynamicState = NULL;
    VkResult r_nodyn = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no dynamic state", r_nodyn);
    VkPipelineShaderStageCreateInfo st[4]; int pinned = 0;
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; st[i].pSpecializationInfo = NULL; }
    c = *ci0; c.pStages = st;
    VkResult r_nospec = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no specialization constants", r_nospec);
    for (uint32_t i = 0; i < npre; i++) { st[i] = pre[i]; if (st[i].pNext) pinned = 1; st[i].pNext = NULL; }
    if (pinned) { VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("no subgroup size pins", r); }
    if (npre > 1) { c = *ci0; c.stageCount = 1; c.pStages = pre; c.pTessellationState = NULL; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("first stage only", r); }
    {   // The pipeline layout with independent sets (diagnostic: a pipeline of such a layout binds only with it, not adopted)
        VkDescriptorSetLayout sets[3] = {P->set0, vka_bindless_layout(), vka_pfb_layout()}; VkPipelineLayout il = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = !sets[1] ? 1 : sets[2] ? 3 : 2, .pSetLayouts = sets};
        if (P->set0 && !vk.vkCreatePipelineLayout(V.dev, &pi, NULL, &il)) { c = *ci0; c.layout = il; VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, g_pc, 0); NOTE("independent-sets layout", r); vk.vkDestroyPipelineLayout(V.dev, il, NULL); }
    }
    VkResult r_comb = VK_ERROR_UNKNOWN;
    {   // Pre-rasterization and the fragment shader in one library
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkDynamicState dl[24]; VkPipelineDynamicStateCreateInfo dy; dyn_of(P, L_PREFS, dl, &dy);
        c = *ci0; c.stageCount = nb; c.pStages = both; c.pMultisampleState = &P->ms; c.pDepthStencilState = &P->ds; c.pDynamicState = &dy;
        r_comb = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader", r_comb);
        c.pDynamicState = NULL; c.pViewportState = &vps;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, g_pc, 0); NOTE("with the fragment shader, no dynamic state", r);
    }
    {   // Everything at once: no cache, no rendering info, no dynamic state, no specialization constants, no pins, the first stage only
        VkPipelineShaderStageCreateInfo s1 = pre[0]; s1.pSpecializationInfo = NULL; s1.pNext = NULL;
        c = *ci0; c.pNext = NULL; c.pViewportState = &vps; c.pDynamicState = NULL; c.stageCount = 1; c.pStages = &s1; c.pTessellationState = NULL;
        VkResult r = try_lib(c, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, VK_NULL_HANDLE, RET); NOTE("all simplified", r);
    }
#undef NOTE
    unsigned a = !r_same ? 0 : !r_nc ? A_NOCACHE : !r_ret ? A_RETAIN : !r_nori ? A_NORI : !r_comb ? A_COMBINED : 0;
    VLOG("graphics pipeline library probe (pre-rasterization refused with %d; vertex %s, %u stage%s): %s; %s", first, npre ? stage_name(pre[0].stage) : "-", npre, npre == 1 ? "" : "s", line,
         !r_same ? "the second try made it (a transient refusal)" : a == A_NOCACHE ? "adopted: libraries without the pipeline cache" : a == A_RETAIN ? "adopted: RETAIN_LINK_TIME_OPTIMIZATION_INFO"
         : a == A_NORI ? "adopted: no VkPipelineRenderingCreateInfo" : a == A_COMBINED ? "adopted: one library for pre-rasterization and the fragment shader" : "nothing adoptable");
    return a;
}
// The parts of P, each made once; then the link. VK_NULL_HANDLE = a part failed (the caller makes the whole pipeline)
static VkPipeline gpl_link(pstate *P, const vka_gkey *k) {
    VkDynamicState dl[NLIB][24]; VkPipelineDynamicStateCreateInfo dy[NLIB];
    for (int i = 0; i < NLIB; i++) dyn_of(P, i, dl[i], &dy[i]);
    VkPipeline part[4]; uint32_t nparts = 0;
    {   vikey key; memset(&key, 0, sizeof key); key.topo = k->topo; key.vin_static = k->vin_static;
        if (k->vin_static) { memcpy(key.va, k->va, sizeof key.va); memcpy(key.vb, k->vb, sizeof key.vb); }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pVertexInputState = &P->vi, .pInputAssemblyState = &P->ia, .pDynamicState = &dy[L_VI]};
        if (!(part[nparts++] = lib_make(L_VI, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, NULL, 0, 0))) return VK_NULL_HANDLE; }
    VkPipelineShaderStageCreateInfo pre[4], *fs = NULL; uint32_t npre = 0;
    for (uint32_t i = 0; i < P->ns; i++) { if (P->ss[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = &P->ss[i]; else if (npre < 4) pre[npre++] = P->ss[i]; }
    VkPipelineRenderingCreateInfo ri0 = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};   // the shader parts take only the view mask (0); the formats are the output's
    prekey pk; memset(&pk, 0, sizeof pk);
    for (uint32_t i = 0; i < npre; i++) { pk.mod[i] = pre[i].module; pk.stage[i] = pre[i].stage; pk.gl[i] = pre[i].pSpecializationInfo != NULL;
                                          pk.pin[i] = pre[i].pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)pre[i].pNext)->requiredSubgroupSize : 0; }
    pk.layout = P->ci.layout; pk.depth_clamp = (uint8_t)P->rs.depthClampEnable; pk.poly_mode = (uint8_t)P->rs.polygonMode; pk.tess = (uint8_t)P->tess; pk.vin_static = (uint8_t)k->vin_static;
    fskey fk; memset(&fk, 0, sizeof fk);
    if (fs) { fk.mod = fs->module; fk.afunc = fs->pSpecializationInfo ? P->afunc : 0; fk.pin = fs->pNext ? ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)fs->pNext)->requiredSubgroupSize : 0; }
    fk.layout = P->ci.layout; fk.samples = (uint8_t)P->ms.rasterizationSamples; fk.a2c = (uint8_t)P->ms.alphaToCoverageEnable; fk.vin_static = (uint8_t)k->vin_static;
    if (g_pre_adapt & A_COMBINED) {
        prefskey key; memset(&key, 0, sizeof key); key.pre = pk; key.fs = fk;
        VkPipelineShaderStageCreateInfo both[5]; uint32_t nb = 0; for (uint32_t i = 0; i < npre; i++) both[nb++] = pre[i]; if (fs) both[nb++] = *fs;
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = nb, .pStages = both, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pMultisampleState = &P->ms, .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_PREFS], .layout = P->ci.layout};
        if (!(part[nparts++] = lib_make(L_PREFS, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL, P->vs_key, P->fs_key))) return VK_NULL_HANDLE;
    } else {
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = npre, .pStages = pre, .pTessellationState = P->tess ? &P->ts : NULL,
                                           .pViewportState = &P->vp, .pRasterizationState = &P->rs, .pDynamicState = &dy[L_PRE], .layout = P->ci.layout};
        int refused = 0;
        VkGraphicsPipelineCreateInfo ci_probe = ci;   // (lib_make chains its own structure onto ci and restores it)
        if (!(part[nparts++] = lib_make(L_PRE, &pk, sizeof pk, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, &refused, P->vs_key, 0))) {
            if (refused && !__atomic_load_n(&g_nlib[L_PRE], __ATOMIC_RELAXED)) {
                int first = 0;
                pthread_mutex_lock(&g_mu); int nref = ++g_pre_refused; if (!g_pre_probed) { g_pre_probed = 1; first = 1; } pthread_mutex_unlock(&g_mu);
                // DM_VK_GPL_PROBE=1 (default 0 since 2026-11-03): the probe makes ~11-13 libraries synchronously on the replay thread, a 1-6 s hitch in the middle of a frame at
                // every run's first refusal; it is a one-off diagnostic run (android/STUTTER-TEST.md step 1), not something every run should pay for
                if (first && VKA_ENV("DM_VK_GPL_PROBE", 0)) {
                    unsigned a = pre_probe(&ci_probe, P, pre, npre, fs, (VkResult)refused);
                    if (a) { pthread_mutex_lock(&g_mu); g_pre_adapt = a; for (int i = L_PRE; i < NLIB; i += L_PREFS - L_PRE) { free(g_lib[i].e); g_lib[i].e = NULL; g_lib[i].cap = g_lib[i].n = 0; g_lib_gen[i]++; } pthread_cond_broadcast(&g_lib_cv); pthread_mutex_unlock(&g_mu); }   // (the refused pre-rasterization entries: tried again with the adaptation)
                }
                int giveup = VKA_ENV("DM_VK_GPL_GIVEUP", 3);
                if (!g_pre_adapt && giveup > 0 && nref >= giveup && !atomic_exchange(&g_gpl_off, 1))
                    VLOG("graphics pipeline libraries: off for this run: %d pre-rasterization libraries refused and none made (the probe found nothing to adopt); a pipeline the cache lacks is made whole (DM_VK_GPL_GIVEUP=0 keeps trying)", nref);
            }
            return VK_NULL_HANDLE;
        }
        VkGraphicsPipelineCreateInfo cf = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri0, .stageCount = fs ? 1 : 0, .pStages = fs, .pMultisampleState = &P->ms,
                                           .pDepthStencilState = &P->ds, .pDynamicState = &dy[L_FS], .layout = P->ci.layout};
        if (!(part[nparts++] = lib_make(L_FS, &fk, sizeof fk, &cf, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, NULL, 0, P->fs_key))) return VK_NULL_HANDLE;
    }
    {   fokey key; memset(&key, 0, sizeof key);
        memcpy(key.cfmt, k->cfmt, sizeof key.cfmt); key.dfmt = k->dfmt; key.sfmt = k->sfmt; key.ncolor = k->ncolor; key.samples = (uint8_t)P->ms.rasterizationSamples; key.a2c = k->a2c;
        key.logic_en = k->logic_en; key.logic_op = k->logic_op; key.vin_static = (uint8_t)k->vin_static; memcpy(key.att, k->att, sizeof key.att);
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &P->ri, .pMultisampleState = &P->ms, .pColorBlendState = &P->bs, .pDynamicState = &dy[L_FO]};
        if (!(part[nparts++] = lib_make(L_FO, &key, sizeof key, &ci, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, NULL, 0, 0))) return VK_NULL_HANDLE; }
    VkPipelineLibraryCreateInfoKHR libs = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = nparts, .pLibraries = part};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &libs, .layout = P->ci.layout};   // no LINK_TIME_OPTIMIZATION: the fast link
    if (deadly_skip("graphics pipeline link", P->vs_key, P->fs_key)) return VK_NULL_HANDLE;   // (a link that crashed: the whole pipeline is skipped too)
    uint64_t t0 = now_ns();
    int bs = build_begin("graphics pipeline (no cache)", P->vs_key, P->fs_key);
    VkPipeline p = VK_NULL_HANDLE; VkResult r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    build_end(bs);
    pthread_mutex_lock(&g_mu); g_link_ms += (now_ns() - t0) / 1e6; if (!r) { g_nlinked++; __atomic_store_n(&g_nlinked_pub, g_nlinked, __ATOMIC_RELAXED); } pthread_mutex_unlock(&g_mu);
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("graphics pipeline link: %d; the whole pipeline is made instead", r); return VK_NULL_HANDLE; }
    return p;
}

// ---------- The background compiles' gate (from upstream Banana Native v0.3.11, issues #127 / #132) ----------
// One gate for every compile made off the frame path (vka_psogate.h): the asynchronous threads' (vk-psoasync: a draw waits for them) first, the optimizer's (vk-psopt)
// and prewarm's (vk-prewarm) after them, as many at once as MemAvailable and the memory maps' headroom allow (DM_VK_PSO_SLOTS at most, default 4; 1 on Turnip, gate_cap),
// a shader known to compile long (.heavy) alone. Before it only prewarm was limited (vka_compile_gate.h, still applied to prewarm on top of this gate), so on Turnip the
// launcher's two asynchronous threads, the optimizer, a prewarm build and the frame path could compile at once (docs/reference/memory.md "Memory maps run out under
// Turnip during a compute-heavy video": Ocarina of Time out of maps after minutes, cause open there). A frame-path build waiting for another thread (busy_wait, g_demand_waiters) counts as a frame compile waiting: no speculative one
// starts meanwhile. The frame path's own builds (no DM_VK_PSO_ASYNC, compute and mesh pipelines) are not gated. The memory and maps it reads are refreshed at most every
// 500 ms, by the thread that finds them stale, outside the gate's lock (/proc/self/maps takes milliseconds). DM_VK_PREWARM_OVERLAP=1 (restart): no gate, every background
// compile on its own (as before).
// Unlike upstream, whose draws are skipped while their pipeline is made, here the draws wait for it (DM_VK_PSO_ASYNC=2 from the launcher, no DM_VK_ASYNC_DISCARD), so
// the frame's compiles do not wait for the speculative ones (gate_apart, vkpg_may_enter_apart): with upstream's shared slots the replay thread waited on Turnip's one slot
// for an optimizer or prewarm compile already running at nice 10 (1-3 s, a known long one 8-18 s), each new pipeline of an area a freeze of seconds. A speculative compile
// still starts only while fewer than the slots run and no frame compile waits, so at most one of each runs on Turnip (before the gate: two asynchronous, the optimizer, a
// prewarm build and the frame path). DM_VK_PSO_FRAME_APART=0 (restart; default 1 unless DM_VK_ASYNC_DISCARD=1): upstream's shared slots
int plat_maps_count(int *max);
static uint64_t mem_avail_mb(void);
static int pso_overlap(void) { return VKA_ENV("DM_VK_PREWARM_OVERLAP", 0) != 0; }
static int gate_apart(void) { int discard = VKA_ENV("DM_VK_ASYNC_DISCARD", 0); return VKA_ENV("DM_VK_PSO_FRAME_APART", !discard) != 0; }
static uint64_t pso_heavy_ns(void) { int ms = VKA_ENV("DM_VK_PREWARM_HEAVY_MS", 1000); return ms > 0 ? (uint64_t)ms * 1000000ull : 0; }
// Whether a thread holds its next compile: while a build anywhere in the process (frame path, asynchronous, prewarm) has run for heavy_ns or more, or while another build
// of the same pre-rasterization or compute shader (key) runs, or (key in .heavy) while a build of another known long shader runs. no_spec: speculative builds (vk-psopt,
// vk-prewarm) are not waited for (a frame job with gate_apart: right after a link the optimizer compiles the same vertex shader, the usual case)
static int pw_heavy_hold(uint64_t key, uint64_t heavy_ns, int no_spec) {
    uint64_t t = now_ns(); int mine = key && heavy_has(key);
    for (int i = 0; i < NBUILD; i++) {
        if (atomic_load(&g_build[i].tid) <= 0 || (no_spec && atomic_load(&g_build[i].spec))) continue;
        uint64_t vs = atomic_load(&g_build[i].vs), fs = atomic_load(&g_build[i].fs);
        if (t - atomic_load(&g_build[i].t0) >= heavy_ns || (key && (vs == key || fs == key)) || (mine && ((vs && heavy_has(vs)) || (fs && heavy_has(fs))))) return 1;
    }
    return 0;
}
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static vkpg_state g_gate; static _Atomic int g_gate_slots = 1, g_gate_slots_logged; static _Atomic uint64_t g_gate_t, g_gate_frame_wait_ns, g_gate_spec_wait_ns;
// During the boot hold (g_prep, prep_wait below) the cap is DM_VK_PREPARE_SLOTS (4) if that is more, except on Turnip: its one-at-a-time cap stays unless
// DM_VK_PREPARE_SLOTS is set, since concurrent compiles lost the device there (docs/reference/rendering.md "Concurrent background pipeline compiles lose the device
// on Turnip") and no run yet shows that a hold, where the game submits nothing, avoids it. vkpg_slots still gives fewer while MemAvailable or the memory maps'
// headroom are short (Banana Native v0.3.52 #576)
static _Atomic int g_prep;
static int prep_raises(void) { char set[16]; return V.driver_id != VK_DRIVER_ID_MESA_TURNIP || vka_env_str("DM_VK_PREPARE_SLOTS", set, sizeof set) == 1; }
static int gate_cap(void) {
    int cap = VKA_ENV("DM_VK_PSO_SLOTS", vkpg_cap_default(V.driver_id == VK_DRIVER_ID_MESA_TURNIP));
    if (atomic_load_explicit(&g_prep, memory_order_relaxed) && prep_raises()) { int p = VKA_ENV("DM_VK_PREPARE_SLOTS", VKPG_CAP); if (p > cap) cap = p; }
    return cap;
}
static int gate_slots(void) {
    uint64_t t = now_ns(), last = atomic_load(&g_gate_t);
    if ((!last || t - last >= 500000000ull) && atomic_compare_exchange_strong(&g_gate_t, &last, t)) {
        int mx = 0, maps = plat_maps_count(&mx);
        uint64_t av = mem_avail_mb();
        int cap = gate_cap(), n = vkpg_slots(av, maps, mx, cap), was = atomic_exchange(&g_gate_slots, n);
        char set[16];
        if ((n != was || !last) && atomic_fetch_add(&g_gate_slots_logged, 1) < 32)
            VLOG("background pipeline compiles: %d at once (was %d): MemAvailable %llu MiB, memory maps %d of %d (DM_VK_PSO_SLOTS at most %d%s)", n, was, (unsigned long long)av, maps, mx,
                 cap, atomic_load(&g_prep) && prep_raises() ? ", DM_VK_PREPARE_SLOTS while the boot hold runs" : vka_env_str("DM_VK_PSO_SLOTS", set, sizeof set) == 1 ? "" : V.driver_id == VK_DRIVER_ID_MESA_TURNIP ? ", 1 on Turnip by default" : "");
    }
    return atomic_load(&g_gate_slots);
}
static void gate_wake(void) { pthread_mutex_lock(&g_gate_mu); pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu); }
// frame: a compile a draw waits for (an asynchronous thread's); heavy: its shader is in the .heavy list. Returns the token gate_leave takes (-1: not gated; else bit 0
// heavy, bit 1 frame)
static int gate_enter(int frame, int heavy) {
    if (pso_overlap()) return -1;
    uint64_t t0 = now_ns(); int waited = 0, apart = gate_apart();
    for (;;) {
        int slots = gate_slots();   // (outside the lock: a refresh reads /proc/self/maps)
        pthread_mutex_lock(&g_gate_mu);
        vkpg_state o = g_gate;   // the others: this thread's own wait left out
        if (waited) { if (frame) { if (heavy) o.frame_heavy_wait--; else o.frame_wait--; } else { if (heavy) o.spec_heavy_wait--; else o.spec_wait--; } }
        o.frame_wait += atomic_load(&g_demand_waiters);   // (frame-path threads waiting for a background build)
        if (apart ? vkpg_may_enter_apart(&o, slots, frame, heavy) : vkpg_may_enter(&o, slots, frame, heavy)) break;   // (with the lock held)
        if (!waited) { waited = 1; if (frame) { if (heavy) g_gate.frame_heavy_wait++; else g_gate.frame_wait++; } else { if (heavy) g_gate.spec_heavy_wait++; else g_gate.spec_wait++; } }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 250000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &ts);   // (timed: memory freed elsewhere raises the slots without a gate_leave)
        pthread_mutex_unlock(&g_gate_mu);
        pause_point();
    }
    if (waited) { if (frame) { if (heavy) g_gate.frame_heavy_wait--; else g_gate.frame_wait--; } else { if (heavy) g_gate.spec_heavy_wait--; else g_gate.spec_wait--; } }
    g_gate.busy++; if (heavy) g_gate.heavy_busy++;
    if (frame) { g_gate.frame_busy++; if (heavy) g_gate.frame_heavy_busy++; }
    pthread_mutex_unlock(&g_gate_mu);
    if (waited) atomic_fetch_add(frame ? &g_gate_frame_wait_ns : &g_gate_spec_wait_ns, now_ns() - t0);
    return (heavy ? 1 : 0) | (frame ? 2 : 0);
}
static void gate_leave(int tok) {
    if (tok < 0) return;
    pthread_mutex_lock(&g_gate_mu);
    g_gate.busy--; if (tok & 1) g_gate.heavy_busy--;
    if (tok & 2) { g_gate.frame_busy--; if (tok & 1) g_gate.frame_heavy_busy--; }
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}
// A job's shaders against the .heavy list: its pre-rasterization (or compute) shader or its fragment shader
static int gate_heavy(uint64_t pre, uint64_t fs) { return (pre && heavy_has(pre)) || (fs && heavy_has(fs)); }
// The hold of pw_heavy_hold inside the gate: another build of the same shader (the driver would compile it twice), or a known long one outside the gate (the frame
// path's own compute and mesh pipelines). Its age clause (any build past DM_VK_PREWARM_HEAVY_MS) is for ungated threads only (DM_VK_PREWARM_OVERLAP=1): in the gate the
// slots already follow memory and the maps. A frame job with gate_apart does not hold for a speculative build (the twin compile is the price of not freezing the frame
// behind the optimizer). The time held, ns
static uint64_t gate_hold(int tok, uint64_t key) {
    uint64_t h = pso_heavy_ns(), t0 = 0;
    if (!h) return 0;
    if (tok >= 0) h = UINT64_MAX;
    int no_spec = tok >= 0 && (tok & 2) && gate_apart();
    while (pw_heavy_hold(key, h, no_spec)) { if (!t0) t0 = now_ns(); usleep(50000); pause_point(); }
    return t0 ? now_ns() - t0 : 0;
}

// The linked pipeline an optimized one replaced in its map entry, destroyed once no submission can still use it. Before, it stayed alive to the end of the process:
// one VkPipeline and its driver memory for every pipeline a run linked (the stage libraries it was linked from stay: they are kept for other links). The replay may
// have read it from the entry just before the swap, into the submission it is recording, whose fence is reserved before that submission or at its commit (vka_worker.c,
// vka_exec.c g_cur_fence); so the destroy waits twice: for the fence after the last one reserved at the swap, then for the one after the last reserved when that
// completed (a recording that read it before the swap and reserved its fence later is behind both). After a GPU hang fences read as done: the pipeline is then kept
static _Atomic uint64_t g_nretired;
typedef struct { VkPipeline p; int phase; } retire_job;
static void linked_retire_cb(void *a) {
    retire_job *r = a;
    if (vka_hung()) { free(r); return; }
    if (!r->phase++) { vka_after(vka_fence_last() + 1, linked_retire_cb, r); return; }
    vk.vkDestroyPipeline(V.dev, r->p, NULL); atomic_fetch_add(&g_nretired, 1); free(r);
}
static void linked_retire(VkPipeline p) {
    retire_job *r = malloc(sizeof *r); if (!r) return;   // (not freed: kept, as before)
    *r = (retire_job){p, 0};
    vka_after(vka_fence_last() + 1, linked_retire_cb, r);
}
// The background thread that makes the optimized pipelines, one after another, oldest first; each takes its linked pipeline's place in its map entry
typedef struct opt_job { struct opt_job *next; gval *g; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k; } opt_job;
static opt_job *g_opt_head, **g_opt_tail = &g_opt_head; static pthread_mutex_t g_opt_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_opt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_opt_waiting;
static void *opt_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psopt"); vka_host_fp("vk-psopt");
    // Under the game's threads, as the prewarm and asynchronous threads: nothing waits for an optimized pipeline (its linked one draws meanwhile). It kept the priority of
    // the thread that queued its first job: the replay thread's without DM_VK_PSO_ASYNC (upstream v0.3.11)
    setpriority(PRIO_PROCESS, 0, 10); t_spec = 1;
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_opt_mu);
        while (!g_opt_head) pthread_cond_wait(&g_opt_cv, &g_opt_mu);
        opt_job *j = g_opt_head; g_opt_head = j->next; if (!g_opt_head) g_opt_tail = &g_opt_head;
        pthread_mutex_unlock(&g_opt_mu);
        const vka_shader *pre = j->sh[7] ? j->sh[7] : j->sh[0];
        uint64_t key = pre ? pre->key : (j->sh[1] ? j->sh[1]->key : 0);
        int tok = gate_enter(0, gate_heavy(pre ? pre->key : 0, j->sh[1] ? j->sh[1]->key : 0));   // speculative: its linked pipeline draws meanwhile
        gate_hold(tok, key);
        pstate *P = malloc(sizeof *P);
        VkPipeline p = P && pstate_fill(P, j->sh, &j->lay, &j->k) ? full_pipeline(P, j->sh, " in the background") : VK_NULL_HANDLE;
        gate_leave(tok);
        pthread_mutex_lock(&g_mu);
        VkPipeline old = VK_NULL_HANDLE;
        if (p) { old = j->g->p; __atomic_store_n(&j->g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&j->g->linked, 0, __ATOMIC_RELEASE); g_nopt++; }   // a failure keeps the linked pipeline (logged)
        if (old && old != p) linked_retire(old);
        g_opt_waiting--;
        pthread_mutex_unlock(&g_mu);
        free(P); free(j);
    }
    return NULL;
}
static void opt_queue(gval *g, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    static int ok;
    opt_job *j = malloc(sizeof *j); if (!j) return;
    j->next = NULL; j->g = g; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay; j->k = *k;
    if (!ok) { pthread_t th; static _Atomic int tried; if (!atomic_exchange(&tried, 1)) { ok = !pthread_create(&th, NULL, opt_main, NULL); if (ok) pthread_detach(th); else VRED("pipeline optimizer thread not started: linked pipelines stay"); } }
    if (!ok) { free(j); return; }
    pthread_mutex_lock(&g_mu); g_opt_waiting++; pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&g_opt_mu); *g_opt_tail = j; g_opt_tail = &j->next; pthread_cond_signal(&g_opt_cv); pthread_mutex_unlock(&g_opt_mu);
}
void vka_gpl_line(char *out, size_t n) {
    pthread_mutex_lock(&g_mu);
    if (n) snprintf(out, n, "libraries %llu vertex input / %llu pre-rasterization / %llu fragment shader / %llu fragment output / %llu combined, %.0f ms; %llu linked (%.1f ms), %llu optimized in the background (%llu linked ones destroyed), %llu waiting; "
                    "cache-only tries %llu hit / %llu missed (%.0f ms in the misses)%s",
                    (unsigned long long)g_nlib[L_VI], (unsigned long long)g_nlib[L_PRE], (unsigned long long)g_nlib[L_FS], (unsigned long long)g_nlib[L_FO], (unsigned long long)g_nlib[L_PREFS], g_lib_ms,
                    (unsigned long long)g_nlinked, g_link_ms, (unsigned long long)g_nopt, (unsigned long long)atomic_load(&g_nretired), (unsigned long long)g_opt_waiting, (unsigned long long)g_probe_hit, (unsigned long long)g_probe_miss,
                    g_probe_miss_ns / 1e6, atomic_load(&g_gpl_off) ? "; OFF for this run (pre-rasterization refused)" : "");
    pthread_mutex_unlock(&g_mu);
}

// ---------- The prewarm list (2026-11-03) ----------
// The pipeline keys a run made on its frame path, kept beside the pipeline cache (vk_pipelines.keys) and made again by DM_VK_PREWARM background threads (default 2, nice 10)
// as soon as the next run makes its first pipeline: by the time the game reaches a scene, the pipelines it needs are in the map, and the frame path neither compiles nor
// even reads the cache for them. With the driver's pipeline cache warm each costs 1-4 ms on a background core; with it cold (a new driver) each is a compile, still off the
// frame path. A key names shaders by their library key and the state bytes (vka_gkey), not driver code, so one list serves every driver and survives a shader library
// change (a shader that is no longer in the library drops its records). Records whose shaders are missing are dropped at the next save; the list keeps at most
// DM_VK_PREWARM_KEEP records (default 16384), and a run makes at most DM_VK_PREWARM_MAX of them (default 8192): about 1 KB of host memory each, plus the driver's pipeline
// The records' format, their check and the layout fingerprint: vka_pwlist.h
static pthread_mutex_t g_pw_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_pw_buf; static size_t g_pw_len, g_pw_cap; static uint32_t g_pw_n; static int g_pw_dirty;   // every record (loaded, then new), under g_pw_mu
static uint64_t *g_pw_set; static size_t g_pw_setcap, g_pw_setn;   // their hashes
static uint8_t *g_pw_load; static size_t g_pw_load_len; static uint32_t g_pw_load_n;   // the loaded records, read by the prewarm threads (not changed after pw_load)
static _Atomic uint32_t g_pw_next, g_pw_next_mesh, g_pw_tix, g_pw_done, g_pw_made, g_pw_had, g_pw_bad; static int g_pw_threads; static uint64_t g_pw_t0; static uint32_t g_pw_seed_n;
static _Atomic uint64_t g_pw_memwait_ns, g_pw_heavywait_ns;
static size_t pw_size(const pwrec *r) { return vka_pw_size(r); }
static int pw_set_add(uint64_t h) {   // 1 = new
    if (!h) h = 1;
    if (2 * (g_pw_setn + 1) > g_pw_setcap) {
        size_t nc = g_pw_setcap ? 2 * g_pw_setcap : 1024; uint64_t *n = calloc(nc, sizeof *n); if (!n) return 0;
        for (size_t i = 0; i < g_pw_setcap; i++) if (g_pw_set[i]) { size_t j = g_pw_set[i] & (nc - 1); while (n[j]) j = (j + 1) & (nc - 1); n[j] = g_pw_set[i]; }
        free(g_pw_set); g_pw_set = n; g_pw_setcap = nc;
    }
    size_t j = h & (g_pw_setcap - 1);
    for (; g_pw_set[j]; j = (j + 1) & (g_pw_setcap - 1)) if (g_pw_set[j] == h) return 0;
    g_pw_set[j] = h; g_pw_setn++; return 1;
}
static void pw_append(const pwrec *r, const void *kb) {   // caller holds g_pw_mu
    size_t sz = pw_size(r);
    if (g_pw_len + sz > g_pw_cap) { size_t nc = g_pw_cap ? 2 * g_pw_cap : 65536; while (nc < g_pw_len + sz) nc *= 2; uint8_t *n = realloc(g_pw_buf, nc); if (!n) return; g_pw_buf = n; g_pw_cap = nc; }
    memcpy(g_pw_buf + g_pw_len, r, sizeof *r); memset(g_pw_buf + g_pw_len + sizeof *r, 0, sz - sizeof *r); memcpy(g_pw_buf + g_pw_len + sizeof *r, kb, r->klen);
    g_pw_len += sz; g_pw_n++;
}
// A graphics pipeline's stages as a record's five slots (vka_pwlist.h): a vertex pipeline's are its first five, a mesh pipeline's mesh, fragment and task
static void pw_slots(vka_shader *const sh[VKA_NSTAGE], vka_shader *s5[5], int *kind) {
    if (sh[7]) { *kind = VKA_PW_MESH; s5[0] = sh[7]; s5[1] = sh[1]; s5[2] = sh[6]; s5[3] = s5[4] = NULL; }
    else { *kind = VKA_PW_GFX; for (int i = 0; i < 5; i++) s5[i] = sh[i]; }
}
static void pw_note(int kind, vka_shader *const sh[5], const void *kb, uint32_t klen) {
    if (!g_pw_path[0]) return;
    pwrec r; memset(&r, 0, sizeof r); r.kind = (uint8_t)kind; r.klen = klen;
    for (int i = 0; i < 5; i++) r.key[i] = sh[i] ? sh[i]->key : 0;
    uint64_t h = vka_pw_hash(&r, kb);
    pthread_mutex_lock(&g_pw_mu);
    if (pw_set_add(h)) { pw_append(&r, kb); g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
static int pw_take(const uint8_t *d, size_t len, uint32_t *skipped, const char **why) {
    size_t o; uint32_t want, n = 0, took = 0; int e = vka_pw_header(d, len, &o, &want);
    if (e) { *why = e == VKA_PW_LAYOUT ? "another key layout (an older or newer host)" : e == VKA_PW_FORMAT ? "another format" : "too short"; return -1; }
    pwrec r; const uint8_t *kb; int ok;
    while (n < want && vka_pw_next(d, len, &o, &r, &kb, &ok)) {
        n++;
        if (!ok) { (*skipped)++; continue; }
        r.bad = 0;
        if (pw_set_add(vka_pw_hash(&r, kb))) { pw_append(&r, kb); took++; }
    }
    return (int)took;
}
static void pw_load(void) {
    if (!g_pw_path[0]) return;
    uint32_t skipped = 0; const char *why = NULL; int own = -1, imp = -1, seed = -1;
    FILE *f = fopen(g_pw_path, "rb");
    uint8_t *d = NULL; long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET); d = sz > 0 ? malloc((size_t)sz) : NULL; if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; } fclose(f); }
    FILE *fi = g_pw_import[0] ? fopen(g_pw_import, "rb") : NULL;
    uint8_t *di = NULL; long szi = 0;
    if (fi) { fseek(fi, 0, SEEK_END); szi = ftell(fi); fseek(fi, 0, SEEK_SET); di = szi > 0 ? malloc((size_t)szi) : NULL; if (di && fread(di, 1, (size_t)szi, fi) != (size_t)szi) { free(di); di = NULL; } fclose(fi); }
    uint8_t *ds = NULL; size_t szs = dm_vk_prewarm_seed_len; const uint8_t *seed_bytes = dm_vk_prewarm_seed;
    const char *module_dir = dm_module_dir();   // loader.c: the pack folder, also for the universal host.
    if (!szs && module_dir && *module_dir) {
        char sp[1100]; snprintf(sp, sizeof sp, "%s/vk_prewarm.bin", module_dir);
        FILE *fs = fopen(sp, "rb"); long l = 0;
        if (fs) { fseek(fs, 0, SEEK_END); l = ftell(fs); fseek(fs, 0, SEEK_SET); ds = l > 0 ? malloc((size_t)l) : NULL; if (ds && fread(ds, 1, (size_t)l, fs) == (size_t)l) { seed_bytes = ds; szs = (size_t)l; } else { free(ds); ds = NULL; } fclose(fs); }
    }
    pthread_mutex_lock(&g_pw_mu);
    if (!f) VLOG("prewarm list %s: none yet (written as this run makes pipelines)", g_pw_path);
    else if ((own = d ? pw_take(d, (size_t)sz, &skipped, &why) : -1) < 0) { VLOG("prewarm list %s: %s (replaced at the next save)", g_pw_path, why); g_pw_dirty = 1; }
    else if (skipped) VLOG("prewarm list %s: %d keys read, %u skipped (impossible values or truncated)", g_pw_path, own, skipped);
    free(d);
    if (fi) {
        uint32_t sk = 0; const char *iwhy = NULL;
        imp = di ? pw_take(di, (size_t)szi, &sk, &iwhy) : -1;
        if (imp < 0) { VRED("imported prewarm list %s: %s (removed)", g_pw_import, iwhy ? iwhy : "empty"); unlink(g_pw_import); }
        else if (imp > 0) g_pw_dirty = 1;
        else unlink(g_pw_import);
        free(di);
    }
    if (VKA_ENV("DM_VK_PREWARM_SEED", 1) && szs) {
        uint32_t sk = 0; const char *swhy = NULL;
        seed = pw_take(seed_bytes, szs, &sk, &swhy);
        if (seed < 0) VRED("prewarm seed list (the profile's vk_prewarm.bin): %s (make it again from runs of this host)", swhy);
        else { g_pw_seed_n = (uint32_t)seed; if (seed) g_pw_dirty = 1; }   // (kept in this device's list from now on: the next save writes them)
    }
    free(ds);
    g_pw_load = malloc(g_pw_len ? g_pw_len : 1); if (g_pw_load) { memcpy(g_pw_load, g_pw_buf, g_pw_len); g_pw_load_len = g_pw_len; g_pw_load_n = g_pw_n; }
    pthread_mutex_unlock(&g_pw_mu);
    if (g_pw_load_n)
        VLOG("prewarm list %s: %u pipeline keys (%d of earlier runs on this device, %d imported, %d more from the profile's seed list%s)",
             g_pw_path, g_pw_load_n, own >= 0 ? own : 0, imp >= 0 ? imp : 0, seed >= 0 ? seed : 0, !VKA_ENV("DM_VK_PREWARM_SEED", 1) ? ", seed off" : "");
}
static int pw_save(void) {
    if (!g_pw_path[0]) return 0;
    pthread_mutex_lock(&g_pw_mu);
    if (!g_pw_dirty) { pthread_mutex_unlock(&g_pw_mu); return 0; }
    uint32_t keep = (uint32_t)VKA_ENV("DM_VK_PREWARM_KEEP", 16384);
    size_t sz = sizeof(pwhdr2); uint32_t n = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_len;) {
        pwrec r; memcpy(&r, g_pw_buf + o, sizeof r);
        if (!r.bad) { sz += pw_size(&r); n++; }
        o += pw_size(&r);
    }
    size_t skip_n = n > keep ? n - keep : 0;
    uint8_t *out = malloc(sz);
    if (!out) { pthread_mutex_unlock(&g_pw_mu); return 0; }
    uint32_t written = 0; size_t w_off = sizeof(pwhdr2), cur_idx = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_len;) {
        pwrec r; memcpy(&r, g_pw_buf + o, sizeof r);
        size_t rsz = pw_size(&r);
        if (!r.bad) {
            if (cur_idx >= skip_n) {
                memcpy(out + w_off, g_pw_buf + o, rsz);
                ((pwrec *)(out + w_off))->bad = 0;
                w_off += rsz;
                written++;
            }
            cur_idx++;
        }
        o += rsz;
    }
    pwhdr2 h; memset(&h, 0, sizeof h); memcpy(h.magic, VKA_PW_MAGIC2, 8); h.gkey_size = sizeof(vka_gkey); h.n = written; h.layout = vka_pw_layout();
    memcpy(out, &h, sizeof h);
    g_pw_dirty = 0;
    pthread_mutex_unlock(&g_pw_mu);

    char tmp[1120]; snprintf(tmp, sizeof tmp, "%s.tmp", g_pw_path);
    FILE *f = fopen(tmp, "wb");
    // The stream is closed exactly once: fclose ends it even when it fails, so closing it again on the failure path (a failed fclose or rename) was undefined behaviour
    // (the same bug Banana Native v0.3.44 fixed in its pc_write_atomic)
    int wrote = f && fwrite(out, 1, w_off, f) == w_off, closed = f ? fclose(f) == 0 : 0;
    int ok = wrote && closed && !rename(tmp, g_pw_path);
    free(out);
    if (ok && g_pw_import[0]) unlink(g_pw_import);   // the imported keys are in g_pw_path now
    if (!ok) {
        unlink(tmp);
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 2) VRED("prewarm list not written to %s", g_pw_path);
        pthread_mutex_lock(&g_pw_mu); g_pw_dirty = 1; pthread_mutex_unlock(&g_pw_mu);
    }
    return ok ? (int)written : 0;
}
static void pw_mark_bad(size_t off) {
    pthread_mutex_lock(&g_pw_mu);
    if (off + sizeof(pwrec) <= g_pw_len) { g_pw_buf[off + offsetof(pwrec, bad)] = 1; g_pw_dirty = 1; }
    pthread_mutex_unlock(&g_pw_mu);
}
static void pw_explain(int kind, vka_shader *const sh[5], const vka_gkey *k, uint32_t klen, double ms, int linked, const char *where) {
    static _Atomic int nlog; int lim = VKA_ENV("DM_VK_PREWARM_WHY", 64);
    if (!g_pw_load || atomic_load(&nlog) >= lim || atomic_fetch_add(&nlog, 1) >= lim) return;
    static const struct { const char *name; size_t lo, hi; } F[] = {
        {"colour formats", offsetof(vka_gkey, cfmt), offsetof(vka_gkey, dfmt)}, {"depth format", offsetof(vka_gkey, dfmt), offsetof(vka_gkey, sfmt)},
        {"stencil format", offsetof(vka_gkey, sfmt), offsetof(vka_gkey, ncolor)}, {"colour count", offsetof(vka_gkey, ncolor), offsetof(vka_gkey, samples)},
        {"samples", offsetof(vka_gkey, samples), offsetof(vka_gkey, topo)}, {"topology", offsetof(vka_gkey, topo), offsetof(vka_gkey, poly_mode)},
        {"polygon mode", offsetof(vka_gkey, poly_mode), offsetof(vka_gkey, depth_clamp)}, {"depth clamp", offsetof(vka_gkey, depth_clamp), offsetof(vka_gkey, a2c)},
        {"alpha to coverage", offsetof(vka_gkey, a2c), offsetof(vka_gkey, logic_en)}, {"logic op", offsetof(vka_gkey, logic_en), offsetof(vka_gkey, gl_depth)},
        {"GL depth", offsetof(vka_gkey, gl_depth), offsetof(vka_gkey, fs_variant)}, {"fragment variant", offsetof(vka_gkey, fs_variant), offsetof(vka_gkey, alpha_func)},
        {"alpha func", offsetof(vka_gkey, alpha_func), offsetof(vka_gkey, gs_fx)}, {"geometry claim", offsetof(vka_gkey, gs_fx), offsetof(vka_gkey, vin_u)},
        {"vertex input retype", offsetof(vka_gkey, vin_u), offsetof(vka_gkey, tex_u)}, {"texture retype", offsetof(vka_gkey, tex_u), offsetof(vka_gkey, att)},
        {"blend", offsetof(vka_gkey, att), offsetof(vka_gkey, vin_static)},
        {"static vertex input", offsetof(vka_gkey, vin_static), sizeof(vka_gkey)}};
    enum { NF = sizeof F / sizeof F[0] };
    uint64_t want[5]; for (int i = 0; i < 5; i++) want[i] = sh[i] ? sh[i]->key : 0;
    int same_sh = 0, best = -1; unsigned bestmask = 0;
    for (size_t o = 0; o + sizeof(pwrec) <= g_pw_load_len;) {
        pwrec r; memcpy(&r, g_pw_load + o, sizeof r); const uint8_t *kb = g_pw_load + o + sizeof r; o += pw_size(&r);
        if (r.kind != kind || memcmp(r.key, want, sizeof want)) continue;
        same_sh++;
        vka_gkey rk; memset(&rk, 0, sizeof rk); memcpy(&rk, kb, r.klen < sizeof rk ? r.klen : sizeof rk);
        const uint8_t *a = (const uint8_t *)&rk, *b = (const uint8_t *)k; int nd = 0; unsigned mask = 0;
        for (int f = 0; f < NF; f++) { size_t hi = F[f].hi < klen ? F[f].hi : klen; if (F[f].lo < hi && memcmp(a + F[f].lo, b + F[f].lo, hi - F[f].lo)) { nd++; mask |= 1u << f; } }
        if (best < 0 || nd < best) { best = nd; bestmask = mask; }
    }
    char d[256] = ""; size_t q = 0;
    for (int f = 0; f < NF && q < sizeof d; f++) if (bestmask >> f & 1) q += (size_t)snprintf(d + q, sizeof d - q, "%s%s", q ? ", " : "", F[f].name);
    if (!same_sh) VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: no record with these shaders (%s %016llx fs %016llx; first met in this run)", where,
                       ms, linked ? ", linked" : "", kind == VKA_PW_MESH ? "mesh" : "vs", (unsigned long long)want[0], (unsigned long long)want[1]);
    else VLOG("pipeline made%s (%.1f ms%s), not in the prewarm list: %d records with these shaders, the nearest differs in %s", where,
              ms, linked ? ", linked" : "", same_sh, d[0] ? d : "nothing: the same key (the prewarm threads had not reached it yet)");
}
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int *made);
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, const vka_imgrt *rt, int prewarm, int *made);
static uint64_t mem_avail_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char line[128]; uint64_t kb = 0;
    while (f && fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtoull(line + 13, NULL, 10); break; }
    if (f) fclose(f);
    return kb >> 10;
}
static int warm_limit(void *unused) {
    (void)unused;
    uint64_t available = mem_avail_mb();
    int reserve = VKA_ENV("DM_VK_PREWARM_MEM_MB", 2048);
    if (atomic_load(&g_demand_waiters) || (reserve > 0 && available && available < (uint64_t)reserve)) return 0;
    int n = VKA_ENV("DM_VK_PREWARM_CONCURRENCY", V.driver_id == VK_DRIVER_ID_MESA_TURNIP ? 1 : g_warm_only || atomic_load(&g_prep) ? 4 : 2);   // (warming alone, or the boot hold where the game waits: as many as the threads)
    return n < 1 ? 1 : n > 4 ? 4 : n;
}
// ---------- The boot hold (DM_VK_PREPARE; Banana Native v0.3.52 #576) ----------
// DM_VK_PREPARE (restart; default 1; 0 off): the prewarm list (the device's keys and the profile's seed) is made before the game goes on. pw_start turns the hold on
// when it starts the prewarm threads in the game's process (not in the closed-game warm, g_warm_only), at the first pipeline the game needs (its first logo, before
// any 3D frame); the frame path's next pipeline call waits in prep_wait (the replay thread holds no pipeline lock there, and the game waits for its frame) until the
// last prewarm thread is done, the player skips (nxhost_pso_prepare_skip), or DM_VK_PREPARE_MAX_S (90; 0 none) passes. Meanwhile the gate admits DM_VK_PREPARE_SLOTS
// compiles at once, Turnip its own cap (gate_cap). With the pipeline cache warm (the launcher's warm after Prepare, vka_pso_warm) a key costs milliseconds and the hold
// is over in a second or two; the launcher shows its screen only from DM_VK_PREPARE_SHOW_MS (1000) into the hold (nxhost_pso_prepare_status). Without the hold the keys
// were made beside the game: upstream, a cold Donkey Kong Bananza on Turnip reached its level about 50 s in with most of its 1,875 keys still to make, and the level's
// first frame made 85 pipelines on the frame path (3.3-5.0 s)
static _Atomic uint32_t g_pw_seen;   // records the prewarm threads have taken (the hold's progress)
static pthread_mutex_t g_prep_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_prep_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_prep_t0;
static uint32_t prep_total(void) { uint32_t mx = (uint32_t)VKA_ENV("DM_VK_PREWARM_MAX", 8192); return g_pw_load_n < mx ? g_pw_load_n : mx; }
// The guest clock stands still while the hold runs (hle_os.c reason 3; upstream nxnative 8d888054f): the game's frame waits in the replay, but its other threads go on,
// and one that bounds its wait for the render thread by the frame's budget (nn::os::GetSystemTick) skipped hundreds of frames over a long hold and moved on to tearing
// down what frame 1 still drew (13 Sentinels: Aegis Rim, a SIGSEGV on the Mac). Guest reads of the clock wait until the hold ends. DM_VK_PREPARE_CLOCK=0: the clock
// runs. Weak: the unit tests and the warm process that link this file have no guest clock
void guest_clock_freeze_for(int reason, int on) __attribute__((weak));
static void prep_clock(int on) { if (guest_clock_freeze_for && VKA_ENV("DM_VK_PREPARE_CLOCK", 1)) guest_clock_freeze_for(3, on); }
static void prep_end(const char *why) {
    pthread_mutex_lock(&g_prep_mu);
    int was = atomic_exchange(&g_prep, 0);
    pthread_cond_broadcast(&g_prep_cv);
    pthread_mutex_unlock(&g_prep_mu);
    if (!was) return;
    prep_clock(0);
    atomic_store(&g_gate_t, 0);   // the slots read again at the next compile: gate_cap's own from now on
    VLOG("prepare: the game goes on (%s) after %.1f s: %u of %u pipeline keys taken, %u made, %u already in the pipeline cache, %u dropped; the prewarm threads go on with what is left",
         why, (now_ns() - g_prep_t0) / 1e9, (unsigned)atomic_load(&g_pw_seen), prep_total(), (unsigned)atomic_load(&g_pw_made), (unsigned)atomic_load(&g_pw_had), (unsigned)atomic_load(&g_pw_bad));
}
// The frame path's pipeline calls (vka_gfx_pipeline, vka_cs_pipeline*): wait while the hold runs. The prewarm threads make their keys through gfx_obtain and cs_obtain,
// never through these, so they are not held
static void prep_wait(void) {
    if (!atomic_load_explicit(&g_prep, memory_order_acquire)) return;
    uint64_t lim = (uint64_t)VKA_ENV("DM_VK_PREPARE_MAX_S", 90) * 1000000000ull;
    pthread_mutex_lock(&g_prep_mu);
    while (atomic_load(&g_prep)) {
        if (lim && now_ns() - g_prep_t0 >= lim) { pthread_mutex_unlock(&g_prep_mu); prep_end("DM_VK_PREPARE_MAX_S passed"); return; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec++;
        pthread_cond_timedwait(&g_prep_cv, &g_prep_mu, &ts);
    }
    pthread_mutex_unlock(&g_prep_mu);
}
// For the launcher (Native.prepareStatus, the game screen's "Compiling shaders" overlay): "<taken> <total> <seconds left or -1>" while the hold runs and has run
// DM_VK_PREPARE_SHOW_MS, else ""
__attribute__((visibility("default"))) void nxhost_pso_prepare_status(char *buf, size_t n) {
    if (!n) return;
    buf[0] = 0;
    if (!atomic_load(&g_prep)) return;
    double s = (now_ns() - g_prep_t0) / 1e9;
    if (s * 1000.0 < VKA_ENV("DM_VK_PREPARE_SHOW_MS", 1000)) return;
    uint32_t tot = prep_total(), done = atomic_load(&g_pw_seen);
    if (done > tot) done = tot;
    int eta = done >= 16 && done < tot ? (int)(s * (tot - done) / done + 0.5) : -1;   // (from the rate so far; the slow kinds come first, so it falls faster than it counts)
    { double lim = VKA_ENV("DM_VK_PREPARE_MAX_S", 90); if (lim > 0 && (eta < 0 || s + eta > lim)) eta = (int)(lim - s + 0.5) > 0 ? (int)(lim - s + 0.5) : 0; }   // (the hold ends at DM_VK_PREPARE_MAX_S whatever is left)
    snprintf(buf, n, "%u %u %d", (unsigned)done, (unsigned)tot, eta);
}
__attribute__((visibility("default"))) void nxhost_pso_prepare_skip(void) { prep_end("the player skipped the rest"); }

static void *pw_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-prewarm"); vka_host_fp("vk-prewarm");
    setpriority(PRIO_PROCESS, 0, 10); t_spec = 1;
    uint32_t max = (uint32_t)VKA_ENV("DM_VK_PREWARM_MAX", 8192);
    size_t *offs = NULL;
    uint32_t n = g_pw_load_n, nm = 0, me = atomic_fetch_add(&g_pw_tix, 1);
    offs = malloc((n ? n : 1) * sizeof *offs);
    if (offs) {
        static const uint8_t rank[3] = {VKA_PW_MESH, VKA_PW_CS, VKA_PW_GFX}; uint32_t i = 0;
        for (int k = 0; k < 3; k++) for (size_t o = 0, j = 0; j < n; j++) {
            pwrec r; memcpy(&r, g_pw_load + o, sizeof r);
            if (r.kind == rank[k] || (k == 2 && r.kind > VKA_PW_MESH)) offs[i++] = o;
            o += pw_size(&r);
        }
        for (uint32_t j = 0; j < n; j++) {
            pwrec r; memcpy(&r, g_pw_load + offs[j], sizeof r);
            if (r.kind != VKA_PW_MESH) break;
            nm++;
        }
    }
    uint32_t mem_mb = (uint32_t)VKA_ENV("DM_VK_PREWARM_MEM_MB", 2048);
    for (;;) {
        pause_point();
        if (mem_mb) {
            uint64_t t0 = 0, av;
            while ((av = mem_avail_mb()) && av < mem_mb && atomic_load(&g_pw_next) < n) {
                if (!t0) t0 = now_ns();
                usleep(250000);
                pause_point();
            }
            if (t0) atomic_fetch_add(&g_pw_memwait_ns, now_ns() - t0);
        }
        uint32_t i = me == 0 ? atomic_fetch_add(&g_pw_next_mesh, 1) : nm;
        if (i >= nm) { uint32_t q = atomic_fetch_add(&g_pw_next, 1); i = nm + q; }
        if (!offs || i >= n || i >= max) break;
        atomic_fetch_add(&g_pw_seen, 1);
        uint64_t gate_start = now_ns();
        vkc_enter(&g_warm_gate, warm_limit, NULL);
        atomic_fetch_add(&g_pw_memwait_ns, now_ns() - gate_start);
        pwrec r; memcpy(&r, g_pw_load + offs[i], sizeof r);
        const uint8_t *kb = g_pw_load + offs[i] + sizeof r;
        vka_shader *sh[VKA_NSTAGE]; memset(sh, 0, sizeof sh); int bad = !vka_pw_rec_ok(&r, kb), skip = 0;
        if (bad) {}
        else if (r.kind == VKA_PW_CS) {
            if (!(sh[5] = vka_shader_by_key(r.key[0]))) bad = 1;
            // a task / mesh program's compute record (written before 2026-11-12 by a run that emulated mesh): with native mesh it is no compute pipeline (cs_obtain), and its
            // mesh record warms it. Skipped, not marked bad: the list is shared by every driver, and one that emulates mesh still makes it
            else if (V.mesh && sh[5]->e->stage >= DMSH_VK_STAGE_TS) bad = skip = 1;
        }
        else if (r.kind == VKA_PW_MESH) {
            if (!(sh[7] = vka_shader_by_key(r.key[0])) || !(sh[1] = vka_shader_by_key(r.key[1])) || (r.key[2] && !(sh[6] = vka_shader_by_key(r.key[2])))) bad = 1;
        }
        else {
            for (int s = 0; s < 5 && !bad; s++) if (r.key[s] && !(sh[s] = vka_shader_by_key(r.key[s]))) bad = 1;
            // A pulling VS deliberately has its source mesh shader's content key.
            // Loading that key returns the compute shader, not the synthetic VS.
            if (!bad && !V.mesh && sh[0] && sh[0]->e->stage == DMSH_VK_STAGE_MS) {
                sh[0] = sh[0]->mesh_vertex;
                if (!sh[0]) bad = 1;
            }
            // and with native mesh such a record (an emulating run's pulling pipeline) is no vertex pipeline: the mesh record of the same draw warms it. Skipped, not marked bad
            else if (!bad && V.mesh && sh[0] && sh[0]->e->stage >= DMSH_VK_STAGE_TS) bad = skip = 1;
        }
        // Through the background compiles' gate after the frame's compiles (gate_enter), then held while another build of this shader runs (gate_hold); the record's
        // keys name its shaders (an emulated mesh record's mesh shader: its compute stages and pulling pipeline are made inside the same admission)
        int tok = -1;
        if (!bad) {
            uint64_t key = r.key[0], fs = r.kind == VKA_PW_CS ? 0 : r.key[1];
            tok = gate_enter(0, gate_heavy(key, fs));
            atomic_fetch_add(&g_pw_heavywait_ns, gate_hold(tok, key));
        }
        int emulated_mesh = !bad && !V.mesh && r.kind == VKA_PW_MESH;
        if (emulated_mesh) {
            // Native-mesh seed records are portable too: warm both compute stages
            // and rebuild the same pulling graphics pipeline used by vka_mesh.inc.
            for (int s = 6; s <= 7 && !bad; s++) if (sh[s]) {
                vka_shader *cs[VKA_NSTAGE] = {0}; cs[5] = sh[s]; vka_layout cl;
                vka_imgrt rt; memset(&rt, 0, sizeof rt); int cm = 0;
                if (!vka_layout_of(cs, 1, &cl)) bad = 1;
                else cs_obtain(sh[s], &cl, &rt, 1, &cm);   // another worker may already be building this stage
            }
            if (!bad) {
                sh[0] = sh[7]->mesh_vertex;
                if (!sh[0]) bad = 1;
                sh[6] = sh[7] = NULL;
            }
        }
        vka_layout lay;
        if (!bad && !vka_layout_of(sh, r.kind == VKA_PW_CS, &lay)) bad = 1;
        int made = 0;
        if (skip) {}
        else if (bad) { atomic_fetch_add(&g_pw_bad, 1); pw_mark_bad(offs[i]); }
        else if (r.kind == VKA_PW_CS) {
            vka_imgrt rt; memset(&rt, 0, sizeof rt);
            if (r.klen == sizeof(rt)) memcpy(&rt, kb, sizeof rt);
            cs_obtain(sh[5], &lay, &rt, 1, &made);
        }
        else {
            vka_gkey k; memset(&k, 0, sizeof k); memcpy(&k, kb, r.klen < sizeof k ? r.klen : sizeof k);
            if (emulated_mesh) {
                k.topo = sh[0]->e->reserved >> 25 & 1 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                k.gs_fx = 0; k.vin_u = k.vin_s = 0;
                memset(k.tex_u, 0, 4 * sizeof k.tex_u[0]); memset(k.tex_s, 0, 4 * sizeof k.tex_s[0]);
                memset(k.va, 0, sizeof k.va); memset(k.vb, 0, sizeof k.vb);
            }
            if (k.vin_static != (uint32_t)!V.vin_dyn) { atomic_fetch_add(&g_pw_bad, 1); }
            else {
                gkey_full fk; memset(&fk, 0, sizeof fk); memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay.layout; fk.k = k;
                size_t len = k.vin_static ? sizeof fk : offsetof(gkey_full, k.va);
                gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, &lay, &k, 1, &made);
            }
        }
        gate_leave(tok);
        if (made) atomic_fetch_add(&g_pw_made, 1); else if (!bad) atomic_fetch_add(&g_pw_had, 1);
        vkc_leave(&g_warm_gate);
        setpriority(PRIO_PROCESS, 0, 10);
    }
    free(offs);
    if (atomic_fetch_add(&g_pw_done, 1) + 1 == (uint32_t)g_pw_threads) {
        prep_end("every key taken");
        VLOG("prewarm: %u of %u pipeline keys in %.1f s on %d threads: %u made, %u already there, %u dropped; the frame path waited %.0f ms for them; threads waited %.1f s for memory, "
             "%.1f s for a long compile (DM_VK_PREWARM_HEAVY_MS)",
             (unsigned)(atomic_load(&g_pw_made) + atomic_load(&g_pw_had) + atomic_load(&g_pw_bad)), n, (now_ns() - g_pw_t0) / 1e9, g_pw_threads, (unsigned)atomic_load(&g_pw_made),
             (unsigned)atomic_load(&g_pw_had), (unsigned)atomic_load(&g_pw_bad), atomic_load(&g_pw_wait_ns) / 1e6, atomic_load(&g_pw_memwait_ns) / 1e9, atomic_load(&g_pw_heavywait_ns) / 1e9);
    }
    return NULL;
}
// Default off on Qualcomm's driver before 512.800 (upstream Banana Native v0.3.1, f48ecff8): 512.676 appears to keep about 6 MB of memory outside the process per pipeline
// (MemAvailable against the pipeline count; Adreno 740), and a list of ~650 pipelines took MemAvailable from 3.3 GB to 1.1-1.6 GB, after which the low-memory killer ended
// the game. Driver versions encode major << 22 | minor << 12 (891.11 and Turnip are not affected). DM_VK_PREWARM=<n> wins
static int qcom_old(void) {
    uint32_t dv = V.props.driverVersion;
    return V.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY && (dv >> 22) == 512 && (dv >> 12 & 0x3ff) < 800;
}
static void pw_start(void) {
    int nt = VKA_ENV("DM_VK_PREWARM", qcom_old() && !g_warm_only ? 0 : 4);
    char set[16];
    if (nt <= 0 || !g_pw_load_n) { if (g_pw_load_n) VLOG("prewarm: off (%s): %u keys not made ahead", vka_env_str("DM_VK_PREWARM", set, sizeof set) == 1 ? "DM_VK_PREWARM=0" : "default on Qualcomm's driver before 512.800; DM_VK_PREWARM=4 on", g_pw_load_n); return; }
    if (nt > 4) nt = 4;
    g_pw_t0 = now_ns();
    // The boot hold (prep_wait) starts with the threads, so their first compiles already have its slots; not in the closed-game warm (no game to hold). g_pw_threads is
    // the count before any thread runs (Banana Native v0.3.52 #575): counted up as each thread was made, a thread that finished its share while later ones were still
    // being made counted itself the last (the done line early, the hold over too soon, and vka_pso_warm's wait over with threads still compiling)
    if (!g_warm_only && VKA_ENV("DM_VK_PREPARE", 1)) { g_prep_t0 = now_ns(); atomic_store(&g_prep, 1); atomic_store(&g_gate_t, 0); prep_clock(1); }
    g_pw_threads = nt;
    int made = 0;
    for (int i = 0; i < nt; i++) { pthread_t th; if (!pthread_create(&th, NULL, pw_main, NULL)) { pthread_detach(th); made++; } }
    if (made < nt) {   // (a thread not made: the count the others compare with comes down; if that leaves the last one done already, the hold ends here)
        uint32_t done = atomic_load(&g_pw_done); g_pw_threads = made;
        if (!made || done >= (uint32_t)made) prep_end("no prewarm thread left to wait for");
    }
    VLOG("prewarm: %u pipeline keys of earlier runs made ahead on %d background threads (DM_VK_PREWARM=0 off)%s", g_pw_load_n, g_pw_threads,
         atomic_load(&g_prep) ? "; the game waits until they are done (the boot hold, DM_VK_PREPARE=0 off)" : "");
}
// ---------- Warming with the game closed ----------
// The prewarm list made into the pipeline cache before the game runs (nxhost_vk_warm, vka_warm.c), in a process of its own that the launcher starts after Prepare, after
// a GPU driver change, or when asked (android WarmActivity). The same records, threads and gate as the in-game prewarm (pw_main), without a game to share the phone with:
// its compiles run at nice 10 under nothing, DM_VK_PREWARM_CONCURRENCY defaults to four, and the cache is saved at the end. The idea is SCSKiller's (fill the driver's
// cache with the game closed; docs/ANALISE-SCSKILLER-2026-11-16.md); the work is this file's prewarm. The game's own prewarm then finds the keys already there ("already
// there" in its line) and its frames wait for no compile of a key the list had. out: made, already in the cache, dropped, the list's length. Returns 0, or -1 without a
// shader library (nothing to compile from)
static _Atomic uint32_t g_warm_total;
int vka_pso_warm(unsigned out[4]) {
    g_warm_only = 1;
    const vka_shlib *L = vka_shader_game_lib();
    if (!L) { VRED("pipeline warm: no shader library for this game: nothing compiled"); return -1; }
    const vka_shlib *z = NULL; atomic_compare_exchange_strong(&g_pc_shlib, &z, L);   // (the cache file's stamp: pc_open's job when a draw's shaders come first)
    uint64_t t0 = now_ns();
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, pc_open_once);   // reads the cache, loads the list, starts the threads (pw_start)
    atomic_store(&g_warm_total, g_pw_load_n);
    while (g_pw_threads && atomic_load(&g_pw_done) < (uint32_t)g_pw_threads) { usleep(200000); vka_pso_save(0); }   // (saved now and then on the saver thread: a warm killed midway keeps most of it)
    vka_pso_save(1);
    out[0] = atomic_load(&g_pw_made); out[1] = atomic_load(&g_pw_had); out[2] = atomic_load(&g_pw_bad); out[3] = g_pw_load_n;
    VLOG("pipeline warm: %u of %u keys made, %u already in the cache, %u dropped, in %.1f s on %d threads; cache saved", out[0], out[3], out[1], out[2], (now_ns() - t0) / 1e9, g_pw_threads);
    return 0;
}
// The warm stopped early (the launcher's Play now): each thread ends after the compile it is in, and vka_pso_warm saves what was made (the next warm or the game's prewarm
// finds those keys there)
void vka_pso_warm_stop(void) { atomic_store(&g_pw_next_mesh, 0x40000000u); atomic_store(&g_pw_next, 0x40000000u); VLOG("pipeline warm: stopped by the launcher"); }
// The warm's progress for the launcher: keys taken so far (made, had or dropped) and the list's length (0 until it is loaded)
void vka_pso_warm_progress(unsigned out[2]) {
    out[0] = atomic_load(&g_pw_made) + atomic_load(&g_pw_had) + atomic_load(&g_pw_bad); out[1] = atomic_load(&g_warm_total);
}
// ---------- Graphics pipelines: the lookup ----------
// The entry of a key, made by this thread when no one has it (made = 1). A key another thread is making (busy): the frame path waits for it (the time goes to the [longframe]
// line's prewarm wait), a prewarm thread goes on to its next key
// The frame path waits for another thread making the same key (caller holds g_mu). A prewarm thread runs at nice 10, under the game's threads, so the frame waited for a
// low-priority build (a priority inversion, worse than making it itself): that thread is raised to the waiter's priority for the rest of its build (pw_main puts itself back
// at nice 10 before its next key)
static void busy_wait(gval *g) {
    uint64_t t0 = now_ns();
    atomic_fetch_add(&g_demand_waiters, 1);
    if (g->tid > 0) { errno = 0; int me = getpriority(PRIO_PROCESS, 0); if (!(me == -1 && errno)) setpriority(PRIO_PROCESS, (id_t)g->tid, me); }
    while (g->busy) pthread_cond_wait(&g_busy_cv, &g_mu);
    atomic_fetch_sub(&g_demand_waiters, 1); vkc_wake(&g_warm_gate); gate_wake();   // (speculative compiles held for this wait may start)
    atomic_fetch_add(&g_pw_wait_ns, now_ns() - t0);
}
// DM_VK_PSO_ASYNC=N (restart; default 0 = off): a stopgap for frame-path graphics pipelines on slow compilers, from upstream Banana Native PR #12 (8fc1a70; there Turnip
// on the Adreno 740 made Donkey Kong Bananza's fragment shader libraries in 77-213 ms: 96 pipelines and 13.7 s of frame waits in 40 s of wall punching; with =2, 131 made
// in the background, the longest skip 426 ms, 30 fps held). The frame path still makes the cache-only try (a fraction of a millisecond when the pipeline cache has it,
// counted as the frame's like any frame-path pipeline); on a miss the entry is made on one of N background threads (vk-psoasync, nice 10, at most 4) and every draw that
// needs it is skipped (vka_draw.c: no pipeline, SKIPQ) until it is there, as Eden's asynchronous shaders do. Nothing waits with a deadline: the draw is dropped, not
// stood in for. A skipped draw is missing from that frame only, except where the game keeps what it drew (a texture or a shadow map drawn once and reused): that stays
// wrong until the game draws it again. With GPL off (DM_VK_GPL=0, or a driver that refused its libraries) there is no cache-only try, so even a pipeline the cache holds
// goes to the background and its draws blink for a frame or two. A key a prewarm thread is making is not waited for either (its draws are skipped too, instead of
// busy_wait). Compute and mesh pipelines are still made on the frame path. Open: the cause, slow pipeline compiles on the phone's drivers, is not addressed
typedef struct as_job { struct as_job *next; gval *g; size_t len; vka_shader *sh[VKA_NSTAGE]; vka_layout lay; vka_gkey k; uint64_t t0, cost; } as_job;
static as_job *g_as_head, **g_as_tail = &g_as_head; static pthread_mutex_t g_as_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_as_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_as_queued, g_as_made, g_as_skips, g_as_ns, g_as_lat_max_ns; static _Atomic int g_as_depth, g_as_depth_max;
// The queue's order (vka_psogate.h vkpg_take_oldest; upstream Banana Native v0.3.11): the job of least predicted cost first, the oldest in its turn (upstream, Turnip in
// Castle Town: missing objects appeared about 3 times sooner). DM_VK_PSO_ORDER=0 (restart): arrival order, as before. DM_VK_PSO_FAIR_MS (restart, 2000; 0 never): how
// long the oldest job waits before it takes turns with the cheapest. Under g_as_mu: g_as_credit (ns). With the draws waiting for their pipeline (the default here, no
// DM_VK_ASYNC_DISCARD) the replay thread queues one job at a time and the order matters only for jobs other threads queue meanwhile
static int g_as_order = -1; static uint64_t g_as_fair_ns; static int64_t g_as_credit; static uint64_t g_as_cheap_n, g_as_old_n, g_as_lat_cheap_ns, g_as_lat_old_ns;
// A job's predicted cost: the SPIR-V bytes of its stages (a library part's compile on Turnip grew with them: ~25-170 ms under 70 KB, 1.6-3.8 s for shaders of 88-130 KB),
// a shader in the .heavy list counted as 64 MiB more, so after every unknown one
static uint64_t as_cost(vka_shader *const sh[VKA_NSTAGE]) {
    uint64_t c = 0;
    for (int i = 0; i < VKA_NSTAGE; i++) {
        const vka_shader *s = sh[i]; if (!s) continue;
        const dmsh_vk_module *m = s->lib && s->e ? vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE) : NULL;
        c += m ? m->size : 65536;
        if (heavy_has(s->key)) c += 64ull << 20;
    }
    return c;
}
void vka_pso_work_stats(int *active, int *queued) {
    int n = 0; for (int i = 0; i < NBUILD; i++) n += atomic_load(&g_build[i].tid) > 0;
    if (active) *active = n; if (queued) *queued = atomic_load(&g_as_depth);
}
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int async, uint64_t t_make);
static void *as_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-psoasync"); vka_host_fp("vk-psoasync");
    setpriority(PRIO_PROCESS, 0, 10);   // under the game's threads, as the prewarm threads: no frame waits for these builds (their draws are skipped instead)
    for (;;) {
        pause_point();
        pthread_mutex_lock(&g_as_mu);
        while (!g_as_head) pthread_cond_wait(&g_as_cv, &g_as_mu);
        // The oldest (the head) in its turn, else the cheapest (the first of least cost: arrival order among equals)
        as_job **pp = &g_as_head; int oldest = 1;
        if (g_as_order > 0 && !vkpg_take_oldest(now_ns() - g_as_head->t0, g_as_fair_ns, g_as_credit)) {
            for (as_job **q = &g_as_head; *q; q = &(*q)->next) if ((*q)->cost < (*pp)->cost) pp = q;
            oldest = pp == &g_as_head;   // (the cheapest is the oldest: counted as an oldest pick)
        }
        as_job *j = *pp; *pp = j->next;
        if (g_as_tail == &j->next) g_as_tail = pp;
        pthread_mutex_unlock(&g_as_mu);
        pthread_mutex_lock(&g_mu); j->g->tid = gettid(); pthread_mutex_unlock(&g_mu);
        // The frame's (a draw waits for it): through the background compiles' gate before the speculative ones and, with gate_apart, not behind one already running;
        // then held while another frame-side build of its shader runs
        const vka_shader *pre = j->sh[7] ? j->sh[7] : j->sh[0];
        uint64_t key = pre ? pre->key : (j->sh[1] ? j->sh[1]->key : 0);
        int tok = gate_enter(1, gate_heavy(pre ? pre->key : 0, j->sh[1] ? j->sh[1]->key : 0));
        gate_hold(tok, key);
        setpriority(PRIO_PROCESS, 0, atomic_load(&g_demand_waiters) ? 0 : 10);
        uint64_t t0 = now_ns();
        gfx_build(j->g, j->len, j->sh, &j->lay, &j->k, 0, 1, j->t0);
        gate_leave(tok);
        uint64_t t1 = now_ns(), lat = t1 - j->t0, cur = atomic_load(&g_as_lat_max_ns);
        while (lat > cur && !atomic_compare_exchange_weak(&g_as_lat_max_ns, &cur, lat)) {}
        pthread_mutex_lock(&g_as_mu);
        if (oldest) { g_as_credit -= (int64_t)(t1 - t0); g_as_old_n++; g_as_lat_old_ns += lat; } else { g_as_credit += (int64_t)(t1 - t0); g_as_cheap_n++; g_as_lat_cheap_ns += lat; }
        if (g_as_credit > (int64_t)g_as_fair_ns) g_as_credit = (int64_t)g_as_fair_ns;   // (cheap time saved up while nothing was old does not buy a run of oldest picks later)
        pthread_mutex_unlock(&g_as_mu);
        atomic_fetch_add(&g_as_ns, t1 - t0); atomic_fetch_add(&g_as_made, 1); atomic_fetch_sub(&g_as_depth, 1);
        free(j);
        setpriority(PRIO_PROCESS, 0, 10);
    }
    return NULL;
}
static int g_as_n;
static void as_start(void) {
    int v = VKA_ENV("DM_VK_PSO_ASYNC", 0); v = v < 0 ? 0 : v > 4 ? 4 : v;
    g_as_order = VKA_ENV("DM_VK_PSO_ORDER", 1) != 0; { int f = VKA_ENV("DM_VK_PSO_FAIR_MS", 2000); g_as_fair_ns = f > 0 ? (uint64_t)f * 1000000ull : (uint64_t)INT64_MAX; }   // (before the threads)
    for (int i = 0; i < v; i++) { pthread_t th; if (!pthread_create(&th, NULL, as_main, NULL)) { pthread_detach(th); g_as_n++; } }
    if (v) VLOG("asynchronous pipelines: %d of %d workers; pending native draws wait to preserve persistent effects (DM_VK_ASYNC_DISCARD=1 opts into dropping them)", g_as_n, v);
    if (v && !g_as_n) VRED("asynchronous pipelines: no background thread started: graphics pipelines are made on the frame path");
}
static int as_threads(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, as_start); return g_as_n; }
static void as_queue(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, uint64_t t0) {
    as_job *j = malloc(sizeof *j);
    if (!j) { gfx_build(g, len, sh, lay, k, 0, 0, t0); return; }   // (no memory for the job: made here, on the frame path)
    j->next = NULL; j->g = g; j->len = len; memcpy(j->sh, sh, sizeof j->sh); j->lay = *lay; j->k = *k; j->t0 = t0; j->cost = as_cost(sh);
    int d = atomic_fetch_add(&g_as_depth, 1) + 1, m = atomic_load(&g_as_depth_max);
    while (d > m && !atomic_compare_exchange_weak(&g_as_depth_max, &m, d)) {}
    atomic_fetch_add(&g_as_queued, 1);
    pthread_mutex_lock(&g_as_mu); *g_as_tail = j; g_as_tail = &j->next; pthread_cond_signal(&g_as_cv); pthread_mutex_unlock(&g_as_mu);
}
void vka_pso_async_line(char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (as_threads() <= 0) return;
    pthread_mutex_lock(&g_as_mu);
    uint64_t cn = g_as_cheap_n, on = g_as_old_n; double cl = cn ? g_as_lat_cheap_ns / 1e6 / cn : 0.0, ol = on ? g_as_lat_old_ns / 1e6 / on : 0.0;
    pthread_mutex_unlock(&g_as_mu);
    snprintf(out, n, "asynchronous: %llu queued, %llu made (%.0f ms in all, longest wait from the miss %.0f ms), deepest queue %d, draws skipped waiting %llu; "
             "background compiles %d at once (%s), their gate held the frame's %.0f ms and the speculative ones %.0f ms; "
             "taken %s: %llu cheapest (mean wait %.0f ms), %llu oldest (mean wait %.0f ms)",
             (unsigned long long)atomic_load(&g_as_queued), (unsigned long long)atomic_load(&g_as_made), atomic_load(&g_as_ns) / 1e6, atomic_load(&g_as_lat_max_ns) / 1e6,
             atomic_load(&g_as_depth_max), (unsigned long long)atomic_load(&g_as_skips), atomic_load(&g_gate_slots), pso_overlap() ? "ungated: DM_VK_PREWARM_OVERLAP=1"
             : gate_apart() ? "by memory, DM_VK_PSO_SLOTS, the frame's apart" : "by memory, DM_VK_PSO_SLOTS, shared: DM_VK_PSO_FRAME_APART=0",
             atomic_load(&g_gate_frame_wait_ns) / 1e6, atomic_load(&g_gate_spec_wait_ns) / 1e6,
             g_as_order > 0 ? "least cost first, the oldest in turn (DM_VK_PSO_ORDER)" : "in arrival order (DM_VK_PSO_ORDER=0)",
             (unsigned long long)cn, cl, (unsigned long long)on, ol);
}
// The [longframe] line's count (vka_perf.c mtl_frame_extra): draws skipped so far because their pipeline was being made in the background; -1 = DM_VK_PSO_ASYNC off
int64_t vka_pso_async_skips(void) { return as_threads() > 0 ? (int64_t)atomic_load(&g_as_skips) : -1; }
static gval *gfx_obtain(const gkey_full *fk, uint64_t h, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int *made) {
    *made = 0;
    // Native draws can populate persistent lighting/shadow targets. Preserve them by default, including when async compilation is enabled.
    // Explicit discard is a visual-quality tradeoff, not proof that a pending draw will be issued again.
    int async = !prewarm && !sh[7] && as_threads() > 0;
    int discard = VKA_ENV("DM_VK_ASYNC_DISCARD", 0);
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_gfx, fk, h);
    if (g) {
        if (g->busy && (prewarm || (async && discard))) { pthread_mutex_unlock(&g_mu); return prewarm ? NULL : g; }
        if (g->busy) busy_wait(g);
        pthread_mutex_unlock(&g_mu);
        return g;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_gfx, fk, h, g);
    pthread_mutex_unlock(&g_mu);
    uint64_t t_make = now_ns();
    if (async) {
        pc_open(sh, VKA_NSTAGE);
        VkPipeline p = VK_NULL_HANDLE;
        pstate *P = malloc(sizeof *P);
        if (P && gpl_on() && pstate_fill(P, sh, lay, k)) {   // (the cache-only try needs pipelineCreationCacheControl, which GPL is on with)
            P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t t0 = now_ns();
            VkResult r = VK_PIPELINE_COMPILE_REQUIRED;   // (a pipeline on the .deadly list: not tried, a miss; its build is refused too)
            if (!deadly_skip("graphics pipeline (cache probe)", P->vs_key, P->fs_key)) {
                int bs = build_begin("graphics pipeline (cache probe)", P->vs_key, P->fs_key);
                r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
                build_end(bs); }
            uint64_t dns = now_ns() - t0;
            if (r != VK_SUCCESS) p = VK_NULL_HANDLE;
            pthread_mutex_lock(&g_mu);
            if (p) { g_ngfx++; __atomic_fetch_add(&g_made, 1, __ATOMIC_RELAXED); g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; } else { g_probe_miss++; g_probe_miss_ns += dns; }
            g_create_ns += dns;   // the try is the frame's time, hit or miss
            pthread_mutex_unlock(&g_mu);
        }
        free(P);
        if (p) {
            int kind; vka_shader *s5[5]; pw_slots(sh, s5, &kind); pw_note(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k))); pw_explain(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, 0, " on the frame path");
            pthread_mutex_lock(&g_mu); __atomic_store_n(&g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE); pthread_cond_broadcast(&g_busy_cv); pthread_mutex_unlock(&g_mu);
        } else as_queue(g, len, sh, lay, k, t_make);
        if (!discard) { pthread_mutex_lock(&g_mu); if (g->busy) busy_wait(g); pthread_mutex_unlock(&g_mu); }
        *made = 1;
        return g;
    }
    gfx_build(g, len, sh, lay, k, prewarm, 0, t_make);
    *made = 1;
    return g;
}
// A new entry g (busy) made and published. async: a frame-path miss made on a background thread (as_main): linked from parts like the frame path (then optimized by
// vk-psopt), its time not the frame's
static void gfx_build(gval *g, size_t len, vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k, int prewarm, int async, uint64_t t_make) {
    pc_open(sh, VKA_NSTAGE);
    VkPipeline p = VK_NULL_HANDLE; int linked = 0;
    pstate *P = malloc(sizeof *P);
    if (P && pstate_fill(P, sh, lay, k)) {   // (else logged by vka_shader_module)
        if (async && gpl_on() && !P->mesh) {   // (the cache-only try was made on the frame path and missed)
            uint64_t t1 = now_ns();
            p = gpl_link(P, k);
            if (p) { linked = 1; pthread_mutex_lock(&g_mu); g_ngfx++; g_create_ms += (now_ns() - t1) / 1e6; pthread_mutex_unlock(&g_mu); }
        }
        else if (!prewarm && !async && gpl_on() && !P->mesh) {   // (a mesh pipeline is made whole: the libraries here are cut for vertex input pipelines)
            // The pipeline cache may already hold the optimized one (made in an earlier run): then it is made at once (a fraction of a millisecond), else linked from parts
            P->ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t t0 = now_ns();
            VkResult r = VK_PIPELINE_COMPILE_REQUIRED;   // (a pipeline on the .deadly list: not tried, a miss; its build is refused too)
            if (!deadly_skip("graphics pipeline (cache probe)", P->vs_key, P->fs_key)) {
                int bs = build_begin("graphics pipeline (cache probe)", P->vs_key, P->fs_key);
                r = vk.vkCreateGraphicsPipelines(V.dev, g_pc, 1, &P->ci, NULL, &p);
                build_end(bs); }
            P->ci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            uint64_t dns = now_ns() - t0;
            if (r == VK_SUCCESS && p) { pthread_mutex_lock(&g_mu); g_ngfx++; __atomic_fetch_add(&g_made, 1, __ATOMIC_RELAXED); g_frame_made++; g_probe_hit++; g_create_ms += dns / 1e6; g_create_ns += dns; pthread_mutex_unlock(&g_mu); }
            else {
                // A miss that took long: the driver compiled before answering COMPILE_REQUIRED (the try then costs a compile and saves nothing)
                pthread_mutex_lock(&g_mu); g_probe_miss++; g_probe_miss_ns += dns; g_create_ns += dns; pthread_mutex_unlock(&g_mu);
                p = VK_NULL_HANDLE;
                uint64_t t1 = now_ns();
                p = gpl_link(P, k);
                if (p) { uint64_t d2 = now_ns() - t1; linked = 1; pthread_mutex_lock(&g_mu); g_ngfx++; g_frame_made++; g_create_ms += d2 / 1e6; g_create_ns += d2; pthread_mutex_unlock(&g_mu); }
                else { uint64_t d2 = now_ns() - t1; pthread_mutex_lock(&g_mu); g_create_ns += d2; pthread_mutex_unlock(&g_mu); }   // the refused parts' time is the frame's too
            }
        }
        if (!p && !linked) p = full_pipeline(P, sh, prewarm ? " (prewarm)" : async ? " in the background (asynchronous)" : NULL);
    }
    free(P);
    if (!prewarm) { int kind; vka_shader *s5[5]; pw_slots(sh, s5, &kind); pw_note(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k)));
                    pw_explain(kind, s5, k, (uint32_t)(len - offsetof(gkey_full, k)), (now_ns() - t_make) / 1e6, linked, async ? " in the background" : " on the frame path"); }
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g->linked, (uint8_t)linked, __ATOMIC_RELEASE); __atomic_store_n(&g->p, p, __ATOMIC_RELEASE); __atomic_store_n(&g->busy, 0, __ATOMIC_RELEASE);   // (read without the lock: gval_use)
    pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (linked) opt_queue(g, sh, lay, k);
}
static inline VkPipeline gval_use(gval *g) {
    if (!g) return VK_NULL_HANDLE;
    if (__atomic_load_n(&g->linked, __ATOMIC_ACQUIRE)) atomic_fetch_add_explicit(&g_hy_draws, 1, memory_order_relaxed);
    VkPipeline p = __atomic_load_n(&g->p, __ATOMIC_ACQUIRE);
    if (!p && __atomic_load_n(&g->busy, __ATOMIC_ACQUIRE)) atomic_fetch_add_explicit(&g_as_skips, 1, memory_order_relaxed);   // (only DM_VK_PSO_ASYNC hands out a busy entry)
    return p;
}
VkPipeline vka_gfx_pipeline(vka_shader *const sh[VKA_NSTAGE], const vka_layout *lay, const vka_gkey *k) {
    static gkey_full fk, last; static gval *last_g;
    prep_wait();   // (the boot hold: one atomic load when it is not running)
    // With vertex input dynamic state the key's static vertex input (va, vb: two thirds of it) stays zero: only what comes before it is hashed and compared with the
    // previous draw's key (draws in a row often share their pipeline). The entry's pipeline is read at each draw: the optimized one replaces a linked one in place
    size_t len = k->vin_static ? sizeof fk : offsetof(gkey_full, k.va);
    memset(&fk, 0, len);   // (the rest stays zero: vin_static is the device's, the same for every key)
    memcpy(fk.sh, sh, sizeof fk.sh); fk.layout = lay->layout; memcpy(&fk.k, k, len - offsetof(gkey_full, k));
    if (last_g && !memcmp(&fk, &last, len)) return gval_use(last_g);
    int made;
    gval *g = gfx_obtain(&fk, fnv(&fk, len, 1469598103934665603ull), len, sh, lay, k, 0, &made);
    memcpy(&last, &fk, len); last_g = g;
    return gval_use(g);
}

// ---------- Compute pipelines ----------
typedef struct { vka_shader *cs; VkPipelineLayout layout; vka_imgrt rt; } ckey;   // rt: the storage and sampled images retyped in its module (vka_shader_img_module), all zero for none
static kmap g_cs = {.klen = sizeof(ckey)};
static VkPipeline cs_obtain(vka_shader *cs, const vka_layout *lay, const vka_imgrt *rt, int prewarm, int *made) {
    *made = 0;
    ckey k; memset(&k, 0, sizeof k); k.cs = cs; k.layout = lay->layout; if (rt) k.rt = *rt;
    int img = k.rt.b[0] != 0;
    uint64_t h = fnv(&k, sizeof k, 1469598103934665603ull);
    pthread_mutex_lock(&g_mu);
    gval *g = km_get(&g_cs, &k, h);
    if (g) {
        if (g->busy && prewarm) { pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; }
        if (g->busy) busy_wait(g);
        VkPipeline p = g->p; pthread_mutex_unlock(&g_mu); return p;
    }
    g = calloc(1, sizeof *g); g->busy = 1; g->pw = (uint8_t)(prewarm != 0); g->tid = prewarm ? gettid() : 0; km_put(&g_cs, &k, h, g);
    pthread_mutex_unlock(&g_mu);
    pc_open(&cs, 1);
    pck_note(cs);   // (the cache stamp's list)
    uint64_t t0 = now_ns();
    uint32_t pin = (cs->e->flags & DMSH_VK_F_SUBGROUP) ? vka_shader_sg_pin(DMSH_VK_STAGE_CS) : 0;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rsg = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = pin};
    int spill = 0;   // shared memory in the spill buffer (vka_draw.c spill_dispatch): dispatched in parts with a base workgroup
    for (unsigned i = 0; i < cs->e->bind_count; i++) if (cs->b[i].kind == DMSH_VK_KIND_SHARED_SPILL) spill = 1;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .flags = spill ? VK_PIPELINE_CREATE_DISPATCH_BASE_BIT : 0, .layout = lay->layout,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = pin ? &rsg : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}};
    // A task or mesh program is a compute pipeline only through its fallback module (DMSH_VK_MOD_MESH_COMPUTE), which is used when mesh is emulated (!V.mesh). With native
    // mesh its module is the TaskEXT / MeshEXT one: handed in as a compute stage it is invalid usage (no GLCompute entry point "main"). Found 2026-11-12 on Turnip V39-patched
    // (Adreno 830, native mesh): the prewarm list, shared by every driver, held compute records of Ocarina of Time's task / mesh programs written by runs that emulated mesh,
    // and the driver, whose tu_compute_pipeline_create does not check tu_spirv_to_nir's result, dereferenced the NULL shader in nir_opt_access 50-60 s into every run.
    // Such a pipeline (and one whose module could not be made) is not handed to the driver: its dispatches are skipped. docs/reference/shaders.md "Task and mesh records
    // replayed as compute pipelines on native mesh"
    int notcs = V.mesh && cs->e->stage >= DMSH_VK_STAGE_TS;
    if (!notcs) ci.stage.module = img ? vka_shader_img_module(cs, &k.rt) : vka_shader_cs_module(cs);
    int stats = V.pexec;   // (DM_VK_PIPE_STATS_KEYS: only the listed programs, see full_pipeline)
    if (stats) { const char *only = getenv("DM_VK_PIPE_STATS_KEYS"); char hex[20];
                 if (only && *only) { snprintf(hex, sizeof hex, "%016llx", (unsigned long long)cs->key); stats = strstr(only, hex) != NULL; } }
    if (stats) ci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    VkPipeline p = VK_NULL_HANDLE; VkResult r; int over = 0;
    // Shared memory past the device's limit is invalid usage (the driver may refuse, crash or run it with less). The translator keeps a program's shared memory past
    // DMSH_VK_SHARED_MAX in a storage buffer (DMSH_VK_KIND_SHARED_SPILL), and e->shared is then what stays in workgroup memory; a program still over this device's
    // limit (a library built before the spill, or a device under DMSH_VK_SHARED_MAX) is not made and its dispatches are skipped. docs/reference/shaders.md "Compute
    // programs over the device's shared memory". DM_VK_CS_SHARED_OVER=1 makes them anyway (an A/B: what the driver does with them)
    uint32_t smax = V.props.limits.maxComputeSharedMemorySize;
    uint64_t shared = cs->e->shared;
    // TaskPayloadWorkgroupEXT becomes an additional Workgroup uint array in the
    // compute variant. Native reflection counts only the guest's original shared memory.
    if (!V.mesh && cs->e->stage == DMSH_VK_STAGE_TS && (cs->e->flags & DMSH_VK_F_MESH_FALLBACK)) shared += (cs->e->reserved & 65535u) * 4u;
    if (smax && shared > smax && !VKA_ENV("DM_VK_CS_SHARED_OVER", 0)) {
        r = VK_ERROR_FEATURE_NOT_PRESENT; over = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute program %016llx takes %llu bytes of shared memory, the device at most %u: not made, its dispatches are skipped (the first 16 are logged; the count is in the pipeline line)", (unsigned long long)cs->key, (unsigned long long)shared, smax);
    } else if (notcs || !ci.stage.module) {
        r = VK_ERROR_INITIALIZATION_FAILED;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute pipeline %016llx not made: %s; its dispatches are skipped", (unsigned long long)cs->key,
                                                                     notcs ? "a task / mesh program with native mesh has no compute module" : "its module was not made");
    } else if (deadly_skip("compute pipeline", cs->key, 0)) r = VK_ERROR_INITIALIZATION_FAILED;
    else { int bs = build_begin("compute pipeline", cs->key, 0); r = vk.vkCreateComputePipelines(V.dev, g_pc, 1, &ci, NULL, &p); build_end(bs); }
    pthread_mutex_lock(&g_mu);
    if (over) { p = VK_NULL_HANDLE; g_ncs_over++; } else if (r) { p = VK_NULL_HANDLE; g_nfail++; } else { g_ncs++; __atomic_fetch_add(&g_made, 1, __ATOMIC_RELAXED); if (!prewarm) g_frame_made++; }
    { uint64_t dns = now_ns() - t0; g_create_ms += dns / 1e6; if (!prewarm) g_create_ns += dns; }
    g->p = p; g->busy = 0; pthread_cond_broadcast(&g_busy_cv);
    pthread_mutex_unlock(&g_mu);
    if (stats && !r && p) pipe_stats(p, cs->e->stage >= DMSH_VK_STAGE_TS ? (cs->e->stage == DMSH_VK_STAGE_TS ? "task (compute)" : "mesh (compute)") : "compute", cs->key, cs->e->local[0], shared);
    if (r && !over) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VRED("compute pipeline %016llx: %d; its dispatches are skipped", (unsigned long long)cs->key, r); }
    { double ms_ = (now_ns() - t0) / 1e6; static _Atomic int n; if (!r && ms_ > 50 && atomic_fetch_add(&n, 1) < 200) VLOG("compute pipeline %016llx took %.0f ms%s", (unsigned long long)cs->key, ms_, prewarm ? " (prewarm)" : "");
    // (one not made stays out of the prewarm list. A task / mesh program's fallback compute is recorded: it is what warms those stages on a driver that emulates mesh, Qualcomm's
    // among them; the replay skips such a record on a driver with native mesh)
    if (!prewarm && !over && !img && !notcs) { vka_shader *s5[5] = {cs, NULL, NULL, NULL, NULL}; pw_note(VKA_PW_CS, s5, "", 0); }
      // the forced save after a slow one, once its key is in the list: before, the save went out without it, and a game killed before the next save made it again at its
      // next start on the frame path (as Banana Native 67a2da0a found and fixed; the graphics path already noted first)
      if (!prewarm && !r && ms_ > 1000) vka_pso_save(2); }
    *made = 1;
    return p;
}
VkPipeline vka_cs_pipeline(vka_shader *cs, const vka_layout *lay) { int made; prep_wait(); return cs_obtain(cs, lay, NULL, 0, &made); }
VkPipeline vka_cs_pipeline_img(vka_shader *cs, const vka_layout *lay, const vka_imgrt *rt) { int made; prep_wait(); return cs_obtain(cs, lay, rt, 0, &made); }
#endif
