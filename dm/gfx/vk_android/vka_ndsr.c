// agl's NDSR upscaler network replaced by FSR 1 EASU on the Vulkan replay (DM_VK_NDSR=fsr; docs/reference/rendering.md "agl's NDSR upscaler network replaced by FSR 1
// EASU on the Vulkan replay"). agl's light upscaler (agl_ndsr_vr_light.pck; Zelda: Ocarina of Time's fa3033d05c0a41d6) is a purely spatial network: one sampled input,
// one storage output, its weights in a storage buffer, and its sizes in the push bank (no history, motion or depth). Its HMMA blocks have no tensor cores to run on
// here: about 7.2 ms of a 33 ms frame on the Adreno 830. This is a deliberate workaround for that platform limit, opt-in and with a quality loss: the dispatch keeps
// its slot, descriptors and group count, and its pipeline is our own compute (fsr/ndsr_easu.comp), FSR 1 EASU from the network's input to its output. The three
// cheap anti-aliasing passes before it (edge classification, blend weights, the blend that packs the input) stay as they are. An output format without storage
// support (Zelda's B10G11R11_UFLOAT on Adreno, where a float storage view of it is refused and its stores go nowhere) takes the packed variant: each texel's 32-bit
// word stored through an R32_UINT view, the binding entered in the dispatch's vka_imgrt so stage_writes asks for that view (vka_ndsr_pipeline).
// Switches (live): DM_VK_NDSR = net (default: the network) | fsr (EASU) | bilinear; DM_VK_NDSR_INPUT = linear (default) | sqrt (the input holds sqrt(colour), the
// output the colour: agl's light upscaler, whose blend a335c1db6270c355 packs sqrt as RGB9E5); DM_VK_NDSR_MAP = full (default: the whole input onto the whole
// output) | push (agl's push bank mapping, experimental); DM_VK_NDSR_LOG=1: the push bank once per input and output size; DM_VK_EASU_FULL=1: AMD's full EASU
// instead of the lighter one (fsr/fsr_easu_light.h; docs/reference/rendering.md).
// DM_VK_NDSR=gsr2 (off by default, experimental): a temporal upscaler in the slot, Snapdragon GSR2 3-pass on camera motion from depth ("The temporal path" below), FSR 1
// EASU where it cannot run; DM_VK_NDSR_CAM_LOG=<frames> (diagnostic, any mode): the scene camera block and the depth it would use, at that many dispatches
#include "vka_ndsr.h"
#include "vka_ndsr_spv.h"
#include <stdlib.h>
#include <string.h>

// ---------- Plain C (the PC test builds this part with VKA_NDSR_PURE) ----------
#define DESC_COMBINED 1   // VkDescriptorType: VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define DESC_STORAGE_IMAGE 3
#define DESC_UNIFORM_BUFFER 6
int vka_ndsr_match(const dmsh_vk_entry *e, const dmsh_vk_bind *b, uint16_t *tex_b, uint16_t *img_b, uint16_t *bank_b) {
    if (!e || !b || e->stage != DMSH_VK_STAGE_CS || !(e->flags & DMSH_VK_F_HMMA)) return 0;
    int nt = 0, ni = 0, ns = 0, nb = 0;
    const uint8_t plain = DMSH_VK_DIM_ARRAY | DMSH_VK_DIM_MS | DMSH_VK_DIM_DEPTH | DMSH_VK_DIM_SINT | DMSH_VK_DIM_UINT;
    for (unsigned i = 0; i < e->bind_count; i++) {
        const dmsh_vk_bind *x = &b[i];
        switch (x->kind) {
        case DMSH_VK_KIND_TEX:
            if (x->desc != DESC_COMBINED || !x->skind || (x->dim & DMSH_VK_DIM_MASK) != 1 || (x->dim & plain) || x->count > 1) return 0;
            nt++; *tex_b = x->binding; break;
        case DMSH_VK_KIND_IMG:
            if (x->desc != DESC_STORAGE_IMAGE || !(x->flags & DMSH_VK_BF_RW) || (x->flags & DMSH_VK_BF_ATOMIC) || (x->dim & DMSH_VK_DIM_MASK) != 1 || (x->dim & plain) || x->count > 1) return 0;
            ni++; *img_b = x->binding; break;
        case DMSH_VK_KIND_SBUF:
            if (x->flags & DMSH_VK_BF_RW) return 0;
            ns++; break;
        case DMSH_VK_KIND_DRVCB:
            if (x->reg == DMSH_PUSH_BANK) { if (x->desc != DESC_UNIFORM_BUFFER) return 0; nb++; *bank_b = x->binding; }
            break;
        case DMSH_VK_KIND_SUPPORT: break;
        default: return 0;   // anything else (a shared memory spill buffer, global memory, pools, more textures): not this interface
        }
    }
    return nt == 1 && ni == 1 && ns == 1 && nb == 1;
}
int vka_ndsr_pack_of(int vkformat) { return vkformat == 122 ? VKA_NDSR_PACK_B10G11R11 : vkformat == 37 ? VKA_NDSR_PACK_RGBA8 : VKA_NDSR_PACK_NONE; }   // (VkFormat numbers)
uint32_t *vka_ndsr_words(int packed, uint16_t tex_b, uint16_t img_b, uint16_t bank_b, size_t *n) {
    const uint32_t *src = packed ? vka_ndsr_easu_packed : vka_ndsr_easu;
    size_t bytes = packed ? sizeof vka_ndsr_easu_packed : sizeof vka_ndsr_easu, nw = bytes / 4;
    uint32_t *w = malloc(bytes); if (!w) return NULL;
    memcpy(w, src, bytes);
    int seen = 0;
    for (size_t i = 5; i < nw;) {   // (after the header) OpDecorate <id> Binding <n>: opcode 71, decoration 33
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > nw) { free(w); return NULL; }
        if (op == 71 && len == 4 && w[i + 2] == 33) {
            uint32_t v = w[i + 3];
            if (v == 100) { w[i + 3] = tex_b; seen |= 1; } else if (v == 101) { w[i + 3] = img_b; seen |= 2; } else if (v == 102) { w[i + 3] = bank_b; seen |= 4; }
        }
        i += len;
    }
    if (seen != 7) { free(w); return NULL; }
    *n = nw; return w;
}

#ifndef VKA_NDSR_PURE
// ---------- The replay ----------
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "vka_ndsr_tsr_spv.h"   // the temporal path's motion pass and the no-op network dispatch

static struct { int mode, enc, map, log, full, camlog; } g_sw;   // mode: 0 net, 1 fsr, 2 bilinear, 3 nearest, 4 gsr2 (the temporal path)
static uint32_t g_sw_gen;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_tsr_want;   // the switches ask for the camera or the depth (gsr2 or the camera log): vka_ndsr_note_rt records only then
uint64_t vka_frame(void);
static void switches(void) {   // read again after a live change (vka_env_gen), under g_mu
    uint32_t g = atomic_load(&vka_env_gen);
    if (g == g_sw_gen) return;
    g_sw_gen = g;
    char v[64];
    g_sw.mode = !vka_env_str("DM_VK_NDSR", v, sizeof v) || !strcmp(v, "net") || !strcmp(v, "0") ? 0 : !strcmp(v, "bilinear") ? 2 : !strcmp(v, "nearest") ? 3 : !strcmp(v, "fsr") || !strcmp(v, "1") ? 1 :
                !strcmp(v, "gsr2") ? 4 : -1;
    if (g_sw.mode < 0) { VRED("DM_VK_NDSR=%s: not net, fsr, bilinear, nearest or gsr2; the network runs", v); g_sw.mode = 0; }
    g_sw.enc = vka_env_str("DM_VK_NDSR_INPUT", v, sizeof v) && !strcmp(v, "sqrt") ? VKA_NDSR_ENC_SQRT : VKA_NDSR_ENC_LINEAR;
    g_sw.map = vka_env_str("DM_VK_NDSR_MAP", v, sizeof v) && !strcmp(v, "push") ? VKA_NDSR_MAP_PUSH : VKA_NDSR_MAP_FULL;
    g_sw.log = vka_env_str("DM_VK_NDSR_LOG", v, sizeof v) && atoi(v) > 0;
    g_sw.full = vka_env_str("DM_VK_EASU_FULL", v, sizeof v) && atoi(v) > 0;
    g_sw.camlog = vka_env_str("DM_VK_NDSR_CAM_LOG", v, sizeof v) ? atoi(v) : 0;
    if (g_sw.camlog < 0) g_sw.camlog = 0;
    atomic_store(&g_tsr_want, g_sw.mode == 4 || g_sw.camlog > 0);
}

// Pipelines made, by shader, layout and modes; the sizes logged, by shader and modes; the shaders refused (logged once each). pack: VKA_NDSR_PACK_* (NONE: the float store)
enum { NPIPE = 64, NLOG = 64, NREF = 32 };
static struct { uint64_t key; VkPipelineLayout layout; int enc, map, kernel, pack; VkPipeline p; } g_pipe[NPIPE];
static int g_npipe;
static struct { uint64_t key; uint32_t iw, ih, ow, oh; int enc, map, kernel, pack, push; } g_log[NLOG];
static int g_nlog;
static uint64_t g_ref[NREF]; static int g_nref;
static void refused(uint64_t key, const char *why) {   // under g_mu
    for (int i = 0; i < g_nref; i++) if (g_ref[i] == key) return;
    if (g_nref < NREF) g_ref[g_nref++] = key;
    VRED("NDSR network %016llx not replaced (DM_VK_NDSR): %s; the network runs", (unsigned long long)key, why);
}

static VkPipeline make(vka_shader *cs, const vka_layout *lay, uint16_t tb, uint16_t ib, uint16_t bb, int enc, int map, int kernel, int pack) {   // under g_mu
    for (int i = 0; i < g_npipe; i++)
        if (g_pipe[i].key == cs->key && g_pipe[i].layout == lay->layout && g_pipe[i].enc == enc && g_pipe[i].map == map && g_pipe[i].kernel == kernel && g_pipe[i].pack == pack) return g_pipe[i].p;
    if (g_npipe == NPIPE) { refused(cs->key, "more than 64 pipelines"); return VK_NULL_HANDLE; }
    size_t nw = 0; uint32_t *w = NULL;
    if (kernel == VKA_NDSR_KERNEL_NOOP) { nw = sizeof vka_ndsr_noop / 4; if ((w = malloc(sizeof vka_ndsr_noop))) memcpy(w, vka_ndsr_noop, sizeof vka_ndsr_noop); }   // (no bindings to place)
    else w = vka_ndsr_words(pack != VKA_NDSR_PACK_NONE, tb, ib, bb, &nw);
    VkPipeline p = VK_NULL_HANDLE; VkShaderModule m = VK_NULL_HANDLE; VkResult r = VK_ERROR_UNKNOWN;
    if (w) {
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = w};
        r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        free(w);
    }
    if (!r) {
        int32_t sv[4] = {enc, map, kernel, pack ? pack : VKA_NDSR_PACK_B10G11R11};   // (the float module has no constant 3: an entry for an absent ID is allowed)
        VkSpecializationMapEntry se[4] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}, {3, 12, 4}};
        VkSpecializationInfo si = {4, se, sizeof sv, sv};
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = lay->layout,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main", .pSpecializationInfo = &si}};
        r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);   // (a small module: no pipeline cache)
        vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (r) p = VK_NULL_HANDLE;
    }
    if (!p) VRED("NDSR network %016llx: its FSR 1 pipeline was not made (%s %d); the network runs", (unsigned long long)cs->key, w ? "vkCreate*" : "no placeholder bindings in the module", r);
    g_pipe[g_npipe++] = (typeof(g_pipe[0])){cs->key, lay->layout, enc, map, kernel, pack, p};   // (a failure is kept too: not tried again)
    return p;
}

// ---------- The temporal path (DM_VK_NDSR=gsr2) and the camera diagnostic (DM_VK_NDSR_CAM_LOG) ----------
// Step B of docs/ANALISE-SGSR2-FSR31-FRAMEGEN-2026-11-16.md. agl's NDSR network is spatial and the game hands it no motion, so the temporal path makes the motion of
// the camera from the scene's depth: this frame's point from its depth, taken through prev_VP * inverse(VP) (vka_tsr_cam.h, in double) to the previous frame's
// (fsr/ndsr_tsr.comp), and runs Snapdragon GSR2 3-pass (vka_dlss.c vka_dlss_gsr2_views) from the network's input to its output; the network's own dispatch then runs
// a no-op in its slot. No jitter (the game has none): GSR2 stabilizes and smooths what moves with the camera; Link and the NPCs move by themselves, which the depth
// clip and velocity rejection of GSR2's tune catch (ghosting is what to look at). The camera lives in a game's scene constant block, so it is read only on the builds
// whose layout is known (k_tsr_cam, by the main module's build ID); without one, or without a camera, depth or output it can use, the dispatch runs FSR 1 EASU as
// DM_VK_NDSR=fsr does. The history starts again on a new size, another depth image, a frame not right after the last, or a camera cut (vka_tsr_cut).
// Inputs found: the camera block among the uniform buffers bound at the dispatch (the layout's stage and slot first, then any graphics stage's buffer of the block's
// size); the depth: the last D32F depth target of the input's size the replay set this frame (vka_ndsr_note_rt, from vka_exec.c OP_SET_RT), else the live D32F
// image of that size used this frame (a scan, cached by serial)
#include "vka_dlss.h"
#include "vka_tsr_cam.h"
#include "../../host/plat/plat.h"

// The scene camera blocks known, by main build. Rows of four floats (vka_tsr_cam.h). The Ocarina of Time row is the profile's `build v131072` main; its layout is the
// semantics NX2navi (recompiled vertex shaders) and Lichii #606 ("vertex cbuf 3") describe, not yet checked on this build: DM_VK_NDSR_CAM_LOG=<frames> logs the
// block's rows, the jitter terms and the depth chosen, to check it on a phone (still camera, then turning) before the temporal path is trusted
typedef struct { const char *main; uint32_t size; int vrow, prow, stage, slot; const char *what; } tsr_layout;
static const tsr_layout k_tsr_cam[] = {
    {"e0f3ccfaf50065ae4ea01eb5aeb13b1a", 0x3500, 0, 7, 0, 3, "Zelda: Ocarina of Time v131072: the scene context block (view rows 0-2, projection rows 7-10; NX2navi / Lichii semantics, unverified here)"},
};
static const tsr_layout *tsr_layout_now(void) {   // under g_mu
    static int done; static const tsr_layout *l;
    if (done) return l;
    done = 1;
    const char *b = dm_main_build();
    for (size_t i = 0; i < sizeof k_tsr_cam / sizeof *k_tsr_cam; i++) if (b && !strncmp(b, k_tsr_cam[i].main, 32)) l = &k_tsr_cam[i];
    if (l) VLOG("NDSR temporal: main build %s: %s", b, l->what);
    else VLOG("NDSR temporal: no scene camera layout for main build %s: DM_VK_NDSR=gsr2 runs FSR 1 EASU, DM_VK_NDSR_CAM_LOG has nothing to read", b && *b ? b : "(none)");
    return l;
}
static int guest_read(uint64_t addr, void *out, size_t n) {   // as vka_draw.c cpu_read: the bytes the CPU sees now, with the kept inline updates over them
    if (vka_addr(addr, n, NULL, NULL, NULL)) memcpy(out, (const void *)(uintptr_t)addr, n);
    else if (plat_mem_read(addr, out, n)) return 0;
    vka_ub_cpu_overlay(addr, out, n);
    return 1;
}
// The camera block bound at the dispatch: the layout's stage and slot when it holds a buffer of the block's size, else the first graphics stage's (0-4, 6, 7). Reads the
// floats up to the projection's last row into blk; *where = stage << 8 | slot, *nc = how many bindings of that size there were. 0 = none
enum { TSR_NF = 64 };
static int tsr_block(const vka_dstate *d, const tsr_layout *l, float blk[TSR_NF], int *where, int *nc, uint64_t *addr) {
    static const int st[] = {0, 1, 2, 3, 4, 6, 7};
    int n = 0, w = -1;
    if (d->b.ubo[l->stage][l->slot].size == l->size && d->b.ubo[l->stage][l->slot].addr) w = l->stage << 8 | l->slot;
    for (size_t i = 0; i < sizeof st / sizeof *st; i++) for (int s = 0; s < 16; s++)
        if (d->b.ubo[st[i]][s].size == l->size && d->b.ubo[st[i]][s].addr) { n++; if (w < 0) w = st[i] << 8 | s; }
    *nc = n; *where = w;
    if (w < 0) return 0;
    *addr = d->b.ubo[w >> 8][w & 0xff].addr;
    size_t need = (size_t)(l->prow + 4) * 16; if (need > TSR_NF * 4) return 0;
    return guest_read(*addr, blk, need);
}

// The depth targets the replay set (vka_ndsr_note_rt): the last one of each size this frame
enum { TSR_NRT = 4 };
static struct { tex_t depth; uint32_t w, h; uint64_t frame; } g_rt[TSR_NRT];
static int g_rt_n;
static int nvn_d32f(int f) { return f == 0x34 || f == 0x36; }   // NVN D32F, D32F_S8
void vka_ndsr_note_rt(const p_set_rt *rt) {
    if (!atomic_load_explicit(&g_tsr_want, memory_order_relaxed) || !rt || !rt->depth || !nvn_d32f(rt->depth->d.format)) return;
    uint32_t w = rt->depth->d.width, h = rt->depth->d.height; uint64_t f = vka_frame();
    pthread_mutex_lock(&g_mu);
    int i = 0; while (i < g_rt_n && !(g_rt[i].w == w && g_rt[i].h == h)) i++;
    if (i == g_rt_n) { if (g_rt_n < TSR_NRT) g_rt_n++; else { i = 0; for (int k = 1; k < TSR_NRT; k++) if (g_rt[k].frame < g_rt[i].frame) i = k; } }
    g_rt[i].depth = *rt->depth; g_rt[i].w = w; g_rt[i].h = h; g_rt[i].frame = f;
    pthread_mutex_unlock(&g_mu);
}

static struct {
    int have; vka_m4 vp, view; uint64_t frame, dserial; uint32_t iw, ih;   // the last frame's camera, for the reprojection
    vka_dlss_wimg col, mot, dep;   // the linear colour, the motion, the reversed depth (render size)
    VkPipeline pipe; int pstate;   // fsr/ndsr_tsr.comp in the stand-in's layout
    uint64_t scan_serial, scan_frame, n;
    uint32_t logs, log_gen;
} T;
// The scene depth of w x h for this frame: the hook's (a tex_t, made into its image with vka_tex_get), else the scan's (a held reference; *held set, dropped by the
// caller). The scan takes the D32F images of that size used this frame (last_frame), the one used last (last_fence) when there are several. *how names the source
static vka_img *tsr_depth(uint32_t w, uint32_t h, VkCommandBuffer cb, vka_img **held, const char **how, int *ncand) {
    uint64_t f = vka_frame(); *held = NULL; *ncand = 0;
    for (int i = 0; i < g_rt_n; i++) if (g_rt[i].w == w && g_rt[i].h == h && g_rt[i].frame == f) {
        vka_img *z = vka_tex_get(&g_rt[i].depth, cb);
        if (z && (z->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) { *how = "the last depth target of the input's size (OP_SET_RT)"; *ncand = 1; return z; }
    }
    *how = "the live D32F image of the input's size used this frame (scan)";
    if (T.scan_serial && f - T.scan_frame < 120) {   // the cached one while it is still used every frame
        vka_img *z = vka_tex_acquire(T.scan_serial);
        if (z && z->w == w && z->h == h && atomic_load(&z->last_frame) + 1 >= f) { *held = z; *ncand = 1; return z; }
        if (z) vka_tex_drop(z);
    }
    enum { MAXL = 8192 }; static vka_img **buf; if (!buf && !(buf = malloc(MAXL * sizeof *buf))) return NULL;
    int n = vka_tex_live_refs(buf, MAXL), c = 0; vka_img *best = NULL;
    for (int i = 0; i < n; i++) {
        vka_img *z = buf[i];
        if ((z->f.vk == VK_FORMAT_D32_SFLOAT || z->f.vk == VK_FORMAT_D32_SFLOAT_S8_UINT) && z->w == w && z->h == h && z->samples == 1 && z->base == 0 &&
            (z->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && atomic_load(&z->last_frame) + 1 >= f) {
            c++; if (!best || atomic_load(&z->last_fence) > atomic_load(&best->last_fence)) best = z; }
    }
    for (int i = 0; i < n; i++) if (buf[i] != best) vka_tex_drop(buf[i]);
    *ncand = c; T.scan_serial = best ? best->serial : 0; T.scan_frame = f;
    *held = best; return best;
}
static void tsr_logrows(const char *what, const float *blk, int row, int nrows) {
    for (int r = 0; r < nrows; r++) { const float *v = blk + (row + r) * 4; VLOG("NDSR camera:   %s row %d: %.6g %.6g %.6g %.6g", what, row + r, v[0], v[1], v[2], v[3]); }
}
// DM_VK_NDSR_CAM_LOG=<frames>: the camera block, the jitter terms and the depth at this many NDSR dispatches (counted again after a live change of the switch)
static void tsr_cam_log(const vka_dstate *d, const tsr_layout *l, uint32_t iw, uint32_t ih, VkCommandBuffer cb) {
    if (T.log_gen != g_sw_gen) { T.log_gen = g_sw_gen; T.logs = 0; }
    if (T.logs >= (uint32_t)g_sw.camlog) return;
    T.logs++;
    float blk[TSR_NF]; int where = -1, nc = 0; uint64_t addr = 0;
    if (!tsr_block(d, l, blk, &where, &nc, &addr)) {
        char b[512]; size_t o = 0;
        for (int s = 0; s < 16 && o < sizeof b - 32; s++) if (d->b.ubo[0][s].size) o += (size_t)snprintf(b + o, sizeof b - o, " %d:%#llx", s, (unsigned long long)d->b.ubo[0][s].size);
        VLOG("NDSR camera (frame %llu): no %#x-byte uniform block bound at the dispatch (vertex slots and sizes:%s)", (unsigned long long)vka_frame(), l->size, o ? b : " none");
    } else {
        vka_m4 v, p; vka_tsr_view_proj(blk, l->vrow, l->prow, &v, &p); double c[3]; vka_tsr_cam_pos(&v, c);
        int zo = vka_depth_zero();
        VLOG("NDSR camera (frame %llu): block at %#llx, stage %d slot %d (%d bound of %#x bytes); camera at %.3f %.3f %.3f; jitter terms P[0][2] %.6g P[1][2] %.6g; depth %s, %s; usable %d",
             (unsigned long long)vka_frame(), (unsigned long long)addr, where >> 8, where & 0xff, nc, l->size, c[0], c[1], c[2], p.m[0][2], p.m[1][2],
             zo ? "near = 0" : "near = -w", vka_tsr_reversed(&p, zo) ? "reversed" : "not reversed", vka_tsr_cam_ok(blk, l->vrow, l->prow));
        tsr_logrows("view", blk, l->vrow, 3); tsr_logrows("projection", blk, l->prow, 4);
    }
    vka_img *held = NULL; const char *how = ""; int ncand = 0;
    vka_img *z = tsr_depth(iw, ih, cb, &held, &how, &ncand);
    if (z) VLOG("NDSR camera:   depth: serial %llu VkFormat %d %ux%u, %s, %d candidate(s)", (unsigned long long)z->serial, z->f.vk, z->w, z->h, how, ncand);
    else VLOG("NDSR camera:   depth: no D32F of %ux%u this frame (hook entries %d)", iw, ih, g_rt_n);
    if (held) vka_tex_drop(held);
}
static int tsr_fail(const char *why) {   // under g_mu: logged at each change of reason
    static const char *last;
    if (why != last) { last = why; VRED("NDSR temporal: %s: FSR 1 EASU this frame", why); }
    T.have = 0;
    return 0;
}
// The temporal path for this dispatch: 1 = the output written (the caller binds the no-op pipeline), 0 = not (EASU runs)
static int tsr_run(const vka_dstate *d, const vka_res *t, vka_img *oi, VkCommandBuffer cb) {
    const tsr_layout *l = tsr_layout_now();
    if (!l) return 0;
    vka_img *in = vka_tex_id_img(t->tid, cb);
    if (!in || in->base || in->samples > 1 || !(in->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return tsr_fail("the network's input has no sampled image of its own levels");
    uint32_t iw = in->w, ih = in->h, olv = 0;
    if (!vka_img_lv(oi, &olv)) return tsr_fail("the output's level is not in its image");
    int ow = (int)(oi->w >> olv ? oi->w >> olv : 1), oh = (int)(oi->h >> olv ? oi->h >> olv : 1);
    uint64_t f = vka_frame();
    if (T.have && T.frame == f) return tsr_fail("a second NDSR dispatch in one frame");
    float blk[TSR_NF]; int where = -1, nc = 0; uint64_t addr = 0;
    if (!tsr_block(d, l, blk, &where, &nc, &addr)) return tsr_fail("no camera block bound at the dispatch");
    if (!vka_tsr_cam_ok(blk, l->vrow, l->prow)) return tsr_fail("the camera block holds no invertible view-projection");
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, oi->f.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return tsr_fail("the output's format is no blit destination");
    VkPipelineLayout dl = vka_dlss_layout();
    if (!dl) return tsr_fail("the DLSS stand-in's pipelines are not ready");
    if (!T.pstate) {
        T.pstate = -1;
        VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_ndsr_tsr, .pCode = vka_ndsr_tsr}; VkShaderModule m = VK_NULL_HANDLE;
        VkResult r = vk.vkCreateShaderModule(V.dev, &mi, NULL, &m);
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = dl};
        if (!r) r = vk.vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &T.pipe);
        if (m) vk.vkDestroyShaderModule(V.dev, m, NULL);
        if (!r) T.pstate = 1; else VRED("NDSR temporal: the motion pass's pipeline: %d", r);
    }
    if (T.pstate < 0) return tsr_fail("no motion pass pipeline");
    vka_img *held = NULL; const char *how = ""; int ncand = 0;
    vka_img *z = tsr_depth(iw, ih, cb, &held, &how, &ncand);
    if (!z) return tsr_fail("no D32F depth of the input's size this frame");
    // The input through its texture's own view, as the network samples it: agl's E5B9G9R9 input is a texture over the storage of the R11G11B10F scene target (vka_tex.c
    // same_shape, Learned view formats), so its image has the target's format, and a view in the image's format read the packed RGB9E5 words as B10G11R11 (colour noise
    // over the whole picture: found 2026-11-20 on the POCO with DM_VK_NDSR=gsr2). The image's own view only when the texture gives none
    VkImageView cv = vka_tex_id_view(t->tid, cb, 1, 0, NULL);
    if (!cv) cv = vka_view(in, in->f.view ? in->f.view : in->f.vk, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, NULL);
    VkImageView dv = (z->usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !z->base ? vka_view(z, z->f.vk, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1, NULL) : VK_NULL_HANDLE;
    if (held) {   // (the scan's image: its use in this submission keeps it, as vka_tex.c img_touch does for an image the replay gets)
        atomic_store(&held->last_frame, f); atomic_store(&held->last_fence, vka_fence_last() + 1);
    }
    uint64_t dserial = z->serial;
    if (held) vka_tex_drop(held);
    if (!cv || !dv) return tsr_fail("no sampled view of the input or the depth");
    if (!vka_dlss_wimg_get(cb, &T.col, iw, ih) || !vka_dlss_wimg_get(cb, &T.mot, iw, ih) || !vka_dlss_wimg_get(cb, &T.dep, iw, ih)) return tsr_fail("work images");
    vka_m4 view, proj, vp; vka_tsr_view_proj(blk, l->vrow, l->prow, &view, &proj); vka_m4_mul(&proj, &view, &vp);
    double tmax = VKA_ENVF("DM_VK_NDSR_TSR_CUT_T", 500), amax = VKA_ENVF("DM_VK_NDSR_TSR_CUT_A", 0.6);
    const char *why = !T.have ? "first frame" : T.iw != iw || T.ih != ih ? "new size" : T.dserial != dserial ? "another depth image" : T.frame + 1 != f ? "a frame skipped" :
                      vka_tsr_cut(&view, &T.view, tmax, amax) ? "camera cut" : VKA_ENV("DM_VK_NDSR_TSR_RESET", 0) ? "DM_VK_NDSR_TSR_RESET" : NULL;
    int zo = vka_depth_zero(), rv = VKA_ENV("DM_VK_NDSR_TSR_REV", -1);
    float R[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!why && !vka_tsr_reproj(&vp, &T.vp, zo, R)) why = "the view-projection is singular";
    if (why) for (int i = 0; i < 16; i++) R[i] = i % 5 == 0;   // (no motion on a reset: GSR2 starts its history again anyway)
    vka_tsr_pc pc; memset(&pc, 0, sizeof pc);
    memcpy(pc.r, R, sizeof R); pc.size[0] = (int32_t)iw; pc.size[1] = (int32_t)ih; pc.yup = VKA_ENV("DM_VK_NDSR_TSR_YUP", 1) != 0; pc.zero_one = zo;
    pc.rev = rv < 0 ? vka_tsr_reversed(&proj, zo) : rv != 0; pc.sqrt_in = g_sw.enc == VKA_NDSR_ENC_SQRT;
    vka_barrier(cb);
    { VkImageView v[4] = {dv, cv, dv, dv}; pc.pass = 0; vka_dlss_pass(cb, T.pipe, v, 0, T.col.view, VK_NULL_HANDLE, &pc, iw, ih);
                                           pc.pass = 1; vka_dlss_pass(cb, T.pipe, v, 0, T.mot.view, T.dep.view, &pc, iw, ih); }
    vka_tex_own(oi, cb);
    if (!vka_dlss_gsr2_views(cb, 1, T.col.view, T.dep.view, T.mot.view, (int)iw, (int)ih, why != NULL, oi, olv, 0, 0, 0, ow, oh)) return tsr_fail("GSR2 did not run");
    vka_img_written(oi);
    T.vp = vp; T.view = view; T.have = 1; T.frame = f; T.dserial = dserial; T.iw = iw; T.ih = ih; T.n++;
    if (why || T.n == 1 || !(T.n & (T.n - 1)) || T.n % 1800 == 0) {
        static int nl; if (nl < 64 || T.n % 1800 == 0) { nl++;
            VLOG("NDSR temporal (GSR2 3-pass, camera motion from depth): %ux%u -> %dx%d, depth serial %llu (%s), %s depth, y %s, sqrt input %d%s%s (%llu frames)", iw, ih, ow, oh,
                 (unsigned long long)dserial, how, pc.rev ? "reversed" : "standard", pc.yup ? "up" : "down", pc.sqrt_in, why ? ", history reset: " : "", why ? why : "", (unsigned long long)T.n); }
    }
    return 1;
}

static const vka_res *res_at(const vka_res *r, int n, uint16_t binding) { for (int i = 0; i < n; i++) if (r[i].binding == binding) return &r[i]; return NULL; }

static void log_sizes(const vka_dstate *d, uint64_t key, const vka_res *t, const vka_res *o, const vka_img *b, int enc, int map, int kernel, int pack, VkCommandBuffer cb) {   // under g_mu
    vka_img *a = vka_tex_id_img(t->tid, cb);
    uint32_t iw = a ? a->w : 0, ih = a ? a->h : 0, ow = b ? b->w : 0, oh = b ? b->h : 0;
    int i = 0; while (i < g_nlog && !(g_log[i].key == key && g_log[i].iw == iw && g_log[i].ih == ih && g_log[i].ow == ow && g_log[i].oh == oh && g_log[i].enc == enc && g_log[i].map == map && g_log[i].kernel == kernel && g_log[i].pack == pack)) i++;
    if (i == g_nlog) {
        if (g_nlog == NLOG) return;
        g_log[g_nlog++] = (typeof(g_log[0])){key, iw, ih, ow, oh, enc, map, kernel, pack, 0};
        VLOG("NDSR network %016llx replaced by %s (input %s, %s mapping, %ux%u -> %ux%u; texture IDs %d -> %d; output VkFormat %d %s)", (unsigned long long)key, kernel == VKA_NDSR_KERNEL_NEAREST ? "nearest" : kernel == VKA_NDSR_KERNEL_BILINEAR ? "bilinear" : kernel == VKA_NDSR_KERNEL_EASU_FULL ? "FSR 1 EASU (AMD's full)" : "FSR 1 EASU (the lighter)",
             enc ? "sqrt" : "linear", map ? "push bank" : "full-image", iw, ih, ow, oh, t->tid, o->tid, b ? (int)b->f.vk : 0,
             pack ? "without storage support: packed words through an R32_UINT view" : "stored as floats");
    }
    if (g_sw.log && !g_log[i].push) {   // agl's c24 as the dispatch sees it (vka_bstate.push[1], the bytes vka_drv_fill copies for DMSH_PUSH_BANK)
        g_log[i].push = 1;
        const uint8_t *p = d->b.push[1];
        for (int row = 0; row < 8; row++) {
            uint32_t u[4]; float f[4]; memcpy(u, p + 16 * row, 16); memcpy(f, p + 16 * row, 16);
            VLOG("NDSR %016llx c24[%d] = %08x %08x %08x %08x (%g %g %g %g) (%d %d %d %d)", (unsigned long long)key, row, u[0], u[1], u[2], u[3], f[0], f[1], f[2], f[3],
                 (int)u[0], (int)u[1], (int)u[2], (int)u[3]);
        }
    }
}

VkPipeline vka_ndsr_pipeline(const vka_dstate *d, vka_shader *cs, const vka_layout *lay, const vka_res *r, int n, vka_imgrt *rt, VkCommandBuffer cb) {
    if (!cs || !cs->e || !(cs->e->flags & DMSH_VK_F_HMMA)) return VK_NULL_HANDLE;   // (every other dispatch: one test)
    pthread_mutex_lock(&g_mu);
    switches();
    VkPipeline p = VK_NULL_HANDLE; uint16_t tb = 0, ib = 0, bb = 0;
    if (!g_sw.mode && !g_sw.camlog) goto out;
    if (!vka_ndsr_match(cs->e, cs->b, &tb, &ib, &bb)) {   // (another HMMA program: NDVE's movie networks; logged once each, so a network of another interface shows)
        int i = 0; while (i < g_nref && g_ref[i] != cs->key) i++;
        if (i == g_nref && g_nref < NREF) { g_ref[g_nref++] = cs->key; VLOG("NDSR: HMMA compute %016llx is not of agl's network interface: it runs as it is", (unsigned long long)cs->key); }
        goto out;
    }
    const vka_res *t = n > 0 ? res_at(r, n, tb) : NULL, *o = n > 0 ? res_at(r, n, ib) : NULL;
    if (g_sw.camlog && t && t->what == VKA_R_TEX && t->tid > 0) {   // DM_VK_NDSR_CAM_LOG: in any mode, before anything is replaced
        const tsr_layout *l = tsr_layout_now(); vka_img *a = l ? vka_tex_id_img(t->tid, cb) : NULL;
        if (a) tsr_cam_log(d, l, a->w, a->h, cb);
    }
    if (!g_sw.mode) goto out;
    if (rt && rt->b[0]) { refused(cs->key, "its images are retyped for integer textures"); goto out; }
    if (!t || !o || t->what != VKA_R_TEX || o->what != VKA_R_IMG || t->tid <= 0 || o->tid <= 0) { refused(cs->key, "its input or output has no texture bound"); goto out; }
    // The output's format: a float store needs its storage support. Without it (B10G11R11_UFLOAT on Adreno: vka_tex.c view_of refuses the float storage view, the
    // binding is unbound and the stores go nowhere, the network's as well) the packed variant, through the R32_UINT view id_view gives an unsigned binding of a float
    // texture of 4-byte texels; a format of neither kind: refused (it would be unbound too)
    vka_img *oi = vka_tex_id_img(o->tid, cb);
    if (!oi) { refused(cs->key, "its output texture has no image"); goto out; }
    VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, oi->f.vk, &fp);
    int storage = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0, pack = storage ? VKA_NDSR_PACK_NONE : vka_ndsr_pack_of(oi->f.vk);
    if (!storage && !pack) {
        char why[96]; snprintf(why, sizeof why, "its output's VkFormat %d has no storage support and no packed store here", (int)oi->f.vk);
        refused(cs->key, why); goto out;
    }
    if (pack && !rt) { refused(cs->key, "its output needs the packed store and the dispatch passed no vka_imgrt"); goto out; }
    if (g_sw.mode == 4 && tsr_run(d, t, oi, cb)) {   // the temporal path wrote the output: the network's dispatch runs a no-op
        p = make(cs, lay, tb, ib, bb, 0, 0, VKA_NDSR_KERNEL_NOOP, pack);
        if (p && pack) { memset(rt, 0, sizeof *rt); rt->b[0] = (uint32_t)ib << 16 | 33u << 8 | 1u; }   // (the binding as the packed EASU path has it)
        if (p) goto out;
        VRED_ONCE("NDSR temporal: no no-op pipeline for the network's layout: FSR 1 EASU over the temporal result");
    }
    int kernel = g_sw.mode == 3 ? VKA_NDSR_KERNEL_NEAREST : g_sw.mode == 2 ? VKA_NDSR_KERNEL_BILINEAR : g_sw.full ? VKA_NDSR_KERNEL_EASU_FULL : VKA_NDSR_KERNEL_EASU;
    p = make(cs, lay, tb, ib, bb, g_sw.enc, g_sw.map, kernel, pack);
    if (p && pack) { memset(rt, 0, sizeof *rt); rt->b[0] = (uint32_t)ib << 16 | 33u << 8 | 1u; }   // (the output an unsigned R32ui image: vka_draw.c rt_dim, then id_view)
    if (p) log_sizes(d, cs->key, t, o, oi, g_sw.enc, g_sw.map, kernel, pack, cb);
out:
    pthread_mutex_unlock(&g_mu);
    return p;
}
#endif
