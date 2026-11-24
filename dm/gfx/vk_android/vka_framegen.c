// Frame generation in the present (Lossless Scaling's LSFG, ported from Eden in dm/gfx/vk_android/lsfg, GPL-3.0-or-later): the present thread hands each game frame here
// before it presents it (vka_fg_frame, one call in vka_present.c present_main). The frame's picture (the window texture's crop) is copied into the chain's input, the shared
// passes run, and the generated frames between the previous game frame and this one are written to images of their own, which the present shows before the game frame,
// each held for an equal share of the game's measured frame interval (display timing). The work is one command buffer per game frame on the replay's queue: it waits for the
// frame's own timeline value as the present does, and the presents after it read the outputs in queue order behind a full barrier. Off by default; the user's own
// Lossless.dll (the launcher finds it and passes DM_LSFG_DLL) is read once. Without NX_LSFG (a build without the GPL folder) every call reports "not in this build".
// Settings: DM_LSFG=1, DM_LSFG_MULT=2..4, DM_LSFG_TARGET=0|fps, DM_LSFG_FLOW=0 (auto: the optical flow at the game's own resolution) or 25..100 (%), changed while running by
// nxhost_framegen_set (the launcher's in-game settings); DM_LSFG_QUALITY=1 the quality shader set instead of performance (nxhost_framegen_quality)
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#ifdef NX_LSFG
#include "lsfg/lsfg.h"
#endif

static _Atomic int g_on = -1, g_mult = 2, g_target, g_flow = 75, g_quality, g_state;   // g_state: 0 off, 1 generating, 2 on and not generating, -1 unavailable
static char g_reason[256] = "off";
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static void reason(const char *fmt, const char *a) { pthread_mutex_lock(&g_rmu); snprintf(g_reason, sizeof g_reason, fmt, a); pthread_mutex_unlock(&g_rmu); }

static void settings_init(void) {
    int exp = -1;
    if (atomic_load(&g_on) != -1) return;
    const char *e;
    if ((e = getenv("DM_LSFG_MULT"))) atomic_store(&g_mult, atoi(e));
    if ((e = getenv("DM_LSFG_TARGET"))) atomic_store(&g_target, atoi(e));
    if ((e = getenv("DM_LSFG_FLOW"))) atomic_store(&g_flow, atoi(e));
    if ((e = getenv("DM_LSFG_QUALITY"))) atomic_store(&g_quality, atoi(e) != 0);
    e = getenv("DM_LSFG");
    atomic_compare_exchange_strong(&g_on, &exp, e && atoi(e) ? 1 : 0);
}

void nxhost_framegen_set(int on, int multiplier, int target_rate, int flow_pct) {
    atomic_store(&g_mult, multiplier < 2 ? 2 : multiplier > 4 ? 4 : multiplier);
    atomic_store(&g_target, target_rate < 0 ? 0 : target_rate);
    atomic_store(&g_flow, flow_pct <= 0 ? 0 : flow_pct < 25 ? 25 : flow_pct > 100 ? 100 : flow_pct);
    atomic_store(&g_on, on ? 1 : 0);
    VLOG("frame generation: %s, x%d, target %d fps, flow %d%%", on ? "on" : "off", atomic_load(&g_mult), atomic_load(&g_target), atomic_load(&g_flow));
}
void nxhost_framegen_quality(int quality) {
    settings_init();
    if (atomic_exchange(&g_quality, quality != 0) != (quality != 0)) VLOG("frame generation: %s shaders", quality ? "quality" : "performance");
}

int nxhost_framegen_status(char *buf, int n) {
    settings_init();
    int st = atomic_load(&g_state);
    if (buf && n > 0) { pthread_mutex_lock(&g_rmu); snprintf(buf, (size_t)n, "%s", g_reason); pthread_mutex_unlock(&g_rmu); }
    return st;
}

#ifndef NX_LSFG
int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, int format, vka_img **gen, double *hold, double *out_period) {
    (void)serial; (void)crop; (void)period; (void)fence; (void)format; (void)gen;
    settings_init(); *hold = 0; *out_period = 0;
    if (atomic_load(&g_on) > 0) { atomic_store(&g_state, -1); reason("%s", "not in this build (built without NX_LSFG)"); vka_fps_fg(-1, 1); }
    return 0;
}
#else
enum { NFG = 3 };
static struct {
    int state;                         // 0 not tried, 1 ready (modules loaded), -1 unavailable
    lsfg_modules mods;
    lsfg_chain *chain; VkExtent2D ext; int flow, quality; size_t gens;   // gens: the generated frames a game frame can have in this chain; quality: its shader set
    vka_img *out[LSFG_MAX_GENERATIONS]; VkImageView out_view[LSFG_MAX_GENERATIONS];
    VkCommandPool pool; struct { VkCommandBuffer cb; VkFence fence; vka_img *held; int timed, serial, flow, probed; size_t ng; uint64_t probe_count; unsigned cgen; } fr[NFG]; unsigned fi;
    unsigned cgen, still_frames;   // cgen: the chain made (counted; the GPU time's average starts again); still_frames: game frames not generated for a still picture
    lsfg_pacer pacer; uint64_t count; uint32_t warm_streak;
    double out_period, longer_since;   // the output period the window's frame rate is set for, and since when a longer one is wanted
    // The still-picture check (still_*): the probe image of each submission copied to the host, the last one read, and how many game frames in a row matched
    VkBuffer probe_buf; VkDeviceMemory probe_mem; uint8_t *probe_map, *probe_last; VkDeviceSize probe_stride; VkExtent2D probe_ext; uint64_t probe_last_count; uint32_t still;
    size_t fit_held, fit_pending; double fit_since; int fit_set;   // the refresh cap's held verdict (cap_hold)
    int auto_flow, af_user, af_dir; double af_since, fg_ms, fg_at;   // the flow the GPU time allows (autoflow), the GPU time's average and when it was last measured
} F;

static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void unavailable(const char *why, const char *a) { F.state = -1; reason(why, a); atomic_store(&g_state, -1); VRED("frame generation unavailable: %s", g_reason); }

// The device must have what the shaders use (Float16, StorageImageExtendedFormats, StorageImageWriteWithoutFormat), null descriptors, and storage images of the chain's formats
static int device_ok(void) {
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    const char *miss = !f12.shaderFloat16 ? "shaderFloat16" : !f2.features.shaderStorageImageExtendedFormats ? "shaderStorageImageExtendedFormats"
                     : !f2.features.shaderStorageImageWriteWithoutFormat ? "shaderStorageImageWriteWithoutFormat" : !V.null_desc ? "null descriptors (robustness2)" : NULL;
    VkFormat fmts[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (int i = 0; i < 3 && !miss; i++) {
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, fmts[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) miss = "storage images of R8, RGBA8 or RGBA16F";
    }
    if (miss) { unavailable("the GPU lacks %s", miss); return 0; }
    return 1;
}

static int load(void) {
    if (F.state) return F.state > 0;
    if (!device_ok()) return 0;
    const char *path = getenv("DM_LSFG_DLL");
    if (!path || !*path) { unavailable("%s", "no Lossless.dll (the launcher looks in Documents/Emulation/bios, Switch2, LSFG, Download, the storage's root and the app's files; or pick it in the launcher)"); return 0; }
    uint64_t t0 = now_ns();
    int r = lsfg_dll_load(path, &F.mods);
    if (r) { char b[200]; snprintf(b, sizeof b, "%s: %s", path, lsfg_status_name(r)); unavailable("%s", b); return 0; }
    VLOG("frame generation: %d shaders from %s (%.1f ms)%s", F.mods.n, path, (now_ns() - t0) / 1e6, F.mods.quality ? ", with the quality set" : ", no quality set in this DLL");
    F.state = 1;
    return 1;
}

static void teardown(void) {
    if (!F.chain && !F.out[0]) return;
    vka_queue_idle();   // (bounded by the hang time, vka_dev.c)
    for (int i = 0; i < NFG; i++) if (F.fr[i].held) { vka_tex_drop(F.fr[i].held); F.fr[i].held = NULL; }
    lsfg_chain_free(F.chain); F.chain = NULL;
    for (int i = 0; i < LSFG_MAX_GENERATIONS; i++) {
        if (F.out_view[i]) vk.vkDestroyImageView(V.dev, F.out_view[i], NULL);
        F.out_view[i] = VK_NULL_HANDLE;
        if (F.out[i]) vka_tex_drop(F.out[i]);   // the last reference: destroyed by vka_tex.c (with the views the present made of it)
        F.out[i] = NULL;
    }
    lsfg_pacer_reset(&F.pacer); F.count = 0; F.warm_streak = 0; F.out_period = 0; F.longer_since = 0;
    if (F.probe_buf) vk.vkDestroyBuffer(V.dev, F.probe_buf, NULL);
    if (F.probe_mem) vk.vkFreeMemory(V.dev, F.probe_mem, NULL);
    free(F.probe_last);
    F.probe_buf = VK_NULL_HANDLE; F.probe_mem = VK_NULL_HANDLE; F.probe_map = F.probe_last = NULL; F.probe_last_count = 0; F.still = 0;
    for (int i = 0; i < NFG; i++) F.fr[i].probed = 0;
    F.fit_set = 0; F.fit_since = 0; F.fg_ms = 0; F.fg_at = 0;
}

// The still-picture check (DM_LSFG_STILL=0: off, live). Between two identical game frames frame generation makes identical frames: GPU time and power for nothing (a paused
// menu, a loading screen, a still dialogue box). Each submission that runs the shared passes copies the chain's small flow image (lsfg_chain_probe, R8 at 1/4 of the flow's
// size) into a host-visible buffer; once its fence is done (looked at without waiting, in frame order) it is compared byte for byte with the frame before. After two
// matches in a row (three identical frames) the next game frames generate nothing until one differs. Exact equality only: a frame that changes at all generates, so motion
// is never cut, and the decision is a frame late at most (the first moving frame after a still stretch has no generated frame before it)
static int probe_make(VkExtent2D e) {
    F.probe_ext = e;
    VkDeviceSize n = (VkDeviceSize)e.width * e.height; F.probe_stride = (n + 255) & ~(VkDeviceSize)255;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = F.probe_stride * NFG, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkResult r = vk.vkCreateBuffer(V.dev, &bi, NULL, &F.probe_buf);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetBufferMemoryRequirements(V.dev, F.probe_buf, &mr);
    uint32_t t = r ? UINT32_MAX : vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (!r && t == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &F.probe_mem);
    if (!r) r = vk.vkBindBufferMemory(V.dev, F.probe_buf, F.probe_mem, 0);
    void *map = NULL; if (!r) r = vk.vkMapMemory(V.dev, F.probe_mem, 0, VK_WHOLE_SIZE, 0, &map);
    if (!r && !(F.probe_last = malloc((size_t)n))) r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (r) { VRED("frame generation: still-picture buffer: %d (the check is off)", r); if (F.probe_buf) vk.vkDestroyBuffer(V.dev, F.probe_buf, NULL); if (F.probe_mem) vk.vkFreeMemory(V.dev, F.probe_mem, NULL);
             F.probe_buf = VK_NULL_HANDLE; F.probe_mem = VK_NULL_HANDLE; return 0; }
    F.probe_map = map;
    return 1;
}
static void probe_poll(void) {   // every finished submission's probe, oldest first
    for (;;) {
        int j = -1;
        for (int i = 0; i < NFG; i++) if (F.fr[i].probed && (j < 0 || F.fr[i].probe_count < F.fr[j].probe_count)) j = i;
        if (j < 0 || vk.vkWaitForFences(V.dev, 1, &F.fr[j].fence, VK_TRUE, 0) != VK_SUCCESS) return;
        F.fr[j].probed = 0;
        size_t n = (size_t)F.probe_ext.width * F.probe_ext.height; const uint8_t *p = F.probe_map + (size_t)j * F.probe_stride;
        int same = F.probe_last_count && F.fr[j].probe_count == F.probe_last_count + 1 && !memcmp(p, F.probe_last, n);
        F.still = same ? F.still + 1 : 0;
        memcpy(F.probe_last, p, n); F.probe_last_count = F.fr[j].probe_count;
    }
}

// An output as a vka_img, so the present shows it as it shows the window texture (blit or the rotated copy) and keeps it alive while it reads it
static vka_img *out_new(VkExtent2D e, int i, VkImageView *view) {
    vka_img *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->serial = 0xF6000000ull + (uint64_t)i; im->refs = 1; im->inited = 1; im->gpu_owned = 1;
    im->f = (vka_fmt){VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 4, 1, 1};
    im->d.target = NVN_TEXTURE_TARGET_2D; im->d.width = e.width; im->d.height = e.height;
    im->usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT; im->w = e.width; im->h = e.height; im->depth = im->layers = im->levels = im->samples = 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = im->f.vk, .extent = {e.width, e.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = im->usage};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &im->img);
    VkMemoryRequirements mr = {0}; if (!r) vk.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (!r && ai.memoryTypeIndex == UINT32_MAX) r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!r) r = vk.vkAllocateMemory(V.dev, &ai, NULL, &im->m.dm);   // its own allocation (vka_imem with no block; size 0: not in the image memory stats)
    if (!r) r = vk.vkBindImageMemory(V.dev, im->img, im->m.dm, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = im->f.vk, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!r) r = vk.vkCreateImageView(V.dev, &vi, NULL, view);
    if (r) { VRED("frame generation: output image %ux%u: %d", e.width, e.height, r); vka_tex_drop(im); return NULL; }
    return im;
}

static VkFormat raw_unorm_twin(VkFormat f) {   // as vka_present.c's
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}
// The window texture's own format when it shares an image of another format (vka_tex.c same_shape; vka_present.c present_fmt presents it through a view in that format),
// else VK_FORMAT_UNDEFINED. Fire Emblem Fortune's Weave's RGBA8 window textures can lie over its A2B10G10R10 target's image when that image is still alive at their first
// use: a blit reads the image's format, the chain got the RGBA8 bits as 10:10:10:2, and every generated frame was blue/violet noise while the game's own frames were clean.
// DM_VK_FG_OWNFMT=0 (live) blits in the image's format as before (A/B)
static int img_lists(const vka_img *im, VkFormat v) {   // the image's format list takes views in v (as vka_tex.c img_lists: its own format, its sRGB twin, the learned ones)
    if (!v) return 0;
    if (v == im->f.vk || v == im->alt || im->any_fmt) return 1;
    for (int k = 0; k < im->nx && k < (int)(sizeof im->xfmt / sizeof im->xfmt[0]); k++) if (im->xfmt[k] == v) return 1;
    return 0;
}
static VkFormat own_fmt(const vka_img *im, int format) {
    VkFormat own = raw_unorm_twin(im->f.vk); vka_fmt wf;
    if (!format || !VKA_ENV("DM_VK_FG_OWNFMT", 1) || !vka_fmt_of(format, &wf)) return VK_FORMAT_UNDEFINED;
    VkFormat w = raw_unorm_twin(wf.vk);
    if (w == own || !(img_lists(im, w) || img_lists(im, wf.vk))) return VK_FORMAT_UNDEFINED;
    return w;
}

static int rebuild_now(VkExtent2D e, int flow, size_t gens, int quality);
// On the present thread, which the game's thread waits for: the gates' time stands still meanwhile (vka_gate_host_stall), or a GPU wait for the game's next signal
// ran out during the chain's 327 ms and banned its event for the rest of the run (upstream Banana Native)
static int rebuild(VkExtent2D e, int flow, size_t gens, int quality) { vka_gate_host_stall(1); int ok = rebuild_now(e, flow, gens, quality); vka_gate_host_stall(0); return ok; }
static int rebuild_now(VkExtent2D e, int flow, size_t gens, int quality) {
    teardown();
    // Only the outputs the chain can write (one per generated frame it has room for): all three before, two full-size RGBA8 images (16.6 MB at 1080p) never written at 2x
    VkImage imgs[LSFG_MAX_GENERATIONS] = {0};
    for (size_t i = 0; i < gens && i < LSFG_MAX_GENERATIONS; i++) { if (!(F.out[i] = out_new(e, (int)i, &F.out_view[i]))) { teardown(); return 0; } imgs[i] = F.out[i]->img; }
    uint64_t t0 = now_ns();
    F.chain = lsfg_chain_new(&F.mods, e, flow ? flow / 100.0f : 1.0f, gens, quality, imgs, F.out_view);
    if (!F.chain) { teardown(); return 0; }
    F.ext = e; F.flow = flow; F.gens = gens; F.quality = quality; F.cgen++;
    { VkExtent2D pe; lsfg_chain_probe(F.chain, &pe); probe_make(pe); }   // (without it the still-picture check is off)
    VLOG("frame generation: chain for %ux%u, %s shaders, up to %zu generated frames, made in %.1f ms", e.width, e.height, quality ? "quality" : "performance", gens, (now_ns() - t0) / 1e6);
    return 1;
}

// GPU time of the frame generation (DM_LSFG_GPUTIME=0: off, live): a timestamp after the first barrier of each game frame's command buffer and one before its last,
// read back when its fence is next waited for. Every 600 game frames with generated frames, one log line: the average and the longest, the generated frames a game
// frame, and the order (DM_LSFG_SCHED). The copy of the window texture into the chain is in it
static struct { int ok; VkQueryPool qp; PFN_vkCmdResetQueryPool reset; PFN_vkCmdWriteTimestamp write; PFN_vkGetQueryPoolResults get; double tick; } T;
static struct { int serial; unsigned n, cgen; double sum, max, ng; } TA;   // over one chain and one order
static int gputime_ok(void) {
    if (T.ok) return T.ok > 0;
    T.ok = -1;
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    T.reset = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    T.write = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp");
    T.get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * NFG};
    if (!create || !T.reset || !T.write || !T.get || V.props.limits.timestampPeriod <= 0 || create(V.dev, &ci, NULL, &T.qp)) { VLOG("frame generation: no GPU timestamps"); return 0; }
    double vka_ts_tick_ns(void); T.tick = vka_ts_tick_ns();   // (the tick measured against the host clock where the driver's period is off: vka_exec.c)
    if (!(T.tick > 0)) T.tick = V.props.limits.timestampPeriod;
    T.ok = 1;
    return 1;
}
static void gputime_take(unsigned k) {   // slot k's fence is signalled
    if (!F.fr[k].timed) return;
    F.fr[k].timed = 0;
    uint64_t t[2];
    if (T.get(V.dev, T.qp, 2 * k, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) || t[1] <= t[0]) return;
    if (TA.serial != F.fr[k].serial || TA.cgen != F.fr[k].cgen) { memset(&TA, 0, sizeof TA); TA.serial = F.fr[k].serial; TA.cgen = F.fr[k].cgen; }
    double ms = (double)(t[1] - t[0]) * T.tick / 1e6;
    if (F.fr[k].ng && F.fr[k].flow == F.flow) { F.fg_ms = F.fg_ms > 0 ? F.fg_ms + (ms - F.fg_ms) * 0.1 : ms; F.fg_at = mono_s(); }   // (autoflow: this chain's frames that generated)
    TA.n++; TA.sum += ms; TA.ng += (double)F.fr[k].ng; if (ms > TA.max) TA.max = ms;
    if (TA.n == 600) {
        VLOG("frame generation: GPU %.2f ms a game frame on average (%.2f at most), %.2f generated frames a game frame, %s order, %s shaders, %ux%u flow %d%%; %u game frames "
             "not generated meanwhile for a still picture", TA.sum / TA.n, TA.max, TA.ng / TA.n, TA.serial ? "serial (DM_LSFG_SCHED=0)" : "wavefront",
             F.quality ? "quality" : "performance", F.ext.width, F.ext.height, F.flow, F.still_frames);
        int s = TA.serial; unsigned cg = TA.cgen; memset(&TA, 0, sizeof TA); TA.serial = s; TA.cgen = cg; F.still_frames = 0;
    }
}

int vka_fg_frame(uint64_t serial, const int *crop, double period, uint64_t fence, int format, vka_img **gen, double *hold, double *out_period) {
    settings_init();
    *hold = 0; *out_period = 0;
    // After a GPU hang (vka_dev.c) nothing more goes to the queue, as in the present: the fence of a submission behind the hang is never signalled, and the wait for it
    // below had no end (the present thread never came back, and the host's exit after the hang ran into its time limit)
    if (vka_hung()) return 0;
    int on = atomic_load(&g_on) > 0, mult = atomic_load(&g_mult), target = atomic_load(&g_target), flow = atomic_load(&g_flow);
    if (!on) {
        if (F.chain) { teardown(); VLOG("frame generation off"); }
        if (F.state >= 0) { atomic_store(&g_state, 0); reason("%s", "off"); }
        vka_fps_fg(F.state < 0 ? -1 : 0, 1);
        return 0;
    }
    if (!load()) { vka_fps_fg(-1, 1); return 0; }
    // The game's frame period is never shorter than the host's frame limit allows (hle_fpslock.c): a game that asks for interval 1 (16.7 ms) but is held to 30 fps has two
    // refreshes of a 60 Hz panel a frame, room for one generated frame, and its output rate is 60, not 120. Donkey Kong Bananza's presents ride the host's tick
    // (DM_PRESENT_INTERVAL=display, 8.3 ms at 120 Hz) whatever its limit
    int nxhost_fps_limit_get(void) __attribute__((weak)), nxhost_fps_decoupled(void) __attribute__((weak));
    { int lim = &nxhost_fps_limit_get ? nxhost_fps_limit_get() : 0; if (lim > 0 && period < 1.0 / lim) period = 1.0 / lim; }
    // The window's frame rate: a shorter output period at once, a longer one only after it held for 2 s. DKB switches its own present interval between 1 and 2 (60 and
    // 30 fps) many times a minute, and each switch would change the panel's mode (120 <-> 60 Hz); the generated frames are paced by hold, not by the panel's rate
    double want = target > 0 ? 1.0 / target : period / (mult < 2 ? 2 : mult > 4 ? 4 : mult), tnow = mono_s();
    if (F.out_period <= 0 || want < F.out_period - 1e-4) { F.out_period = want; F.longer_since = 0; }
    else if (want > F.out_period + 1e-4) { if (!F.longer_since) F.longer_since = tnow; else if (tnow - F.longer_since >= 2.0) { F.out_period = want; F.longer_since = 0; } }
    else F.longer_since = 0;
    *out_period = F.out_period;
    vka_img *im = vka_tex_acquire(serial);
    if (!im) return 0;
    int cx = 0, cy = 0, cw = (int)im->w, ch = (int)im->h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    VkFormatProperties fp = {0}; vk.vkGetPhysicalDeviceFormatProperties(V.pd, im->f.vk, &fp);
    if (!im->inited || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->f.kind || im->samples != 1 || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || cw <= 0 || ch <= 0) {
        VRED_ONCE("frame generation: window texture serial %llu (format %#x) cannot be copied into the chain: not generating", (unsigned long long)im->serial, im->d.format);
        vka_tex_drop(im); F.warm_streak = 0; atomic_store(&g_state, 2); vka_fps_fg(2, 1); return 0;
    }
    // A window texture over an image of another format: copied bit for bit (both 32-bit formats, one sample) into the RGBA8 input (lsfg_chain.c LSFG_FORMAT), so the chain reads it in its own format.
    // Any other format has no such copy into the input: no frame generated
    VkFormat wv = own_fmt(im, format);
    if (wv && (wv != VK_FORMAT_R8G8B8A8_UNORM || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) || !(im->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))) {
        VRED_ONCE("frame generation: window texture serial %llu (NVN format %#x, VkFormat %d) shares an image of VkFormat %d (owner serial %llu) and cannot be copied into the chain in its own format: not generating",
                  (unsigned long long)serial, format, wv, im->f.vk, (unsigned long long)im->serial);
        vka_tex_drop(im); F.warm_streak = 0; atomic_store(&g_state, 2); vka_fps_fg(2, 1); return 0;
    }
    if (wv) { static int told; if (!told++) VLOG("frame generation: window texture serial %llu (NVN format %#x, VkFormat %d) shares an image of VkFormat %d (owner serial %llu): copied in its own format",
                                                (unsigned long long)serial, format, wv, im->f.vk, (unsigned long long)im->serial); }
    VkExtent2D e = {(uint32_t)cw, (uint32_t)ch};
    // The chain no larger than the picture on screen (DM_LSFG_FIT=0: the crop's size as before, live). A picture shown smaller than its crop (a 3840x2160 window texture
    // on a 1920x1080 panel, a Picture size under 100 %) is scaled down by the present anyway: the frame goes into the chain at the size it is shown at (a linear blit,
    // as the present's own), the chain's passes and the generated frames have that size, and the present shows them 1:1. Only when both sides are smaller, the format
    // filters linearly, and the frame is blitted (a window texture copied in its own format, wv, cannot be scaled)
    int fit = 0;
    if (!wv && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && VKA_ENV("DM_LSFG_FIT", 1)) {
        int pw = 0, ph = 0; vka_present_picture_size(cw, ch, &pw, &ph);
        if (pw >= 64 && ph >= 64 && pw < cw && ph < ch) { e = (VkExtent2D){(uint32_t)pw, (uint32_t)ph}; fit = 1; }
    }
    // Room for the generated frames the settings can ask for (each has working images of its own, lsfg_chain.c): the multiplier's, or all with a target rate. A larger
    // multiplier makes the chain again; a smaller one keeps it until it is made again for another reason
    size_t gens = target > 0 ? LSFG_MAX_GENERATIONS : (size_t)((mult < 2 ? 2 : mult > 4 ? 4 : mult) - 1);
    // The shader set: quality when asked for and the DLL has it (else performance, said once in the log and in the status)
    int quality = atomic_load(&g_quality) && F.mods.quality;
    if (atomic_load(&g_quality) && !F.mods.quality) VRED_ONCE("frame generation: quality asked for, but this Lossless.dll has no quality shader set: performance");
    // Autoflow (DM_LSFG_AUTOFLOW=<% of the game's frame interval>, live, default 25; 0 = off): the flow resolution the user picked is the most it uses. While frame
    // generation's measured GPU time (DM_LSFG_GPUTIME; the average of the frames that generated, at this chain's flow) takes more than that share of the game's frame
    // interval for 2 s, the flow drops a step of 25 points (not under 25 %); while a step up would keep it under 0.8 of the share (the time taken to grow with the flow's
    // area) for 10 s, it rises a step, never above the user's. Each step makes the chain again (its pipelines are kept). The pacer only drops generated frames when the
    // game slows; this lowers their cost first
    {
        int user = flow > 0 ? flow : 100, budget = VKA_ENV("DM_LSFG_AUTOFLOW", 25);
        if (budget <= 0 || !F.auto_flow || F.auto_flow > user || F.af_user != user) { F.auto_flow = user; F.af_user = user; F.af_dir = 0; }
        else if (F.fg_ms > 0 && tnow - F.fg_at < 1.0 && F.pacer.smoothed_interval > 0) {
            double share = F.fg_ms / (F.pacer.smoothed_interval * 1e3) * 100.0, up = F.auto_flow + 25 > user ? user : F.auto_flow + 25;
            int lo = F.auto_flow - 25 < 25 ? 25 : F.auto_flow - 25, hi = (int)up;
            int dir = share > budget && lo < F.auto_flow ? -1 : hi > F.auto_flow && share * (up / F.auto_flow) * (up / F.auto_flow) < budget * 0.8 ? 1 : 0;
            if (dir != F.af_dir) { F.af_dir = dir; F.af_since = tnow; }
            else if (dir && tnow - F.af_since >= (dir < 0 ? 2.0 : 10.0)) {
                int nf = dir < 0 ? lo : hi;
                VLOG("frame generation: flow %d%% -> %d%% (autoflow: GPU %.2f ms a game frame, %.0f%% of the %.1f ms game frame interval, DM_LSFG_AUTOFLOW=%d%%; the setting is %d%%)",
                     F.auto_flow, nf, F.fg_ms, share, F.pacer.smoothed_interval * 1e3, budget, user);
                F.auto_flow = nf; F.af_dir = 0; F.fg_ms = 0;
            }
        } else F.af_dir = 0;
        flow = F.auto_flow;
    }
    if (!F.chain || F.ext.width != e.width || F.ext.height != e.height || F.flow != flow || F.gens < gens || F.quality != quality) {
        if (!rebuild(e, flow, gens, quality)) { vka_tex_drop(im); unavailable("%s", "the frame generation chain could not be made (see the [RED] lines)"); vka_fps_fg(-1, 1); return 0; }
    }
    F.pacer.decoupled = &nxhost_fps_decoupled && nxhost_fps_decoupled();   // the game's speed follows its measured rate (hle_fpslock.c sead_rate), not only held by the limiter
    F.pacer.min_base = (float)VKA_ENV("DM_LSFG_MIN_FPS", 30);   // its floor (live)
    lsfg_plan plan = lsfg_pacer_plan(&F.pacer, F.gens, mult, target, mono_s());
    uint64_t count = F.count++;
    int warm = plan.warm && count + 1 >= 2;
    F.warm_streak = warm ? F.warm_streak + 1 : 0;
    size_t ng = warm && F.warm_streak >= 2 ? plan.generations : 0;
    if (ng > F.gens) ng = F.gens;
    probe_poll();   // (the still-picture check: the submissions done since)
    int still = ng && F.still >= 2 && F.probe_buf && VKA_ENV("DM_LSFG_STILL", 1);
    if (still) { ng = 0; F.pacer.issued = 0; F.still_frames++; }
    // The panel shows one image a refresh (FIFO): more images per game frame than refreshes in the game's period hold the present thread, and the game behind it, to a
    // fraction of its rate. The window asks for the output rate (out_period above), but the system may keep the panel lower: on the Honor phone (LTPO) it sometimes stayed at
    // 60 Hz with the request for 120, and x2 held Donkey Kong Bananza's 60 fps menus at 30 fps (its level was reached 18 s later, int-792f6e4d t1). Generated frames only
    // as many as the refreshes in the game's own period leave room for (its requested period, not the measured interval, which a throttled game would lengthen)
    // DM_VK_FG_REFRESH_CAP=0: no cap (a driver whose display timing reports a refresh the panel does not run at)
    uint64_t rns = VKA_ENV("DM_VK_FG_REFRESH_CAP", 1) ? vka_refresh_ns() : 0; int capped = 0;
    if (ng && rns && period > 0) {
        // The game's frame period: the present's (at least the frame limit's, above), or the measured one when longer. With DM_PRESENT_INTERVAL=display (Donkey Kong
        // Bananza's uncapped mode) every present declares the host's tick (8.3 ms at 120 Hz) whatever the game's own rate, which left no refresh for a generated frame and
        // generation never started; held at 60 on a 120 Hz panel its measured interval came out at 15.3-15.5 ms (1.85 refreshes, under the 1.9 below), where the limit's
        // 16.7 ms leaves room for the one generated frame that fits
        double gp = F.pacer.smoothed_interval > period ? F.pacer.smoothed_interval : period;
        double n = gp * 1e9 / (double)rns;   // refreshes in a game frame (60 fps on 120.46 Hz: 2.01; on 60.00 Hz: 1.00)
        size_t fit = n < 1.9 ? 0 : (size_t)(n + 0.1) - 1;
        // The verdict held (DM_LSFG_CAP_HOLD=<ms>, live, default 3000; 0 = none): a new one is taken after it held for that long, a third of it when it is lower. Generating
        // slows a game whose speed follows its rate, which lengthens its interval and lets it fit, and not generating speeds it up again: Donkey Kong Bananza at 4x on
        // the Thor's 60 Hz panel (Turnip, 2026-11-19) ran 20-25 fps generating and 37 without, and the verdict flipped with the game's 30.3-31.7 ms intervals
        double hold_s = VKA_ENV("DM_LSFG_CAP_HOLD", 3000) / 1000.0;
        if (!F.fit_set || hold_s <= 0) { F.fit_set = 1; F.fit_held = F.fit_pending = fit; F.fit_since = 0; }
        else if (fit == F.fit_held) { F.fit_pending = fit; F.fit_since = 0; }
        else if (fit != F.fit_pending || !F.fit_since) { F.fit_pending = fit; F.fit_since = tnow; }
        else if (tnow - F.fit_since >= (fit < F.fit_held ? hold_s / 3 : hold_s)) { F.fit_held = fit; F.fit_since = 0; }
        fit = F.fit_held;
        if (ng > fit) { ng = fit; capped = 1; F.pacer.issued = ng; }   // the pacer counts what was generated (its unloaded rate is measured on frames without)
    }

    unsigned k = F.fi++ % NFG;
    if (!F.fr[k].cb) {
        if (!F.pool) { VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf}; vk.vkCreateCommandPool(V.dev, &pi, NULL, &F.pool); }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = F.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        vk.vkAllocateCommandBuffers(V.dev, &ai, &F.fr[k].cb); vk.vkCreateFence(V.dev, &fi, NULL, &F.fr[k].fence);
    }
    for (VkResult w; !vka_hung() && (w = vk.vkWaitForFences(V.dev, 1, &F.fr[k].fence, VK_TRUE, 100000000ull)) == VK_TIMEOUT;) {}   // bounded, as present_one's
    if (vka_hung()) { vka_tex_drop(im); return 0; }
    gputime_take(k); probe_poll();
    if (F.fr[k].held) { vka_tex_drop(F.fr[k].held); F.fr[k].held = NULL; }
    vk.vkResetFences(V.dev, 1, &F.fr[k].fence);
    VkCommandBuffer cb = F.fr[k].cb; vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    // DM_LSFG_SCHED=0 (live): the generated frames one after another with a barrier before every pass (Eden's order) instead of the wavefront (lsfg_chain.c), for A/B
    int in_order = !VKA_ENV("DM_LSFG_SCHED", 1), timed = ng && VKA_ENV("DM_LSFG_GPUTIME", 1) && gputime_ok();
    if (timed) T.reset(cb, T.qp, 2 * k, 2);
    vka_barrier(cb);   // the replay's writes to the window texture, and the presents still reading the outputs (earlier submissions)
    if (timed) T.write(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, T.qp, 2 * k);
    if (wv) {   // the same rectangle as the blit (1:1, no flip), without the read in the image's format
        VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {cx, cy, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {(uint32_t)cw, (uint32_t)ch, 1}};
        vk.vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, lsfg_chain_input(F.chain, count), VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{cx, cy, 0}, {cx + cw, cy + ch, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)e.width, (int32_t)e.height, 1}}};
        vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, lsfg_chain_input(F.chain, count), VK_IMAGE_LAYOUT_GENERAL, 1, &b, fit ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    if (warm) lsfg_chain_shared(F.chain, cb, count);
    if (ng) lsfg_chain_generate(F.chain, cb, count, ng, in_order);
    if (timed) T.write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, T.qp, 2 * k + 1);
    F.fr[k].timed = timed; F.fr[k].serial = in_order; F.fr[k].ng = ng; F.fr[k].flow = F.flow; F.fr[k].cgen = F.cgen;
    vka_barrier(cb);   // the outputs for the presents after this
    if (warm && F.probe_buf) {   // the still-picture check's probe, behind the barrier above (the shared passes wrote it)
        VkExtent2D pe; VkImage pimg = lsfg_chain_probe(F.chain, &pe);
        VkBufferImageCopy pc = {(VkDeviceSize)k * F.probe_stride, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {pe.width, pe.height, 1}};
        vk.vkCmdCopyImageToBuffer(cb, pimg, VK_IMAGE_LAYOUT_GENERAL, F.probe_buf, 1, &pc);
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        F.fr[k].probed = 1; F.fr[k].probe_count = count;
    }
    vk.vkEndCommandBuffer(cb);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &fence};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = fence ? 1 : 0, .pWaitSemaphores = &V.tl, .pWaitDstStageMask = &st,
                       .commandBufferCount = 1, .pCommandBuffers = &cb};
    // The game frame done on the GPU before the submission, as vka_present.c present_one does before its copy (DM_VK_PRESENT_PREWAIT): a submission that waits for a
    // timeline value still pending is held in the driver (Turnip on KGSL emulates timeline semaphores), and the generated frames' copies and presents queued behind it
    // were shown by Android before the copies were done. On the AYN Thor's rotated main screen (Turnip, 60 Hz) the part of the swapchain image not yet written showed as
    // a dark vertical bar that moved from frame to frame; the frames themselves (upstream's DM_LSFG_DUMP) and a screen recording (composed later) were clean. Only a value already
    // handed to the queue is waited for (a later one is never waited for: the submission below would wait for a batch queued behind it), and never past a hang
    if (fence && !vka_hung() && VKA_ENV("DM_VK_PRESENT_PREWAIT", 1)) {
        VkResult vka_tl_wait(VkSemaphore s, uint64_t v, uint64_t timeout_ns);
        if (fence <= __atomic_load_n(&V.tl_queued, __ATOMIC_ACQUIRE)) vka_tl_wait(V.tl, fence, vka_hang_ns());
    }
    pthread_mutex_lock(&V.qmu);
    if (fence > V.tl_queued) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("frame generation waited for fence %llu, %llu is the last handed to the queue: waits for that", (unsigned long long)fence, (unsigned long long)V.tl_queued); fence = V.tl_queued; }
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, F.fr[k].fence); pthread_mutex_unlock(&V.qmu);
    if (r) { vk.vkDestroyFence(V.dev, F.fr[k].fence, NULL); VkFenceCreateInfo sfi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; vk.vkCreateFence(V.dev, &sfi, NULL, &F.fr[k].fence); }   // (reset and never submitted: the slot's next wait would not end)
    F.fr[k].held = im;   // the window texture stays alive until this submission is done
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("frame generation submission %d", r); ng = 0; F.fr[k].timed = 0; F.fr[k].probed = 0; }
    for (size_t g = 0; g < ng; g++) gen[g] = F.out[g];
    double interval = F.pacer.smoothed_interval > 0 ? F.pacer.smoothed_interval : period;
    if (interval < period) interval = period;
    if (ng) *hold = interval / (double)(ng + 1);
    // A hold within 0.15 refresh of a whole number of refreshes is that number: the measured interval runs a little long (34 ms at a 30 fps frame step), and a hold of 17 ms on a
    // 16.7 ms panel let each image's desired time (vka_present.c, the previous one's plus the hold) drift across a refresh every few dozen frames, two images in one refresh
    if (ng && rns) { double r = *hold * 1e9 / (double)rns, w = floor(r + 0.5); if (w >= 1 && fabs(r - w) <= 0.15) *hold = w * (double)rns / 1e9; }
    int st2 = ng ? 1 : 2;
    static int was_capped, was_still;
    // Why nothing is generated, in the status the launcher shows ("waiting for a steady frame rate" covered every case before, also a game held under its floor for good)
    char idle[160] = "on, waiting for a steady frame rate";
    float base = F.pacer.smoothed_interval > 0 ? 1.0f / F.pacer.smoothed_interval : 0;
    if (st2 == 2 && target > 0 && plan.warm && base > 0) {
        if (F.pacer.decoupled && base < lsfg_pacer_floor(&F.pacer))
            snprintf(idle, sizeof idle, "on, the game runs at %.0f fps, under the %.0f fps it is held to (DM_LSFG_MIN_FPS): generating would slow it", base, lsfg_pacer_floor(&F.pacer));
        else if (base >= target * 0.95f) snprintf(idle, sizeof idle, "on, the game itself reaches the %d fps target", target);
    }
    static char last_idle[160];
    int idle_changed = st2 == 2 && strcmp(idle, last_idle);
    if (atomic_exchange(&g_state, st2) != st2 || (st2 == 2 && (capped != was_capped || still != was_still || idle_changed))) {
        snprintf(last_idle, sizeof last_idle, "%s", idle);
        char why[96]; snprintf(why, sizeof why, "on, the display runs at %.0f Hz: no refresh left for a generated frame", rns ? 1e9 / (double)rns : 0.0);
        reason("%s", st2 == 1 ? "generating" : still ? "on, the picture is still: nothing to generate" : capped ? why : idle);
        { static double last; if (st2 == 2 && still && tnow - last >= 10.0) { last = tnow; VLOG("frame generation: the picture is still (%u identical game frames): not generating until it changes (at most a line in 10 s)", F.still + 1); } }
        static int ncap; if (st2 == 2 && capped && ncap++ < 4) VLOG("frame generation: not generating, %s (game frame period %.1f ms, measured %.1f ms)", why + 4, period * 1e3, F.pacer.smoothed_interval * 1e3);
        static int logged; if (st2 == 1 && !logged) { logged = 1; VLOG("frame generation: first %zu generated frame(s), game frame interval %.1f ms", ng, interval * 1e3); }
    }
    was_capped = st2 == 2 && capped; was_still = st2 == 2 && still;
    vka_fps_fg(st2, (int)ng + 1);
    return (int)ng;
}
#endif
#endif
