// Shaders of the Vulkan replay: the game's library (android/SHADERS.md), its entries as VkShaderModules, and NVN programs resolved to one shader per stage.
// The library is <pack folder>/shaders/v-<DMSH_VK_CHAIN>/dmsh_vk.lib beside its inputs.tsv (the pack folder is the one the modules came from, dm_module_dir), or the file
// DM_VK_SHLIB names. One whose chain, Vulkan chain or game is not this host's is refused with a [RED] line: the game runs on with no draws. While this version's library is
// not in the pack yet (the app's delta update makes it, android/SHADERS.md#delta-updates), another v-<V>/ of the pack whose header carries this host's DMSH_VK_ABI is played
// instead (the newest): what the replay reads of it is the same; the new library is taken from the next start. Modules are made at a program's first draw (the submit
// thread), not at nvnProgramSetShaders
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/dmsh_chain.h"
#include "../nvn/dmsh_chain_vk.h"
#include "game_profile.h"
#include "vka_spvpatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static vka_shlib g_lib; static int g_lib_state;   // 0 not tried, 1 loaded, -1 none
static char g_lib_path[1024];                       // the library loaded (vka_shader_lib_path)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// How each stage runs the subgroup operations of a library built for sg lanes (DMSH_VK_F_SUBGROUP shaders). A module built for 64 lanes splits ballots and shuffles into
// 32-lane halves, so it is right on 32 or 64 lanes; one built for n lanes, on n. Per stage, in this order: no subgroup operations in the stage on this device = rewritten
// as one-invocation subgroups (single_lane); a size the library is right for that the device may vary and lets a pipeline pin (subgroupSizeControl, the stage in
// requiredSubgroupSizeStages) = pinned to it (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo, vka_pso.c); the device's fixed size right for the library = as built;
// otherwise (a wrong or varying size that cannot be pinned) = one-invocation subgroups too. Mesa Turnip on the Adreno 840 reports 128 lanes (64-128) with fragment and
// compute pinnable: before this, every stage ran the 64-lane modules at up to 128 lanes (black cave walls in DKB); now fragment and compute are pinned to 64 and the
// vertex, tessellation and geometry stages run one-lane subgroups, as on Qualcomm's driver (which has none in those stages). DM_VK_SUBGROUP_PIN (restart): 0 = no pinning,
// 1 = every stage the device lets pin (default), any other value = a VkShaderStageFlags mask of the stages that may pin (0x30: fragment and compute)
static uint32_t g_sg_pin[8]; static uint8_t g_sg_one[8];   // by DMSH_VK_STAGE_*
static const uint32_t g_stage_bit[8] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT};
static void sg_plan(unsigned sg) {
    static const char *const nm[8] = {"vertex", "tess control", "tess evaluation", "geometry", "fragment", "compute", "task", "mesh"};
#define SG_OK(n) (sg == 64 ? ((n) == 32 || (n) == 64) : (n) == sg)
    unsigned pin = 0; int pm = VKA_ENV("DM_VK_SUBGROUP_PIN", 1); uint32_t pin_stages = pm == 1 ? V.subgroup_req_stages : V.subgroup_req_stages & (uint32_t)pm;
    if (V.subgroup_min != V.subgroup_max && pin_stages)
        for (unsigned c = sg; c >= 32 && !pin; c /= 2) if (SG_OK(c) && c >= V.subgroup_min && c <= V.subgroup_max) pin = c;
    // Turnip's geometry stage computes subgroup operations wrong at a pinned 64 (T30, Mesa 26.3: DKB's terrain culling GS 5d537f7630ec3671 left the cave walls black; one-lane
    // subgroups there drew them right, with every other stage pinned). Eden leaves geometry out of Turnip's subgroup stages too. DM_VK_SUBGROUP_GS (restart): 0 = the geometry
    // stage runs one-lane subgroups, 1 = as the device reports; default 0 on VK_DRIVER_ID_MESA_TURNIP, 1 on other drivers
    uint32_t stages = V.subgroup_stages;
    if (!VKA_ENV("DM_VK_SUBGROUP_GS", V.driver_id != VK_DRIVER_ID_MESA_TURNIP)) stages &= ~(uint32_t)VK_SHADER_STAGE_GEOMETRY_BIT;
    // DM_VK_SUBGROUP_STAGES=<VkShaderStageFlags> (restart, diagnostic): only these stages keep the device's subgroups, the others run one-lane subgroups (0x30: fragment and
    // compute only, as Qualcomm's driver on the Adreno 830; 0x10: fragment only). Tells whether a driver computes a stage's subgroup operations differently
    { char sv[32]; if (vka_env_str("DM_VK_SUBGROUP_STAGES", sv, sizeof sv) && *sv) stages &= (uint32_t)strtoul(sv, NULL, 0); }
    char q[256]; size_t qn = 0; q[0] = 0; int one_new = 0;
    for (int i = 0; i < 8; i++) {
        if (i >= DMSH_VK_STAGE_TS && !V.mesh) continue;
        uint32_t b = g_stage_bit[i]; g_sg_pin[i] = 0; g_sg_one[i] = 0;
        if (!(stages & b)) g_sg_one[i] = 1;
        else if (pin && (pin_stages & b)) g_sg_pin[i] = pin;
        else if (!SG_OK(V.subgroup_size) || V.subgroup_min != V.subgroup_max) g_sg_one[i] = 1, one_new = 1;
        qn += (size_t)snprintf(q + qn, sizeof q - qn, "%s%s %s", i ? ", " : "", nm[i], g_sg_one[i] ? "one lane" : g_sg_pin[i] ? (pin == 64 ? "pinned 64" : "pinned 32") : "as built");
        if (qn >= sizeof q) qn = sizeof q - 1;
    }
#undef SG_OK
    VLOG("subgroup shaders (built for %u lanes; the device's %u, %u-%u, pinnable in %#x, pinned in %#x): %s", sg, V.subgroup_size, V.subgroup_min, V.subgroup_max, V.subgroup_req_stages, pin ? pin_stages : 0, q);
    if (one_new) VLOG("subgroup shaders: stages whose subgroup size is wrong for the library or varies unpinned run them as one-invocation subgroups");
}
uint32_t vka_shader_sg_pin(int stage) { return stage >= 0 && stage < 8 ? g_sg_pin[stage] : 0; }

// Compute shaders' subgroup shuffles through workgroup memory (the library's DMSH_VK_MOD_SHUFFLE_LDS modules, vk_android_spv.py shuffle_lds). Turnip has no
// arbitrary-index shuffle instruction: ir3 makes each OpGroupNonUniformShuffle a loop of rotates, one pass per distinct (index - lane) in the subgroup, and the translator's
// LDSM / MOVM gathers take up to 24 (agl's upscaler network in DKB, 14a8aaf3e0a9edbd: 10.7 ms a dispatch on the Adreno 840, 2.0 ms with the module through workgroup
// memory, upstream BananaNative v0.1.3). Qualcomm's driver shuffles natively and runs the base module faster. DM_VK_SHUFFLE_LDS (restart): 1 = those modules, 0 = the
// base ones; default 1 on VK_DRIVER_ID_MESA_TURNIP, 0 on other drivers. Needs DMSH_VK_SHARED_MAX bytes of shared memory (the library keeps the modules within it).
// The plan is made once, when the library loads, and holds for the whole run: a shader's compute module (and the image-retyped modules made from it) never changes
static int g_shuffle_lds = -1;
static void shuffle_plan(void) {
    int want = VKA_ENV("DM_VK_SHUFFLE_LDS", V.driver_id == VK_DRIVER_ID_MESA_TURNIP) != 0;
    int fits = V.props.limits.maxComputeSharedMemorySize >= DMSH_VK_SHARED_MAX;
    g_shuffle_lds = want && fits;
    VLOG("compute subgroup shuffles: %s", g_shuffle_lds ? "through workgroup memory where the library has the module (DM_VK_SHUFFLE_LDS=0: the instruction)"
         : want ? "the instruction (the device's shared memory is under DMSH_VK_SHARED_MAX)" : "the instruction (DM_VK_SHUFFLE_LDS=1: through workgroup memory)");
}
int vka_shader_cs_variant(vka_shader *s) {
    if (!V.mesh && s->e->stage >= DMSH_VK_STAGE_TS) return DMSH_VK_MOD_MESH_COMPUTE;
    if (g_shuffle_lds < 0) shuffle_plan();
    // (that module not made: logged by vka_shader_module; the base one)
    return g_shuffle_lds && vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_SHUFFLE_LDS) && vka_shader_module(s, DMSH_VK_MOD_SHUFFLE_LDS) ? DMSH_VK_MOD_SHUFFLE_LDS : DMSH_VK_MOD_BASE;
}
VkShaderModule vka_shader_cs_module(vka_shader *s) { return vka_shader_module(s, vka_shader_cs_variant(s)); }

const char *vka_shader_lib_path(void) { return g_lib_path; }

static int lib_load(void) {
    if (g_lib_state) return g_lib_state > 0;
    g_lib_state = -1;
    char path[1024], err[256]; const char *e = getenv("DM_VK_SHLIB"); const char *dir = dm_module_dir(); dir = dir && *dir ? dir : ".";
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else snprintf(path, sizeof path, "%s/shaders/" DMSH_VK_DIR_PREFIX "%s/" DMSH_VK_LIB_FILE, dir, DMSH_VK_CHAIN);
    int have = !access(path, R_OK) && vka_shlib_open(&g_lib, path, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, err, sizeof err);
    if (!have && !(e && *e)) {   // this version's library not in the pack (yet): the newest other v-<V>/ that this host reads the same (its ABI)
        char why[256]; snprintf(why, sizeof why, "%s", access(path, R_OK) ? "missing" : err);
        char sd[1024], best[1024] = "", old[1024] = ""; time_t bt = 0, ot = 0; snprintf(sd, sizeof sd, "%s/shaders", dir);
        DIR *d = opendir(sd); struct dirent *de;
        while (d && (de = readdir(d))) {
            if (strncmp(de->d_name, DMSH_VK_DIR_PREFIX, strlen(DMSH_VK_DIR_PREFIX))) continue;
            char p[1024]; struct stat st; vka_shlib t; char e2[256];
            if (snprintf(p, sizeof p, "%s/%s/" DMSH_VK_LIB_FILE, sd, de->d_name) >= (int)sizeof p || stat(p, &st) || !strcmp(p, path)) continue;
            if (vka_shlib_open(&t, p, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, e2, sizeof e2)) {
                vka_shlib_close(&t);
                if (!*best || st.st_mtime > bt) { snprintf(best, sizeof best, "%s", p); bt = st.st_mtime; }
                continue;
            }
            // A library made before the abi field (all zero): its ABI is not known, so it is the last resort, played with a warning rather than nothing drawn
            // (the user's choice: never block Play on it; the app suggests one more shader update, after which updates are deltas of the same ABI)
            int legacy = vka_shlib_open(&t, p, NULL, NULL, DM_GAME_ID, NULL, e2, sizeof e2);
            if (legacy) { static const char z[12]; legacy = !memcmp(t.h->abi, z, sizeof z); vka_shlib_close(&t); if (!legacy) snprintf(e2, sizeof e2, "another ABI than this host's %s", DMSH_VK_ABI); }
            if (!legacy) { VLOG("shader library %s not usable here: %s", p, e2); continue; }
            if (!*old || st.st_mtime > ot) { snprintf(old, sizeof old, "%s", p); ot = st.st_mtime; }
        }
        if (d) closedir(d);
        if (*best && vka_shlib_open(&g_lib, best, DMSH_CHAIN, DMSH_VK_CHAIN, DM_GAME_ID, DMSH_VK_ABI, err, sizeof err)) {
            char v[17]; vka_shlib_str(g_lib.h->vk_chain, v);
            VLOG("shader library %s: this host's version %s is %s (the app's shader update makes it); playing version %s with a compatible ABI (host %s)", path, DMSH_VK_CHAIN, why, v, DMSH_VK_ABI);
            snprintf(path, sizeof path, "%s", best); have = 1;
        } else if (!*best && *old && vka_shlib_open(&g_lib, old, NULL, NULL, DM_GAME_ID, NULL, err, sizeof err)) {
            char v[17]; vka_shlib_str(g_lib.h->vk_chain, v);
            VRED("shader library %s: this host's version %s is %s; playing version %s, made before versioned shader updates (no ABI in its header, this host's is %s): "
                 "it may draw wrong. Update the shaders once in the app; later updates are then partial", path, DMSH_VK_CHAIN, why, v, DMSH_VK_ABI);
            snprintf(path, sizeof path, "%s", old); have = 1;
        } else { VRED("no shader library at %s (the pack's shaders/" DMSH_VK_DIR_PREFIX "%s: %s) and none of another version with this host's ABI %s: nothing is drawn", path, DMSH_VK_CHAIN, why, DMSH_VK_ABI); return 0; }
    }
    if (!have) { VRED("shader library %s refused: %s; nothing is drawn", path, access(path, R_OK) ? "missing" : err); return 0; }
    snprintf(g_lib_path, sizeof g_lib_path, "%s", path);
    // The translator's answers in the header against this device: modules built for 64-lane subgroups split ballots and shuffles into 32-lane halves, so they are right on
    // 32 or 64 lanes; storage buffers are bound rounded down to ssbo_align, which the device's alignment must divide (android/SHADERS.md "Translator answers")
    unsigned sg = g_lib.h->subgroup_size ? g_lib.h->subgroup_size : 32, sa = g_lib.h->ssbo_align;
    sg_plan(sg); if (g_shuffle_lds < 0) shuffle_plan();   // (once a run: a selftest compute pipeline may have made the plan already)
    if (sa && (sa % V.sb_align)) { VRED("shader library %s refused: built for storage buffers at %u-byte boundaries, the device needs %llu; nothing is drawn", path, sa, (unsigned long long)V.sb_align); vka_shlib_close(&g_lib); return 0; }
    char inputs[17]; vka_shlib_str(g_lib.h->inputs, inputs);
    VLOG("shader library %s: %u shaders, %u modules, %.1f MB (chain %s, Vulkan chain %s, inputs %s; %u-lane subgroups, storage buffers at %u)", path, g_lib.h->entry_count, g_lib.h->mod_count,
         g_lib.size / 1e6, DMSH_CHAIN, DMSH_VK_CHAIN, inputs, sg, sa ? sa : 1);
    g_lib_state = 1;
    const char *ck = getenv("DM_VK_SHLIB_CHECK");
    if (ck && atoi(ck)) vka_shlib_check_all(&g_lib);
    return 1;
}

// ---------- Static memory: buffers that live as long as the process ----------
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; } stblk;
static stblk g_st;
static int stblk_make(VkDeviceSize size, stblk *out) {
    stblk b = {0}; b.size = size;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b.size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkMemoryRequirements mr; void *p = NULL; uint32_t t = UINT32_MAX;
    if (!vk.vkCreateBuffer(V.dev, &bi, NULL, &b.buf)) {
        vk.vkGetBufferMemoryRequirements(V.dev, b.buf, &mr);
        t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
        if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b.mem) || vk.vkBindBufferMemory(V.dev, b.buf, b.mem, 0) || vk.vkMapMemory(V.dev, b.mem, 0, VK_WHOLE_SIZE, 0, &p)) p = NULL;
    }
    if (!p) {
        if (b.mem) vk.vkFreeMemory(V.dev, b.mem, NULL);
        if (b.buf) vk.vkDestroyBuffer(V.dev, b.buf, NULL);
        VRED("static block of %llu bytes not allocated", (unsigned long long)b.size); return 0;
    }
    b.ptr = p; *out = b; return 1;
}
// A request over 4 MiB gets a block of its own and the shared 4 MiB block goes on being filled (DM_VK_STATIC_SPLIT=0: the old way, where the big block replaced the shared one,
// and the next small request opened a fresh 4 MiB block, abandoning the rest of the old one)
int vka_static(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    if (n > (4u << 20) && VKA_ENV("DM_VK_STATIC_SPLIT", 1)) {
        stblk b; if (!stblk_make((n + 0xFFFF) & ~(VkDeviceSize)0xFFFF, &b)) return 0;   // (kept for the process's life, as every static block: vka_static_block for one that is freed)
        *out = (vka_span){b.buf, b.ptr, 0}; return 1;
    }
    pthread_mutex_lock(&g_mu);
    VkDeviceSize o = (g_st.used + align - 1) / align * align;
    if (!g_st.buf || o + n > g_st.size) {
        stblk b; if (!stblk_make(n > (4u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 4u << 20, &b)) { pthread_mutex_unlock(&g_mu); return 0; }
        g_st = b; o = 0;   // the old block stays (its buffers are still bound by shaders)
    }
    g_st.used = o + n; *out = (vka_span){g_st.buf, g_st.ptr + o, o};
    pthread_mutex_unlock(&g_mu);
    return 1;
}
// A host-visible block of its own that its owner gives back (vka_mesh.inc's task payload): vka_static_block_free once no recorded command uses it any more (vka_after)
void *vka_static_block(VkDeviceSize n, vka_span *out) {
    stblk *b = malloc(sizeof *b); if (!b) return NULL;
    if (!stblk_make((n + 0xFFFF) & ~(VkDeviceSize)0xFFFF, b)) { free(b); return NULL; }
    b->used = n; *out = (vka_span){b->buf, b->ptr, 0}; return b;
}
void vka_static_block_free(void *h) {
    stblk *b = h; if (!b) return;
    vk.vkUnmapMemory(V.dev, b->mem); vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL); free(b);
}

// ---------- Texture pool reads: every library module made reports the pool IDs it reads (vka_spv_pool_fb; vka_tex.c "Texture pool reads") ----------
// Ported from Banana Native v0.3.11 (eden/v0.3.11 vka_shader.c csm_pfb). The hook sits over vkCreateShaderModule (installed after vka_dev.c's FPCR wrapper, so it calls
// through it), and every module of the library and every variant made from it at pipeline time (vertex inputs, outputs, textures retyped, geometry claim, the task / mesh
// fallback's compute and pulling modules) carries the code. Unlike upstream, only modules made by this file's library paths (lib_csm) are rewritten: the replay's own
// modules (present, FSR, NDSR, NaN scan, frame generation, the mesh controller, masked stencil clears) pass unchanged, so a host module with something at set 1 binding 0
// is never given a set 2 its pipeline layout lacks. A pixel module given EarlyFragmentTests (its stores would otherwise move its depth test after shading) gets a twin
// without them for the pipelines whose tests must stay late: alpha to coverage, or the alpha test on when its discard is the module's only one (vka_shader_pfb_pick,
// vka_pso.c). A module the rewrite refuses is used as it is: its pool reads report nothing (counted, logged, and vka_tex_pfb_blind puts the pool slots back to eager)
static PFN_vkCreateShaderModule g_csm_next; static PFN_vkDestroyShaderModule g_dsm_next;
static _Thread_local int t_lib_mod;   // the module being made is the library's (lib_csm)
typedef struct { VkShaderModule early, late; int kind; } pfb_twin;
static pfb_twin *g_tw; static size_t g_ntw, g_twcap; static pthread_mutex_t g_twmu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pfb_mods, g_pfb_sites, g_pfb_early, g_pfb_blind;
// Pool arrays fitted to this device's tables (vka_spv_array_len): the library declares the texture, sampler and image pools with the translator's lengths
// (DMSH_VK_POOL_*_COUNT: 2^20, 4096, 65536), and a device with lower update-after-bind limits has shorter tables (vka_tex.c bindless_init). Every library module goes
// through here, the pool-read rewrite's output included, so a pipeline never declares more elements than its layout binding has. Nothing changes on a device whose
// tables have the full lengths (every Adreno 8xx)
static _Atomic uint64_t g_fit_mods;
static VkResult csm_fit(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    static _Atomic int need = -1; static uint32_t nt, ns, ni;
    if (!t_lib_mod || !ci || !ci->pCode || ci->codeSize < 20) return g_csm_next(d, ci, a, m);
    int nd = atomic_load(&need);
    if (nd < 0) { uint32_t t, sp, im; vka_bindless_counts(&t, &sp, &im); nt = t; ns = sp; ni = im;
                  nd = (t && t < DMSH_VK_POOL_TEX_COUNT) | (sp && sp < DMSH_VK_POOL_SAMP_COUNT) << 1 | (im && im < 0x10000u) << 2; atomic_store(&need, nd); }
    if (!nd) return g_csm_next(d, ci, a, m);
    const uint32_t *w = ci->pCode; size_t nw = ci->codeSize / 4; uint32_t *own = NULL;
    static const uint32_t bind[3] = {DMSH_VK_POOL_TEX_BINDING, DMSH_VK_POOL_SAMP_BINDING, 2}; const uint32_t len[3] = {nt, ns, ni};
    for (int k = 0; k < 3; k++) if (nd >> k & 1) { size_t on = 0; uint32_t *o = vka_spv_array_len(w, nw, DMSH_VK_SET_POOL, bind[k], len[k], &on); if (o) { free(own); own = o; w = o; nw = on; } }
    if (!own) return g_csm_next(d, ci, a, m);
    uint64_t n = atomic_fetch_add(&g_fit_mods, 1) + 1;
    if (n == 1 || !(n & (n - 1))) VLOG("pool arrays: %llu modules fitted to this device's tables (%u textures, %u samplers, %u images; the library declares %u, %u, 65536)",
                                       (unsigned long long)n, nt, ns, ni, (unsigned)DMSH_VK_POOL_TEX_COUNT, (unsigned)DMSH_VK_POOL_SAMP_COUNT);
    VkShaderModuleCreateInfo c2 = *ci; c2.pCode = own; c2.codeSize = nw * 4;
    VkResult r = g_csm_next(d, &c2, a, m); free(own); return r;
}
static VkResult VKAPI_CALL csm_pfb(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    unsigned fl = t_lib_mod ? vka_tex_pfb_flags() : 0;
    if (!fl || !ci || !ci->pCode || ci->codeSize < 20) return csm_fit(d, ci, a, m);
    size_t on = 0; int sites = 0, kind = 0; char err[200] = "";
    uint32_t *o = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl, DMSH_VK_SPEC_ALPHA_FUNC, &on, &sites, &kind, err, sizeof err);
    if (!o) {
        if (err[0]) { uint64_t n = atomic_fetch_add(&g_pfb_blind, 1); if (n < 8) VRED("texture pool reads: a module's pool reads report nothing (%s): a texture it alone reads through the pool is not made", err); vka_tex_pfb_blind(); }
        return csm_fit(d, ci, a, m);
    }
    VkShaderModuleCreateInfo c2 = *ci; c2.codeSize = on * 4; c2.pCode = o;
    VkResult r = csm_fit(d, &c2, a, m);
    free(o);
    if (r) return r;
    uint64_t nm = atomic_fetch_add(&g_pfb_mods, 1) + 1; atomic_fetch_add(&g_pfb_sites, (uint64_t)sites);
    if (kind) {   // the twin without EarlyFragmentTests
        atomic_fetch_add(&g_pfb_early, 1);
        size_t ln = 0; int ls = 0, lk = 0; uint32_t *l = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl & ~(unsigned)VKA_PFB_EARLY,
                                                            DMSH_VK_SPEC_ALPHA_FUNC, &ln, &ls, &lk, err, sizeof err);
        VkShaderModule lm = VK_NULL_HANDLE;
        if (l) { c2.codeSize = ln * 4; c2.pCode = l; if (csm_fit(d, &c2, a, &lm)) lm = VK_NULL_HANDLE; free(l); }
        pthread_mutex_lock(&g_twmu);
        if (lm && g_ntw == g_twcap) { size_t nc = g_twcap ? 2 * g_twcap : 256; pfb_twin *n = realloc(g_tw, nc * sizeof *n); if (n) { g_tw = n; g_twcap = nc; } }
        if (lm && g_ntw < g_twcap) { g_tw[g_ntw++] = (pfb_twin){*m, lm, kind}; lm = VK_NULL_HANDLE; }
        pthread_mutex_unlock(&g_twmu);
        if (lm || !l) {   // no twin: the module without the mode serves every pipeline (late tests: correct, slower)
            if (lm) g_dsm_next(d, lm, a);
            g_dsm_next(d, *m, a); *m = VK_NULL_HANDLE;
            uint32_t *w2 = vka_spv_pool_fb(ci->pCode, ci->codeSize / 4, DMSH_VK_SET_POOL, DMSH_VK_POOL_TEX_BINDING, VKA_POOL_FB_SET, VKA_POOL_FB_BINDING, fl & ~(unsigned)VKA_PFB_EARLY, DMSH_VK_SPEC_ALPHA_FUNC, &ln, &ls, &lk, err, sizeof err);
            if (!w2) return csm_fit(d, ci, a, m);
            c2.codeSize = ln * 4; c2.pCode = w2; r = csm_fit(d, &c2, a, m); free(w2);
        }
    }
    if (nm == 1 || !(nm & (nm - 1)))
        VLOG("texture pool reads: %llu modules report the pool IDs they read (%llu reads given the code, %llu pixel modules with early depth tests and a twin without, %llu refused; DM_VK_POOL_FB=0 off)",
             (unsigned long long)nm, (unsigned long long)atomic_load(&g_pfb_sites), (unsigned long long)atomic_load(&g_pfb_early), (unsigned long long)atomic_load(&g_pfb_blind));
    return r;
}
static void VKAPI_CALL dsm_pfb(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) {
    VkShaderModule late = VK_NULL_HANDLE;
    if (m) { pthread_mutex_lock(&g_twmu);
             for (size_t i = 0; i < g_ntw; i++) if (g_tw[i].early == m) { late = g_tw[i].late; g_tw[i] = g_tw[--g_ntw]; break; }
             pthread_mutex_unlock(&g_twmu); }
    if (late) g_dsm_next(d, late, a);
    g_dsm_next(d, m, a);
}
void vka_shader_pfb_hook(void) {
    if (!vk.vkCreateShaderModule || vk.vkCreateShaderModule == csm_pfb || !vk.vkDestroyShaderModule) return;
    g_csm_next = vk.vkCreateShaderModule; vk.vkCreateShaderModule = csm_pfb;
    g_dsm_next = vk.vkDestroyShaderModule; vk.vkDestroyShaderModule = dsm_pfb;
}
VkShaderModule vka_shader_pfb_pick(VkShaderModule m, int a2c, int alpha_func) {
    if (!m) return m;
    VkShaderModule r = m;
    pthread_mutex_lock(&g_twmu);
    for (size_t i = 0; i < g_ntw; i++) if (g_tw[i].early == m) { if (a2c || (g_tw[i].kind == 2 && alpha_func)) r = g_tw[i].late; break; }
    pthread_mutex_unlock(&g_twmu);
    return r;
}
// Every subgroup shuffle as ShuffleXor by lane ^ index (vka_spv_shuffle_xor, from DroidPlay Core 8115593e): Qualcomm's shader compiler in its V@07xx drivers (the major
// version in driverVersion bits 12-21) crashes on OpGroupNonUniformShuffle while it builds a pipeline (libllvm-qgl.so, fault address 0x48; Adreno 650 at V@0746, Adreno
// 735 at V@0762), and builds the same module with the shuffle as ShuffleXor, which reads the same lane. V@0615 / V@0676 (730 / 740) and V@0800 and up build the shuffle.
// DM_VK_SHUFFLE_XOR (restart): default 1 on Qualcomm's own driver V@0700-0799, 0 elsewhere; 1 forces it on any driver (an A/B), 0 off. After every other rewrite
static int shuffle_xor_on(void) {
    static int v = -1;
    if (v < 0) { uint32_t ver = V.props.driverVersion >> 12 & 0x3ff;
                 v = VKA_ENV("DM_VK_SHUFFLE_XOR", V.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY && ver >= 700 && ver < 800) != 0;
                 if (v) VLOG("subgroup shuffles: made ShuffleXor by lane ^ index (driver %#x, V@%04u; DM_VK_SHUFFLE_XOR=0: as they are)", V.props.driverVersion, ver); }
    return v;
}
// Applied in lib_csm, so every library module and every variant made from one (retyped, claimed, fitted) takes it. NULL: the module as it is
static uint32_t *shuffle_xor(const VkShaderModuleCreateInfo *ci, size_t *on) {
    if (!shuffle_xor_on() || ci->codeSize < 20) return NULL;
    int ns = 0; char e[256] = ""; uint32_t *o = vka_spv_shuffle_xor(ci->pCode, ci->codeSize / 4, on, &ns, e, sizeof e);
    if (!o) { if (e[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("a module's shuffles left as they are, which this driver's compiler may crash on: %s", e); } return NULL; }
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("a module's %d shuffles made ShuffleXor (DM_VK_SHUFFLE_XOR)", ns);
    return o;
}
// A library module (or a variant made from one): the only modules the hook above rewrites
static VkResult lib_csm(const VkShaderModuleCreateInfo *ci, VkShaderModule *m) {
    size_t sn = 0; uint32_t *sx = shuffle_xor(ci, &sn); VkShaderModuleCreateInfo c2;
    if (sx) { c2 = *ci; c2.codeSize = sn * 4; c2.pCode = sx; ci = &c2; }
    t_lib_mod = 1; VkResult r = vk.vkCreateShaderModule(V.dev, ci, NULL, m); t_lib_mod = 0;
    free(sx); return r;
}

// ---------- Shaders by key ----------
typedef struct { uint64_t key; vka_shader *sh; const vka_shlib *lib; } shent;
static shent *g_sh; static size_t g_shcap, g_shn;
static size_t hslot(uint64_t k, size_t cap) { return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 17) & (cap - 1); }
static shent *sh_find(uint64_t key, const vka_shlib *L) {
    if (!g_shcap) return NULL;
    for (size_t i = hslot(key, g_shcap);; i = (i + 1) & (g_shcap - 1)) { if (!g_sh[i].sh) return NULL; if (g_sh[i].key == key && g_sh[i].lib == L) return &g_sh[i]; }
}
static void sh_put(uint64_t key, const vka_shlib *L, vka_shader *s) {
    if (2 * (g_shn + 1) > g_shcap) {
        size_t nc = g_shcap ? 2 * g_shcap : 1024; shent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_shcap; i++) if (g_sh[i].sh) { size_t j = hslot(g_sh[i].key, nc); while (n[j].sh) j = (j + 1) & (nc - 1); n[j] = g_sh[i]; }
        free(g_sh); g_sh = n; g_shcap = nc;
    }
    size_t j = hslot(key, g_shcap); while (g_sh[j].sh) j = (j + 1) & (g_shcap - 1);
    g_sh[j] = (shent){key, s, L}; g_shn++;
}

static const char *stage_name(int s) { static const char *n[] = {"vertex", "tess control", "tess evaluation", "geometry", "fragment", "compute", "task", "mesh"}; return s >= 0 && s < 8 ? n[s] : "?"; }
// What the replay cannot run yet, decided once per shader: refused shaders keep their entry, so the draws that use them are skipped and counted
static const char *refusal(const vka_shlib *L, const dmsh_vk_entry *e) {
    if (e->flags & (DMSH_VK_F_NV_PASSTHROUGH | DMSH_VK_F_NV_VIEWPORT)) return "NV-only geometry passthrough / viewport mask (a library built before the gs_passthrough answer)";
    if (e->flags & DMSH_VK_F_INTERLOCK) return "fragment shader interlock";
    if (e->stage == DMSH_VK_STAGE_GS && !V.geom) return "a geometry shader on a device without geometryShader";
    if ((e->stage == DMSH_VK_STAGE_TCS || e->stage == DMSH_VK_STAGE_TES) && !V.tess) return "a tessellation shader on a device without tessellationShader";
    // Native mesh support or a validated compute/pulling variant of the same program.
    if (e->stage >= DMSH_VK_STAGE_TS && !V.mesh) {
        if (!(e->flags & DMSH_VK_F_MESH_FALLBACK) || !vka_shlib_module(L, e, DMSH_VK_MOD_MESH_COMPUTE))
            return "a task / mesh shader without a compute fallback in this library; update Prepare";
        uint64_t inv = (uint64_t)e->local[0] * e->local[1] * e->local[2];
        if (!inv || inv > V.props.limits.maxComputeWorkGroupInvocations) return "an emulated task / mesh workgroup larger than the compute limit";
        for (unsigned i=0;i<3;i++) if (e->local[i] > V.props.limits.maxComputeWorkGroupSize[i]) return "an emulated task / mesh workgroup dimension exceeds the compute limit";
    }
    if (e->stage == DMSH_VK_STAGE_TS && V.mesh && !V.task) return "a task shader on a device with mesh shaders but without taskShader";
    if (V.mesh && (e->stage == DMSH_VK_STAGE_TS || e->stage == DMSH_VK_STAGE_MS)) {   // the workgroup (the entry's local) within the device's limit
        uint64_t inv = (uint64_t)e->local[0] * e->local[1] * e->local[2];
        if (!inv || inv > (e->stage == DMSH_VK_STAGE_TS ? V.task_inv : V.mesh_inv)) return "a task / mesh workgroup larger than the device's (maxTaskWorkGroupInvocations / maxMeshWorkGroupInvocations)";
    }
    const dmsh_vk_bind *b = vka_shlib_binds(L, e);
    for (unsigned i = 0; i < e->bind_count; i++) if (b[i].kind == DMSH_VK_KIND_UNKNOWN) return "a binding the build could not place";
    return NULL;
}
// A module of a stage sg_plan runs as one lane (no subgroup operations there on this device, or a subgroup size the library is not right for that cannot be pinned) has them
// rewritten as subgroups of one invocation (vka_spv_subgroup_single; Qualcomm's Adreno 840 driver has them in fragment, compute, task and mesh shaders, not in the vertex pipeline's). Logged once per shader key; a module that cannot be rewritten is used as it is, with a red line
static void single_lane(uint64_t key, int stage, uint32_t **w, size_t *nb) {
    if (stage < 0 || stage > 7 || !g_sg_one[stage]) return;   // sg_plan
    // A task or mesh workgroup uses its subgroup operations across the workgroup (output counts, compaction): one-lane subgroups give it wrong results. The Adreno 840's
    // driver has them in both stages at its fixed 64 (subgroupSupportedStages 0xf0), so this is a device the library was not made for
    if (stage >= DMSH_VK_STAGE_TS) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("%s shader %016llx: its subgroup operations run as one lane (the device's subgroups in this stage do not fit the library): its output is likely wrong", stage == DMSH_VK_STAGE_TS ? "task" : "mesh", (unsigned long long)key); }
    size_t on = 0; int ch = 0; char e[256] = ""; uint32_t *o = vka_spv_subgroup_single(*w, *nb / 4, &on, &ch, e, sizeof e);
    if (!o) { if (e[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx: subgroup operations in a stage without them not rewritten: %s", (unsigned long long)key, e); } return; }
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("shader %016llx: subgroup operations rewritten as one-invocation subgroups (%s)", (unsigned long long)key,
                                                            stage == DMSH_VK_STAGE_GS && V.driver_id == VK_DRIVER_ID_MESA_TURNIP ? "Turnip's geometry stage (DM_VK_SUBGROUP_GS)" : V.subgroup_stages & g_stage_bit[stage] ? "the device's subgroup size in this stage is not the library's and cannot be pinned" : "the device has none in this stage");
    free(*w); *w = o; *nb = on * 4;
}
// Shuffles of lane (lane & -32) | k as two constant-lane broadcasts (vka_spv_shuffle_halves): Turnip has no arbitrary-index shuffle and ir3 makes each such shuffle a loop of
// up to 32 rotates, the translator's form of Maxwell's SHFL.IDX of lane k on the library's 64-lane subgroups (Ocarina of Time's foliage mesh programs, 32 in a row each:
// their compute fallback 51.8 → 20.0 ms of a Kokiri frame on the Adreno 830). Only in a stage whose subgroups are 64 lanes (fixed, or pinned by sg_plan) and that keeps them
// (not one-lane, single_lane). DM_VK_SHUFFLE_HALVES (restart): default 1 on VK_DRIVER_ID_MESA_TURNIP, 0 on other drivers (Qualcomm's shuffles natively); 0 / 1 to choose
static void shuffle_halves(uint64_t key, int stage, uint32_t **w, size_t *nb) {
    if (stage < 0 || stage > 7 || g_sg_one[stage]) return;
    static int on = -1; if (on < 0) on = VKA_ENV("DM_VK_SHUFFLE_HALVES", V.driver_id == VK_DRIVER_ID_MESA_TURNIP) != 0;
    uint32_t sg = g_sg_pin[stage] ? g_sg_pin[stage] : V.subgroup_min == V.subgroup_max ? V.subgroup_size : 0;
    if (!on || sg != 64) return;
    size_t on_w = 0; int ns = 0; char e[256] = ""; uint32_t *o = vka_spv_shuffle_halves(*w, *nb / 4, &on_w, &ns, e, sizeof e);
    if (!o) { if (e[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx: lane-of-half shuffles not rewritten: %s", (unsigned long long)key, e); } return; }
    static _Atomic int nm, nsites; int m = atomic_fetch_add(&nm, 1) + 1, t = atomic_fetch_add(&nsites, ns) + ns;
    if (m <= 4 || !(m & (m - 1))) VLOG("shader %016llx: %d shuffles of a lane of the invocation's 32-lane half rewritten as broadcasts (DM_VK_SHUFFLE_HALVES; %d modules, %d shuffles so far)",
                                       (unsigned long long)key, ns, m, t);
    free(*w); *w = o; *nb = on_w * 4;
}
// A geometry shader that may emit nothing for a primitive emits a degenerate one instead (vka_spv_gs_fill: Qualcomm's driver stops the GPU when the binning and rendering
// passes of Adreno's tiled rendering disagree on what such a shader emitted; Banana Native 0749171c, Donkey Kong Bananza's terrain). DM_VK_GS_FILL (restart): default 1
// on Adreno drivers (Qualcomm's, Turnip), 0 elsewhere; 0 / 1 to choose. Applied after the one-lane subgroup rewrite, before the memory write claim. Logged for the first few
static void gs_fill(uint64_t key, int stage, uint32_t **w, size_t *nb) {
    if (stage != DMSH_VK_STAGE_GS) return;
    int adreno = V.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY || V.driver_id == VK_DRIVER_ID_MESA_TURNIP;
    if (!VKA_ENV("DM_VK_GS_FILL", adreno)) return;
    size_t on = 0; char e[256] = ""; uint32_t *o = vka_spv_gs_fill(*w, *nb / 4, &on, e, sizeof e);
    if (!o) { if (e[0]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("geometry shader %016llx: not made to emit a primitive where it emits none: %s", (unsigned long long)key, e); } return; }
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("geometry shader %016llx: emits a degenerate primitive where it would emit none (DM_VK_GS_FILL)", (unsigned long long)key);
    free(*w); *w = o; *nb = on * 4;
}
// DM_VK_SPV_OVERRIDE=<folder> (a diagnostic, android/TESTING.md): <folder>/<key>_<variant>.spv, when it is there, is that module's SPIR-V instead of the library's (a shader
// rewritten to show its inputs, say, in nxreplay). Logged once per module; the interface (locations, bindings) must stay the library's, which the pipeline layout follows
static uint32_t *spv_override(uint64_t key, unsigned variant, size_t *nb) {
    const char *d = getenv("DM_VK_SPV_OVERRIDE");
    if (!d || !*d) return NULL;
    char p[1100]; snprintf(p, sizeof p, "%s/%016llx_%u.spv", d, (unsigned long long)key, variant);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = n >= 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (w && fread(w, 1, (size_t)n, f) != (size_t)n) { free(w); w = NULL; }
    fclose(f);
    if (!w || w[0] != 0x07230203u) { VRED("DM_VK_SPV_OVERRIDE: %s is not a SPIR-V module: the library's is used", p); free(w); return NULL; }
    VLOG("DM_VK_SPV_OVERRIDE: shader %016llx variant %u from %s (%ld bytes)", (unsigned long long)key, variant, p, n);
    *nb = (size_t)n; return w;
}
// The byte offset of the primitive count in an emulated mesh shader's group structure: the last member of the element of the runtime array in the block at the
// fallback's output binding (DMSH_VK_MESH_OUT; vk_android_mesh.py appends the count after the outputs), read off the pulling vertex module's OpMemberDecorate
// Offset. -1 when the module does not have that shape (vka_mesh.inc then leaves the batch uncompacted)
static int mesh_count_offset(const uint32_t *w, size_t nw) {
    if (nw < 5) return -1;
    uint32_t var = 0, ptr = 0, block = 0, rta = 0, group = 0, last = UINT32_MAX;
    for (int pass = 0; pass < 4; pass++)
        for (size_t i = 5; i < nw;) {
            uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n || i + n > nw) return -1;
            const uint32_t *x = w + i;
            if (pass == 0 && op == 71 && n >= 4 && x[2] == 33 && x[3] == DMSH_VK_MESH_OUT) var = x[1];                      // OpDecorate Binding
            else if (pass == 1 && op == 59 && n >= 4 && x[2] == var) ptr = x[1];                                            // OpVariable
            else if (pass == 2 && op == 32 && n >= 4 && x[1] == ptr) block = x[3];                                          // OpTypePointer
            else if (pass == 3 && op == 30 && n >= 3 && x[1] == block) rta = x[2];                                          // OpTypeStruct: member 0
            i += n;
        }
    for (size_t i = 5; i < nw && rta;) { uint32_t n = w[i] >> 16; if (!n) return -1; if ((w[i] & 0xffff) == 29 && w[i + 1] == rta) group = w[i + 2]; i += n; }   // OpTypeRuntimeArray
    for (size_t i = 5; i < nw && group;) { uint32_t n = w[i] >> 16; if (!n) return -1; if ((w[i] & 0xffff) == 30 && w[i + 1] == group) last = n - 3; i += n; }   // members - 1
    if (last == UINT32_MAX) return -1;
    for (size_t i = 5; i < nw;) {
        uint32_t n = w[i] >> 16; if (!n) return -1;
        if ((w[i] & 0xffff) == 72 && n >= 5 && w[i + 1] == group && w[i + 2] == last && w[i + 3] == 35) return (int)w[i + 4];   // OpMemberDecorate Offset
        i += n;
    }
    return -1;
}
static void mesh_fragment(const dmsh_vk_entry *e, uint32_t *w, size_t *nb) {
    if (!V.mesh && e->stage == DMSH_VK_STAGE_FS && (e->flags & DMSH_VK_F_MESH_PRIM)) {
        size_t nw = *nb / 4;
        if (vka_spv_mesh_fragment(w, &nw)) *nb = nw * 4;
    }
}
static VkShaderModule spv_module(const vka_shlib *L, const dmsh_vk_entry *e, const dmsh_vk_module *m, uint64_t key, const char **why, char *err, size_t errn) {
    size_t nb = 0; uint32_t *w = spv_override(key, m->variant, &nb);
    if (!w) w = vka_shlib_spirv(L, m, &nb, err, errn);
    if (!w) { *why = err; return VK_NULL_HANDLE; }
    mesh_fragment(e, w, &nb);
    int stage = m->variant == DMSH_VK_MOD_MESH_COMPUTE ? DMSH_VK_STAGE_CS : m->variant == DMSH_VK_MOD_MESH_VERTEX ? DMSH_VK_STAGE_VS : e->stage;
    if (e->flags & DMSH_VK_F_SUBGROUP) { single_lane(key, stage, &w, &nb); shuffle_halves(key, stage, &w, &nb); }
    gs_fill(key, stage, &w, &nb);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nb, .pCode = w};
    VkShaderModule mod = VK_NULL_HANDLE; VkResult r = lib_csm(&ci, &mod);
    free(w);
    if (r) { snprintf(err, errn, "vkCreateShaderModule %d (variant %u of %016llx)", r, m->variant, (unsigned long long)key); *why = err; return VK_NULL_HANDLE; }
    return mod;
}
VkShaderModule vka_shader_module(vka_shader *s, int variant) {
    if (variant < 0 || variant > DMSH_VK_MOD_MAX) return VK_NULL_HANDLE;
    if (s->mods[variant]) return s->mods[variant];
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, variant);
    if (!m) return VK_NULL_HANDLE;
    const char *why = NULL; char err[256];
    VkShaderModule mod = spv_module(s->lib, s->e, m, s->key, &why, err, sizeof err);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("shader %016llx variant %d: %s", (unsigned long long)s->key, variant, why); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->mods[variant]) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->mods[variant]; }
    s->mods[variant] = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A geometry shader that writes memory, with those writes made by one run per primitive (vka_spv_gs_claim: the Adreno 840 runs a geometry shader several times for each
// primitive, each run writing memory). out NULL: only counts them. Made from the base module after the one-invocation subgroup rewrite
static int gs_words(vka_shader *s, uint32_t **out, size_t *on, char *err, size_t errn) {
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, errn) : NULL;
    if (!w) return -1;
    if (s->e->flags & DMSH_VK_F_SUBGROUP) single_lane(s->key, s->e->stage, &w, &nb);
    gs_fill(s->key, s->e->stage, &w, &nb);
    int r = vka_spv_gs_claim(w, nb / 4, VKA_GS_CLAIM_BINDING, out, on, err, errn);
    free(w); return r;
}
VkShaderModule vka_shader_gs_module(vka_shader *s) {
    if (s->gs_fx <= 0) return VK_NULL_HANDLE;
    if (s->gs_claim) return s->gs_claim;
    uint32_t *o = NULL; size_t on = 0; char err[256] = ""; VkShaderModule mod = VK_NULL_HANDLE;
    if (gs_words(s, &o, &on, err, sizeof err) > 0 && o) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
        VkResult r = lib_csm(&ci, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; }
    }
    free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("geometry shader %016llx: module with the claim not made: %s", (unsigned long long)s->key, err); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&g_mu);
    if (s->gs_claim) { pthread_mutex_unlock(&g_mu); vk.vkDestroyShaderModule(V.dev, mod, NULL); return s->gs_claim; }
    s->gs_claim = mod;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// A pixel shader whose only effect is its colour outputs (vka_spvpatch.h vka_spv_fs_inert), read once from its base module's code as the pipelines get it (the override,
// the mesh fragment rewrite and the one-lane subgroup rewrite applied, as spv_module does): 1 always, 2 with the alpha test off, 0 not or unreadable
int vka_shader_fs_inert(vka_shader *s) {
    if (!s || !s->e || s->e->stage != DMSH_VK_STAGE_FS) return 0;
    int k = __atomic_load_n(&s->fs_inert, __ATOMIC_RELAXED);
    if (k) return k - 1;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    char err[256]; size_t nb = 0; uint32_t *w = m ? spv_override(s->key, m->variant, &nb) : NULL;
    if (!w && m) w = vka_shlib_spirv(s->lib, m, &nb, err, sizeof err);
    int r = 0;
    if (w) {
        mesh_fragment(s->e, w, &nb);
        if (s->e->flags & DMSH_VK_F_SUBGROUP) single_lane(s->key, s->e->stage, &w, &nb);
        r = vka_spv_fs_inert(w, nb / 4, DMSH_VK_SPEC_ALPHA_FUNC);
        free(w);
    }
    __atomic_store_n(&s->fs_inert, r + 1, __ATOMIC_RELAXED);
    return r;
}
// The base vertex module with the inputs of umask / smask retyped (vka_spvpatch.c): made once per pair of masks, kept for the process like the base module
struct vka_vinmod { struct vka_vinmod *next; uint32_t u, s; VkShaderModule mod; };
VkShaderModule vka_shader_vin_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->vin; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;   // own: made here (the base module of the fallback is the shader's, never destroyed)
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w) mesh_fragment(s->e, w, &nb);
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage >= DMSH_VK_STAGE_TS && !V.mesh ? DMSH_VK_STAGE_CS : s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_vin_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = lib_csm(&ci, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("vertex shader %016llx: inputs %#x (uint) / %#x (int) not retyped: %s; the base module reads them as floats", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    // Two threads (the frame path and a prewarm or optimizer thread) may have made it at once: the first one in the list wins, the other's module is destroyed
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *o = s->vin; o; o = o->next) if (o->u == umask && o->s == smask) {
        VkShaderModule w = o->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != w) vk.vkDestroyShaderModule(V.dev, mod, NULL);   // (no pipeline was made from it: it never left this function)
        return w; }
    struct vka_vinmod *v = malloc(sizeof *v); *v = (struct vka_vinmod){s->vin, umask, smask, mod}; s->vin = v;
    pthread_mutex_unlock(&g_mu);
    return mod;
}
// The base pixel module with the float outputs of umask / smask retyped for integer attachments (vka_spvpatch.c vka_spv_fout_retype): made once per pair of masks, as the vertex ones
VkShaderModule vka_shader_fout_module(vka_shader *s, uint32_t umask, uint32_t smask) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *v = s->fout; v; v = v->next) if (v->u == umask && v->s == smask) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;
    const dmsh_vk_module *m = vka_shlib_module(s->lib, s->e, DMSH_VK_MOD_BASE);
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w) mesh_fragment(s->e, w, &nb);
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage >= DMSH_VK_STAGE_TS && !V.mesh ? DMSH_VK_STAGE_CS : s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_fout_retype(w, nb / 4, umask, smask, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = lib_csm(&ci, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    if (!mod) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("pixel shader %016llx: outputs %#x (uint) / %#x (int) not retyped for their integer targets: %s; the base module, whose float outputs those targets do not take", (unsigned long long)s->key, umask, smask, err);
                mod = vka_shader_module(s, DMSH_VK_MOD_BASE); }
    else { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("pixel shader %016llx: float outputs %#x (uint) / %#x (int) retyped for their integer targets (their bits written as they are)", (unsigned long long)s->key, umask, smask); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_vinmod *x = s->fout; x; x = x->next) if (x->u == umask && x->s == smask) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_vinmod *v = malloc(sizeof *v);
    if (v) { *v = (struct vka_vinmod){s->fout, umask, smask, mod}; s->fout = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
int vka_shader_fout_retyped(vka_shader *s, uint32_t umask, uint32_t smask) {
    VkShaderModule m = vka_shader_fout_module(s, umask, smask);
    return m && m != vka_shader_module(s, DMSH_VK_MOD_BASE);
}
// The compute module vka_shader_cs_module picks (its DMSH_VK_MOD_SHUFFLE_LDS module where shuffle_plan takes it, else the base) with the formatted storage images and
// sampled images of rt (set 0 bindings) as integer images for the integer textures bound there (vka_spvpatch.c vka_spv_img_retype): made once per rt, as the vertex ones.
// The cache is keyed by rt alone: the variant is fixed for the run (shuffle_plan reads DM_VK_SHUFFLE_LDS once, a restart switch). A module that cannot be rewritten
// falls back to the unretyped one, whose bindings then stay unbound (vka_draw.c img_retype_of)
struct vka_imgmod { struct vka_imgmod *next; vka_imgrt rt; VkShaderModule mod; };
VkShaderModule vka_shader_img_module(vka_shader *s, const vka_imgrt *rt) {
    pthread_mutex_lock(&g_mu);
    for (struct vka_imgmod *v = s->imgm; v; v = v->next) if (!memcmp(&v->rt, rt, sizeof *rt)) { VkShaderModule m = v->mod; pthread_mutex_unlock(&g_mu); return m; }
    pthread_mutex_unlock(&g_mu);
    VkShaderModule mod = VK_NULL_HANDLE; char err[256] = ""; int own = 0;
    vka_spv_imgsel sel[VKA_IMGRT_MAX]; int n = 0;
    for (; n < VKA_IMGRT_MAX && rt->b[n]; n++) sel[n] = (vka_spv_imgsel){0, rt->b[n] >> 16, (uint8_t)(rt->b[n] & 0xff), (uint8_t)(rt->b[n] >> 8 & 0xff)};
    int var = vka_shader_cs_variant(s);
    const dmsh_vk_module *m = n ? vka_shlib_module(s->lib, s->e, var) : NULL;
    size_t nb = 0; uint32_t *w = m ? vka_shlib_spirv(s->lib, m, &nb, err, sizeof err) : NULL;
    if (w) mesh_fragment(s->e, w, &nb);
    if (w && (s->e->flags & DMSH_VK_F_SUBGROUP)) single_lane(s->key, s->e->stage >= DMSH_VK_STAGE_TS && !V.mesh ? DMSH_VK_STAGE_CS : s->e->stage, &w, &nb);
    size_t on = 0; uint32_t *o = w ? vka_spv_img_retype(w, nb / 4, sel, n, &on, err, sizeof err) : NULL;
    if (o) { VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = on * 4, .pCode = o};
             VkResult r = lib_csm(&ci, &mod); if (r) { snprintf(err, sizeof err, "vkCreateShaderModule %d", r); mod = VK_NULL_HANDLE; } else own = 1; }
    free(w); free(o);
    char q[160]; size_t qn = 0; q[0] = 0;
    for (int k = 0; k < n && qn < sizeof q; k++) qn += (size_t)snprintf(q + qn, sizeof q - qn, "%s%u %s (format %u)", k ? ", " : "", rt->b[k] >> 16, (rt->b[k] & 0xff) == 1 ? "uint" : "sint", rt->b[k] >> 8 & 0xff);
    if (!mod) { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VRED("shader %016llx: images (storage or sampled) at bindings %s not retyped for their integer textures: %s; they read as unbound", (unsigned long long)s->key, q, err);
                mod = vka_shader_module(s, var); }
    else { static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VLOG("shader %016llx: images (storage or sampled) at bindings %s retyped for their integer textures (texels as the hardware returns them)%s", (unsigned long long)s->key, q,
                                                                     var == DMSH_VK_MOD_SHUFFLE_LDS ? " in its shuffle_lds module" : ""); }
    pthread_mutex_lock(&g_mu);
    for (struct vka_imgmod *x = s->imgm; x; x = x->next) if (!memcmp(&x->rt, rt, sizeof *rt)) {
        VkShaderModule had = x->mod; pthread_mutex_unlock(&g_mu);
        if (own && mod != had) vk.vkDestroyShaderModule(V.dev, mod, NULL);
        return had; }
    struct vka_imgmod *v = malloc(sizeof *v);
    if (v) { *v = (struct vka_imgmod){s->imgm, *rt, mod}; s->imgm = v; }
    pthread_mutex_unlock(&g_mu);
    return mod;
}
int vka_shader_img_retyped(vka_shader *s, const vka_imgrt *rt) {
    VkShaderModule m = vka_shader_img_module(s, rt);   // (a fallback is the unretyped module of its variant, made before it: no new module made here)
    return m && m != s->mods[DMSH_VK_MOD_BASE] && m != s->mods[DMSH_VK_MOD_SHUFFLE_LDS];
}
vka_shader *vka_shader_make(const vka_shlib *L, uint64_t key) {
    pthread_mutex_lock(&g_mu);
    shent *h = sh_find(key, L);
    if (h) { vka_shader *s = h->sh; pthread_mutex_unlock(&g_mu); return s->bad ? NULL : s; }
    pthread_mutex_unlock(&g_mu);
    vka_shader *s = calloc(1, sizeof *s); s->key = key; s->lib = L;
    const dmsh_vk_entry *e = vka_shlib_find(L, key);
    const char *why = NULL; char err[256] = "";
    if (!e) why = "not in the shader library";
    else {
        s->e = e; s->b = vka_shlib_binds(L, e);
        why = refusal(L, e);
        int emulated = !V.mesh && e->stage >= DMSH_VK_STAGE_TS;
        const dmsh_vk_module *m = why ? NULL : vka_shlib_module(L, e, emulated ? DMSH_VK_MOD_MESH_COMPUTE : DMSH_VK_MOD_BASE);
        if (!why && !m) why = "no base module";
        if (m) s->mods[DMSH_VK_MOD_BASE] = spv_module(L, e, m, key, &why, err, sizeof err);
        if (m && emulated) s->mods[DMSH_VK_MOD_MESH_COMPUTE] = s->mods[DMSH_VK_MOD_BASE];
        if (!why && emulated && e->stage == DMSH_VK_STAGE_MS) {
            VkShaderModule vs = vka_shader_module(s, DMSH_VK_MOD_MESH_VERTEX);
            if (!vs) why = "no pulling vertex module for emulated mesh shader";
            else {
                vka_shader *v = calloc(1, sizeof *v); dmsh_vk_entry *ve = malloc(sizeof *ve);
                if (!v || !ve) { free(v); free(ve); why = "no mesh vertex metadata memory"; }
                else { *ve = *e; ve->stage = DMSH_VK_STAGE_VS; ve->bind_count = 0; ve->vin_mask = 0; ve->vin_types = 0;
                    ve->flags = DMSH_VK_F_MESH_FALLBACK | DMSH_VK_F_GL_DEPTH;
                    v->key = key; v->e = ve; v->lib = L; v->mods[DMSH_VK_MOD_BASE] = vs; s->mesh_vertex = v;
                    const dmsh_vk_module *pm = vka_shlib_module(L, e, DMSH_VK_MOD_MESH_VERTEX); size_t pn = 0; char pe[64];
                    uint32_t *pw = pm ? vka_shlib_spirv(L, pm, &pn, pe, sizeof pe) : NULL;
                    s->mesh_count_off = pw ? mesh_count_offset(pw, pn / 4) : -1; free(pw);
                    if (s->mesh_count_off < 0) { static _Atomic int nc; if (atomic_fetch_add(&nc, 1) < 4) VLOG("mesh shader %016llx: no primitive count offset in its pulling module: its batches are not compacted", (unsigned long long)key); } }
            }
        }
        if (!why && e->stage == DMSH_VK_STAGE_GS && VKA_ENV("DM_VK_GS_CLAIM", 1)) {   // memory writes: made once per primitive (vka_spv_gs_claim)
            char e2[256] = ""; s->gs_fx = gs_words(s, NULL, NULL, e2, sizeof e2);
            if (s->gs_fx < 0) VRED("geometry shader %016llx: memory writes not put under a claim (%s): drawn as it is, its writes repeated by the device's runs of each primitive", (unsigned long long)key, e2);
            else if (s->gs_fx) VLOG("geometry shader %016llx: %d memory writes, made by one run per primitive (a claim word per primitive at set 0 binding %d)", (unsigned long long)key, s->gs_fx, VKA_GS_CLAIM_BINDING);
        }
        if (!why && e->c1_len) {   // the c1 bytes: a uniform buffer of their own, made once
            if (vka_static(e->c1_len, V.ub_align, &s->c1)) { memcpy(s->c1.ptr, vka_shlib_c1(L, e), e->c1_len); s->c1_len = e->c1_len; }
            else why = "no static memory for its c1 bytes";
        }
    }
    if (why) {
        s->bad = 1;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64) VRED("shader %016llx (%s): %s; its draws are skipped", (unsigned long long)key, e ? stage_name(e->stage) : "?", why);
    }
    pthread_mutex_lock(&g_mu);
    h = sh_find(key, L);
    if (h) { if (s->mods[0]) vk.vkDestroyShaderModule(V.dev, s->mods[0], NULL); free(s); s = h->sh; }   // another thread made it meanwhile
    else sh_put(key, L, s);
    pthread_mutex_unlock(&g_mu);
    return s->bad ? NULL : s;
}

// The game's library, loaded if it is not yet (vka_pso.c vka_pso_warm: the warm has no draw whose shaders would load it); NULL = none usable
const vka_shlib *vka_shader_game_lib(void) {
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    return ok ? &g_lib : NULL;
}
// A shader of the game's library by key, the library loaded if it is not yet (the prewarm threads, vka_pso.c); NULL = no library, not in it, or refused
vka_shader *vka_shader_by_key(uint64_t key) {
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    return ok ? vka_shader_make(&g_lib, key) : NULL;
}

// Every shader of a library through the device's compiler: each module made, each stage's set-0 layout made, each compute shader's pipeline built (graphics pipelines need
// a pair and state, so vertex and pixel shaders stop at the module and layout). DM_VK_SHLIB_CHECK=1 runs it when the game's library loads; the selftest runs it on DM_VK_SHLIB.
// Returns the number of failures; one log line per failure kind and a summary
int vka_shlib_check_all(const vka_shlib *L) {
    // Standalone checks must use the same subgroup plan as normal library loading.
    sg_plan(L->h->subgroup_size ? L->h->subgroup_size : 32);
    if (g_shuffle_lds < 0) shuffle_plan();
    unsigned n = L->h->entry_count, made = 0, refused = 0, nolayout = 0, cs = 0, csfail = 0; uint64_t t0 = now_ns();
    const char *only=getenv("DM_VK_SHLIB_CHECK_KEY");uint64_t selected=only?strtoull(only,NULL,16):0;
    for (unsigned i = 0; i < n; i++) {
        const dmsh_vk_entry *e = &L->ent[i];
        if(selected && e->key!=selected)continue;
        vka_shader *s = vka_shader_make(L, e->key);
        if (!s) { refused++; continue; }
        made++;
        for (int v = 1; v <= DMSH_VK_MOD_SHUFFLE_LDS; v++) if (vka_shlib_module(L, e, v) && !vka_shader_module(s, v)) nolayout++;   // the variants through the compiler too (counted as failures)
        vka_shader *sh[VKA_NSTAGE] = {0}; int st = vka_nvn_stage(e->stage); if (st < 0) { nolayout++; continue; } sh[st] = s;
        vka_layout lay;
        if (!vka_layout_of(sh, st == 5, &lay)) { nolayout++; continue; }
        if (st == 5 || (!V.mesh && e->stage>=DMSH_VK_STAGE_TS)) { uint64_t t = now_ns(); cs++; if (!vka_cs_pipeline(s, &lay)) csfail++;
                       if (cs <= 3 || cs % 25 == 0) VLOG("shader library check: compute pipeline %u (%016llx) %.0f ms; %u / %u shaders", cs, (unsigned long long)e->key, (now_ns() - t) / 1e6, i + 1, n); }
        if ((i + 1) % 2000 == 0) VLOG("shader library check: %u / %u shaders, %.1f s", i + 1, n, (now_ns() - t0) / 1e9);
    }
    VLOG("shader library check: %u shaders, %u modules made, %u refused, %u without a layout, %u compute pipelines (%u failed), %.1f s",
         n, made, refused, nolayout, cs, csfail, (now_ns() - t0) / 1e9);
    vka_pso_save(1);
    return (int)(nolayout + csfail);
}

// ---------- Programs ----------
// By serial (a new serial at every nvnProgramSetShaders; mtl_prog_release drops it): the program's stage keys resolved to shaders, each at the NVN stage its entry names
typedef struct { uint64_t serial; vka_progsh ps; } prent;
static prent *g_pr; static size_t g_prcap, g_prn;
static prent *pr_find(uint64_t s) {
    if (!g_prcap) return NULL;
    for (size_t i = hslot(s, g_prcap);; i = (i + 1) & (g_prcap - 1)) { if (!g_pr[i].serial) return NULL; if (g_pr[i].serial == s) return &g_pr[i]; }
}
static void pr_put(uint64_t s, const vka_progsh *ps) {
    prent *e = pr_find(s); if (e) { e->ps = *ps; return; }
    if (2 * (g_prn + 1) > g_prcap) {
        size_t nc = g_prcap ? 2 * g_prcap : 1024; prent *n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_prcap; i++) if (g_pr[i].serial && g_pr[i].serial != ~0ull) { size_t j = hslot(g_pr[i].serial, nc); while (n[j].serial) j = (j + 1) & (nc - 1); n[j] = g_pr[i]; }
        free(g_pr); g_pr = n; g_prcap = nc; size_t live = 0; for (size_t i = 0; i < nc; i++) live += n[i].serial != 0; g_prn = live;
    }
    size_t j = hslot(s, g_prcap); while (g_pr[j].serial && g_pr[j].serial != ~0ull) j = (j + 1) & (g_prcap - 1);
    if (!g_pr[j].serial) g_prn++;
    g_pr[j] = (prent){s, *ps};
}
void vka_prog_put(uint64_t serial, const vka_progsh *ps) { pthread_mutex_lock(&g_mu); pr_put(serial, ps); pthread_mutex_unlock(&g_mu); }
void mtl_prog_release(prog_t *p) {
    if (!p || !p->serial) return;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); if (e) { e->serial = ~0ull; memset(&e->ps, 0, sizeof e->ps); } pthread_mutex_unlock(&g_mu);   // a tombstone
}
void mtl_prog_prefetch(const prog_t *p) { (void)p; }   // modules are made at the program's first draw (vka_prog_shaders)

int vka_prog_shaders(const prog_t *p, vka_progsh *out) {
    memset(out, 0, sizeof *out);
    if (!p || !p->serial) return 0;
    pthread_mutex_lock(&g_mu); prent *e = pr_find(p->serial); if (e) *out = e->ps; pthread_mutex_unlock(&g_mu);
    if (e) return 1;
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    if (!ok) return 0;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {   // (a program has at most 6 shaders: hle_nvn.c nvnProgramSetShaders)
        uint8_t md[8];
        if (!prog_key(p, i, md)) continue;
        uint64_t key = vka_shlib_key(md);
        const dmsh_vk_entry *en = vka_shlib_find(&g_lib, key);
        if (!en) {
            out->bad |= VKA_BAD_MISSING; static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) VRED("shader %016llx (program stage %u) is not in the shader library", (unsigned long long)key, i);
            // DM_NVN_SHDUMP=directory, as nvn_mtl.m: <key>.ctl (the whole NVN2 control section) and <key>.code (the code area), which shader_split.py nvn_dumps takes into the map
            static const char *shd; static int shd_init; if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; if (shd && *shd) mkdir(shd, 0777); }
            if (shd && *shd && p->sd[i].control) {
                char path[1024]; FILE *f; uint32_t ncode, nctl; nvn_ctl_lens(p->sd[i].control, &nctl, &ncode);
                if (nctl == 0x48) nctl = 0xd8;   // NVN2: the whole section (its stage byte +0x56, thread group +0x58); hle_nvn.c keeps a copy of it with SHDUMP set
                snprintf(path, sizeof path, "%s/%016llx.ctl", shd, (unsigned long long)key);
                if (access(path, F_OK) && (f = fopen(path, "wb"))) {
                    fwrite(p->sd[i].control, 1, nctl, f); fclose(f);
                    snprintf(path, sizeof path, "%s/%016llx.code", shd, (unsigned long long)key);
                    if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); }
                }
            }
            continue;
        }
        int st = vka_nvn_stage(en->stage);
        if (st < 0) { out->bad |= VKA_BAD_MISSING; continue; }   // (vka_shlib_open refuses a stage past DMSH_VK_STAGE_MS)
        if (out->sh[st]) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("program serial %llu has two %s shaders (%016llx, %016llx): the second is used", (unsigned long long)p->serial, stage_name(en->stage), (unsigned long long)out->sh[st]->key, (unsigned long long)key); }
        out->sh[st] = vka_shader_make(&g_lib, key);
        if (!out->sh[st]) out->bad |= 1u << st;
    }
    vka_prog_put(p->serial, out);
    return 1;
}
unsigned vka_prog_stage_mask(const prog_t *p) {
    if (!p || !p->nstage) return 0;
    static struct { uint64_t serial; unsigned mask; } memo[256];   // (a program's serial is new at each SetShaders: hle_nvn.c)
    unsigned h = (unsigned)(p->serial ^ p->serial >> 8) & 255u;
    if (p->serial && memo[h].serial == p->serial) return memo[h].mask;
    pthread_mutex_lock(&g_mu); int ok = lib_load(); pthread_mutex_unlock(&g_mu);
    if (!ok) return VKA_ST_UNKNOWN;
    unsigned m = 0;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        uint8_t md[8];
        if (!prog_key(p, i, md)) { m |= VKA_ST_UNKNOWN; continue; }
        const dmsh_vk_entry *en = vka_shlib_find(&g_lib, vka_shlib_key(md));
        int st = en ? vka_nvn_stage(en->stage) : -1;
        m |= st >= 0 ? 1u << st : VKA_ST_UNKNOWN;
    }
    if (p->serial) { memo[h].serial = p->serial; memo[h].mask = m; }
    return m;
}
#endif
