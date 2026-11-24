// NVN textures as Vulkan images, and the texture / sampler pools as update-after-bind descriptor arrays.
// An image lives in its own device memory, not over the pool: Vulkan's optimal tiling is not the Tegra block-linear layout. A texture the CPU filled (packaged data, WriteTexels,
// a CPU write the dirty tracker saw: mtl_tex_cpu_written) is deswizzled from its pool storage (nvn_tex.c layout, tex/nvn_deswizzle.c) into scratch memory and copied in;
// every other texture starts cleared to zero and is GPU content from then on (render targets, copy destinations), as nvn_mtl.m's private textures are.
// Images are found by the texture's serial (the game copies texture objects by value). A texture over the same storage and shape as a live one shares its image, so a clear through
// one object and a present through another meet. Every image stays in VK_IMAGE_LAYOUT_GENERAL after its first use: no layout tracking until the draw path needs it
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_texown.h"
#include "vka_wbfollow.h"
#include "../nvn/nvn_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../nvn/dmsh_abi_vk.h"
#include "../nvn/nvn_dirty.h"
#include "../../host/plat/plat.h"   // plat_mem_read (gw_shadow_read)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "vka_zshare.h"
#include "vka_texlimit.h"
#include "vka_spvpatch.h"   // (VKA_PFB_* flags: "Texture pool reads")
#include "vka_bcdec.h"

// ---------- Formats ----------
typedef struct { VkFormat vk; uint8_t kind, srgb; } fmt_ent;
static fmt_ent fmt_ent_of(int f) {
#define C(v) (fmt_ent){VK_FORMAT_##v, 0, 0}
#define U(v) (fmt_ent){VK_FORMAT_##v, 1, 0}
#define S(v) (fmt_ent){VK_FORMAT_##v, 2, 0}
#define SRGB(v) (fmt_ent){VK_FORMAT_##v, 0, 1}
    if (f >= 0x79 && f <= 0x86) return (fmt_ent){(VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (f - 0x79)), 0, 0};   // the 14 ASTC block sizes are in the same order in both enums
    if (f >= 0x87 && f <= 0x94) return (fmt_ent){(VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (f - 0x87)), 0, 1};
    switch (f) {
    case 0x01: case 0x51: case 0x52: return f == 0x01 ? C(R8_UNORM) : f == 0x51 ? U(R8_UINT) : S(R8_SINT);
    case 0x02: return C(R8_SNORM); case 0x03: return U(R8_UINT); case 0x04: return S(R8_SINT);
    case 0x05: return C(R16_SFLOAT); case 0x06: return C(R16_UNORM); case 0x07: return C(R16_SNORM); case 0x08: case 0x53: return U(R16_UINT); case 0x09: case 0x54: return S(R16_SINT);
    case 0x0A: return C(R32_SFLOAT); case 0x0B: case 0x55: return U(R32_UINT); case 0x0C: case 0x56: return S(R32_SINT);
    case 0x0D: return C(R8G8_UNORM); case 0x0E: return C(R8G8_SNORM); case 0x0F: case 0x57: return U(R8G8_UINT); case 0x10: case 0x58: return S(R8G8_SINT);
    case 0x11: return C(R16G16_SFLOAT); case 0x12: return C(R16G16_UNORM); case 0x13: return C(R16G16_SNORM); case 0x14: case 0x59: return U(R16G16_UINT); case 0x15: case 0x5A: return S(R16G16_SINT);
    case 0x16: return C(R32G32_SFLOAT); case 0x17: case 0x5B: return U(R32G32_UINT); case 0x18: case 0x5C: return S(R32G32_SINT);
    case 0x22: return C(R32G32B32_SFLOAT); case 0x23: case 0x61: return U(R32G32B32_UINT); case 0x24: case 0x62: return S(R32G32B32_SINT);
    case 0x25: case 0x6D: return C(R8G8B8A8_UNORM); case 0x26: case 0x6E: return C(R8G8B8A8_SNORM); case 0x27: case 0x6F: case 0x63: return U(R8G8B8A8_UINT); case 0x28: case 0x70: case 0x64: return S(R8G8B8A8_SINT);
    case 0x29: case 0x71: return C(R16G16B16A16_SFLOAT); case 0x2A: case 0x72: return C(R16G16B16A16_UNORM); case 0x2B: case 0x73: return C(R16G16B16A16_SNORM);
    case 0x2C: case 0x74: case 0x65: return U(R16G16B16A16_UINT); case 0x2D: case 0x75: case 0x66: return S(R16G16B16A16_SINT);
    case 0x2E: case 0x76: return C(R32G32B32A32_SFLOAT); case 0x2F: case 0x77: case 0x67: return U(R32G32B32A32_UINT); case 0x30: case 0x78: case 0x68: return S(R32G32B32A32_SINT);
    case 0x31: return (fmt_ent){VK_FORMAT_S8_UINT, 4, 0};
    case 0x32: return (fmt_ent){VK_FORMAT_D16_UNORM, 3, 0};
    case 0x33: return (fmt_ent){VK_FORMAT_X8_D24_UNORM_PACK32, 3, 0};
    case 0x34: return (fmt_ent){VK_FORMAT_D32_SFLOAT, 3, 0};
    case 0x35: return (fmt_ent){VK_FORMAT_D24_UNORM_S8_UINT, 5, 0};
    case 0x36: return (fmt_ent){VK_FORMAT_D32_SFLOAT_S8_UINT, 5, 0};
    case 0x37: case 0x38: return SRGB(R8G8B8A8_UNORM);
    case 0x39: return C(A4B4G4R4_UNORM_PACK16);   // Maxwell's packed formats start with R in the low bits
    case 0x3A: case 0x3B: return C(A1B5G5R5_UNORM_PACK16);
    case 0x3C: return C(B5G6R5_UNORM_PACK16); case 0x95: return C(R5G6B5_UNORM_PACK16);
    case 0x3D: return C(A2B10G10R10_UNORM_PACK32); case 0x3E: case 0x6B: return U(A2B10G10R10_UINT_PACK32); case 0x69: return C(A2B10G10R10_SNORM_PACK32); case 0x6A: case 0x6C: return S(A2B10G10R10_SINT_PACK32);
    case 0x3F: return C(B10G11R11_UFLOAT_PACK32); case 0x40: return C(E5B9G9R9_UFLOAT_PACK32);
    case 0x41: return C(BC1_RGB_UNORM_BLOCK); case 0x42: return C(BC1_RGBA_UNORM_BLOCK); case 0x43: return C(BC2_UNORM_BLOCK); case 0x44: return C(BC3_UNORM_BLOCK);
    case 0x45: return SRGB(BC1_RGB_UNORM_BLOCK); case 0x46: return SRGB(BC1_RGBA_UNORM_BLOCK); case 0x47: return SRGB(BC2_UNORM_BLOCK); case 0x48: return SRGB(BC3_UNORM_BLOCK);
    case 0x49: return C(BC4_UNORM_BLOCK); case 0x4A: return C(BC4_SNORM_BLOCK); case 0x4B: return C(BC5_UNORM_BLOCK); case 0x4C: return C(BC5_SNORM_BLOCK);
    case 0x4D: return C(BC7_UNORM_BLOCK); case 0x4E: return SRGB(BC7_UNORM_BLOCK); case 0x4F: return C(BC6H_SFLOAT_BLOCK); case 0x50: return C(BC6H_UFLOAT_BLOCK);
    case 0x99: case 0x9A: return C(B8G8R8A8_UNORM); case 0x9B: case 0x9C: return SRGB(B8G8R8A8_UNORM);
    }
#undef C
#undef U
#undef S
#undef SRGB
    return (fmt_ent){VK_FORMAT_UNDEFINED, 0, 0};
}
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK; case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK; case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK ? (VkFormat)(f + 1) : f;
    }
}
static VkFormatFeatureFlags feats(VkFormat f) {
    static struct { VkFormat f; VkFormatFeatureFlags ff; } cache[128]; static int n; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (cache[i].f == f) { VkFormatFeatureFlags ff = cache[i].ff; pthread_mutex_unlock(&mu); return ff; }
    VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(V.pd, f, &p);
    if (n < 128) cache[n++].f = f, cache[n - 1].ff = p.optimalTilingFeatures;
    pthread_mutex_unlock(&mu);
    return p.optimalTilingFeatures;
}
static int fmt_of_slow(int nf, vka_fmt *o);
// By NVN format, answered once (the draws ask for every target and texture view: ~3% of the replay thread in format lookups and the feature cache's lock)
int vka_fmt_of(int nf, vka_fmt *o) {
    static _Atomic uint8_t st[512]; static vka_fmt memo[512];   // st: 0 unknown, 1 has a format (memo), 2 none
    if (nf >= 0 && nf < 512) {
        int k = atomic_load_explicit(&st[nf], memory_order_acquire);
        if (k == 1) { *o = memo[nf]; return 1; }
        if (k == 2) return 0;
        vka_fmt f; int ok = fmt_of_slow(nf, &f);
        if (ok) { memo[nf] = f; *o = f; }
        atomic_store_explicit(&st[nf], ok ? 1 : 2, memory_order_release);
        return ok;
    }
    return fmt_of_slow(nf, o);
}
static int fmt_of_slow(int nf, vka_fmt *o) {
    fmt_ent e = fmt_ent_of(nf); int bw, bh, bpb = nvn_fmt_bpb(nf, &bw, &bh);
    if (e.vk == VK_FORMAT_D24_UNORM_S8_UINT && !(feats(e.vk) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) e.vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (e.vk == VK_FORMAT_X8_D24_UNORM_PACK32 && !(feats(e.vk) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) e.vk = VK_FORMAT_D32_SFLOAT;
    { VkFormat to; uint8_t t2; static int force = -1; if (force < 0) { const char *v = getenv("DM_VK_BC_DECODE"); force = v && atoi(v) > 0; }
      if (vka_bc_kind(e.vk, &to, &t2) && (force || !(feats(e.vk) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) goto decoded; }   // (BC without BC textures: decided below)
    if (e.vk == VK_FORMAT_UNDEFINED || !bpb || !(feats(e.vk) & (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) {
        static uint64_t logged[4]; unsigned b = (unsigned)nf & 255;
        if (!(__atomic_fetch_or(&logged[b >> 6], 1ull << (b & 63), __ATOMIC_RELAXED) >> (b & 63) & 1)) VRED("NVN format %#x has no Vulkan format on this device (VkFormat %d)", nf, e.vk);
        return 0;
    }
decoded:;
    // A BC format on a GPU without BC textures (Mali, Xclipse): the image is made in the decoded format and its blocks are decoded on the CPU at each upload (vka_bcdec.h,
    // bc_copy below); the blocks' own layout stays in bpb / bw / bh. DM_VK_BC_DECODE=1 (restart, a diagnostic) decodes on a GPU that has BC, to run this path there
    uint8_t dec = 0, tb = 0;
    { VkFormat to = e.vk; uint8_t t2 = 0; int k = vka_bc_kind(e.vk, &to, &t2);
      static int force = -1; if (force < 0) { const char *v = getenv("DM_VK_BC_DECODE"); force = v && atoi(v) > 0; }
      if (k && (force || !(feats(e.vk) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) {
          static _Atomic int told; if (!atomic_exchange(&told, 1)) VLOG("BC textures: %s: decoded on the CPU into uncompressed images (first: NVN format %#x into VkFormat %d; 4 to 8 times the blocks' memory)",
                                                                        force ? "DM_VK_BC_DECODE=1" : "this GPU has none", nf, to);
          dec = (uint8_t)k; tb = t2; e.vk = to; } }
    if (e.vk == VK_FORMAT_UNDEFINED || !bpb || !(feats(e.vk) & (VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) return 0;
    *o = (vka_fmt){e.vk, e.srgb ? srgb_twin(e.vk) : e.vk, e.kind, e.srgb, (uint8_t)bpb, (uint8_t)bw, (uint8_t)bh, dec, tb};
    return 1;
}
static VkImageAspectFlags aspect_of(int kind) { return kind == 3 ? VK_IMAGE_ASPECT_DEPTH_BIT : kind == 4 ? VK_IMAGE_ASPECT_STENCIL_BIT : kind == 5 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
static int compressed(const vka_fmt *f) { return f->bw > 1 || f->bh > 1; }

uint64_t vka_n_bar;   // full barriers recorded (vka_exec.c replay statistics)
void vka_barrier_at(VkCommandBuffer cb, const char *file, int line) {
    vka_n_bar++;
    // This barrier also orders the dispatches before it, so a barrier one of them left pending is settled by it (vka_draw.c "Dispatch order"). Only a full memory barrier
    // does that: vka_img_barrier (one image) and the HOST-stage barrier at the submission's end are not global memory dependencies, so they leave it pending
    { extern uint64_t vka_disp_cleared; VkCommandBuffer p = cb; if (cb && atomic_compare_exchange_strong(&vka_disp_pend, &p, VK_NULL_HANDLE)) vka_disp_cleared++; }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vka_gp_active) vka_gp_mark(cb, file, line);
}
// The end of a draw pass none of whose draws wrote memory (vka_draw.c vka_rp_end, DM_VK_RP_SCOPE; after Erick's Ocarina fork, ac09632 / 986c7fa, for every game). The source
// stages are the pass's last ones, colour output and both fragment tests: a source stage mask takes in every stage logically earlier in both primitive pipelines (indirect
// reads, vertex input, the vertex, tessellation, geometry, task and mesh shaders, the fragment shader), so everything the pass read is done before anything after the
// barrier writes over it. The source accesses are everything such a pass can write: its attachments' writes, the loadOp CLEAR, vkCmdClearAttachments and the storeOp STORE.
// The destination is the full barrier's (every later stage, every read and write), so the host, the present, frame generation and later passes see what they saw before.
// It relies on the invariant above vka_rp_end: whatever else was recorded before the pass ended with a barrier of its own covering its stage, reads included
uint64_t vka_n_attachment_bar;   // (vka_exec.c replay statistics)
void vka_attachment_write_barrier_at(VkCommandBuffer cb, const char *file, int line) {
    vka_n_attachment_bar++;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vka_gp_active) vka_gp_mark(cb, file, line);
}
// After the replay's inline uniform copies (vka_exec.c "Inline uniform updates"; after Erick's Ocarina fork, 521269e / d1dca49, for every game). When the copies are recorded,
// everything before them has ended with a full barrier or, for a draw pass without memory writes, the attachment-scoped one (vka_rp_end; its source stages take in every
// stage that read the ranges the copies write), or the one every other replay op ends with (clears, copies, texture copies, dispatches,
// the mesh batch, an image's own memory), or this barrier after the copies before; the parts and submissions before end with theirs, and a barrier's scope runs across
// command buffers in submission order. The copies read fresh scratch memory (the host's writes are visible at the submit) and write guest RAM, so their own writes are all
// that is left to publish: to ALL_COMMANDS with MEMORY_READ | MEMORY_WRITE (uniform, vertex, index and indirect reads, storage, later transfers, and the submission end's
// HOST barrier through this one). Not a full barrier, so it does not settle a dispatch's pending one (none is: exec settled it before the update)
uint64_t vka_n_transfer_bar;   // (vka_exec.c replay statistics)
void vka_transfer_write_barrier_at(VkCommandBuffer cb, const char *file, int line) {
    if (!VKA_ENV("DM_VK_UB_SCOPED", 1)) { vka_barrier_at(cb, file, line); return; }
    vka_n_transfer_bar++;
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    if (vka_gp_active) vka_gp_mark(cb, file, line);
}
void vka_img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// ---------- Images by serial ----------
typedef struct view_ent { struct view_ent *next; VkImageView v; VkFormat f; VkImageViewType t; VkImageAspectFlags asp; uint32_t l0, nl, a0, na; VkComponentMapping swz; } view_ent;
#define VKA_SMAP_IM vka_img
#include "vka_smap.h"
static vksm g_smap;   // (under g_tmu) texture serial -> image; the entry's im NULL = a texture that has no image (format refused)
static vka_img **g_live; static int g_nlive, g_livecap;
static vksm g_cpuw;   // (under g_tmu) serials the CPU wrote before their image existed (a set: vksm with no images)
static pthread_mutex_t g_tmu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_img_bytes;
static _Atomic uint64_t g_retired_bytes;
uint64_t vka_tex_retired_bytes(void) { return atomic_load(&g_retired_bytes); }
static _Atomic int g_dead_pools;
uint64_t vka_frame(void);
static inline void img_touch(vka_img *im) {
    if (!im) return;
    atomic_store_explicit(&im->last_frame, vka_frame(), memory_order_relaxed);
    atomic_store_explicit(&im->last_fence, vka_fence_last() + 1, memory_order_relaxed);
}
static int g_zwant;   // (under g_tmu) img_create_ may make a zero-shared image: vka_tex_get's new image whose content is ST_ZERO ("Zero-shared images" below)

// The images' memory for the stats line: all of it, and the part whose content the GPU wrote (render targets, copy and storage destinations); the rest was filled from the
// texture's storage in guest memory, which stays in its memory pool, so those bytes are held twice
uint64_t vka_tex_mem_nolock(int *n) { if (n) *n = g_nlive; return g_img_bytes + vka_imem_zbytes_nolock() + atomic_load(&g_retired_bytes); }
uint64_t vka_tex_mem(int *n, uint64_t *gpu_written) {
    pthread_mutex_lock(&g_tmu); uint64_t all = g_img_bytes + vka_imem_zbytes() + atomic_load(&g_retired_bytes), gw = 0;
    for (int i = 0; i < g_nlive; i++) if (g_live[i]->gpu_owned) gw += g_live[i]->bytes;
    if (n) *n = g_nlive; if (gpu_written) *gpu_written = gw;
    pthread_mutex_unlock(&g_tmu); return all;
}
// The images' bytes by where their content came from, and the part whose memory requirements would let it live on its texture's storage in the pool (the memory type the
// pools use, a size within the storage, the storage's offset aligned as the image needs): what binding images onto pool memory could give back. Only measured: an image over
// its storage would overwrite the guest's block-linear bytes with the driver's layout, so a texture the CPU rewrites in part (streamed mip levels) would re-upload garbage;
// only content no CPU write follows is a candidate (docs/reference/rendering.md "Images filled from pool storage hold texture bytes twice on the Vulkan replay")
void vka_tex_alias_stats(char *b, size_t n) {
    uint64_t by[3][2] = {{0}}, gw = 0; int cnt[3] = {0};
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < g_nlive; i++) { const vka_img *im = g_live[i]; if (im->gpu_owned) { gw += im->bytes; continue; }
        int m = im->mode < 3 ? im->mode : 0; by[m][0] += im->bytes; cnt[m]++; if (im->pool_fit) by[m][1] += im->bytes; }
    pthread_mutex_unlock(&g_tmu);
    snprintf(b, n, "images not GPU-written: cleared %.0f MB (%d, %.0f MB would fit their storage), filled once %.0f MB (%d, %.0f MB fit), tracked %.0f MB (%d, %.0f MB fit); GPU-written %.0f MB",
             by[0][0] / 1e6, cnt[0], by[0][1] / 1e6, by[1][0] / 1e6, cnt[1], by[1][1] / 1e6, by[2][0] / 1e6, cnt[2], by[2][1] / 1e6, gw / 1e6);
}
// The serial map and the cpuw set (vka_smap.h, from upstream Banana Native v0.3.13): thin wrappers so the callers (and t_android_texlife.c's map_put) keep their names
static smap_ent *map_get(uint64_t s) { return vksm_get(&g_smap, s); }
static _Atomic uint32_t g_tgen, *g_tver, *g_sver;   // the recording thread's view memo: see unref_locked
// The generation of image view destruction: img_destroy bumps it before it destroys any view, so a pool slot memo entry (view handle, g_vgen) taken after the slot's view was
// read still names that live view while g_vgen has not moved ("Pool slot writes" at vka_tex_prepare). img_destroy runs from vka_after callbacks, on any thread.
// g_vgen_h: the same per bucket of view handles, bumped for each view img_destroy destroys (after g_vgen, before vkDestroyImageView). The memo keys on a view's bucket,
// so a streaming game's retired images (most frames) leave the memo of every other view warm; one global counter alone would let any destroy cool all of it
enum { VGEN_HB = 4096 };
static _Atomic uint32_t g_vgen, g_vgen_h[VGEN_HB];
static inline _Atomic uint32_t *vgen_h(VkImageView v) { return &g_vgen_h[(size_t)(((uint64_t)(uintptr_t)v * 0x9E3779B97F4A7C15ull) >> 52) & (VGEN_HB - 1)]; }
static void map_put(uint64_t s, vka_img *im) { vksm_put(&g_smap, s, im); }   // (no memory for a new key: the texture has no entry, so its next lookup makes its image again; the old image goes when its refs do)
static void map_del(uint64_t s) { if (vksm_del(&g_smap, s)) atomic_fetch_add_explicit(&g_tgen, 1, memory_order_release); }   // a tombstone (never a valid serial)
static void map_del_im(vka_img *im) { if (im && vksm_del_im(&g_smap, im)) atomic_fetch_add_explicit(&g_tgen, 1, memory_order_release); }
static int cpuw_take(uint64_t s, int add) {   // add: insert; else: remove and say whether it was there
    if (add) { vksm_put(&g_cpuw, s, NULL); return 0; }
    return vksm_del(&g_cpuw, s);
}

static void img_destroy(void *arg) {
    // First, before the zero-shared image's views and its own: every vkDestroyImageView of a pool view comes after a g_vgen bump (the pool slot memo then writes again,
    // since a destroyed view's handle may be recycled for a new one). This must stay the only place that destroys views the texture pool names
    atomic_fetch_add_explicit(&g_vgen, 1, memory_order_release);
    vka_img *im = arg;
    if (im->zold) { img_destroy(im->zold); im->zold = NULL; }   // (the zero-shared image it left: no holder is left either)
    for (view_ent *v = im->views, *n; v; v = n) { n = v->next; atomic_fetch_add_explicit(vgen_h(v->v), 1, memory_order_release); vk.vkDestroyImageView(V.dev, v->v, NULL); free(v); }
    if (im->rs) { vks_free(im->rs); vks_va_give(im->va); }   // (its binds dropped or run first)
    if (im->img) vk.vkDestroyImage(V.dev, im->img, NULL);
    vka_imem_free(&im->m);   // after the image: its run or allocation (vka_after: the GPU is done with it)
    atomic_fetch_sub(&g_retired_bytes, im->retired_bytes);
    free(im->snap); free(im);
}
static inline void im_unhold_locked(vka_img *im) {
    if (!im) return;
    if (im->holds > 0) im->holds--;
    if (!im->holds && im->zold) {
        vka_after(vka_fence_last() + 1, img_destroy, im->zold);
        im->zold = NULL;
    }
}
#define VIEWS(im) (*(view_ent **)&(im)->views)

// The recording thread's memo of texture ID -> view (vka_tex_id_view_rec) and sampler ID -> sampler (vka_samp_id_rec): an entry holds while the ID was not registered
// again (g_tver / g_sver, per ID) and no texture mapping was dropped or image let go since (g_tgen). It saves the table lock and the lookups 3-4 times a texture binding
static vka_img *g_big[64]; static int g_nbig;   // large images (DM_VK_BIGIMG_MB), for the storage overlap check (big_note)
static void unref_locked_at(vka_img *im, uint64_t fence) {
    atomic_fetch_add_explicit(&g_tgen, 1, memory_order_release);
    if (--im->refs) return;
    map_del_im(im);
    for (int i = 0; i < g_nlive; i++) if (g_live[i] == im) { g_live[i] = g_live[--g_nlive]; break; }
    for (int i = 0; i < g_nbig; i++) if (g_big[i] == im) { g_big[i] = g_big[--g_nbig]; break; }
    g_img_bytes -= im->bytes;
    im->retired_bytes = im->bytes;
    atomic_fetch_add(&g_retired_bytes, im->retired_bytes);
    vka_after(fence, img_destroy, im);
}
static void unref_locked(vka_img *im) { unref_locked_at(im, vka_fence_last() + 1); }

// Textures of one memory and shape share an image (one storage, many texture objects: a clear through one and a present through another meet), and so do ones of another
// uncompressed colour format of the same texel size whose formats (storage and view) the image's format list takes: they see the image through views in their own format.
// A texture whose format the list lacks gets its own image of its format, and the pair is learned (Learned view formats, below), so the images made from then on, and in
// the next runs from their first use, take it and are shared. Shared without the format, its views fell back to the image's format (vka_view): DKB's R11G11B10 HDR target
// over an RGBA8 texture's memory was drawn and sampled as RGBA8 UNORM, and a colour attachment view in another format than the pipeline's rendering format breaks a valid
// usage rule. The split loses a bit-for-bit reinterpretation between the two formats through the memory in that run, which the format list never allowed anyway; it stays
// for the run without DM_TEMP's file, once the table is full, or under DM_VK_VIEW_LEARN=0. Not compressed textures of another format: a view takes only a format of the image's own compatibility class (BC1 and BC4 blocks have one size, as BC3, BC5, BC7 and ASTC 4x4 blocks have, but no view
// reinterprets one as the other), and view learning takes uncompressed pairs only, so a BC4 texture over a BC1 image's memory was sampled through the BC1 view: garbage.
// Such a texture gets its own image, filled from its own storage; what it loses is GPU content passing between the two through the memory, which compressed textures
// (never render targets) do not have
static int img_lists(const vka_img *a, VkFormat v) {   // the image's format list takes views in v (img_create)
    if (v == a->f.vk || v == a->alt || a->any_fmt) return 1;
    for (int k = 0; k < a->nx; k++) if (a->xfmt[k] == v) return 1;
    return 0;
}
static int vl_learn(VkFormat a, VkFormat b, uint32_t w, uint32_t h);
static pthread_mutex_t g_vlmu;
static int same_shape(const vka_img *a, const tex_t *t, const vka_fmt *f, int learn) {   // learn = 0: a diagnostic's question (vka_cap_shared), nothing learned or logged
    if (a->mem != t->mem || a->d.target != t->d.target || a->d.width != t->d.width || a->d.height != t->d.height || a->d.depth != t->d.depth || a->d.levels != t->d.levels || a->d.samples != t->d.samples) return 0;
    if (a->f.vk == f->vk) return 1;
    if (!(a->f.kind == 0 && f->kind == 0 && a->f.bpb == f->bpb && !compressed(&a->f) && !compressed(f))) return 0;
    int lv = img_lists(a, f->vk), lw = img_lists(a, f->view);
    if (lv && lw) return 1;
    if (!learn) return 0;
    pthread_mutex_lock(&g_vlmu);   // (under g_tmu: the order vka_view takes them in)
    int learned = (!lv && vl_learn(a->f.vk, f->vk, a->w, a->h)) | (!lw && f->view != f->vk && vl_learn(a->f.vk, f->view, a->w, a->h));
    pthread_mutex_unlock(&g_vlmu);
    static struct { VkFormat a, b; } seen[16]; static int ns; int k = 0;   // (under g_tmu)
    while (k < ns && !(seen[k].a == a->f.vk && seen[k].b == f->vk)) k++;
    if (k == ns && ns < 16) { seen[ns].a = a->f.vk; seen[ns++].b = f->vk;
        VLOG("textures of VkFormat %d and %d over the same memory (%ux%u): separate images, the first's format list cannot take the second%s", a->f.vk, f->vk, a->w, a->h,
             learned ? " (learned: images of the first format and this size made from now on, and in the next runs, take it and are shared)" : ""); }
    return 0;
}

// ---------- Learned view formats ----------
// Textures of one memory and shape share one image (same_shape) when the image's format list takes their formats: content passes between them through it, whatever the
// operation (draws, copies, clears, presents, write-back). Images keep Adreno's compression with a short format list (own format, sRGB twin) instead of a bare
// MUTABLE_FORMAT, and an image cannot take a new format once made, so the formats are learned: a texture of another format over the memory (same_shape), or a view asked
// in a format outside the list (vka_view: a texture view's own format), records the pair (image format, other format, size), and every image made from then on with that
// format and size takes the other format in its list (MUTABLE_FORMAT; that image loses UBWC). The pairs are kept in $DM_TEMP/vk_view_formats.txt, so the next run makes
// the image right from its first use. Meanwhile the texture gets its own image (same_shape), and a view keeps the fallback to the image's own format ([RED]) for the life
// of an image made before the pair was learned (its views are in use; replacing it would mean rebinding every view and descriptor); so it stays in a run without DM_TEMP,
// once the table is full (64 pairs) or with 4 formats learned for that image format and size (VL_PER_IMAGE).
// A pair is taken (learned, or read from the file) only when both formats are uncompressed colour formats (float / unorm, uint or sint: kinds 0-2) of one texel size: a
// format list may hold only formats size-compatible with the image's (Vulkan's compatibility classes of uncompressed colour formats are by texel size, so RGBA8 and R32_UINT
// are one class), and a bad pair in the file (an older build's, a hand edit) would break that rule on every image of its format in every run. Integer views are the usual
// reinterpretation of a compute pass: Zelda: Ocarina of Time's agl upscaler (agl_ndsr_vr*.pck) stores its RGBA8 and R11G11B10F output through R32_UINT storage views; with
// only kind 0 taken those pairs were never learned, its output stayed unbound and gameplay showed a black picture. The file is
// written whole (a temporary file renamed over it), so a run that ends while writing leaves the old one. DM_VK_VIEW_LEARN=0: nothing learned or read (views in the image's
// own format, as before)
typedef struct { VkFormat a, b; uint32_t w, h; } vlearn;
enum { NVL = 64 };
static vlearn g_vl[NVL]; static int g_nvl, g_vl_loaded; static char g_vl_path[1024]; static pthread_mutex_t g_vlmu = PTHREAD_MUTEX_INITIALIZER;
enum { VL_PER_IMAGE = 4 };   // learned formats one image takes at most (vka_img.xfmt)   // under g_vlmu
static int vl_bytes(VkFormat v) {   // bytes per texel of an uncompressed colour format (float / unorm, uint or sint: kinds 0-2) of the textures (an image's own format or its sRGB view format), else 0
    for (int nf = 1; nf < 0x100; nf++) {
        fmt_ent e = fmt_ent_of(nf); int bw = 0, bh = 0, bpb;
        if (e.vk == VK_FORMAT_UNDEFINED || !(e.vk == v || (e.srgb && srgb_twin(e.vk) == v))) continue;
        return e.kind <= 2 && (bpb = nvn_fmt_bpb(nf, &bw, &bh)) && bw == 1 && bh == 1 ? bpb : 0;
    }
    return 0;
}
static int vl_pair_ok(VkFormat a, VkFormat b) { int n = vl_bytes(a); return n && a != b && n == vl_bytes(b); }
static void vl_save(void) {   // caller holds g_vlmu
    if (!g_vl_path[0]) return;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.%d.tmp", g_vl_path, (int)getpid());
    FILE *f = fopen(tmp, "we"); if (!f) { VRED("learned view formats: cannot write %s (errno %d): the pairs learned now are not kept", tmp, errno); return; }
    for (int i = 0; i < g_nvl; i++) fprintf(f, "%u %u %u %u\n", (unsigned)g_vl[i].a, (unsigned)g_vl[i].b, g_vl[i].w, g_vl[i].h);
    int bad = fflush(f) != 0 || ferror(f); bad |= fclose(f) != 0;
    if (bad || rename(tmp, g_vl_path)) { VRED("learned view formats: cannot write %s (errno %d): the pairs learned now are not kept", g_vl_path, errno); unlink(tmp); }
}
static void vl_load(void) {   // caller holds g_vlmu
    if (g_vl_loaded) return; g_vl_loaded = 1;
    const char *t = getenv("DM_TEMP"); if (!t || !*t) return;
    snprintf(g_vl_path, sizeof g_vl_path, "%s/vk_view_formats.txt", t);
    FILE *f = fopen(g_vl_path, "re"); if (!f) return;
    char ln[128]; int dropped = 0;
    while (g_nvl < NVL && fgets(ln, sizeof ln, f)) {
        unsigned a, b, w, h; char more;
        if (sscanf(ln, "%u %u %u %u %c", &a, &b, &w, &h, &more) != 4 || !w || !h || !vl_pair_ok((VkFormat)a, (VkFormat)b)) { dropped++; continue; }
        int dup = 0, same = 0;
        for (int i = 0; i < g_nvl; i++) { dup |= g_vl[i].a == (VkFormat)a && g_vl[i].b == (VkFormat)b && g_vl[i].w == w && g_vl[i].h == h; same += g_vl[i].a == (VkFormat)a && g_vl[i].w == w && g_vl[i].h == h; }
        if (dup || same >= VL_PER_IMAGE) { dropped++; continue; }
        g_vl[g_nvl++] = (vlearn){(VkFormat)a, (VkFormat)b, w, h};
    }
    fclose(f);
    if (g_nvl || dropped) VLOG("learned view formats %s: %d pairs (images of those formats and sizes take the second format in their format list)%s", g_vl_path, g_nvl,
                               dropped ? "; dropped lines that are not a pair of uncompressed colour formats of one texel size, duplicates and those past 4 for one format and size (the file is written again without them)" : "");
    if (dropped) vl_save();
}
static int vl_formats(VkFormat a, uint32_t w, uint32_t h, VkFormat *out, int max) {   // caller holds g_vlmu
    if (!VKA_ENV("DM_VK_VIEW_LEARN", 1)) return 0;
    vl_load(); int n = 0;
    for (int i = 0; i < g_nvl && n < max; i++) if (g_vl[i].a == a && g_vl[i].w == w && g_vl[i].h == h) { int dup = 0; for (int k = 0; k < n; k++) dup |= out[k] == g_vl[i].b; if (!dup) out[n++] = g_vl[i].b; }   // (vl_learn and vl_load keep at most VL_PER_IMAGE a format and size)
    return n;
}
static int vl_learn(VkFormat a, VkFormat b, uint32_t w, uint32_t h) {   // caller holds g_vlmu; 1 = new
    if (!VKA_ENV("DM_VK_VIEW_LEARN", 1) || !vl_pair_ok(a, b)) return 0;
    vl_load();
    int same = 0;
    for (int i = 0; i < g_nvl; i++) { if (g_vl[i].a == a && g_vl[i].b == b && g_vl[i].w == w && g_vl[i].h == h) return 0; same += g_vl[i].a == a && g_vl[i].w == w && g_vl[i].h == h; }
    if (g_nvl == NVL || same >= VL_PER_IMAGE) {
        static int nr; if (nr++ < 4) VRED("learned view formats: VkFormat %d as a view of VkFormat %d (%ux%u) not learned: %s", b, a, w, h, same >= VL_PER_IMAGE ? "that image format and size has 4 already" : "the table is full (64 pairs)");
        return 0;
    }
    g_vl[g_nvl++] = (vlearn){a, b, w, h};
    vl_save();
    return 1;
}

static VkImageViewType view_type(int target, uint32_t layers) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return VK_IMAGE_VIEW_TYPE_2D; case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // one-row 2D images (img_create)
    case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return layers >= 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return layers >= 12 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

uint64_t nvn_pool_live(const void *pool, unsigned *flags, int *sparse); void nvn_pool_use(int on);   // hle_nvn.c: the replay reads a texture's pool only through it (the game may have finalized it)
uint64_t nvn_pool_fin(void);
static int pool_sparse(const void *pool) { int sp = 0; return pool && nvn_pool_live(pool, NULL, &sp) && sp; }
static unsigned pool_flags(const void *pool) { unsigned f = 0; nvn_pool_live(pool, &f, NULL); return f; }
uint64_t nvn_sparse_gen(void); uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n);   // hle_nvn.c: sparse virtual pools
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp); void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n);
int nvn_sparse_any(const void *pool, uint64_t a, uint64_t n); int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n);
static void upload(vka_img *im, const tex_t *t, VkCommandBuffer cb);
static uint64_t g_rows_up;   // bytes of partial row re-uploads (upload_rows_tracked)

// ---------- Sparse textures: images of their mapped levels ----------
// A sampled texture of a sparse virtual pool holds what the physical pages mapped behind it hold (hle_nvn.c), and games map only part of it: Mario Kart World's 8192x8192 x 68-layer
// ASTC 8x8 arrays (1.45 GB each, 12 levels) have 25 to 130 MB mapped at the title, its levels from 2048 or 1024 texels down; 45 such textures were 8.8 GB uploaded in one frame
// ("Sparse texture arrays made at their full size" in docs/reference/nvn-objects.md). Vulkan's answer, sparse residency, does not fit them on the Adreno 840: its 3 GiB of sparse
// address space is under the 6.8 GB these textures span. So the image holds only the levels where pages are mapped: image level 0 is the texture's finest level with a mapped
// page in any layer (base), the finer ones are left out. A view or a target of a left-out level starts at the image's level 0 (the finest data there is). Sampling with an
// implicit level of detail picks the same texels as with the whole chain (normalized coordinates on an image 2^base times smaller: the level of detail is base lower, which is
// the level the image numbers from 0), except that the levels finer than base read base; an explicit level of detail, a texel fetch with a level or a size query sees the image's
// numbering (open, nvn-objects.md). Each level and layer is uploaded from the page table only when its pages or mappings change, unmapped pages as zero texels; when a finer level
// is mapped the image is made again with it (rebase), and when the finest mapped level becomes two or more levels coarser it is made again without the finer ones, which frees them.
// DM_VK_SPARSE_LEVELS=0 makes every sparse texture at its full size (the earlier behaviour)
static int sparse_sampled(const tex_t *t, const vka_fmt *f) {   // a sparse pool's texture whose content is its storage (storage_mode ST_SPARSE)
    const nvn_texdesc *d = &t->d;
    return t->mem && t->pool && pool_sparse(t->pool) && d->samples <= 1 && f->kind < 3 && d->target != NVN_TEXTURE_TARGET_BUFFER
        && !(d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE));
}
// A sparse pool's SPARSE texture whose content is the GPU's (a render target or shader image: COMPRESSIBLE or IMAGE, which sparse_sampled leaves out): on the Switch only
// its mapped 64 KB pages have memory. Mario Kart World 1.8.0 makes ten of them after its first frames (16384x16384 R8, 8192x8192 RG16 / R16 terrain render targets, kartnative's
// note: 8-26 MB mapped each), 3.4 GB as dense images. A residency image from level 0 (the game renders into it), its tiles bound behind the mapped pages and nothing uploaded (sparse_bind)
static int sparse_gpu(const tex_t *t, const vka_fmt *f) {
    const nvn_texdesc *d = &t->d;
    return t->mem && t->pool && (d->flags & NVN_TEXTURE_FLAGS_SPARSE) && (d->flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE)) && pool_sparse(t->pool) && d->samples <= 1
        && f->kind < 3 && (d->target == NVN_TEXTURE_TARGET_2D || d->target == NVN_TEXTURE_TARGET_2D_ARRAY) && !(d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
static int sparse_levels_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_SPARSE_LEVELS"); on = !e || atoi(e) != 0; } return on; }
static void sub_range(const nvn_texdesc *d, const nvn_texlayout *L, uint32_t lv, uint32_t ly, uint64_t *off, uint64_t *len) {   // level lv of layer ly in the storage
    uint32_t n = d->levels ? d->levels : 1;
    uint64_t o = nvn_tex_level_offset(d, lv), e = lv + 1 < n ? nvn_tex_level_offset(d, lv + 1) : L->layer_size;
    *off = (uint64_t)ly * L->layer_size + o; *len = e > o ? e - o : 0;
}
// The finest level with a mapped page in any layer (the image's base); the coarsest level when nothing is mapped. 3D textures keep every level (their depth halves with the level too).
// *vmin: the finest level from which every level of every layer is wholly mapped (the coarsest when none is), where views start (nvn_mtl.m sparse_view: the translator drops
// TEX's level-of-detail clamp and residency predicate, so a game that clamps its samples to the mapped levels would read the unmapped tiles of a partly mapped level as zeros)
static uint32_t sparse_base(const void *pool, const uint8_t *mem, const nvn_texdesc *d, const nvn_texlayout *L, uint32_t *vmin) {
    uint32_t n = d->levels ? d->levels : 1, layers = L->layers ? L->layers : 1, b = n - 1, r = n;
    if (vmin) *vmin = 0;
    if (d->target == NVN_TEXTURE_TARGET_3D || !sparse_levels_on()) return 0;
    for (uint32_t lv = 0; lv + 1 < n && b == n - 1; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) { uint64_t o, l; sub_range(d, L, lv, ly, &o, &l); if (l && nvn_sparse_any(pool, (uint64_t)(uintptr_t)mem + o, l)) { b = lv; break; } }
    for (int full = 1; r > b && full; ) {
        for (uint32_t ly = 0; ly < layers && full; ly++) { uint64_t o, l; sub_range(d, L, r - 1, ly, &o, &l); full = !l || nvn_sparse_mapped(pool, (uint64_t)(uintptr_t)mem + o, l); }
        if (full) r--;
    }
    if (vmin) *vmin = r < n ? r : n - 1;
    return b;
}
// A sparse residency image (vka_sparse.c) instead of a dense one: a sampled sparse pool texture, 2D or 2D array, single-sampled, with NVN tiled levels from base on, whose dense
// image from base would take DM_VK_SPARSE_RES_MB (default 32) or more; or a GPU-written one (sparse_gpu) from level 0. Smaller ones stay dense (the tiles' bookkeeping and binds cost more than they save)
static int res_candidate(const tex_t *t, const vka_fmt *f, uint32_t base) {
    const nvn_texdesc *d = &t->d; uint32_t tw, th, td;
    if (!vks_on() || !(sparse_sampled(t, f) || (base == 0 && sparse_gpu(t, f))) || (d->target != NVN_TEXTURE_TARGET_2D && d->target != NVN_TEXTURE_TARGET_2D_ARRAY) || d->samples > 1) return 0;
    if (nvn_tex_sparse_tiles(d, &tw, &th, &td) <= (int)base || td != 1) return 0;
    uint64_t lo = nvn_tex_level_offset(d, base), ly = t->L.layer_size > lo ? t->L.layer_size - lo : 0;
    return (double)ly * (t->L.layers ? t->L.layers : 1) >= VKA_ENVF("DM_VK_SPARSE_RES_MB", 32.0) * 1048576.0;
}
static vka_img *img_create_(const tex_t *t, const vka_fmt *f, uint32_t base, int res, int *why, uint64_t *need);
// A sparse texture with no residency image (its format has no sparse residency, or the residency image was refused) is a dense image from base: Mario Kart World's 8192x8192 x68
// terrain arrays are 1.45 GB of ASTC 8x8 and 6.08 GB of BC6H each that way, of which the game maps 3-12%. The Adreno 840 gives sparse residency to no block-compressed format
// (probe 2026-11-03: vkGetPhysicalDeviceSparseImageFormatProperties returns no properties for any BC, ETC2 or ASTC format, with any usage, flags or format list; RGBA8 and
// RGBA16F have 128x128 / 128x64 tiles), and an uncompressed residency image would take 16x (ASTC 8x8 to RGBA8) or 8x (BC6H to RGBA16F) the bytes of each mapped tile and more
// sparse address space than the device has (4 GiB). So a dense image of a sparse texture starts at the finest level from base whose levels take at most DM_VK_SPARSE_DENSE_MB
// (default 128; NVN storage bytes, which the Vulkan image's size is close to; 0: no cap): its finer levels are left out and sampled from that level, as for the address space (bfloor). A platform limit, logged
static uint64_t dense_bytes(const nvn_texdesc *d, const nvn_texlayout *L, uint32_t b) {
    uint64_t lo = nvn_tex_level_offset(d, b); return (L->layer_size > lo ? L->layer_size - lo : 0) * (L->layers ? L->layers : 1);
}
static uint32_t dense_floor(const nvn_texdesc *d, const nvn_texlayout *L, uint32_t b) {
    double cap = VKA_ENVF("DM_VK_SPARSE_DENSE_MB", 128.0) * 1048576.0; uint32_t n = d->levels ? d->levels : 1;
    if (cap <= 0 || d->target == NVN_TEXTURE_TARGET_3D || !sparse_levels_on()) return b;
    while (b + 1 < n && (double)dense_bytes(d, L, b) > cap) b++;
    return b;
}
// The image of texture t from level base: a sparse residency image when it is one (res_candidate). One that does not fit the sparse address space starts at a coarser level
// (bfloor: its finer levels are left out, sampled from the coarser one; a platform limit, logged), down to where it is no candidate any more, then dense from there
static vka_img *img_create(const tex_t *t, const vka_fmt *f, uint32_t base) {
    uint32_t b = base; uint64_t need = 0; int wlast = 0;   // (the address space the last finer level asked for and did not get: once that much is free, a finer start is tried again, sparse_refresh)
    for (; res_candidate(t, f, b); b++) {
        int why = 0; vka_img *im = img_create_(t, f, b, 1, &why, &need); wlast = why;
        if (im) {
            im->bfloor = b > base ? b : 0; im->bneed = need;   // (0: nothing left out; a later rebase may go finer)
            if (b > base) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) { char st[256]; vks_stats(st, sizeof st);
                VRED("sparse texture serial %llu %ux%u layers %u format %#x: levels %u-%u left out, the sparse address space is full (%s)", (unsigned long long)t->serial, t->d.width, t->d.height,
                     im->layers, t->d.format, base, b - 1, st); } }
            return im;
        }
        if (why != 1 || !sparse_sampled(t, f)) { b = base; break; }   // refused for another reason (3: the format has no sparse residency at this shape), or a GPU-written one (drawn into at level 0: no coarser start): dense from base
    }
    uint32_t bd = sparse_sampled(t, f) ? dense_floor(&t->d, &t->L, b) : b;   // (at most DM_VK_SPARSE_DENSE_MB from there)
    if (bd > b) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16)
        VRED("sparse texture serial %llu %ux%u layers %u format %#x: no residency image; dense from level %u (levels %u-%u left out: %.0f MiB from level %u, %.0f MiB from level %u, DM_VK_SPARSE_DENSE_MB)",
             (unsigned long long)t->serial, t->d.width, t->d.height, t->L.layers, t->d.format, bd, b, bd - 1, dense_bytes(&t->d, &t->L, b) / 1048576.0, b, dense_bytes(&t->d, &t->L, bd) / 1048576.0, bd); }
    int capped = bd > b; b = bd;
    vka_img *im = img_create_(t, f, b, 0, NULL, NULL);
    // dcap only when residency is not to be tried again (a transient refusal, 2, retries it at the next finer start: sparse_refresh does not clamp it)
    if (im) { im->bfloor = b > base ? b : 0; im->bneed = need; im->dcap = (uint8_t)(capped && wlast != 2); }
    if (im && b > base && !capped) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16)
        VRED("sparse texture serial %llu %ux%u format %#x: dense from level %u (levels %u-%u left out: the sparse address space is full)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, b, base, b - 1); }
    return im;
}
// res: a sparse residency image; *why (with res, on NULL): 1 = the sparse address space is full, 2 = refused otherwise (a dense one may be made), 3 = the format has no sparse
// residency at this shape (a dense one may be made)
static vka_img *img_create_(const tex_t *t, const vka_fmt *f, uint32_t base, int res, int *why, uint64_t *need) {
    const nvn_texdesc *d = &t->d;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER) { VRED_ONCE("texture buffers are not images yet (first: serial %llu)", (unsigned long long)t->serial); return NULL; }
    if (res && f->dec) { if (why) *why = 3; return NULL; }   // BC decoded on the CPU: a dense image (its tiles would be the decoded format's, not the blocks')
    vka_img *im = calloc(1, sizeof *im);
    im->serial = t->serial; im->mem = t->mem; im->d = *d; im->L = t->L; im->f = *f; im->refs = 1;
    if (t->pool && pool_sparse(t->pool)) im->vpool = t->pool;
    im->pool = t->pool; im->pfin = nvn_pool_fin(); im->pgen = nvn_pool_live(t->pool, NULL, NULL);   // (the count first: a pool event in between is seen at the next check)
    // 1D and 1D-array textures are one-row 2D / 2D-array images, as nvn_mtl.m makes them: the translator declares every such read as a 2D one (no Dim1D in any DKB module),
    // and Vulkan has no 2D view of a 1D image, so a 1D image read as unbound
    int is3d = d->target == NVN_TEXTURE_TARGET_3D, is1d = d->target == NVN_TEXTURE_TARGET_1D || d->target == NVN_TEXTURE_TARGET_1D_ARRAY;
    im->w = d->width ? d->width : 1; im->h = is1d ? 1 : d->height ? d->height : 1; im->depth = is3d ? (d->depth ? d->depth : 1) : 1;
    im->layers = is3d ? 1 : t->L.layers ? t->L.layers : 1; im->levels = d->levels ? d->levels : 1; im->samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (base >= im->levels || is3d) base = 0;
    im->base = base; im->levels -= base; im->w = im->w >> base ? im->w >> base : 1; im->h = im->h >> base ? im->h >> base : 1;
    im->aspect = aspect_of(f->kind);
    VkFormatFeatureFlags ff = feats(f->vk);
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && !compressed(f)) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    // An sRGB texture's image too: it is made in the UNORM format (f->vk), which has storage support, and a storage binding views it in that format (view_of), the raw
    // bits a Maxwell / Ampere surface store writes (no sRGB encoding on stores); its sRGB views leave storage out (vka_view). Zelda: Ocarina of Time's NDVE final pass
    // (4f41ccf24f163e0d) stores into texture 1931, made RGBA8_SRGB (NVN 0x38) with the IMAGE flag, through an RGBA8 UNORM image view: with sRGB textures left out its
    // image had no storage use, storage_able refused remaking it with one, and that binding stayed unbound (the pass's output went nowhere)
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (d->flags & NVN_TEXTURE_FLAGS_IMAGE)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    // Views in another format: only an 8-bit / BC / ASTC UNORM image as its sRGB twin (NVN sRGB formats are stored as UNORM, and UNORM textures have sRGB views; the
    // only reinterpretation DKB's level makes). A two-entry format list instead of a bare MUTABLE_FORMAT keeps Adreno's UBWC compression on the image; every other image
    // takes views of its own format only
    // DM_VK_UBWC=0: every colour image MUTABLE_FORMAT without a list, as before (any view format; no UBWC on Adreno)
    int ubwc = VKA_ENV("DM_VK_UBWC", 1);   // live: images made from now on
    VkFormat alt = f->kind == 0 && srgb_twin(f->vk) != f->vk ? srgb_twin(f->vk) : VK_FORMAT_UNDEFINED;
    VkFormat fls[6] = {f->vk, alt}; int nfl = 2;
    if (ubwc && f->kind == 0) { pthread_mutex_lock(&g_vlmu); im->nx = vl_formats(f->vk, im->w, im->h, im->xfmt, VL_PER_IMAGE); pthread_mutex_unlock(&g_vlmu); for (int k = 0; k < im->nx; k++) fls[nfl++] = im->xfmt[k]; }   // learned view formats
    if (!alt && im->nx) fls[1] = fls[--nfl];   // (no sRGB twin: no UNDEFINED entry in the list)
    // A shader image whose own format has no storage support (B10G11R11_UFLOAT on Adreno) but a learned view format that has (R32_UINT): storage use through that view.
    // EXTENDED_USAGE lets the image take a use its own format lacks; only such an image gets it: every other mutable-format image (an sRGB twin, a learned float view, all
    // of them under DM_VK_UBWC=0) takes only uses its own format has, as before, so its compression and format-property answers stay what they were
    VkImageCreateFlags xu = 0;
    if (!(u & VK_IMAGE_USAGE_STORAGE_BIT) && (d->flags & NVN_TEXTURE_FLAGS_IMAGE)) for (int k = 0; k < im->nx; k++) if (feats(im->xfmt[k]) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) { u |= VK_IMAGE_USAGE_STORAGE_BIT; xu = VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; break; }
    VkImageFormatListCreateInfo fli = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = (uint32_t)nfl, .pViewFormats = fls};
    VkImageCreateFlags fl = alt || im->nx || (!ubwc && f->kind == 0) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | xu : 0;
    im->alt = ubwc ? alt : VK_FORMAT_UNDEFINED; im->any_fmt = !ubwc && f->kind == 0;
    if (!res && !is3d && !is1d && im->w == im->h && im->layers >= 6 && im->layers % 6 == 0 && im->samples == 1) fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    uint32_t rtw = 0, rth = 0, rtd = 0; int ntl = res ? nvn_tex_sparse_tiles(d, &rtw, &rth, &rtd) : 0;
    if (res) {   // the format takes sparse residency at this shape, with the usage it would have (storage use dropped: sampled sparse textures have none)
        VkImageFormatProperties sp = {0}; uint32_t nsp = 0;
        VkImageCreateFlags sfl = fl | VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT;
        int gpu = !sparse_sampled(t, f);   // (sparse_gpu: drawn into, so its attachment and storage use stay, or no residency image)
        if (!gpu) { u &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT; sfl &= ~xu; }   // (with the storage use, the extended use it came with)
        VkResult r1 = vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, u, sfl, &sp), r2 = r1;
        if (r1 && !gpu) { u &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; r2 = vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, f->vk, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, u, sfl, &sp); }
        if (!r2 && sp.maxArrayLayers >= im->layers && sp.maxMipLevels >= im->levels)
            vk.vkGetPhysicalDeviceSparseImageFormatProperties(V.pd, f->vk, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, u, VK_IMAGE_TILING_OPTIMAL, &nsp, NULL);
        if (!nsp || ntl <= (int)base) {   // logged once per VkFormat (up to 8 formats), with what refused it
            static _Atomic int seen[8]; int k = 0; for (; k < 8; k++) { int e = 0; if (atomic_compare_exchange_strong(&seen[k], &e, (int)f->vk) || e == (int)f->vk) break; }
            static _Atomic int logged[8]; if (k < 8 && !atomic_exchange(&logged[k], 1))
                VRED("VkFormat %d (serial %llu): no sparse residency at %ux%u x%u levels %u: format query %d/%d (layers %u levels %u), %u sparse format properties, usage %#x, flags %#x%s, NVN tiled levels %d from %u: dense images instead",
                     f->vk, (unsigned long long)t->serial, im->w, im->h, im->layers, im->levels, r1, r2, sp.maxArrayLayers, sp.maxMipLevels, nsp, u, sfl, alt || im->nx ? " (format list)" : "", ntl, base);
            *why = 3; free(im); return NULL; }
        fl = sfl;
    }
    // A 3D texture is drawn into slice by slice (NVN render target views of its slices, or all of them layered): 2D / 2D-array views of slices need this (Vulkan 1.1 maintenance1)
    if (is3d && (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) fl |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // Zero-shared images (below): a large GPU-content image the caller makes (g_zwant: vka_tex_get, its content ST_ZERO) is made to alias the zeroed memory of its create info
    // (the upper limit, vkz_max: 32 MiB for depth / stencil, none for color on Turnip, 32 MiB for color on other drivers; DM_VK_ZSHARE_MAX_MB sets the color one, 0 = none)
    int zs = g_zwant && VKA_ENV("DM_VK_ZSHARE", 1) && vkz_candidate(t->L.size, (uint64_t)(VKA_ENVF("DM_VK_ZSHARE_MB", 4.0) * 1048576.0),
                                                                    vkz_max(f->kind >= 3, V.driver_id == VK_DRIVER_ID_MESA_TURNIP, (int64_t)VKA_ENVF("DM_VK_ZSHARE_MAX_MB", -1.0)), res, im->samples, is3d, compressed(f),
                                                                    (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_DISPLAY)) != 0);
    if (zs) fl |= VK_IMAGE_CREATE_ALIAS_BIT;
    VkImageType it = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    VkImageFormatProperties ip;
    for (int k = 0; k < 3 && vk.vkGetPhysicalDeviceImageFormatProperties(V.pd, f->vk, it, VK_IMAGE_TILING_OPTIMAL, u, fl, &ip); k++) {
        if (k == 0) fl &= ~xu;   // (storage use dropped: the extended use it came with too)
        u &= k == 0 ? ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT : ~(VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
    im->usage = u;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = (alt || im->nx) && ubwc ? &fli : NULL, .flags = fl, .imageType = it, .format = f->vk, .extent = {im->w, im->h, im->depth}, .mipLevels = im->levels,
        .arrayLayers = im->layers, .samples = (VkSampleCountFlagBits)im->samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vk.vkCreateImage(V.dev, &ci, NULL, &im->img);
    if (r && res) { VRED_ONCE("sparse residency image of serial %llu: vkCreateImage %d: dense images instead", (unsigned long long)t->serial, r); *why = 2; free(im); return NULL; }
    if (r) { VRED("texture serial %llu %ux%ux%u format %#x (VkFormat %d) levels %u layers %u samples %u: vkCreateImage %d", (unsigned long long)t->serial, im->w, im->h, im->depth, d->format, f->vk, im->levels, im->layers, im->samples, r); free(im); return NULL; }
    VkMemoryRequirements mr; vk.vkGetImageMemoryRequirements(V.dev, im->img, &mr);
    uint32_t mt = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0); if (mt == UINT32_MAX) mt = vka_mem_type(mr.memoryTypeBits, 0, 0);
    if (res) {   // no memory of its own: tiles bound as the page table maps them (vks_tile), the rest now (vks_new)
        int ok = mt != UINT32_MAX && mr.alignment && 0x10000 % mr.alignment == 0;
        if (ok && !vks_va_take(mr.size)) { vk.vkDestroyImage(V.dev, im->img, NULL); free(im); *why = 1; *need = mr.size; return NULL; }
        if (ok && !(im->rs = vks_new(im->img, mt, im->w, im->h, im->levels, im->layers, rtw, rth, (uint32_t)ntl - base))) { vks_va_give(mr.size); ok = 0; }
        else if (!ok) VRED_ONCE("sparse residency image of serial %llu: memory alignment %llu, type %u: dense images instead", (unsigned long long)t->serial, (unsigned long long)mr.alignment, mt);
        if (!ok) { vk.vkDestroyImage(V.dev, im->img, NULL); free(im); *why = 2; return NULL; }
        im->va = mr.size; im->rs_tw = rtw; im->rs_th = rth; im->bytes = 0;
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32) { char st[256]; vks_stats(st, sizeof st);
            VLOG("sparse residency image: serial %llu %ux%u layers %u levels %u+%u format %#x, tiles %ux%u, %u levels by tile, %.0f MiB of address space (%s)", (unsigned long long)t->serial, im->w, im->h,
                 im->layers, base, im->levels, d->format, rtw, rth, vks_tiled(im->rs), mr.size / 1048576.0, st); }
        return im;
    }
    int fresh = 0;
    if (zs && mt != UINT32_MAX) {   // the key: the whole create info (ALIAS_BIT in its flags), the format list, the memory type and requirements (images that may alias consistently)
        struct { VkImageCreateFlags fl; VkFormat f; VkImageType it; uint32_t w, h, dp, lv, ly, sm; VkImageUsageFlags u; uint32_t mt, nfl; VkFormat fls[6]; VkDeviceSize size, al; } k;
        memset(&k, 0, sizeof k);
        k.fl = fl; k.f = f->vk; k.it = it; k.w = im->w; k.h = im->h; k.dp = im->depth; k.lv = im->levels; k.ly = im->layers; k.sm = im->samples; k.u = u; k.mt = mt; k.size = mr.size; k.al = mr.alignment;
        if (ci.pNext) { k.nfl = (uint32_t)nfl; memcpy(k.fls, fls, (size_t)nfl * sizeof *fls); }
        if ((r = vka_imem_bind_zero(im->img, mt, &mr, &k, (uint32_t)sizeof k, &im->m, &fresh))) zs = 0;   // (refused: the image's own memory, as before)
        else if (!im->m.z) zs = 0;   // (the table had no room: the allocation is its own)
    } else zs = 0;
    if (!im->m.dm && (r = vka_imem_bind(im->img, mt, &mr, &im->m))) {   // a run of a shared block, or a dedicated allocation (vka_imgmem.c)
        VRED("texture serial %llu: %llu bytes of image memory not allocated (%d; %.0f MiB in %d images, %llu image allocations, at most %u allocations)", (unsigned long long)t->serial,
             (unsigned long long)mr.size, r, g_img_bytes / 1048576.0, g_nlive, (unsigned long long)vka_imem_allocations(), V.props.limits.maxMemoryAllocationCount);
        vk.vkDestroyImage(V.dev, im->img, NULL); free(im); return NULL;
    }
    if (zs) {   // its bytes are the shared allocation's, counted once (vka_tex_mem)
        im->bytes = 0; if (fresh) vka_mem_grew(mr.size);
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32)
            VLOG("zero-shared image: texture serial %llu %ux%u layers %u levels %u format %#x flags %#x: %.1f MiB of zeroed memory %s (memory of its own at its first write)", (unsigned long long)t->serial,
                 im->w, im->h, im->layers, im->levels, d->format, d->flags, mr.size / 1048576.0, fresh ? "made for its create info" : "shared with images of the same create info");
    } else { im->bytes = mr.size; g_img_bytes += mr.size; vka_mem_grew(mr.size); }
    if (mr.size >= (VkDeviceSize)(VKA_ENVF("DM_VK_BIGIMG_MB", 64.0) * 1048576.0)) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 32)   // large images, for memory walls
        VLOG("large image: texture serial %llu %ux%ux%u layers %u levels %u (base %u) samples %u format %#x target %d flags %#x: %.1f MiB of image memory (storage %.1f MiB at %p, pool flags %#x)",
             (unsigned long long)t->serial, im->w, im->h, im->depth, im->layers, im->levels, im->base, im->samples, t->d.format, t->d.target, t->d.flags, mr.size / 1048576.0,
             t->L.size / 1048576.0, (void *)t->mem, pool_flags(t->pool)); }
    { VkBuffer sb; VkDeviceSize so;   // could the image be bound on its storage's own pool memory (HANDOFF "pool-aliased images")? Measured only (vka_tex_alias_stats)
      im->pool_fit = t->mem && V.guest_type < 32 && (mr.memoryTypeBits >> V.guest_type & 1) && mr.size <= t->L.size && vka_addr((uintptr_t)t->mem, t->L.size ? t->L.size : 1, &sb, &so, NULL)
                     && mr.alignment && so % mr.alignment == 0; }
    if ((u & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) && im->w >= 1024 && im->levels == 1 && im->layers == 1) { static int n; if (n++ < 6)   // UBWC shows as room for its metadata
        VLOG("target %ux%u VkFormat %d%s: %llu bytes (%.2f a texel)", im->w, im->h, f->vk, im->alt ? " (format list)" : fl ? " (mutable)" : "", (unsigned long long)mr.size, (double)mr.size / ((double)im->w * im->h)); }
    return im;
}

// Where a new image's content comes from (nvn_mtl.m tex_mtl_, tex_from_storage, tex_cpu_content, tex_init_fill, tex_linear). On the Switch a texture is its storage: the GPU reads
// what the pool holds, whoever wrote it. The replay keeps images in memory of their own, so content the CPU put into the storage is copied in:
//   ST_TRACK: uploaded now and again whenever the CPU writes its pages (write-protected after each upload, nvn_dirty.c; vka_tex_prepare re-uploads at the next submission):
//     a packaged layout (BNTX assets: the game loads the file into the pool and sets the layout; only compressed formats used to be uploaded, so every uncompressed asset
//     texture, font sheets among them, read zeros: Donkey Kong Bananza's menus had their panels but no text), CPU-written ones (WriteTexels, packaged data), linear textures in a
//     pool the CPU can write (Metal makes those on the pool buffer itself: SMBW's glyph cache, rasterized into an R8 linear texture by the CPU), and block-compressed textures
//     whose storage holds data at their first use (the GPU cannot render into them)
//   ST_FILL: uploaded once: what a CPU_NO_ACCESS or CPU_UNCACHED pool, or a range the CPU flushed before the first use, holds (the CPU cannot or did not say it would write it again)
//   ST_ZERO: GPU content (render targets, copy destinations): cleared
// The GPU writing an image (gpu_owned) ends its tracking, as on Metal: its content is the GPU's from then on. DM_VK_TEX_TRACK=0 uploads ST_TRACK textures once (no write protection)
enum { ST_ZERO, ST_FILL, ST_TRACK, ST_SPARSE };
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
static int mem_any(const uint8_t *p, uint64_t n) { uint64_t i = 0, v; for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); if (v) return 1; } for (; i < n; i++) if (p[i]) return 1; return 0; }
static uint64_t g_st_up[4], g_reup;
// Time and bytes of uploads from guest memory (deswizzle / row copy into scratch and the copy commands), on the replay thread: the [longframe] line's "texture uploads ... MB / ms"
// (hle_nvn.c mtl_upload_ns_take, mtl_upload_bytes; stubs that said 0 until 2026-11-03)
static _Atomic uint64_t g_up_ns, g_up_bytes;
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_up_ns, 0); }
uint64_t mtl_upload_bytes(void) { return atomic_load(&g_up_bytes); }
// mem_any over guest storage: pages the guest has not mapped are skipped (inaccessible; they read as zeros: unmapped_content)
static int mem_any_mapped(const uint8_t *c, const tex_t *t) {
    if (c != t->mem || !dm_guest_unmapped((uintptr_t)c, t->L.size)) return mem_any(c, t->L.size);
    for (uintptr_t p = (uintptr_t)c, e = p + t->L.size; p < e; ) {
        uintptr_t q = (p & ~(uintptr_t)0xFFF) + 0x1000; if (q > e) q = e;
        uint8_t pg[0x1000];   // copied, not read in place: the guest may unmap the page after the check (dm_guest_read_mapped)
        if (!dm_guest_unmapped(p, 1) && (dm_guest_read_mapped(pg, p, q - p), mem_any(pg, q - p))) return 1;
        p = q;
    }
    return 0;
}
// Switch 2 (NVN2) memory is coherent: the driver's FlushMappedRange / FlushTexels are bare returns, so the GPU reads what the CPU stored, flushed or not. A texture whose first
// use is a sample (a texture ID written into the sampled table: write_tex / id_view, g_first_sampled) that the GPU has not written holds what its storage holds, whatever its
// format. So on NVN2 an uncompressed texture in a pool the CPU can write is taken from its storage (tracked: re-uploaded when the CPU writes its pages, flushed or uncached pools
// too) when its first use is a sample, unless an image the GPU drew into already lies over the same storage (a render target sampled through another texture object keeps the GPU's
// content; one drawn only later is not seen here, and its draw ends the tracking: gpu_owned). Storage still all zero is tracked too: a later texture object of the same shape over a
// reused slot shares this image (same_shape) and never has a first use of its own, so the CPU's later fill must reach it through the tracking. The sampled table's descriptors are
// written for every dirty registered ID at each submission (vka_tex_prepare → write_tex), so a render target registered for sampling counts as sampled first when its draw comes later:
// its stale storage is uploaded (what the Switch shows too) until its first draw makes it GPU content.
// Mario Kart World stores its terrain tiles (260x260 R16 height, RG8 material, RGBA8 and R11G11B10F colour, 68x68 RGBA16; MINIMAL_LAYOUT, one CPU_CACHED | GPU_CACHED pool,
// flags 0x24) into reused slots before nvnTextureInitialize and never flushes them: as zero images its ground beside the roads came out flat and untextured. A texture first used
// as an attachment, a copy destination or a shader image stays GPU content. The rule and the finding follow kartnative (pahorema/kartnative, MIT: "Switch 2: uncompressed storage
// the CPU wrote before a texture's first sample", nvn_mtl.m tex_cpu_content). DM_VK_STORAGE_FIRST_USE=0 turns it off (to measure it)
static _Thread_local int g_first_sampled;
static int img_pool_dead(vka_img *im);
static int gpu_image_over(const uint8_t *mem) {   // caller holds g_tmu; images of finalized pools do not count (a new pool may lie at the same address)
    for (int i = 0; i < g_nlive; i++) if (g_live[i]->mem == mem && g_live[i]->gpu_owned && !img_pool_dead(g_live[i])) return 1;
    return 0;
}
static int nvn2_sampled_content(const tex_t *t, const vka_fmt *f, unsigned pf, int sampled) {
    static int on = -1; if (on < 0) on = VKA_ENV("DM_VK_STORAGE_FIRST_USE", 1) != 0 && vka_nvn2();   // (vka_nvn2: captures too)
    const nvn_texdesc *d = &t->d;
    if (!on || !sampled || !t->pool || t->has_layout || compressed(f) || f->kind >= 3) return 0;
    if (pf & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;
    if (d->flags & (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE)) return 0;
    return !gpu_image_over(t->mem);
}
static uint64_t g_nvn2_first[2];   // textures taken from their storage at a first sample (nvn2_sampled_content): [0] with data, [1] storage all zero (tracked from zeros)
static int storage_mode(const tex_t *t, const uint8_t *c, const vka_fmt *f, int cpuw, int sampled) {   // c: the storage's content as the GPU will see it (t->mem or gw_content's copy)
    const nvn_texdesc *d = &t->d;
    if (!t->mem || d->samples > 1 || f->kind >= 3 || d->target == NVN_TEXTURE_TARGET_BUFFER) return ST_ZERO;
    // A sparse virtual pool's texture (nvn_mtl.m tex_from_storage): a sampled one holds what the CPU wrote into the physical pages mapped behind it (streamed textures), read
    // through the page table and uploaded again when its mappings or those pages change (ST_SPARSE); render targets and images are GPU content. Its address is reserved, inaccessible
    // memory where nothing is mapped (no host alias on Android), so mem_any and the other checks below must not read it
    int sp = 0; unsigned pf = 0; if (t->pool && !nvn_pool_live(t->pool, &pf, &sp)) return ST_ZERO;   // a finalized pool: its memory may be unmapped (treg_dead)
    if (sp) return sparse_sampled(t, f) ? ST_SPARSE : ST_ZERO;
    int linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0, comp = compressed(f);
    int want = ST_ZERO;
    if (cpuw || t->has_layout) want = ST_TRACK;
    else if (linear && t->pool) want = ST_TRACK;
    else if (comp && !(d->flags & (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_SPARSE)) && mem_any_mapped(c, t)) want = ST_TRACK;
    else if (nvn2_sampled_content(t, f, pf, sampled)) {
        int any = mem_any_mapped(c, t); uint64_t k = g_nvn2_first[!any]++; want = ST_TRACK;
        if (any && k < 4) VLOG("texture serial %llu %ux%ux%u format %#x pool flags %#x: written by the CPU before its first use, a sample (NVN2): uploaded from its storage, re-uploaded when it changes (%llu so far, %llu more with empty storage)",
                               (unsigned long long)t->serial, d->width, d->height, d->depth, d->format, pf, (unsigned long long)k + 1, (unsigned long long)g_nvn2_first[1]);
    }
    else if (DM_GAME_TEX_DIRTY_UPLOAD && sampled && !(d->flags & (NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_SPARSE))
             && !(pf & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL))) want = ST_TRACK;
    else if (!(d->flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->pool && ((pf & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED)) || nvn_flushed(t->mem, t->L.size))
             && mem_any_mapped(c, t)) want = ST_FILL;
    // No later CPU writes to follow: a pool the CPU cannot access, a virtual pool (its pages are not the storage's), storage outside every tracked pool, or tracking off
    int trk = VKA_ENV("DM_VK_TEX_TRACK", 1) != 0;   // live: textures made from now on
    if (want == ST_TRACK && (!trk || (pf & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) || !nvn_dirty_span(t->mem, t->L.size))) want = ST_FILL;
    // Diagnostic DM_VK_ZEROLOG=<n>: the first n textures made as zero images whose storage holds data (virtual pools left out: their range may be reserved, inaccessible memory) (what they are, and whether their first use was a sample)
    { int zl = VKA_ENV("DM_VK_ZEROLOG", 0); static _Atomic int nz;
      if (zl > 0 && want == ST_ZERO && !(pf & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && atomic_load(&nz) < zl && mem_any_mapped(c, t) && atomic_fetch_add(&nz, 1) < zl)
          VLOG("zero image over data: serial %llu %ux%ux%u levels %u format %#x target %d flags %#x pool flags %#x layout %d sampled %d cpuw %d gpu over %d",
               (unsigned long long)t->serial, d->width, d->height, d->depth, d->levels, d->format, d->target, d->flags, pf, t->has_layout, sampled, cpuw, gpu_image_over(t->mem)); }
    return want;
}
static uint64_t mem_hash(const uint8_t *p, uint64_t n) { uint64_t h = 1469598103934665603ull, v; uint64_t i = 0; for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; } for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
// GPU writes into guest memory. On the Switch a texture is its storage, so a copy, fill or inline update into that memory (or a shader's store into a storage buffer over it)
// changes what the texture reads from then on, in GPU order. Here a texture is an image of its own, which the CPU uploads from the storage when the replay records its first use
// (or again after a CPU write the dirty tracker saw). A GPU write is not a CPU write: it does not fault, and the CPU sees its bytes only when the submission holding it completes.
// Donkey Kong Bananza streams textures that way (a buffer copy into the pool, then the first draw with the texture, in the same submission): the image got the bytes the storage held
// before the copy (an earlier texture's or zeros; ASTC blocks of another block size or all zero decode to NaN on the Adreno 840, so its lamps, rocks and signs rendered as black
// cores with white bloom), and kept them, since no fault ever came. So each GPU write is kept here, in recording order (seq), with its submission's fence:
//   - an upload reads the storage as the GPU will see it at that point: the copies, fills and inline updates not yet done are applied over a copy of the storage (gw_content);
//   - once a write's fence is done, the images over its range whose upload did not include it (gw_seen) are uploaded again (vka_tex_prepare): writes whose bytes the host cannot
//     know at recording (shader stores, texture → buffer copies), and images first used before the write
// Images whose content is the GPU's (gpu_owned: render targets, copy destinations) keep it, as for CPU writes
enum { GW_COPY, GW_FILL, GW_UPDATE, GW_OPAQUE };
typedef struct { uint64_t lo, hi, src, seq, fence; uint32_t kind, value; uint8_t *data; } gw_t;
static gw_t *g_gw; static int g_ngw, g_gwcap; static uint64_t g_gwseq = 1, g_gw_redo, g_gw_over, g_gw_late;
static void gw_add(int kind, uint64_t dst, uint64_t n, uint64_t src, uint32_t value, const void *data) {
    if (!n) return;
    int merge = kind == GW_UPDATE && data && VKA_ENV("DM_VK_GW_MERGE", 1);
    pthread_mutex_lock(&g_tmu);
    if (g_ngw == g_gwcap) { g_gwcap = g_gwcap ? 2 * g_gwcap : 256; g_gw = realloc(g_gw, (size_t)g_gwcap * sizeof *g_gw); }
    if (kind == GW_OPAQUE)   // the same range again in this submission (a storage buffer bound by draw after draw): one entry, at the newest place
        for (int i = g_ngw - 1; i >= 0 && i >= g_ngw - 16 && !g_gw[i].fence; i--) if (g_gw[i].kind == GW_OPAQUE && g_gw[i].lo == dst && g_gw[i].hi == dst + n) {
            gw_t w = g_gw[i]; memmove(&g_gw[i], &g_gw[i + 1], (size_t)(g_ngw - 1 - i) * sizeof *g_gw); w.seq = g_gwseq++; g_gw[g_ngw - 1] = w; pthread_mutex_unlock(&g_tmu); return; }
    // The same inline update range again in this submission (upstream Banana Native v0.3.12, 03f5e80b: Splatoon Raiders' occlusion queries made 11,010 updates to 7 ranges):
    // the new bytes overwrite that entry, which moves to the newest place with a new seq, instead of one entry (and one malloc) per update. The list stays short, so
    // gw_shadow_read, gw_content and vka_tex_prepare walk fewer entries; our exec records every inline update here, kept in a pass or not (vka_exec.c OP_UPDATE_UB).
    // Safe because the walk stops at a copy (it may have read the old bytes as its source) and at any other write overlapping the range: between the old place and the end
    // nothing else touched these bytes, so the GPU-side content at the end is the same; an image uploaded in between has a gw_seen below the new seq and is uploaded again
    // once the fence is done, as for a new entry. DM_VK_GW_MERGE=0: one entry per update, as before (merge: read before g_tmu was taken)
    if (merge)
        for (int i = g_ngw - 1; i >= 0 && i >= g_ngw - 16 && !g_gw[i].fence; i--) {
            if (g_gw[i].kind == GW_COPY) break;
            if (g_gw[i].kind == GW_UPDATE && g_gw[i].lo == dst && g_gw[i].hi == dst + n && g_gw[i].data) {
                memcpy(g_gw[i].data, data, (size_t)n);
                gw_t w = g_gw[i]; memmove(&g_gw[i], &g_gw[i + 1], (size_t)(g_ngw - 1 - i) * sizeof *g_gw); w.seq = g_gwseq++; g_gw[g_ngw - 1] = w;
                pthread_mutex_unlock(&g_tmu); return;
            }
            if (g_gw[i].lo < dst + n && dst < g_gw[i].hi) break;
        }
    uint8_t *cp = NULL; if (kind == GW_UPDATE && (cp = malloc(n))) memcpy(cp, data, n); else if (kind == GW_UPDATE) kind = GW_OPAQUE;
    g_gw[g_ngw++] = (gw_t){dst, dst + n, src, g_gwseq++, 0, (uint32_t)kind, value, cp};
    pthread_mutex_unlock(&g_tmu);
}
void vka_tex_gw_copy(uint64_t src, uint64_t dst, uint64_t n) { gw_add(GW_COPY, dst, n, src, 0, NULL); }
void vka_tex_gw_fill(uint64_t dst, uint64_t n, uint32_t value) { gw_add(GW_FILL, dst, n, 0, value, NULL); }
void vka_tex_gw_update(uint64_t dst, uint64_t n, const void *data) { gw_add(GW_UPDATE, dst, n, 0, 0, data); }
void vka_tex_gw_opaque(uint64_t dst, uint64_t n) { gw_add(GW_OPAQUE, dst, n, 0, 0, NULL); }
// A uniform range as the GPU will read it after the writes not done yet (vka_exec.c "Inline uniform updates": the shadow a draw reads while an update waits for the pass end;
// Erick's Ocarina fork 8ce6d80). Only inline updates and fills have bytes the host knows: a copy or an unknown-bytes write over the range (shader stores, texture → buffer
// copies, raw global stores) refuses it (0), and so does an update kept without its bytes. Writes of earlier submissions still in flight count, in recording order
static int gw_shadow_read(uint64_t addr, void *out, uint64_t n, int copy) {
    if (!n || addr > UINT64_MAX - n || n > SIZE_MAX) return 0;
    uint64_t done = vka_done(), end = addr + n;
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < g_ngw; i++) {
        const gw_t *w = &g_gw[i];
        if ((w->fence && w->fence <= done) || w->hi <= addr || w->lo >= end) continue;
        if (w->kind == GW_OPAQUE || w->kind == GW_COPY || (w->kind == GW_UPDATE && !w->data)) { pthread_mutex_unlock(&g_tmu); return 0; }
    }
    if (copy) {
        if (vka_addr(addr, n, NULL, NULL, NULL)) memcpy(out, (const void *)(uintptr_t)addr, (size_t)n);
        else if (plat_mem_read(addr, out, (size_t)n)) memset(out, 0, (size_t)n);
        for (int i = 0; i < g_ngw; i++) {
            const gw_t *w = &g_gw[i];
            if ((w->fence && w->fence <= done) || w->hi <= addr || w->lo >= end) continue;
            uint64_t lo = w->lo > addr ? w->lo : addr, hi = w->hi < end ? w->hi : end;
            if (w->kind == GW_UPDATE) memcpy((uint8_t *)out + (lo - addr), w->data + (lo - w->lo), (size_t)(hi - lo));
            else for (uint64_t k = lo; k < hi; k++) ((uint8_t *)out)[k - addr] = (uint8_t)(w->value >> (8 * ((k - w->lo) & 3)));   // GW_FILL: the 32-bit value, little-endian
        }
    }
    pthread_mutex_unlock(&g_tmu);
    return 1;
}
int vka_tex_gw_shadow_safe(uint64_t addr, uint64_t n) { return gw_shadow_read(addr, NULL, n, 0); }
int vka_tex_gw_shadow_read(uint64_t addr, void *out, uint64_t n) { return gw_shadow_read(addr, out, n, 1); }
// Stores through raw global memory (an address the shader loaded: dmsh_abi.h DMSH_GMEM_LOG_*) land where only the GPU knows: each draw or dispatch with such stores gets a write
// log of its own, read once its submission is done; its pages become writes of unknown bytes, done (the images over them are uploaded again)
enum { GML_N = 256, GML_BYTES = (1 + (1 << DMSH_GMEM_LOG_LOG2)) * 4 };
static struct { vka_span s; uint64_t fence; int used; } g_gml[GML_N]; static int g_gml_init; static uint64_t g_gml_pages, g_gml_full, g_gml_none;
int vka_tex_gml_take(VkDescriptorBufferInfo *bi) {   // 0 = no log free (the caller binds an unread one)
    pthread_mutex_lock(&g_tmu);
    if (!g_gml_init) { g_gml_init = 1; for (int i = 0; i < GML_N; i++) if (!vka_static(GML_BYTES, V.sb_align, &g_gml[i].s)) break; }
    for (int i = 0; i < GML_N; i++) if (!g_gml[i].used && g_gml[i].s.ptr) {
        g_gml[i].used = 1; g_gml[i].fence = 0; memset(g_gml[i].s.ptr, 0, GML_BYTES);
        *bi = (VkDescriptorBufferInfo){g_gml[i].s.buf, g_gml[i].s.off, GML_BYTES}; pthread_mutex_unlock(&g_tmu); return 1; }
    g_gml_none++; pthread_mutex_unlock(&g_tmu);
    VRED_ONCE("more than %d raw global memory write logs in flight: stores of later draws are not followed into textures", GML_N);
    return 0;
}
static int img_by_mem(const void *a, const void *b) { uintptr_t x = (uintptr_t)(*(vka_img *const *)a)->mem, y = (uintptr_t)(*(vka_img *const *)b)->mem; return x < y ? -1 : x > y; }
static void gml_poll(void) {   // caller holds g_tmu
    uint64_t done = vka_done();
    for (int i = 0; i < GML_N; i++) {
        if (!g_gml[i].used || !g_gml[i].fence || g_gml[i].fence > done) continue;
        const uint32_t *w = (const uint32_t *)g_gml[i].s.ptr; g_gml_full += w[0] != 0;
        if (w[0]) VRED_ONCE("a raw global memory write log ran out of slots: some stores are not followed into textures");
        for (int k = 1; k <= 1 << DMSH_GMEM_LOG_LOG2; k++) if (w[k]) {
            if (g_ngw == g_gwcap) { g_gwcap = g_gwcap ? 2 * g_gwcap : 256; g_gw = realloc(g_gw, (size_t)g_gwcap * sizeof *g_gw); }
            uint64_t a = (uint64_t)w[k] << 12; g_gw[g_ngw++] = (gw_t){a, a + 4096, 0, g_gwseq++, g_gml[i].fence, GW_OPAQUE, 0, NULL}; g_gml_pages++;
        }
        g_gml[i].used = 0;
    }
}
// DM_VK_NANSCAN storage watch: the first 64 bytes of every tracked texture's storage, checked at each submission; a change the dirty tracker did not see (no CPU fault) is logged
// with the compute shaders the submissions since its upload dispatched (a GPU writer)
enum { WD_N = 4096 }; static struct { uint64_t fence, key; } g_wd[WD_N]; static uint64_t g_nwd;
void vka_tex_watch_dispatch(uint64_t key) {
    uint64_t f = vka_fence_last() + 1;
    for (uint64_t i = g_nwd > 64 ? g_nwd - 64 : 0; i < g_nwd; i++) if (g_wd[i % WD_N].fence == f && g_wd[i % WD_N].key == key) return;
    g_wd[g_nwd % WD_N].fence = f; g_wd[g_nwd % WD_N].key = key; g_nwd++;
}
uint64_t vka_frame(void);
// The GPU's view of the same bytes: copied out of the pool's buffer in this submission, compared with what the CPU reads once it is done
typedef struct { vka_span s; const uint8_t *mem; uint64_t serial; } gv_t;
static void gview_report(void *arg) {
    gv_t *g = arg; char a[40], b[40]; for (int j = 0; j < 16; j++) { snprintf(a + 2 * j, 3, "%02x", g->s.ptr[j]); snprintf(b + 2 * j, 3, "%02x", g->mem[j]); }
    char sd[400]; void vka_mem_describe(uint64_t, char *, size_t); vka_mem_describe((uint64_t)(uintptr_t)g->mem, sd, sizeof sd);
    char mp_[200] = ""; { FILE *f = fopen("/proc/self/maps", "re"); char ln[512]; uintptr_t ad = (uintptr_t)g->mem;
        while (f && fgets(ln, sizeof ln, f)) { unsigned long lo, hi; if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 && ad >= lo && ad < hi) { ln[strcspn(ln, "\n")] = 0; snprintf(mp_, sizeof mp_, "%.120s", ln); break; } } if (f) fclose(f); }
    VLOG("NaN scan: watch: serial %llu at %p: the GPU reads %s, the CPU reads %s (%s); %s; maps %s", (unsigned long long)g->serial, (const void *)g->mem, a, b, memcmp(g->s.ptr, g->mem, 256) ? "DIFFERENT memory" : "the same", sd, mp_);
}
static void gview(VkCommandBuffer cb, const vka_img *im) {
    static gv_t g[16]; static int n; if (n >= 16 || !cb) return;
    VkBuffer b; VkDeviceSize o; if (!vka_addr((uint64_t)(uintptr_t)im->mem, 256, &b, &o, NULL)) return;
    gv_t *x = &g[n]; if (!vka_static(256, 16, &x->s)) return; n++;
    x->mem = im->mem; x->serial = im->serial;
    vka_barrier(cb); VkBufferCopy c = {o, x->s.off, 256}; vk.vkCmdCopyBuffer(cb, b, x->s.buf, 1, &c); vka_barrier(cb);
    vka_after(vka_fence_last() + 1, gview_report, x);
}
// DM_VK_NANSCAN: placed memory against mprotect, once. Six 4 KB pages of a placed pool: each written by the CPU, some protected read-only and back as the dirty tracker
// does, then copied by the GPU into a buffer; when the submission is done the GPU's bytes are compared with what the CPU reads
static uint8_t *g_pt; static vka_span g_pt_out;
static void ptest_report(void *arg) {
    (void)arg; static const char *what[6] = {"written, untouched", "written, then read-only, then read-write", "written, then read-only (still read-only)", "read-only then read-write, then written",
                                             "written, read-only, read-write, written again", "written, read-only and back 64 times"};
    for (int p = 0; p < 6; p++) { const uint8_t *c = g_pt + 4096 * p, *g = g_pt_out.ptr + 4096 * p; int d = 0; for (int i = 0; i < 4096; i++) d += c[i] != g[i];
        VLOG("NaN scan: placed memory test: page %d (%s): %d of 4096 bytes differ between the CPU (%02x%02x%02x%02x) and the GPU (%02x%02x%02x%02x)", p, what[p], d, c[0], c[1], c[2], c[3], g[0], g[1], g[2], g[3]); }
}
static void ptest(VkCommandBuffer cb) {
    static int done; if (done || !cb) return; done = 1;
    uint8_t *m = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (m == MAP_FAILED) return;
    memset(m, 0x11, 1 << 16);
    mtl_pool_add(m, 1 << 16);
    VkBuffer b; VkDeviceSize o; if (!vka_addr((uint64_t)(uintptr_t)m, 6 * 4096, &b, &o, NULL) || !vka_static(6 * 4096, 16, &g_pt_out)) { VLOG("NaN scan: placed memory test: not placed"); return; }
    g_pt = m; uint8_t *P[6]; for (int i = 0; i < 6; i++) P[i] = m + 4096 * i;
    memset(P[0], 0xa0, 4096);
    memset(P[1], 0xa1, 4096); mprotect(P[1], 4096, PROT_READ); mprotect(P[1], 4096, PROT_READ | PROT_WRITE);
    memset(P[2], 0xa2, 4096); mprotect(P[2], 4096, PROT_READ);
    mprotect(P[3], 4096, PROT_READ); mprotect(P[3], 4096, PROT_READ | PROT_WRITE); memset(P[3], 0xa3, 4096);
    memset(P[4], 0x44, 4096); mprotect(P[4], 4096, PROT_READ); mprotect(P[4], 4096, PROT_READ | PROT_WRITE); memset(P[4], 0xa4, 4096);
    memset(P[5], 0xa5, 4096); for (int k = 0; k < 64; k++) { mprotect(P[5], 4096, PROT_READ); mprotect(P[5], 4096, PROT_READ | PROT_WRITE); }
    vka_barrier(cb); VkBufferCopy c = {o, g_pt_out.off, 6 * 4096}; vk.vkCmdCopyBuffer(cb, b, g_pt_out.buf, 1, &c); vka_barrier(cb);
    vka_after(vka_fence_last() + 1, ptest_report, NULL);
}
// An image whose texture's pool was finalized since the image was made (or is another pool now): the game may have unmapped its storage, so it is not read again (re-uploads,
// scans); the image keeps its content. Caller holds g_tmu and nvn_pool_use from this check until it is done reading the storage. Cheap: the table is looked up only when a
// pool was finalized since the image's last check
static int img_pool_dead(vka_img *im) {
    if (im->pdead) return 1;
    if (!im->pool || !im->pgen) return 0;
    uint64_t f = nvn_pool_fin(); if (f == im->pfin) return 0;
    im->pfin = f;
    if (nvn_pool_live(im->pool, NULL, NULL) == im->pgen) return 0;
    im->pdead = 1; im->track = 0;   // (vtrack stays: view_of's level cut follows it; every loop that refreshes checks pdead first)
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("texture serial %llu (storage %p): its pool %p was finalized: its image keeps its content, its storage is not read again", (unsigned long long)im->serial, (void *)im->mem, im->pool);
    return 1;
}
static void watch_scan(VkCommandBuffer cb) {   // caller holds g_tmu and nvn_pool_use
    ptest(cb);
    static int nlog;
    if (nlog >= 24) return;
    for (int i = 0; i < g_nlive && nlog < 24; i++) {
        vka_img *im = g_live[i];
        if (!im->track || im->gpu_owned || !im->mem || !im->up_fence || img_pool_dead(im)) continue;
        uint64_t h = mem_hash(im->mem, im->L.size < 64 ? im->L.size : 64);
        if (h == im->head) continue;
        im->head = h;
        if (nvn_dirty_since_span(im->span, im->mem, im->L.size, im->stamp)) continue;   // a CPU write the tracker saw
        char keys[300] = ""; size_t o = 0; int nk = 0;
        for (uint64_t j = g_nwd > WD_N ? g_nwd - WD_N : 0; j < g_nwd && nk < 12; j++) if (g_wd[j % WD_N].fence >= im->up_fence) { o += (size_t)snprintf(keys + o, sizeof keys - o, " %016llx@%llu", (unsigned long long)g_wd[j % WD_N].key, (unsigned long long)g_wd[j % WD_N].fence); nk++; }
        nlog++;
        char why[600], mp_[160] = ""; nvn_dirty_debug(im->span, im->mem, im->L.size, im->stamp, why, sizeof why);
        { FILE *f = fopen("/proc/self/maps", "re"); char ln[512]; uintptr_t a = (uintptr_t)im->mem; int nv = 0;
          while (f && fgets(ln, sizeof ln, f)) { nv++; unsigned long lo, hi; if (!mp_[0] && sscanf(ln, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) { ln[strcspn(ln, "\n")] = 0; snprintf(mp_, sizeof mp_, "%.100s", ln); } }
          if (f) fclose(f); size_t k = strlen(mp_); snprintf(mp_ + k, sizeof mp_ - k, " (%d mappings)", nv); }
        char df[200] = ""; if (im->snap) { size_t n = im->L.size < 256 ? im->L.size : 256, k = 0; while (k < n && im->snap[k] == im->mem[k]) k++; k &= ~(size_t)15;
            size_t o = (size_t)snprintf(df, sizeof df, "first change at +%#zx: ", k); for (size_t j = k; j < k + 16 && j < n; j++) o += (size_t)snprintf(df + o, sizeof df - o, "%02x", im->snap[j]);
            o += (size_t)snprintf(df + o, sizeof df - o, " -> "); for (size_t j = k; j < k + 16 && j < n; j++) o += (size_t)snprintf(df + o, sizeof df - o, "%02x", im->mem[j]);
            memcpy(im->snap, im->mem, n); }
        VLOG("NaN scan: watch: %s; frame %llu", df, (unsigned long long)vka_frame());
        gview(cb, im);
        VLOG("NaN scan: watch: storage of texture serial %llu (%p+%#llx, format %#x %ux%u) changed with no CPU fault; uploaded in submission %llu, now %llu done / %llu recorded; compute since:%s; %s; maps: %s",
             (unsigned long long)im->serial, (const void *)im->mem, (unsigned long long)im->L.size, im->d.format, im->w, im->h, (unsigned long long)im->up_fence, (unsigned long long)vka_done(), (unsigned long long)vka_fence_last(), keys, why, mp_);
    }
}
void vka_tex_gw_commit(uint64_t fence) {   // the writes recorded since the last commit belong to this submission
    pthread_mutex_lock(&g_tmu);
    for (int i = g_ngw - 1; i >= 0 && !g_gw[i].fence; i--) g_gw[i].fence = fence;
    for (int i = 0; i < GML_N; i++) if (g_gml[i].used && !g_gml[i].fence) g_gml[i].fence = fence;
    pthread_mutex_unlock(&g_tmu);
}
// The storage [mem, mem + n) as the GPU sees it after the writes recorded so far: NULL = memory as it is (no pending write over it), else a malloc'd copy with the pending writes
// applied in order. *seen = the newest write the content includes with every older one (the writes not done and not applied, opaque ones, cap it). Caller holds g_tmu
static uint8_t *gw_content(const uint8_t *mem, uint64_t n, uint64_t *seen) {
    uint64_t a = (uint64_t)(uintptr_t)mem, b = a + n, done = vka_done(), cap = g_gwseq - 1; uint8_t *c = NULL;
    for (int i = 0; i < g_ngw; i++) {
        gw_t *w = &g_gw[i];
        if (w->fence && w->fence <= done) continue;   // landed: memory has it
        if (w->hi <= a || w->lo >= b) continue;
        if (w->kind == GW_OPAQUE) { if (w->seq - 1 < cap) cap = w->seq - 1; continue; }
        if (!c) { if (!(c = malloc(n))) { cap = 0; break; } dm_guest_read_mapped(c, (uintptr_t)mem, n); }
        uint64_t lo = w->lo > a ? w->lo : a, hi = w->hi < b ? w->hi : b;
        if (w->kind == GW_FILL) { for (uint64_t k = lo; k < hi; k++) { uint32_t v = w->value; c[k - a] = ((const uint8_t *)&v)[(k - w->lo) & 3]; } }
        else if (w->kind == GW_UPDATE) memcpy(c + (lo - a), w->data + (lo - w->lo), hi - lo);
        else {   // a copy: its source as the GPU will read it, which an earlier pending write over the source would change (then the write lands later: re-uploaded)
            const uint8_t *s = (const uint8_t *)(uintptr_t)(w->src + (lo - w->lo));
            for (int j = 0; j < i; j++) { const gw_t *x = &g_gw[j]; if ((!x->fence || x->fence > done) && x->lo < w->src + (hi - w->lo) && x->hi > w->src + (lo - w->lo)) { if (w->seq - 1 < cap) cap = w->seq - 1; break; } }
            memmove(c + (lo - a), s, hi - lo);
        }
    }
    *seen = cap;
    return c;
}
// Storage the guest has not mapped (all or part of it): its content as zeros, in a copy (NULL: all mapped). A memory pool is placed memory only once the GPU gets an address
// in it (vka_mem.c mtl_pool_need); before, placing mapped the whole pool read-write, and Mario Kart World 1.8.0 uploads a texture of a CPU_NO_ACCESS pool whose pages its
// SDK has not mapped (SIGSEGV in the deswizzle at 0x4074003000, phone m22)
static uint8_t *unmapped_content(const tex_t *t) {
    uint64_t u = dm_guest_unmapped((uintptr_t)t->mem, t->L.size); if (!u) return NULL;
    uint8_t *c = malloc(t->L.size); if (!c) return NULL;
    dm_guest_read_mapped(c, (uintptr_t)t->mem, t->L.size);
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("texture serial %llu: %llu of its %llu storage pages at %p are not mapped by the guest: read as zeros", (unsigned long long)t->serial,
                                                              (unsigned long long)u, (unsigned long long)((t->L.size + 4095) >> 12), (const void *)t->mem);
    return c;
}
// ---------- BC blocks decoded on the CPU (im->f.dec; vka_bcdec.h) ----------
// The blocks at src (bx a block row, pitch bytes a row, rows block rows a slice, nz slices one after the other) become texels in scratch memory, copied into level lv,
// layer ly at texel (px, py, pz), w x h texels a slice. src is read once, in order: the callers deswizzle into heap memory for a decoded image, not into scratch memory,
// which a driver may map uncached (Mali's coherent type has no cached twin: reads from it are slow). Counted, and logged at each power of two of uploads
static _Atomic uint64_t g_bc_n, g_bc_bytes, g_bc_ns;
static int bc_copy(VkCommandBuffer cb, vka_img *im, const uint8_t *src, size_t pitch, uint32_t bx, uint32_t rows, uint32_t nz, uint32_t lv, uint32_t ly, int32_t px, int32_t py, int32_t pz, uint32_t w, uint32_t h) {
    const vka_fmt *f = &im->f; size_t tp = (size_t)bx * 4 * f->tb, slice = tp * rows * 4; vka_span s;
    if (!f->dec || !bx || !rows || !nz) return 0;
    if (!vka_scratch(slice * nz, 16, &s)) { VRED_ONCE("BC decode: no scratch memory for %zu bytes of texels (texture serial %llu): not uploaded", slice * nz, (unsigned long long)im->serial); return 0; }
    uint64_t t0 = now_ns();
    for (uint32_t k = 0; k < nz; k++) vka_bc_rows(f->dec, src + (size_t)k * pitch * rows, pitch, bx, rows, s.ptr + (size_t)k * slice, tp);
    VkBufferImageCopy c = {s.off, bx * 4, rows * 4, {im->aspect, lv, ly, 1}, {px, py, pz}, {w, h, nz}};
    vk.vkCmdCopyBufferToImage(cb, s.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    uint64_t n = atomic_fetch_add(&g_bc_n, 1) + 1, by = atomic_fetch_add(&g_bc_bytes, slice * nz) + slice * nz, ns = atomic_fetch_add(&g_bc_ns, now_ns() - t0) + (now_ns() - t0);
    if (n >= 64 && !(n & (n - 1))) VLOG("BC textures decoded on the CPU: %llu uploads, %.1f MiB of texels, %.0f ms in all (%.0f MiB a second)", (unsigned long long)n, by / 1048576.0, ns / 1e6, ns ? by / 1048576.0 / (ns / 1e9) : 0);
    return 1;
}
int vka_tex_bc_copy(VkCommandBuffer cb, vka_img *im, const uint8_t *src, size_t pitch, uint32_t bx, uint32_t rows, uint32_t nz, uint32_t lv, uint32_t ly, int32_t px, int32_t py, int32_t pz, uint32_t w, uint32_t h) {
    return bc_copy(cb, im, src, pitch, bx, rows, nz, lv, ly, px, py, pz, w, h);
}
static void upload_tracked(vka_img *im, const tex_t *t, VkCommandBuffer cb) {   // stamp, protect, then read (nvn_mtl.m pkg_upload): a write during the copy is seen at the next check
    im->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);
    if (vka_nan_on()) im->hash = mem_hash(t->mem, t->L.size);
    uint8_t *c = im->vpool ? NULL : gw_content(t->mem, t->L.size, &im->gw_seen);   // (a sparse pool's storage is read through its page table: upload_)
    if (!c && !im->vpool) c = unmapped_content(t);
    if (c) { tex_t tc = *t; tc.mem = c; upload(im, &tc, cb); free(c); g_gw_over++; } else upload(im, t, cb);
    if (vka_nan_on()) { im->head = mem_hash(t->mem, t->L.size < 64 ? t->L.size : 64); im->up_fence = vka_fence_last() + 1;
        if (!im->snap) im->snap = malloc(256); if (im->snap) memcpy(im->snap, t->mem, t->L.size < 256 ? t->L.size : 256); }
}
static void upload_fill(vka_img *im, const tex_t *t, VkCommandBuffer cb) {   // the same without tracking (ST_FILL)
    uint8_t *c = im->vpool ? NULL : gw_content(t->mem, t->L.size, &im->gw_seen);
    if (!c && !im->vpool) c = unmapped_content(t);
    if (c) { tex_t tc = *t; tc.mem = c; upload(im, &tc, cb); free(c); g_gw_over++; } else upload(im, t, cb);
}
// A linear 2D texture of one level and layer whose pages the CPU wrote between byte lo and hi of its storage: only those rows again (Donkey Kong Bananza's glyph cache,
// 1024x8192 R8: the whole 8 MB was deswizzled and copied each time a glyph was rasterized into it). 0 = not such a texture (the caller uploads all of it)
static int upload_rows_tracked(vka_img *im, const tex_t *t, VkCommandBuffer cb, size_t lo, size_t hi) {
    const nvn_texdesc *d = &t->d; const vka_fmt *f = &im->f;
    if (!(d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || im->levels > 1 || im->layers > 1 || im->depth > 1 || f->kind >= 3 || f->dec) return 0;
    uint32_t bx = (im->w + f->bw - 1) / f->bw, by = (im->h + f->bh - 1) / f->bh; size_t pitch = (size_t)bx * f->bpb, st = d->stride ? d->stride : pitch;
    if (st < pitch) return 0;
    uint32_t r0 = (uint32_t)(lo / st), r1 = (uint32_t)((hi + st - 1) / st); if (r1 > by) r1 = by;
    if (r0 >= r1) r0 = 0, r1 = by;
    img_touch(im);   // A CPU re-upload is GPU use too, even when no draw samples the texture this frame.
    im->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);   // stamp, protect, then read (upload_tracked)
    vka_span s; if (!vka_scratch(pitch * (r1 - r0), 16, &s)) return 1;
    for (uint32_t y = r0; y < r1; y++) memcpy(s.ptr + (size_t)(y - r0) * pitch, t->mem + (size_t)y * st, pitch);
    uint32_t y0 = r0 * f->bh, y1 = r1 * f->bh > im->h ? im->h : r1 * f->bh;
    VkBufferImageCopy c = {s.off, bx * f->bw, (r1 - r0) * f->bh, {im->aspect, 0, 0, 1}, {0, (int32_t)y0, 0}, {im->w, y1 - y0, 1}};
    vk.vkCmdCopyBufferToImage(cb, s.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    g_rows_up += pitch * (r1 - r0);
    return 1;
}
// A zero texel block: zeros, except ASTC, whose all-zero block is a reserved mode (the error colour): a void-extent block of colour (0, 0, 0, 0) there
static int is_astc(VkFormat v) { return v >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && v <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; }
static const uint8_t k_astc_zero[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static void astc_zero_blocks(uint8_t *p, size_t n) {   // the zeros of unmapped pages, as zero texels
    for (size_t i = 0; i + 16 <= n; i += 16) { uint64_t a, b; memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); if (!(a | b)) memcpy(p + i, k_astc_zero, 16); }
}
// Zero texels without staging: a buffer of zero blocks (first half) and of ASTC's zero blocks (second half), the source of every unmapped part of a sparse texture
enum { ZHALF = 2 << 20 };
static VkBuffer g_zbuf; static int g_zstate;   // 0 not tried, 1 made, -1 failed
static int zero_src(void) {
    if (g_zstate) return g_zstate > 0;
    g_zstate = -1;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * ZHALF, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    VkBuffer b; VkDeviceMemory m = VK_NULL_HANDLE; void *p = NULL;
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &b)) return 0;
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, b, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &m) || vk.vkBindBufferMemory(V.dev, b, m, 0) || vk.vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &p)) {
        VRED("zero texel buffer not made: unmapped parts of sparse textures go through scratch memory"); if (m) vk.vkFreeMemory(V.dev, m, NULL); vk.vkDestroyBuffer(V.dev, b, NULL); return 0; }
    memset(p, 0, ZHALF);
    for (size_t k = 0; k < ZHALF; k += 16) memcpy((uint8_t *)p + ZHALF + k, k_astc_zero, 16);
    vk.vkUnmapMemory(V.dev, m);
    g_zbuf = b; g_zstate = 1; return 1;
}
// Block rows [y0, y1) of image level i, layer ly (w x h texels, bx blocks a row, pitch bytes a row) as zero texels from the zero buffer: 0 = not done (no buffer, or a row wider
// than it), the caller zeroes them through scratch memory
static int zero_rows(vka_img *im, VkCommandBuffer cb, uint32_t i, uint32_t ly, uint32_t w, uint32_t h, uint32_t bx, size_t pitch, uint32_t y0, uint32_t y1) {
    const vka_fmt *f = &im->f; uint32_t per = (uint32_t)(ZHALF / pitch);
    if (!per || f->dec || !zero_src()) return 0;   // (a decoded BC image takes texels: the caller's zero blocks go through the decode)
    VkBufferImageCopy rg[64]; uint32_t n = 0;
    for (uint32_t y = y0; y < y1; y += per) {
        uint32_t ye = y + per < y1 ? y + per : y1, py0 = y * f->bh, py1 = ye * f->bh > h ? h : ye * f->bh;
        rg[n++] = (VkBufferImageCopy){is_astc(f->vk) ? ZHALF : 0, bx * f->bw, 0, {im->aspect, i, ly, 1}, {0, (int32_t)py0, 0}, {w, py1 - py0, 1}};
        if (n == 64) { vk.vkCmdCopyBufferToImage(cb, g_zbuf, im->img, VK_IMAGE_LAYOUT_GENERAL, n, rg); n = 0; }
    }
    if (n) vk.vkCmdCopyBufferToImage(cb, g_zbuf, im->img, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    return 1;
}
// Tiles across image level i of a sparse residency image (its tile width in texels from the format's granularity)
static uint32_t rs_ntx(const vka_img *im, uint32_t i) { uint32_t w = im->w >> i ? im->w >> i : 1; return (w + im->rs_tw - 1) / im->rs_tw; }
// Image level i is bound tile by tile, and its bands are its rows of 64 KB tiles (one band = one tile high, tiles across x 64 KB): 1. A tiled level whose bands are not that
// shape (never seen) has all its tiles bound and is uploaded as a dense one
static int res_level(vka_img *im, uint32_t i, uint32_t band_rows, uint64_t band_bytes) {
    if (!im->rs || i >= vks_tiled(im->rs)) return 0;
    uint32_t ntx = rs_ntx(im, i), h = im->h >> i ? im->h >> i : 1, nty = (h + im->rs_th - 1) / im->rs_th;
    if (band_rows * im->f.bh == im->rs_th && band_bytes == (uint64_t)ntx * 0x10000) return 1;
    VRED_ONCE("sparse texture serial %llu level %u: bands of %u block rows / %llu bytes are not rows of %ux%u tiles: the level is bound whole", (unsigned long long)im->serial, i + im->base,
              band_rows, (unsigned long long)band_bytes, im->rs_tw, im->rs_th);
    for (uint32_t ly = 0; ly < im->layers; ly++) for (uint32_t ty = 0; ty < nty; ty++) for (uint32_t tx = 0; tx < ntx; tx++) vks_tile(im->rs, i, ly, tx, ty, 1);
    return 0;
}
// The bound tiles of bands [k, k1) of image level i, layer ly from staging s (rows from block row y0 at pitch): one copy region per run of bound tiles in a band
static void copy_bound(vka_img *im, VkCommandBuffer cb, const vka_span *s, uint32_t i, uint32_t ly, uint32_t w, uint32_t h, size_t pitch, uint32_t band_rows, uint32_t k, uint32_t k1, uint32_t y0) {
    const vka_fmt *f = &im->f; uint32_t ntx = rs_ntx(im, i), bx = (w + f->bw - 1) / f->bw;
    VkBufferImageCopy rg[32]; uint32_t n = 0;
    for (uint32_t kk = k; kk < k1; kk++) {
        uint32_t r0 = kk * band_rows, py0 = r0 * f->bh; if (py0 >= h) break;
        uint32_t py1 = (r0 + band_rows) * f->bh > h ? h : (r0 + band_rows) * f->bh;
        for (uint32_t tx = 0, t1; tx < ntx; tx = t1 + 1) {
            for (t1 = tx; t1 < ntx && vks_tile_bound(im->rs, i, ly, t1, kk); t1++) {}
            if (t1 == tx) continue;
            uint32_t px0 = tx * im->rs_tw, px1 = t1 * im->rs_tw > w ? w : t1 * im->rs_tw;
            rg[n++] = (VkBufferImageCopy){s->off + (VkDeviceSize)(r0 - y0) * pitch + (VkDeviceSize)(px0 / f->bw) * f->bpb, bx * f->bw, (py1 - py0 + f->bh - 1) / f->bh * f->bh,
                                          {im->aspect, i, ly, 1}, {(int32_t)px0, (int32_t)py0, 0}, {px1 - px0, py1 - py0, 1}};
            if (n == 32) { vk.vkCmdCopyBufferToImage(cb, s->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, n, rg); n = 0; }
        }
    }
    if (n) vk.vkCmdCopyBufferToImage(cb, s->buf, im->img, VK_IMAGE_LAYOUT_GENERAL, n, rg);
}
// Levels and layers of a sparse texture's image whose pages or mappings changed since (gen, stamp), or all of them, gathered from the page table and deswizzled by bands:
// a band is one row of GOB blocks (8 << block height block rows), which is one range of the storage. Bands with nothing mapped are zero texels from the zero buffer, bands that
// did not change (all = 0) are left alone, and runs of mapped bands go through scratch memory of at most 16 MiB a run, gathered from the page table (unmapped pages of a
// band as zero texels): the staging is about what is mapped, never a whole level (Mario Kart World 1.8.0's 6144x6144 RGBA8 array levels, 144 MiB each, have about half of
// their pages mapped at boot, and its 8192x8192 x68 arrays' levels a few MiB in all). 3D textures go by whole levels, as before. Each run is a point where the upload
// batch may go to the queue (vka_up_point)
static void upload_sparse_parts(vka_img *im, VkCommandBuffer cb, uint64_t gen, uint64_t stamp, int all, int bind_only) {
    img_touch(im);
    const nvn_texdesc *d = &im->d; const vka_fmt *f = &im->f; uint64_t mem = (uint64_t)(uintptr_t)im->mem;
    uint8_t *part = NULL, *bst = NULL; uint64_t cap = 0, up = 0, mapped = 0, zeroed = 0; unsigned nsub = 0, nzero = 0; uint32_t bst_cap = 0;
    int is3d = d->target == NVN_TEXTURE_TARGET_3D;
    for (uint32_t i = 0; i < im->levels && cb; i++) {
        uint32_t lv = i + im->base, w = im->w >> i ? im->w >> i : 1, h = im->h >> i ? im->h >> i : 1;
        uint32_t bx = (w + f->bw - 1) / f->bw, by = (h + f->bh - 1) / f->bh;
        size_t pitch = (size_t)bx * f->bpb;
        // Bands: block rows and storage bytes of each (a 3D level: one band of all of it)
        uint32_t band_rows = by; uint64_t band_bytes = 0;
        if (is3d || nvn_deswizzle_block_rows(d, lv) != (int)by || nvn_deswizzle_bands(d, lv, &band_rows, &band_bytes)) band_rows = by, band_bytes = 0;
        for (uint32_t ly = 0; ly < im->layers && cb; ly++) {
            uint64_t off, len; sub_range(d, &im->L, lv, ly, &off, &len);
            if (!len || (!all && !nvn_sparse_since(im->vpool, mem + off, len, gen, stamp))) continue;
            uint32_t nb = band_bytes ? (by + band_rows - 1) / band_rows : 1;
            // Band k: 0 = left alone, 1 = zero texels, 2 = from the page table
            #define BAND_OFF(k) ((uint64_t)(k) * band_bytes)
            #define BAND_LEN(k) (band_bytes ? (BAND_OFF(k) < len ? (len - BAND_OFF(k) < band_bytes ? len - BAND_OFF(k) : band_bytes) : 0) : len)
            // (a band is redone only when it changed: unmapped since the last upload counts, so a run never mixes changed and unchanged bands)
            #define BAND_CHANGED(k) (all || nvn_sparse_since(im->vpool, mem + off + BAND_OFF(k), BAND_LEN(k), gen, stamp))
            #define BAND_STATE(k) (!BAND_LEN(k) ? all : !BAND_CHANGED(k) ? 0 : nvn_sparse_any(im->vpool, mem + off + BAND_OFF(k), BAND_LEN(k)) ? 2 : 1)
            int any = 0;
            // A sparse residency image's tiled level (vka_sparse.c): a band is one row of 64 KB tiles. Each changed tile is bound when a page of it is mapped and unbound when
            // none is; a band with a bound tile is gathered (2) and only its bound tiles copied, the rest is left alone (0: unbound tiles read 0, no zero texels)
            int rt = res_level(im, i, band_rows, band_bytes);
            if (bind_only && !rt) continue;   // (a GPU-written image's levels past the tiled ones are bound whole, vks_new; their content is the GPU's)
            if (bst_cap < nb) { free(bst); bst_cap = nb; if (!(bst = malloc(bst_cap))) { bst_cap = 0; VRED("sparse texture serial %llu: band states not allocated", (unsigned long long)im->serial); goto out; } }
            for (uint32_t k = 0; k < nb; k++) {
                if (!rt) { bst[k] = (uint8_t)BAND_STATE(k); continue; }
                int b2 = 0;
                if (BAND_CHANGED(k)) for (uint32_t tx = 0; tx < rs_ntx(im, i); tx++) {
                    uint64_t po = off + BAND_OFF(k) + (uint64_t)tx * 0x10000;
                    b2 |= vks_tile(im->rs, i, ly, tx, k, po < off + len && nvn_sparse_any(im->vpool, mem + po, 0x10000));
                }
                bst[k] = b2 ? 2 : 0;
            }
            if (bind_only) continue;
            for (uint32_t k = 0, k1; k < nb && cb; k = k1) {
                int st = bst[k]; k1 = k + 1;
                size_t maxrun = 16u << 20; if (maxrun < (size_t)band_rows * pitch) maxrun = (size_t)band_rows * pitch;
                while (k1 < nb && (size_t)(k1 + 1 - k) * band_rows * pitch <= maxrun && bst[k1] == st) k1++;
                if (!band_bytes) k1 = nb;
                uint32_t y0 = k * band_rows, y1 = k1 * band_rows < by ? k1 * band_rows : by;
                if (!st) continue;
                any = 1;
                size_t sz = (size_t)(y1 - y0) * pitch * (is3d ? (im->depth >> i ? im->depth >> i : 1) : 1);
                if (st == 1 && !is3d && zero_rows(im, cb, i, ly, w, h, bx, pitch, y0, y1)) { zeroed += sz; continue; }
                vka_span s; if (!vka_scratch(sz, 16, &s)) { VRED("sparse texture serial %llu: no scratch memory for level %u layer %u (%zu bytes)", (unsigned long long)im->serial, lv, ly, sz); goto out; }
                if (st == 1) { if (is_astc(f->vk)) for (size_t q = 0; q + 16 <= sz; q += 16) memcpy(s.ptr + q, k_astc_zero, 16); else memset(s.ptr, 0, sz); zeroed += sz; }
                else {
                    uint64_t so = off + BAND_OFF(k), sl = band_bytes ? BAND_OFF(k1) - BAND_OFF(k) : len;
                    if (so + sl > off + len) sl = off + len - so;
                    if (sl > cap) { free(part); cap = sl; if (!(part = malloc(cap))) { VRED("sparse texture serial %llu: copy of %llu bytes not allocated", (unsigned long long)im->serial, (unsigned long long)sl); goto out; } }
                    mapped += sl - nvn_sparse_copy(im->vpool, part, mem + so, sl);
                    if (is_astc(f->vk)) astc_zero_blocks(part, sl);
                    // the deswizzler reads level lv of layer ly at its offset in the whole storage, and writes row y at y * pitch: part holds just this run's bytes
                    if (nvn_deswizzle_rows(d, lv, ly, (const uint8_t *)((uintptr_t)part - (uintptr_t)so), s.ptr - (size_t)y0 * pitch, pitch, y0, y1)) {
                        VRED_ONCE("texture serial %llu format %#x target %d: no CPU deswizzle", (unsigned long long)im->serial, d->format, d->target); goto out; }
                    up += sz;
                }
                uint32_t py0 = y0 * f->bh, py1 = y1 * f->bh > h ? h : y1 * f->bh;
                uint32_t dz = is3d ? (im->depth >> i ? im->depth >> i : 1) : 1;
                VkBufferImageCopy c = {s.off, bx * f->bw, (y1 - y0) * f->bh, {im->aspect, i, ly, 1}, {0, (int32_t)py0, 0}, {w, py1 - py0, dz}};
                if (rt) copy_bound(im, cb, &s, i, ly, w, h, pitch, band_rows, k, k1, y0);   // only the bound tiles (an unbound one takes no write)
                else if (f->dec) bc_copy(cb, im, s.ptr, pitch, bx, y1 - y0, dz, i, ly, 0, (int32_t)py0, 0, w, py1 - py0);   // (a sparse pool's BC texture: decoded from the staged rows)
                else vk.vkCmdCopyBufferToImage(cb, s.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                cb = vka_up_point(cb);
            }
            #undef BAND_STATE
            #undef BAND_CHANGED
            #undef BAND_LEN
            #undef BAND_OFF
            nsub += any; if (any && !nvn_sparse_any(im->vpool, mem + off, len)) nzero++;
        }
    }
out:
    free(part); free(bst);
    static _Atomic int nl; if (nsub && atomic_fetch_add(&nl, 1) < 64)
        VLOG("sparse texture serial %llu %ux%u layers %u levels %u+%u of %u format %#x: %u levels x layers uploaded (%u with nothing mapped), %.1f MiB through staging, %.1f MiB of zero texels, %.1f MiB of pages mapped behind them%s",
             (unsigned long long)im->serial, im->d.width, im->d.height, im->layers, im->base, im->levels, im->base + im->levels, d->format, nsub, nzero, up / 1048576.0, zeroed / 1048576.0, mapped / 1048576.0, all ? "" : " (changed since the last upload)");
}
static void upload_sparse(vka_img *im, VkCommandBuffer cb, int all) {   // generation and stamp, protect the physical pages behind it, then read (nvn_mtl.m pkg_upload)
    uint64_t gen = im->vgen, stamp = im->stamp;
    im->vgen = nvn_sparse_gen(); im->stamp = nvn_dirty_now(); nvn_sparse_protect(im->vpool, (uint64_t)(uintptr_t)im->mem, im->L.size);
    upload_sparse_parts(im, cb, gen, stamp, all, 0);
}
// A GPU-written sparse texture's residency image (sparse_gpu): its tiles bound where pages are mapped and unbound where none is, nothing read (all: every tile; else those whose
// mappings changed since). Open: a tile bound again starts with what its memory slot held (the Switch keeps the physical page's content), not cleared
static void sparse_bind(vka_img *im, VkCommandBuffer cb, int all) {
    uint64_t gen = im->vgen, stamp = im->stamp;
    im->vgen = nvn_sparse_gen(); im->stamp = nvn_dirty_now();
    upload_sparse_parts(im, cb, gen, stamp, all, 1);
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 24)
        VLOG("sparse GPU-written texture serial %llu %ux%u layers %u levels %u format %#x: tiles bound %s, %.1f MiB bound of %.1f MiB of address space", (unsigned long long)im->serial, im->w, im->h, im->layers,
             im->levels, im->d.format, all ? "at its first use" : "again (its mappings changed)", vks_img_bytes(im->rs) / 1048576.0, im->va / 1048576.0);
}
static void dirty(int **list, int *n, int *cap, uint8_t **flag, int nslots, int id);
static int *g_tdirty, g_ntd, g_tdcap; static uint8_t *g_tdflag;
typedef struct { tex_t t; view_t v; uint8_t has_v, img; uint64_t pgen; } treg_ent;   // pgen: the texture's pool's Initialize serial at registration (nvn_pool_live)
static treg_ent *g_treg; static int g_ntreg;   // indexed by host texture ID
// What each pool slot holds now, per texture ID: [0] binding 0 (sampled), [1] binding 2 (storage), the view written there and its g_vgen_h bucket when it was ("Pool slot writes" at
// vka_tex_prepare). Grown with g_treg (under g_tmu); NULL / 0 after a failed allocation, which writes every slot as before
typedef struct { VkImageView view[2]; uint32_t gen[2]; uint8_t valid; } pool_written;
static pool_written *g_pool_written; static int g_pool_written_cap;
static uint64_t g_pool_slot_write, g_pool_slot_skip;   // (under g_tmu) slot writes sent and skipped as unchanged (DM_VK_POOL_SLOT_DEDUP)
// The image made again from texture level base (caller holds g_tmu): the old image, its memory and views go when the GPU is done with them, and the texture IDs that name it
// are written again at this submission (vka_tex_prepare); then everything is uploaded
static uint64_t g_rebase;
static unsigned redirty_ids(vka_img *im) {   // the texture IDs registered with im's texture, written again at this submission (caller holds g_tmu)
    unsigned nid = 0;
    for (int id = 1; id < g_ntreg; id++) { if (!g_treg[id].t.serial) continue; smap_ent *e = map_get(g_treg[id].t.serial); if (e && e->im == im) { dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id); nid++; } }
    return nid;
}
static int img_rebase(vka_img *im, uint32_t base, VkCommandBuffer cb) {
    tex_t t = {0}; t.serial = im->serial; t.d = im->d; t.L = im->L; t.mem = (uint8_t *)im->mem; t.pool = (pool_t *)im->vpool;   // (the pool: a sparse residency image again)
    if (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) t.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;   // (made with storage use, or upgraded to it: its storage use and learned formats stay)
    vka_img *n = img_create(&t, &im->f, base);
    if (!n) return 0;
    // Going finer but the address space (which still counts this image's own) did not take it: the image stays as it is, rather than one at a coarser level or dense
    // (and a sparse image is not swapped for a dense one from a finer level: a large memory jump when only the residency image was refused)
    if ((base < im->base && (n->base >= im->base || (im->rs && !n->rs))) || (n->base == im->base && n->levels == im->levels && !!n->rs == !!im->rs)) {
        int floored = base < im->base;
        uint64_t need = n->bneed > im->va ? n->bneed : im->va + 1;   // (a start finer than im->base asks for more than im itself: n's ask may be of a coarser level)
        if (n->dcap) im->dcap = 1;   // (its dense images are capped: sparse_refresh does not make this one again)
        g_img_bytes -= n->bytes; img_destroy(n);
        if (floored) { im->bfloor = im->base; im->bneed = need; }   // (tried again once what the finer level asked for is free)
        return 0;
    }
    vka_img *old = calloc(1, sizeof *old);
    if (!old) { g_img_bytes -= n->bytes; img_destroy(n); return 0; }
    old->img = im->img; old->m = im->m; old->views = im->views; old->rs = im->rs; old->va = im->va;
    uint32_t ob = im->base, ol = im->levels; uint64_t obytes = im->bytes;
    g_img_bytes -= im->bytes;
    im->img = n->img; im->m = n->m; im->views = NULL; im->usage = n->usage; im->w = n->w; im->h = n->h; im->levels = n->levels; im->base = n->base; im->bytes = n->bytes;
    im->alt = n->alt; im->any_fmt = n->any_fmt; im->nx = n->nx; memcpy(im->xfmt, n->xfmt, sizeof im->xfmt);
    im->rs = n->rs; im->va = n->va; im->rs_tw = n->rs_tw; im->rs_th = n->rs_th; im->bfloor = n->bfloor; im->bneed = n->bneed; im->dcap = n->dcap;   // (old's address space, still counted, goes back
                                                                                                                           // at its img_destroy: a finer start is tried again once enough is free)
    free(n);
    vka_after(vka_fence_last() + 1, img_destroy, old);
    vka_img_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    unsigned nid = redirty_ids(im);
    if (g_rebase++ < 32) VLOG("sparse texture serial %llu %ux%u format %#x: levels %u+%u → %u+%u (%.1f → %.1f MiB of image memory, %u texture IDs written again)", (unsigned long long)im->serial, im->d.width, im->d.height,
                              im->d.format, ob, ol, im->base, im->levels, obytes / 1048576.0, im->bytes / 1048576.0, nid);
    return 1;
}
// A sparse texture whose pages or mappings changed: its finest mapped level moved finer, or two or more levels coarser, → made again; else the changed parts uploaded
static void sparse_refresh(vka_img *im, VkCommandBuffer cb) {
    img_touch(im);
    uint32_t vmin, b = sparse_base(im->vpool, im->mem, &im->d, &im->L, &vmin);
    if (b < im->bfloor && vks_va_free() < im->bneed) b = im->bfloor;   // (levels left out for the sparse address space; tried again once what the finer level asked for is free: a
                                                                          // rebase counts the old image's address space until it goes, vka_after, and a finer start must fit beside the current image)
    if (im->dcap && !im->rs) { uint32_t bd = dense_floor(&im->d, &im->L, b); if (bd > b) b = bd; }   // (a dense image under DM_VK_SPARSE_DENSE_MB: no rebase to a start the cap moves back)
    if (vmin != im->vmin) {   // the views start elsewhere: the texture IDs that name it are written again
        if (g_rebase < 32) VLOG("sparse texture serial %llu %ux%u format %#x: sampled from level %u (was %u)", (unsigned long long)im->serial, im->d.width, im->d.height, im->d.format, vmin, im->vmin);
        im->vmin = vmin; redirty_ids(im);
    }
    // A new image (rebase): its barrier and its whole upload in the upload batch when cb is the replay's (nothing recorded before reads the new image)
    if (im->d.target != NVN_TEXTURE_TARGET_3D && (b < im->base || b >= im->base + 2)) {
        VkCommandBuffer ucb = cb && cb == vka_rp_cb() && vka_up_on() ? vka_up_begin() : VK_NULL_HANDLE;
        int made = img_rebase(im, b, ucb ? ucb : cb);
        if (made) upload_sparse(im, ucb ? ucb : cb, 1);
        if (ucb) vka_up_end();
        if (made) return;
    }
    upload_sparse(im, cb, 0);
}
// ---------- Large images (diagnostic, DM_VK_BIGIMG_MB) ----------
// For each image of at least DM_VK_BIGIMG_MB (default 64; memrun-style runs set 16): its first GPU write (vka_img_written_at: frame and site), the draws that bound it for sampling
// (id_view), and the other textures found over its storage (a texture of the same storage and shape shares the image, same_shape; one of another shape or format gets an image
// of its own, so content drawn through one is not in the other). The ctl stats add the largest ones the GPU has not written (vka_tex_big_stats). Caller holds g_tmu
static uint64_t big_min(void) { double mb = VKA_ENVF("DM_VK_BIGIMG_MB", 64.0); return mb > 0 ? (uint64_t)(mb * 1048576.0) : UINT64_MAX; }
static int ranges_meet(const uint8_t *a, uint64_t an, const uint8_t *b, uint64_t bn) { return a && b && a < b + bn && b < a + an; }
static void ovl_add(vka_img *big, uint64_t serial) { if (big->novl < 4) big->ovl[big->novl] = serial; big->novl++; }
static void big_note(vka_img *im, const tex_t *t) {   // a new image of texture t
    uint64_t mn = big_min(); static _Atomic int nl;
    if (!t->mem || mn == UINT64_MAX) return;
    for (int i = 0; i < g_nbig; i++) { vka_img *b = g_big[i];
        if (b == im || !ranges_meet(b->mem, b->L.size, t->mem, t->L.size)) continue;
        ovl_add(b, t->serial);
        if (atomic_fetch_add(&nl, 1) < 32) VLOG("large image serial %llu (%ux%u format %#x, storage %p+%#llx): texture serial %llu %ux%ux%u layers %u format %#x flags %#x target %d over its storage (%p+%#llx), an image of its own%s",
            (unsigned long long)b->serial, b->w, b->h, b->d.format, (const void *)b->mem, (unsigned long long)b->L.size, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->L.layers,
            t->d.format, t->d.flags, t->d.target, (const void *)t->mem, (unsigned long long)t->L.size, im->m.z ? " (zero-shared)" : ""); }
    if (im->m.size < mn) return;
    for (int i = 0; i < g_nlive; i++) { vka_img *o = g_live[i];
        if (o == im || !ranges_meet(o->mem, o->L.size, t->mem, t->L.size)) continue;
        ovl_add(im, o->serial);
        if (atomic_fetch_add(&nl, 1) < 32) VLOG("large image serial %llu (%ux%u format %#x, storage %p+%#llx): made over the storage of texture serial %llu %ux%u format %#x flags %#x (%p+%#llx, %s)",
            (unsigned long long)im->serial, im->w, im->h, im->d.format, (const void *)t->mem, (unsigned long long)t->L.size, (unsigned long long)o->serial, o->w, o->h, o->d.format, o->d.flags,
            (const void *)o->mem, (unsigned long long)o->L.size, o->gpu_owned ? "GPU-written" : "not GPU-written"); }
    if (g_nbig < 64) g_big[g_nbig++] = im;
}
static void big_share(vka_img *im, const tex_t *t) {   // texture t shares im (same storage and shape)
    if (im->m.size < big_min() || t->serial == im->serial) return;
    ovl_add(im, t->serial);
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) VLOG("large image serial %llu (%ux%u format %#x): texture serial %llu (format %#x) of the same storage and shape shares it", (unsigned long long)im->serial,
                                                                  im->w, im->h, im->d.format, (unsigned long long)t->serial, t->d.format);
}
void vka_tex_big_stats(char *b, size_t n) {
    uint64_t mn = big_min(); if (n) b[0] = 0;
    if (mn == UINT64_MAX || !n) return;
    pthread_mutex_lock(&g_tmu);
    vka_img *top[8]; int nt = 0, all = 0;
    for (int i = 0; i < g_nlive; i++) { vka_img *im = g_live[i];
        if (im->gpu_owned || im->mode != ST_ZERO || im->from_mem || im->m.size < mn) continue;
        all++;
        int k = nt < 8 ? nt++ : 8; if (k == 8) { if (im->m.size <= top[7]->m.size) continue; k = 7; }
        while (k > 0 && top[k - 1]->m.size < im->m.size) { top[k] = top[k - 1]; k--; }
        top[k] = im; }
    size_t l = (size_t)snprintf(b, n, "large images the GPU has not written: %d", all);
    for (int i = 0; i < nt && l < n; i++) { vka_img *im = top[i];
        l += (size_t)snprintf(b + l, n - l, "; serial %llu %ux%u format %#x %.0f MiB%s, sampled by %u draws, %d refs, %u other textures over its storage", (unsigned long long)im->serial, im->w, im->h, im->d.format,
                              im->m.size / 1048576.0, im->m.z ? " zero-shared" : "", im->nsamp, im->refs, im->novl);
        for (uint32_t k = 0; k < im->novl && k < 4 && l < n; k++) { smap_ent *e = map_get(im->ovl[k]);
            l += (size_t)snprintf(b + l, n - l, "%s%llu%s", k ? " " : " (", (unsigned long long)im->ovl[k], !e || !e->im ? "?" : e->im == im ? "=" : e->im->gpu_owned ? "W" : "-"); }
        if (im->novl && l < n) l += (size_t)snprintf(b + l, n - l, ")"); }
    pthread_mutex_unlock(&g_tmu);
}

// ---------- Zero-shared images ----------
// A large image whose content is the GPU's (ST_ZERO: render targets, copy destinations, shader images) holds zeros until the GPU first writes it, and many engines make such
// targets long before they draw into them, or never: Mario Kart World 1.8.0 keeps four 16384² R8 terrain targets (341 MiB each, flags SPARSE | COMPRESSIBLE, no sparse
// residency for 8-bit formats on the Adreno 840) and about 300 MB of other cleared images that its scene never writes. On the Switch such a texture costs only its pool
// storage. So a new image of at least DM_VK_ZSHARE_MB (default 4; NVN storage bytes) that is dense, single-sampled, not 3D, not block-compressed, not linear and not a display
// texture, made with ST_ZERO content, is created with VK_IMAGE_CREATE_ALIAS_BIT and bound at offset 0 of one zeroed allocation per identical create info (vka_imgmem.c
// vka_imem_bind_zero; Vulkan's "Memory Aliasing": images of identical create info with ALIAS_BIT bound at the same offset read the memory alike). Each image's first use clears
// it to zero behind a full barrier as before (zeros onto zeros, which also sets that image's layout and the driver's compression metadata). Before its first write (vka_tex_own:
// an attachment at render-pass begin, a clear, a copy or blit destination, a buffer-to-image copy, a shader image, a CPU re-upload) it gets memory of its own (img_own): a new
// image of the same texture, cleared to zero in the upload batch (nothing recorded before reads it), swapped in place as img_storage_upgrade does (every serial and view that
// shares it follows; the recording memo and the texture IDs that name it are made again), the old one destroyed after the submission, or once its holders let it go.
// A write that comes without it is logged [RED] (vka_img_written_at). DM_VK_ZSHARE=0 (live: images made from then on): every image has memory of its own, as before
static uint64_t g_zown;
// A depth / stencil image's first content (its zero clear at its first use, img_first_use, or with the memory of its own a zero-shared one gets, img_own) is recorded into
// the submission being recorded when no pass is open there, not into the upload batch: a clear has no staging for the batch to retire. On Turnip (Adreno 840, Mainline
// Turnip v32) upstream saw a depth image first cleared in the upload batch (another command buffer, earlier on the queue) and then cleared and drawn into with LRZ by a pass
// right after a pass over another depth target lose the device (upstream's docs/reference/rendering.md "Turnip hangs in the first 3D frame of a cold run"); with the clear in
// the pass's own command buffer it did not. No broken valid-usage rule was found (queue order, a full barrier at the batch's end), so this is a workaround for the driver,
// whose cause is open; it holds for every driver. A sole zero allocation adopted in place (vka_imem_adopt_zero) is cleared again into that submission too: its first clear
// may have gone into an earlier one (write_tex at an earlier vka_tex_prepare, or vka_tex_get while a pass was open). DM_VK_DS_FIRST_HERE=0 (live): through the upload batch, as before
static int ds_here(const vka_img *im, VkCommandBuffer cb) { return im->f.kind >= 3 && cb && cb == vka_rp_cb() && !vka_rp_is_open() && VKA_ENV("DM_VK_DS_FIRST_HERE", 1); }
static int img_own(vka_img *im, VkCommandBuffer cb) {   // caller holds g_tmu; 1 = im has memory of its own
    if (!im->m.z) return 1;
    if (vka_imem_adopt_zero(&im->m)) {
        im->bytes = im->m.size; g_img_bytes += im->bytes; img_touch(im);
        if (ds_here(im, cb)) {   // (its first clear may be in an earlier command buffer: clear it again here, as a new image would be; the memory holds zeros already)
            vka_img_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            VkImageSubresourceRange rg = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            VkClearDepthStencilValue z = {0, 0}; vk.vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
            vka_barrier(cb);
        }
        if (g_zown++ < 16) VLOG("zero-shared image serial %llu: sole allocation adopted in place (%.1f MiB), no duplicate allocation", (unsigned long long)im->serial, im->bytes / 1048576.0);
        return 1;
    }
    tex_t t = {0}; t.serial = im->serial; t.mem = (uint8_t *)im->mem; t.d = im->d; t.L = im->L;
    if (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) t.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;   // (made with storage use, or upgraded to it)
    VkCommandBuffer ucb = !vka_scr_up && !ds_here(im, cb) && cb && cb == vka_rp_cb() && vka_up_on() ? vka_up_begin() : VK_NULL_HANDLE, rc = ucb ? ucb : cb;
    vka_img *n = rc ? img_create(&t, &im->f, im->base) : NULL;   // (g_zwant is 0: memory of its own)
    if (!n) { if (ucb) vka_up_end();
        static _Atomic int k; if (atomic_fetch_add(&k, 1) < 8) VRED("zero-shared image serial %llu %ux%u format %#x: no memory of its own (%.0f MiB%s): its writes go to the memory it shares", (unsigned long long)im->serial,
                                                                      im->w, im->h, im->d.format, im->m.size / 1048576.0, rc ? "" : ", no command buffer");
        return 0; }
    vka_img_barrier(rc, n->img, n->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkImageSubresourceRange rg = {n->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (n->f.kind >= 3) { VkClearDepthStencilValue z = {0, 0}; vk.vkCmdClearDepthStencilImage(rc, n->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    else { VkClearColorValue z = {{0}}; vk.vkCmdClearColorImage(rc, n->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    vka_barrier(rc);
    if (ucb) vka_up_end();
    n->inited = 1;
    VkImage oi = im->img; vka_imem om = im->m; void *ov = im->views;
    im->img = n->img; im->m = n->m; im->views = NULL; im->usage = n->usage; im->bytes = n->bytes; im->alt = n->alt; im->any_fmt = n->any_fmt; im->nx = n->nx; memcpy(im->xfmt, n->xfmt, sizeof im->xfmt);
    n->img = oi; n->m = om; n->views = ov; n->bytes = 0;
    if (im->holds) { n->zold = im->zold; im->zold = n; }   // (a holder may read im->img unlocked: the old image stays until it lets go, vka_tex_drop)
    else vka_after(vka_fence_last() + 1, img_destroy, n);   // (after the submission being recorded, which may hold commands that read it)
    atomic_fetch_add_explicit(&g_tgen, 1, memory_order_release);
    unsigned nid = redirty_ids(im);
    vka_imem_zowned();
    if (g_zown++ < 16) VLOG("zero-shared image serial %llu %ux%u layers %u format %#x: memory of its own at its first write (%.1f MiB, frame %llu, %u texture IDs written again)", (unsigned long long)im->serial,
                            im->w, im->h, im->layers, im->d.format, im->bytes / 1048576.0, (unsigned long long)vka_frame(), nid);
    return 1;
}
void vka_tex_own(vka_img *im, VkCommandBuffer cb) {
    if (!im || !im->m.z) return;   // (m.z changes on the recording thread only)
    if (!vka_up_on() && cb && cb == vka_rp_cb() && vka_rp_is_open()) vka_rp_end();   // (no upload batch: the clear goes into cb, outside a pass)
    pthread_mutex_lock(&g_tmu); img_own(im, cb); pthread_mutex_unlock(&g_tmu);
}

// The content a new image of t starts with (storage_mode), decided before the image is made (vka_tex_get: a zero-shared image is made only for ST_ZERO)
static int first_mode(const tex_t *t, const vka_fmt *f, int cpu) {
    int samples = t->d.samples > 1 ? t->d.samples : 1, vpool = t->pool && pool_sparse(t->pool);
    uint64_t seen; uint8_t *c = samples == 1 && t->mem && !vpool ? gw_content(t->mem, t->L.size, &seen) : NULL;   // the content the mode's "holds data" checks look at
    int mode = samples == 1 ? storage_mode(t, c ? c : t->mem, f, cpu, g_first_sampled) : ST_ZERO;
    free(c);
    return mode;
}
static void img_first_use(vka_img *im, const tex_t *t, VkCommandBuffer cb, int mode) {
    vka_img_barrier(cb, im->img, im->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    im->inited = 1;
    if (mode != ST_ZERO) im->from_mem = 1;
    im->mode = (uint8_t)mode;
    if (im->rs && mode == ST_ZERO && sparse_gpu(t, &im->f)) { im->vbind = 1; sparse_bind(im, cb, 1); }   // (a GPU-written sparse texture: its tiles bound before the clear below)
    // (an upload into the upload batch may send it and go on in the next one: vka_up_cur is where it ended; NULL: none could be begun after a GPU hang)
    if (mode == ST_TRACK) { im->track = 1; im->span = nvn_dirty_span(t->mem, t->L.size); upload_tracked(im, t, cb); g_st_up[mode]++; if ((cb = vka_up_cur(cb))) vka_barrier(cb); return; }
    if (mode == ST_SPARSE) { im->vtrack = 1; upload_sparse(im, cb, 1); g_st_up[mode]++; if ((cb = vka_up_cur(cb))) vka_barrier(cb); return; }
    if (mode == ST_FILL) { upload_fill(im, t, cb); g_st_up[mode]++; if ((cb = vka_up_cur(cb))) vka_barrier(cb); return; }   // the copy before the draw or copy that reads it
    g_st_up[ST_ZERO]++;
    VkImageSubresourceRange rg = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (im->f.kind >= 3) { VkClearDepthStencilValue z = {0, 0}; vk.vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    else if (!compressed(&im->f) || im->f.dec) { VkClearColorValue z = {{0}}; vk.vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }   // (dec: BC decoded on the CPU, an uncompressed image)
    else if (t->mem) { im->from_mem = 1; upload_fill(im, t, cb); cb = vka_up_cur(cb); }   // a compressed texture with no data yet: its storage (zeros) is a valid block pattern; Vulkan cannot clear it
    if (cb) vka_barrier(cb);
}

// Drop only serial-map ownership. Presentation/capture and writeback references retain their image until released.
// Called on the recording thread, never midway through a draw; invalidating g_tgen also invalidates borrowed view memos.
static int img_detach_locked_at(vka_img *im, uint64_t fence) {
    int owners = vksm_count_im(&g_smap, im);
    if (!owners) return 0;
    if (im->refs < owners + im->holds + !!im->wb_pending) {
        VRED_ONCE("image cache ownership mismatch: %d refs, %d serials, %d holds, writeback %d", im->refs, owners, im->holds, im->wb_pending);
        return 0;
    }
    redirty_ids(im);
    map_del_im(im);
    im->refs -= owners - 1;
    unref_locked_at(im, fence);
    return 1;
}
static int img_detach_locked(vka_img *im) { return img_detach_locked_at(im, vka_fence_last() + 1); }
void vka_tex_drop_pool(const void *pool) {
    // Finalization can race recording. Defer lookup/removal until prepare owns both the pool-use and image locks.
    // img_pool_dead checks the generation too, so a new pool at the same address is never discarded.
    if (pool) atomic_store(&g_dead_pools, 1);
}
static uint64_t g_img_lim, g_lim_frame, g_lim_ns, g_prune_frame = UINT64_MAX, g_prune_bytes, g_prune_ns;
static int g_psi_pressed;   // memory pressure past VKTL_PSI_HOLD at the last limit update: measured every 15 frames instead of 60
static uint64_t img_time_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static uint64_t meminfo_kb(const char *b, const char *key) { const char *p = strstr(b, key); return p ? strtoull(p + strlen(key), NULL, 10) : 0; }
// Memory pressure (vka_texlimit.h vktl_pressure_limit): "some avg10=" of /proc/pressure/memory, in percent; -1 when the kernel has no PSI or the app may not read it
static double psi_some10(void) {
    int fd = open("/proc/pressure/memory", O_RDONLY | O_CLOEXEC), err = 0;
    char b[256]; ssize_t n = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
    if (n < 0) err = errno;
    if (fd >= 0) close(fd);
    const char *a = n > 0 ? (b[n] = 0, strstr(b, "some avg10=")) : NULL;
    if (!a) { static _Atomic int k; if (!atomic_exchange(&k, 1)) VLOG("image cache: /proc/pressure/memory not readable (%s): the limit follows MemAvailable and the heap budget only", err ? strerror(err) : "no some avg10"); return -1; }
    return strtod(a + 11, NULL);
}
static void img_limit_update(uint64_t frame) {
    uint64_t now = img_time_ns();
    if (g_img_lim && frame < g_lim_frame + (g_psi_pressed ? 15 : 60) && now - g_lim_ns < 1000000000ull) return;
    g_lim_frame = frame; g_lim_ns = now;
    int override = VKA_ENV("DM_VK_IMG_LIMIT_MB", 0);
    vktl_in in = {.img_bytes = g_img_bytes + vka_imem_zbytes() + atomic_load(&g_retired_bytes), .psi_some10 = -1};
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { char b[2048]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
        if (n > 0) { b[n] = 0; in.mem_total = meminfo_kb(b, "MemTotal:") << 10; in.mem_avail = meminfo_kb(b, "MemAvailable:") << 10; } }
    if (override <= 0 && VKA_ENV("DM_VK_IMG_PSI", 1)) in.psi_some10 = psi_some10();   // (DM_VK_IMG_PSI=0: the RAM and budget rules only)
    uint32_t heap = V.local_type < V.memp.memoryTypeCount ? V.memp.memoryTypes[V.local_type].heapIndex : 0;
    if (heap < V.memp.memoryHeapCount) in.heap_size = V.memp.memoryHeaps[heap].size;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memory_properties = V.mem_budget
        ? (PFN_vkGetPhysicalDeviceMemoryProperties2)vk.vkGetInstanceProcAddr(V.inst, "vkGetPhysicalDeviceMemoryProperties2") : NULL;
    if (memory_properties) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        memory_properties(V.pd, &mp);
        in.heap_budget = bp.heapBudget[heap]; in.heap_usage = bp.heapUsage[heap];
    }
    uint64_t old = g_img_lim;
    g_img_lim = override > 0 ? (uint64_t)override << 20 : vktl_limit(&in);
    int pressed = override <= 0 && in.psi_some10 >= VKTL_PSI_HOLD;
    if (pressed != g_psi_pressed) {
        static _Atomic int np;
        if (atomic_fetch_add(&np, 1) < 32) VLOG("image cache: memory pressure %.1f %% (some avg10): %s, limit %llu MiB, images %.0f MiB", in.psi_some10 < 0 ? 0 : in.psi_some10,
                                                 pressed ? (in.psi_some10 >= VKTL_PSI_SHRINK ? "shrinking" : "holding") : "eased, the RAM rule again", (unsigned long long)(g_img_lim >> 20), in.img_bytes / 1048576.0);
        g_psi_pressed = pressed;
    }
    uint64_t diff = old > g_img_lim ? old - g_img_lim : g_img_lim - old;
    if (!old || diff >= 256 * VKTL_MIB)
        VLOG("image cache limit %llu MiB: live %.0f MiB, retired %.0f MiB, available %.0f MiB, heap budget %.0f MiB",
             (unsigned long long)(g_img_lim >> 20), (g_img_bytes + vka_imem_zbytes()) / 1048576.0,
             atomic_load(&g_retired_bytes) / 1048576.0, in.mem_avail / 1048576.0, in.heap_budget / 1048576.0);
}
typedef struct { vka_img *im; uint64_t last; } prune_ent;
static int prune_cmp(const void *a, const void *b) {
    const prune_ent *x = a, *y = b;
    return vktl_order(0, x->last, 0, y->last);
}
static void vka_tex_prune_pressure_locked(void);
static void vka_tex_prune_locked(void) {
    if (atomic_exchange(&g_dead_pools, 0)) {
        int removed = 0;
        for (int i = 0; i < g_nlive; ) {
            vka_img *im = g_live[i];
            if (img_pool_dead(im)) {
                int detached = img_detach_locked(im); removed += detached;
                if (i < g_nlive && g_live[i] != im) continue;
            }
            i++;
        }
        if (removed) VLOG("finalized pool cleanup: detached %d images; external holds/writebacks and GPU fences preserved", removed);
    }
    vka_tex_prune_pressure_locked();
}
// Called at prepare and before a new image allocation. A large single-frame streaming burst must not bypass the cache budget.
// Repeat a same-frame scan only after 16 MiB growth or one second; retained working-set images are not rescanned for every miss.
static uint64_t g_prune_dry = UINT64_MAX;   // the frame whose scan left the cache over its limit with nothing more to evict
static int tex_eager(void);   // ("Texture pool reads" below)
static void vka_tex_prune_pressure_locked(void) {
    uint64_t frame = vka_frame(), now = img_time_ns();
    // What the prune can give back: images already retired wait for their fence (g_retired_bytes) and go by themselves, so they are not counted here (they kept the
    // cache over its limit and every miss rescanned for nothing; upstream eden/v0.3.11 leaves them out too). The limit itself (img_limit_update) still sees them.
    // DM_VK_PRUNE_RETIRED=1 (live): counted, as before 2026-11-12
    const int with_retired = VKA_ENV("DM_VK_PRUNE_RETIRED", 0) != 0;
    uint64_t total = g_img_bytes + vka_imem_zbytes() + (with_retired ? atomic_load(&g_retired_bytes) : 0);
    // A scan of this frame that found nothing more to evict (dry, g_prune_dry): a miss later in the same frame finds nothing new either (what it could evict is unused
    // for 2 frames), so the 16 MiB growth trigger waits for the next frame; the 1 s trigger stays (a long loading stall in one frame, fences completing meanwhile).
    // DM_VK_PRUNE_DRY=0 (live): every 16 MiB of growth scans again, as before
    const int dry = frame == g_prune_dry && VKA_ENV("DM_VK_PRUNE_DRY", 1);
    if (!frame || (frame == g_prune_frame && (total < g_prune_bytes + 16 * VKTL_MIB || dry) && now - g_prune_ns < 1000000000ull)) return;
    g_prune_frame = frame; g_prune_bytes = total; g_prune_ns = now;
    img_limit_update(frame);
    if (total <= g_img_lim) return;
    static prune_ent *c; static int ccap;   // (kept: a scan a frame over the limit, thousands of images)
    if (ccap < g_nlive || !c) { int n = g_nlive + 256; prune_ent *p = realloc(c, (size_t)n * sizeof *c); if (!p) return; c = p; ccap = n; }
    int nc = 0; uint64_t done = vka_done();
    // A texture a shader reads only through the pool (a bindless handle from a record in a storage buffer: Ocarina of Time's lightmaps) is never bound by a draw,
    // so its last_frame does not move: evicting it rewrote its slot (redirty_ids), write_tex made it again at the next submission, and the cache evicted and
    // remade thousands of images a second (Turnip under 1 fps, a crash in Qualcomm's driver). With eager slots (tex_eager: DM_VK_POOL_FB=0 or DM_VK_EAGER_TEX=1) an
    // image a live pool slot names is in use: it stays, and a working set over the limit is kept (logged below). With the pool reads reported, a pool read stamps the
    // image used (pfb_hit) and nothing makes an image for a slot alone, so nothing is kept for its slot and nothing is scanned for it
    int eager = tex_eager(), pool_keep = eager, npool = 0;
    if (pool_keep)
        for (int id = 1; id < g_ntreg; id++) {
            const treg_ent *te = &g_treg[id];
            if (!te->t.serial || (te->t.pool && te->pgen && nvn_pool_live(te->t.pool, NULL, NULL) != te->pgen)) continue;   // (a finalized pool's slot has the null view: treg_dead)
            smap_ent *se = map_get(te->t.serial);
            if (se && se->im && se->im->pool_frame != frame) { se->im->pool_frame = frame; npool++; }
        }
    for (int i = 0; i < g_nlive; i++) {
        vka_img *im = g_live[i]; uint64_t last = atomic_load(&im->last_frame);
        if (pool_keep && im->pool_frame == frame) continue;
        // Never evict the only copy of live GPU-written content. Linear images may belong to the video decoder.
        if (im->gpu_owned || im->wb_pending || (im->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) continue;
        if (vktl_evictable(frame, last, im->holds, 0, 0, done < atomic_load(&im->last_fence))) c[nc++] = (prune_ent){im, last};
    }
    qsort(c, (size_t)nc, sizeof *c, prune_cmp);
    uint64_t target = vktl_target(g_img_lim), released = 0; int n = 0;
    // (without eager slots: an image a pool slot names may be read through the pool by a submission in flight that no stamp shows yet, and its slot keeps the view
    // until the next vka_tex_prepare writes the null descriptor there (redirty_ids, slot_write_keep), before any later submission records: it goes once every
    // submission recorded so far is done)
    uint64_t later = eager ? 0 : vka_fence_last() + 1;
    for (int i = 0; i < nc && total - released > target; i++) {
        vka_img *im = c[i].im; uint64_t bytes = im->bytes, at = atomic_load(&im->last_fence);
        // Eligibility above proves its last GPU use completed, with no current-frame commands or external holds.
        // Waiting for an unrelated future submission retains memory throughout loading/compilation stalls.
        if (at < later) at = later;
        if (img_detach_locked_at(im, at)) { released += bytes; n++; }
    }
    g_prune_bytes = g_img_bytes + vka_imem_zbytes() + (with_retired ? atomic_load(&g_retired_bytes) : 0);
    if (n) VLOG("image cache pruned: %d completed CPU-backed images detached, %.0f MiB reclaimed from cache; live %.0f MiB, pending %.0f MiB",
                n, released / 1048576.0, g_img_bytes / 1048576.0, atomic_load(&g_retired_bytes) / 1048576.0);
    if (g_prune_bytes > g_img_lim) {   // every candidate went and the cache is still over: dry for the rest of this frame (logged when the excess first appears and each 256 MiB more)
        g_prune_dry = frame;
        static uint64_t logged; static _Atomic int nl;
        if (g_prune_bytes >= logged + 256 * VKTL_MIB && atomic_fetch_add(&nl, 1) < 16) {
            logged = g_prune_bytes;
            VLOG("image cache over its limit: %.0f MiB of images, limit %.0f MiB; the rest was used in the last %d frames, is held, GPU-written%s, so it stays (evicting it would upload it again)",
                 g_prune_bytes / 1048576.0, g_img_lim / 1048576.0, VKTL_KEEP_FRAMES, pool_keep ? " or named by a texture pool slot (eager slots)" : "");
            if (pool_keep) VLOG("image cache over its limit: %d images named by live texture pool slots this frame", npool);
        }
    }
}

vka_img *vka_tex_get(const tex_t *t, VkCommandBuffer cb) {
    if (!t || !t->serial || !vka_ok()) return NULL;
    pthread_mutex_lock(&g_tmu);
    smap_ent *e = map_get(t->serial);
    if (e) { vka_img *im = e->im; img_touch(im); pthread_mutex_unlock(&g_tmu); return im; }
    vka_fmt f; vka_img *im = NULL; int throttle = 0;
    if (vka_fmt_of(t->d.format, &f)) {
        // (not an image whose pool was finalized: a new pool over the same memory has new content. Shared with a texture of another live pool, the image follows the newest one's)
        for (int i = 0; i < g_nlive && !im; i++) if (t->mem && same_shape(g_live[i], t, &f, 1) && !img_pool_dead(g_live[i]) && !(g_live[i]->base && sparse_gpu(t, &f))) {   // (drawn into at level 0)
            im = g_live[i]; im->refs++;
            if (t->pool && im->pool != t->pool) { uint64_t fin = nvn_pool_fin(), g = nvn_pool_live(t->pool, NULL, NULL); if (g) { im->pool = t->pool; im->pfin = fin; im->pgen = g; } } }
        if (im) big_share(im, t);   // (another texture object of the same storage and shape: DM_VK_BIGIMG_MB diagnostic)
        uint32_t vmin = 0, base = sparse_sampled(t, &f) ? sparse_base(t->pool, t->mem, &t->d, &t->L, &vmin) : 0;
        int mode = 0;
        if (!im) {
            vka_tex_prune_pressure_locked();
            // (a shader image, NVN_TEXTURE_FLAGS_IMAGE, is written by shaders: never zero-shared, as upstream; its size limit is vkz_candidate's)
            mode = first_mode(t, &f, cpuw_take(t->serial, 0)); g_zwant = mode == ST_ZERO && !(t->d.flags & NVN_TEXTURE_FLAGS_IMAGE); im = img_create(t, &f, base); g_zwant = 0;
        }
        if (im && !im->inited) {
            im->vmin = vmin;
            if (g_nlive == g_livecap) { g_livecap = g_livecap ? g_livecap * 2 : 256; g_live = realloc(g_live, (size_t)g_livecap * sizeof *g_live); }
            g_live[g_nlive++] = im;
            big_note(im, t);
            // Its first content through the upload batch when cb is the replay's (vka_dev.c "Upload batches"): staging that retires on the batch's own completion
            // (a depth / stencil image's zero clear into cb when no pass is open there: ds_here)
            VkCommandBuffer ucb = !ds_here(im, cb) && cb && cb == vka_rp_cb() && vka_up_on() ? vka_up_begin() : VK_NULL_HANDLE;
            img_first_use(im, t, ucb ? ucb : cb, mode);
            if (ucb) { vka_up_point(vka_up_cur(ucb)); vka_up_end(); }   // (an upload of many pieces, upload_mt, checks the batch budget only here)
            throttle = ucb != VK_NULL_HANDLE;
        }
    }
    img_touch(im);
    map_put(t->serial, im);
    pthread_mutex_unlock(&g_tmu);
    if (throttle) vka_up_throttle();
    return im;
}
vka_img *vka_tex_acquire(uint64_t serial) {
    pthread_mutex_lock(&g_tmu); smap_ent *e = map_get(serial); vka_img *im = e ? e->im : NULL; if (im) { im->refs++; im->holds++; } pthread_mutex_unlock(&g_tmu); return im;
}
// References to the live images (at most max), for the replayer's dumps; each dropped with vka_tex_drop. Returns how many were taken
int vka_tex_live_refs(vka_img **out, int max) {
    pthread_mutex_lock(&g_tmu); int n = 0;
    for (int i = 0; i < g_nlive && n < max; i++) { vka_img *im = g_live[i]; im->refs++; im->holds++; out[n++] = im; }
    pthread_mutex_unlock(&g_tmu); return n;
}
// (an image made by its holder, frame generation's outputs, starts with a reference and no hold: its last drop finds holds at 0)
void vka_tex_drop(vka_img *im) {
    if (!im) return;
    pthread_mutex_lock(&g_tmu);
    im_unhold_locked(im);
    unref_locked(im);
    pthread_mutex_unlock(&g_tmu);
}
vka_img *vka_tex_ref(vka_img *im) { if (im) { pthread_mutex_lock(&g_tmu); im->refs++; im->holds++; pthread_mutex_unlock(&g_tmu); } return im; }

VkImageView vka_view(vka_img *im, VkFormat fmt, VkImageViewType type, VkImageAspectFlags asp, uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers, const VkComponentMapping *swz) {
    VkComponentMapping id = {0}; if (!swz) swz = &id;
    pthread_mutex_lock(&g_tmu);
    int listed = fmt == im->f.vk || fmt == im->alt || im->any_fmt; for (int k = 0; k < im->nx; k++) listed |= fmt == im->xfmt[k];
    if (!listed) {   // a format outside the image's format list (img_create): its own format instead; the pair is learned for the images made from now on (Learned view formats)
        int learned = 0;   // (only uncompressed colour formats of one texel size: vl_learn checks the pair)
        if (im->f.kind == 0 && !compressed(&im->f)) { pthread_mutex_lock(&g_vlmu); learned = vl_learn(im->f.vk, fmt, im->w, im->h); pthread_mutex_unlock(&g_vlmu); }
        if (learned)
            VLOG("image VkFormat %d (NVN %#x, %ux%u) asked for a view as VkFormat %d, which its format list does not have: viewed in its own format; learned (images of this format and size made from now on, and in the next runs, take it)", im->f.vk, im->d.format, im->w, im->h, fmt);
        else { static struct { VkFormat a, b; } seen[32]; static int ns; int k = 0;
            while (k < ns && !(seen[k].a == im->f.vk && seen[k].b == fmt)) k++;
            if (k == ns && ns < 32) { seen[ns].a = im->f.vk; seen[ns++].b = fmt;
                VRED("image VkFormat %d (NVN %#x, %ux%u) asked for a view as VkFormat %d, which its format list does not have: viewed in its own format%s", im->f.vk, im->d.format, im->w, im->h, fmt,
                     VKA_ENV("DM_VK_VIEW_LEARN", 1) ? " (an image made before the pair was learned, or the table is full)" : " (DM_VK_VIEW_LEARN=0)"); } }
        fmt = im->f.vk;
    }
    // Moved to the front when found: an image shared by many textures (one storage, many views) has a long list, and the draws ask for the same few views again and again
    for (view_ent **pv = &VIEWS(im), *v; (v = *pv); pv = &v->next)
        if (v->f == fmt && v->t == type && v->asp == asp && v->l0 == level && v->nl == nlevels && v->a0 == layer && v->na == nlayers && !memcmp(&v->swz, swz, sizeof *swz)) {
            if (pv != &VIEWS(im)) { *pv = v->next; v->next = VIEWS(im); VIEWS(im) = v; }
            pthread_mutex_unlock(&g_tmu); return v->v; }
    // A view of an image with storage use in a format without storage support (an sRGB view; the image's own format when its storage use comes from a learned format,
    // img_create_): the view leaves storage out. A view in a format with storage support keeps it (an R32_UINT view bound as a storage image)
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = im->usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(feats(fmt) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? &vu : NULL, .image = im->img, .viewType = type, .format = fmt, .components = *swz, .subresourceRange = {asp, level, nlevels, layer, nlayers}};
    VkImageView view = VK_NULL_HANDLE; VkResult r = vk.vkCreateImageView(V.dev, &ci, NULL, &view);
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("image view of serial %llu format %d type %d levels %u+%u layers %u+%u: %d", (unsigned long long)im->serial, fmt, type, level, nlevels, layer, nlayers, r); }
    else { view_ent *v = malloc(sizeof *v); *v = (view_ent){VIEWS(im), view, fmt, type, asp, level, nlevels, layer, nlayers, *swz}; VIEWS(im) = v; }
    pthread_mutex_unlock(&g_tmu);
    return view;
}

// Guest storage → image: every level and layer deswizzled (block-linear) or copied by rows (LINEAR, pitch d.stride) into scratch memory, then one copy per piece
static void upload_(vka_img *im, const tex_t *t, VkCommandBuffer cb, uint64_t *nbytes);
// Large block-linear uploads deswizzled on several threads (DM_VK_UPLOAD_MT, live: helper threads, default 3, 0 = on the calling thread only): a texture's pieces (each level
// and layer) are cut into runs of block rows (nvn_deswizzle_rows, at least ~64 KB each) that the helpers and the calling thread take in turn; the copies are recorded after
// all of them, in the same order as before. Only uploads of at least DM_VK_UPLOAD_MT_MIN bytes (default 256 KB): the first uploads of a level's textures came in loading
// frames, 1-93 textures a frame, all deswizzled on the replay thread one after another
typedef struct { const nvn_texdesc *d; const uint8_t *src; uint8_t *dst; size_t pitch; uint32_t lv, ly, y0, y1; _Atomic int *bad; } up_part;   // bad: its piece's flag
// A part is claimed by compare-and-swap on next while next < n: a helper that wakes late, or comes back from the last batch, can only claim a part of the batch being set up
// once it is complete (p, then next = 0, then n; n = 0 between batches)
static struct { pthread_mutex_t mu; pthread_cond_t cv, done_cv; _Atomic(up_part *) p; _Atomic uint32_t n, next, done; _Atomic int fail; uint64_t gen; int nth; } UP = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER};
static pthread_mutex_t g_up_one = PTHREAD_MUTEX_INITIALIZER;   // one parallel upload at a time (another thread's upload runs on its own thread)
static void up_run(void) {   // take parts until none is left
    for (;;) {
        uint32_t i = atomic_load(&UP.next), n = atomic_load(&UP.n);
        if (i >= n) return;
        if (!atomic_compare_exchange_weak(&UP.next, &i, i + 1)) continue;
        up_part *q = &atomic_load(&UP.p)[i];
        if (nvn_deswizzle_rows(q->d, q->lv, q->ly, q->src, q->dst, q->pitch, q->y0, q->y1)) { atomic_store(&UP.fail, 1); atomic_store(q->bad, 1); }
        if (atomic_fetch_add(&UP.done, 1) + 1 == n) { pthread_mutex_lock(&UP.mu); pthread_cond_broadcast(&UP.done_cv); pthread_mutex_unlock(&UP.mu); }
    }
}
static void *up_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "vk-upload"); vka_host_fp("vk-upload");
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&UP.mu); while (UP.gen == seen) pthread_cond_wait(&UP.cv, &UP.mu); seen = UP.gen; pthread_mutex_unlock(&UP.mu);
        up_run();
    }
    return NULL;
}
// 1 = done in parallel (fail: a piece could not be deswizzled); 0 = not here (the caller does it piece by piece)
static int up_parallel(up_part *p, uint32_t n, int *fail) {
    int want = VKA_ENV("DM_VK_UPLOAD_MT", 3); if (want <= 0 || n < 2) return 0;
    if (want > 6) want = 6;
    if (pthread_mutex_trylock(&g_up_one)) return 0;
    while (UP.nth < want) { pthread_t th; if (pthread_create(&th, NULL, up_main, NULL)) break; pthread_detach(th); UP.nth++; }
    if (!UP.nth) { pthread_mutex_unlock(&g_up_one); return 0; }
    pthread_mutex_lock(&UP.mu);
    atomic_store(&UP.fail, 0); atomic_store(&UP.done, 0); atomic_store(&UP.p, p); atomic_store(&UP.next, 0); atomic_store(&UP.n, n); UP.gen++;
    pthread_cond_broadcast(&UP.cv);
    pthread_mutex_unlock(&UP.mu);
    up_run();   // this thread takes parts too
    pthread_mutex_lock(&UP.mu); while (atomic_load(&UP.done) < n) pthread_cond_wait(&UP.done_cv, &UP.mu); atomic_store(&UP.n, 0); pthread_mutex_unlock(&UP.mu);
    *fail = atomic_load(&UP.fail);
    pthread_mutex_unlock(&g_up_one);
    return 1;
}
// The block-linear pieces of a texture of at least DM_VK_UPLOAD_MT_MIN bytes, deswizzled in parallel into their scratch spans and copied; 0 = not taken (the serial path does it)
static int upload_mt(vka_img *im, const tex_t *t, VkCommandBuffer cb, uint64_t *nbytes) {
    const nvn_texdesc *d = &t->d; const vka_fmt *f = &im->f;
    int is3d = d->target == NVN_TEXTURE_TARGET_3D;
    if (vka_nan_on() || (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || f->kind >= 3) return 0;
    uint64_t total = 0; uint32_t npieces = im->levels * im->layers;
    for (uint32_t lv = 0; lv < im->levels; lv++) {
        uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, z = is3d ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
        total += (uint64_t)((w + f->bw - 1) / f->bw) * f->bpb * ((h + f->bh - 1) / f->bh) * z * im->layers;
    }
    int minb = VKA_ENV("DM_VK_UPLOAD_MT_MIN", 256 << 10);
    if (total < (uint64_t)(minb > 0 ? minb : 1) || npieces > 4096 || VKA_ENV("DM_VK_UPLOAD_MT", 3) <= 0) return 0;
    typedef struct { vka_span s; uint32_t lv, ly, w, h, z, bx, by; size_t pitch; _Atomic int bad; } piece_t;
    piece_t *pc = malloc(npieces * sizeof *pc); up_part *parts = NULL; uint32_t np = 0, cap = 0;
    if (!pc) return 0;
    uint32_t k = 0;
    for (uint32_t lv = 0; lv < im->levels; lv++) {
        uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, z = is3d ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
        uint32_t bx = (w + f->bw - 1) / f->bw, by = (h + f->bh - 1) / f->bh; size_t pitch = (size_t)bx * f->bpb, piece = pitch * by * z;
        int rows = nvn_deswizzle_block_rows(d, lv);
        if (rows < 0 || (uint32_t)rows != by) { free(pc); free(parts); return 0; }   // (a shape the serial path reports)
        for (uint32_t ly = 0; ly < im->layers; ly++, k++) {
            piece_t *q = &pc[k]; *q = (piece_t){{0}, lv, ly, w, h, z, bx, by, pitch, 0};
            if (f->dec) { q->s = (vka_span){0}; if (!(q->s.ptr = malloc(piece))) goto cut; }   // (decoded from heap memory: bc_copy)
            else if (!vka_scratch(piece, 16, &q->s)) goto cut;   // (no scratch: as the serial path, the pieces before it are uploaded)
            // Runs of block rows of at least ~64 KB (all slices of a 3D level in each)
            uint32_t per = (uint32_t)((65536 + pitch * z - 1) / (pitch * z)); if (!per) per = 1;
            for (uint32_t y = 0; y < by; y += per) {
                if (np == cap) { cap = cap ? cap * 2 : 64; up_part *n2 = realloc(parts, cap * sizeof *parts); if (!n2) { free(pc); free(parts); return 0; } parts = n2; }
                parts[np++] = (up_part){d, t->mem, q->s.ptr, pitch, lv, ly, y, y + per < by ? y + per : by, &q->bad};
            }
        }
    }
cut:;
    int fail = 0;
    if (!up_parallel(parts, np, &fail)) { for (uint32_t i = 0; i < np; i++) if (nvn_deswizzle_rows(parts[i].d, parts[i].lv, parts[i].ly, parts[i].src, parts[i].dst, parts[i].pitch, parts[i].y0, parts[i].y1)) { fail = 1; *parts[i].bad = 1; } }
    free(parts);
    // A piece that could not be deswizzled ends the upload there, as on the serial path: the pieces before it are copied, it and those after it are not
    if (fail) VRED_ONCE("texture serial %llu format %#x target %d: no CPU deswizzle", (unsigned long long)t->serial, d->format, d->target);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < k; i++) {
        piece_t *q = &pc[i];
        if (atomic_load(&q->bad)) break;
        VkBufferImageCopy c = {q->s.off, q->bx * f->bw, q->by * f->bh, {im->aspect, q->lv, q->ly, 1}, {0, 0, 0}, {q->w, q->h, q->z}};
        if (f->dec) bc_copy(cb, im, q->s.ptr, q->pitch, q->bx, q->by, q->z, q->lv, q->ly, 0, 0, 0, q->w, q->h);
        else vk.vkCmdCopyBufferToImage(cb, q->s.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        bytes += q->pitch * q->by * q->z;
    }
    if (f->dec) for (uint32_t i = 0; i < k; i++) free(pc[i].s.ptr);
    free(pc);
    *nbytes = bytes;
    return 1;
}
static int img_own(vka_img *im, VkCommandBuffer cb);
static void upload(vka_img *im, const tex_t *t, VkCommandBuffer cb) {   // (callers hold g_tmu)
    // Dirty tracking can re-upload an image that no draw has used for many frames. The encoder may still have
    // its copy queued: refresh lifetime before recording, so allocation-time pruning cannot destroy its VkImage.
    img_touch(im);
    if (im->m.z) img_own(im, cb);   // a CPU write into a zero-shared image's storage (vka_tex_prepare): memory of its own before the upload
    uint64_t t0 = now_ns(), nb = 0; upload_(im, t, cb, &nb);
    atomic_fetch_add_explicit(&g_up_ns, now_ns() - t0, memory_order_relaxed); atomic_fetch_add_explicit(&g_up_bytes, nb, memory_order_relaxed);
}
static void upload_(vka_img *im, const tex_t *t, VkCommandBuffer cb, uint64_t *nbytes) {
    if (!im->vpool && upload_mt(im, t, cb, nbytes)) return;   // (a sparse pool's storage is read through its page table below, never at its address)
    const nvn_texdesc *d = &t->d; const vka_fmt *f = &im->f;
    int linear = d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET), is3d = d->target == NVN_TEXTURE_TARGET_3D;
    if (f->kind >= 3) { VRED_ONCE("depth / stencil textures are not uploaded from guest memory yet (format %#x)", d->format); return; }
    if (linear && (im->levels > 1 || im->layers > 1 || is3d)) { VRED_ONCE("linear texture with levels or layers (format %#x %ux%u): only level 0 uploaded", d->format, d->width, d->height); }
    uint64_t bytes = 0;
    // A sparse pool's texture: gathered from its page table by bands, unmapped ones as zero texels (until 2026-11-03 a copy of its whole range was made first)
    if (im->vpool) { if (!im->vbind) upload_sparse_parts(im, cb, 0, 0, 1, 0); return; }   // (a GPU-written one's content is the GPU's: sparse_bind)
    uint8_t *part = NULL; const uint8_t *src = t->mem;
    for (uint32_t lv = 0; lv < (linear ? 1 : im->levels); lv++) {
        uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1, z = is3d ? (im->depth >> lv ? im->depth >> lv : 1) : 1;
        uint32_t bx = (w + f->bw - 1) / f->bw, by = (h + f->bh - 1) / f->bh;
        size_t pitch = (size_t)bx * f->bpb, piece = pitch * by * z;
        for (uint32_t ly = 0; ly < (linear ? 1 : im->layers); ly++) {
            vka_span s = {0};
            if (f->dec) { if (!(s.ptr = malloc(piece))) { free(part); return; } }   // (decoded from heap memory: bc_copy)
            else if (!vka_scratch(piece, 16, &s)) { free(part); return; }
            if (linear) { size_t st = d->stride ? d->stride : pitch; for (uint32_t y = 0; y < by; y++) memcpy(s.ptr + y * pitch, src + y * st, pitch); }
            else if (nvn_deswizzle(d, lv, ly, src, s.ptr, pitch)) { VRED_ONCE("texture serial %llu format %#x target %d: no CPU deswizzle", (unsigned long long)t->serial, d->format, d->target); if (f->dec) free(s.ptr); free(part); return; }
            if (d->format >= 0x79 && d->format <= 0x94 && vka_nan_on()) {   // DM_VK_NANSCAN: all-zero blocks of ASTC textures (an all-zero ASTC block is an error block)
                uint64_t zb = 0, nb = piece / 16; for (uint64_t k = 0; k < nb; k++) if (!mem_any(s.ptr + 16 * k, 16)) zb++;
                const char *dm = getenv("DM_VK_NANSCAN_DUMP"); static _Atomic int nd;   // <format>x<w>: level 0 of such textures written to DM_TEMP (diagnostic, at most 64)
                if (dm && lv == 0 && ly == 0 && (int)strtol(dm, NULL, 16) == d->format && (uint32_t)atoi(strchr(dm, 'x') ? strchr(dm, 'x') + 1 : "0") == im->w && atomic_fetch_add(&nd, 1) < 64) {
                    char fn[512]; snprintf(fn, sizeof fn, "%s/nan-%llu-%#x-%ux%u.blk", getenv("DM_TEMP") ? getenv("DM_TEMP") : "/data/local/tmp", (unsigned long long)t->serial, d->format, w, h);
                    FILE *fo = fopen(fn, "wb"); if (fo) { fwrite(s.ptr, 1, piece, fo); fclose(fo); VLOG("NaN scan: level 0 of serial %llu written to %s", (unsigned long long)t->serial, fn); }
                }
                static _Atomic int nz; if (zb && atomic_fetch_add(&nz, 1) < 64) VLOG("NaN scan: upload of serial %llu format %#x level %u layer %u: %llu of %llu blocks all zero (storage %p)", (unsigned long long)t->serial, d->format, lv, ly, (unsigned long long)zb, (unsigned long long)nb, (const void *)t->mem);
            }
            VkBufferImageCopy c = {s.off, bx * f->bw, by * f->bh, {im->aspect, lv, ly, 1}, {0, 0, 0}, {w, h, z}};
            if (f->dec) { bc_copy(cb, im, s.ptr, pitch, bx, by, z, lv, ly, 0, 0, 0, w, h); free(s.ptr); }
            else vk.vkCmdCopyBufferToImage(cb, s.buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            bytes += piece;
            if (!(cb = vka_up_point(cb))) { free(part); *nbytes = bytes; return; }
        }
    }
    free(part);
    *nbytes = bytes;
    static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VLOG("texture serial %llu %ux%u format %#x: %llu bytes uploaded from guest memory", (unsigned long long)t->serial, im->w, im->h, d->format, (unsigned long long)bytes);
}

// The object each serial was initialized in (vka_texown.h): a Finalize of a copy of a texture object whose original lives on ends nothing
static nvn_hmap g_tex_own;   // serial -> the texture object nvnTextureInitialize ran on (g_tmu)
static int tex_obj_holds(uintptr_t obj, uint64_t serial) {   // read through the guest's mappings: the game may have freed and unmapped the object
    tex_t seen; dm_guest_read_mapped(&seen, obj, sizeof seen);
    return seen.magic == MAGIC('T', 'E', 'X', 0) && seen.serial == serial;
}
void mtl_tex_init(const tex_t *t) { if (!t || !t->serial) return; pthread_mutex_lock(&g_tmu); vto_own(&g_tex_own, t->serial, t); pthread_mutex_unlock(&g_tmu); }
void mtl_tex_release(tex_t *t) {
    pthread_mutex_lock(&g_tmu);
    if (!vto_release(&g_tex_own, t->serial, t, tex_obj_holds)) {
        static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
        if (k <= 4 || !(k & (k - 1))) VLOG("texture serial %llu: Finalize of a copy of its texture object, the original still holds it: its image stays (%llu so far)", (unsigned long long)t->serial, (unsigned long long)k);
        pthread_mutex_unlock(&g_tmu); return;
    }
    smap_ent *e = map_get(t->serial); vka_img *im = e ? e->im : NULL;
    if (e) map_del(t->serial);
    cpuw_take(t->serial, 0);
    if (im) unref_locked(im);
    pthread_mutex_unlock(&g_tmu);
}
// Linear textures the GPU wrote in this submission. Metal makes a linear texture on the pool buffer, so what the GPU renders there is in guest memory for the CPU to read
// (agl reads small linear render targets back on the CPU, such as a 1x1 R16F one in Donkey Kong Bananza's level); here the image is copied back into the storage
static vka_img **g_wb; static int g_nwb, g_wbcap;
// A linear texture whose GPU writes are copied back into its storage (vka_tex_writeback): one level-free, single-sample colour texture of plain texels
static int img_wb_kind(const vka_img *im) { return (im->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && im->mem && im->samples == 1 && im->f.kind < 3 && im->f.bw == 1; }
static int lin_room(void); static int is_linear(const vka_img *im);   // (Linear textures' diagnostics, below)
void vka_img_written_at(vka_img *im, const char *file, int line) {
    if (!im) return;
    pthread_mutex_lock(&g_tmu);
    // A zero-shared image must have memory of its own before any write (vka_tex_own at the write's site): this one did not, so every image sharing its memory sees the write
    if (im->m.z) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("zero-shared image serial %llu %ux%u format %#x written at %s:%d without memory of its own (a write path without vka_tex_own): the images that share its memory see the write",
                                                                            (unsigned long long)im->serial, im->w, im->h, im->d.format, file, line); }
    if (!im->gpu_owned && im->m.size >= big_min()) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 64)   // large images: the first GPU write (DM_VK_BIGIMG_MB)
        VLOG("large image serial %llu %ux%u layers %u format %#x (%.1f MiB): first GPU write at frame %llu (%s:%d), sampled by %u draws before, %u other textures over its storage", (unsigned long long)im->serial,
             im->w, im->h, im->layers, im->d.format, im->m.size / 1048576.0, (unsigned long long)vka_frame(), file, line, im->nsamp, im->novl); }
    if (!im->gpu_owned && is_linear(im) && !(im->lin_seen & 4)) {   // (a linear texture's first GPU write ends the following of its storage: where it came from, once; vka_tex.c lin_note_sample)
        im->lin_seen |= 4;
        if (lin_room()) VLOG("linear texture serial %llu %ux%u format %#x flags %#x: first GPU write at frame %llu (%s:%d), %s: from now on its image is the GPU's content and the CPU's writes into its storage are not uploaded",
                             (unsigned long long)im->serial, im->w, im->h, im->d.format, im->d.flags, (unsigned long long)vka_frame(), file, line, im->track ? "it was tracked" : "it was not tracked");
    }
    im->gpu_owned = 1;
    img_touch(im);
    if (img_wb_kind(im) && !im->wb_pending) {
        if (g_nwb == g_wbcap) { g_wbcap = g_wbcap ? 2 * g_wbcap : 32; g_wb = realloc(g_wb, (size_t)g_wbcap * sizeof *g_wb); }
        im->wb_pending = 1; im->refs++; g_wb[g_nwb++] = im;
    }
    pthread_mutex_unlock(&g_tmu);
}
void vka_tex_writeback(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_tmu);
    int any = 0;
    for (int i = 0; i < g_nwb; i++) {
        vka_img *im = g_wb[i]; im->wb_pending = 0;
        uint32_t bpb = im->f.bpb, h = im->h; size_t row = im->d.stride ? im->d.stride : (size_t)im->w * bpb;
        VkBuffer b; VkDeviceSize o;
        if (row % bpb || !vka_addr((uint64_t)(uintptr_t)im->mem, (uint64_t)row * (h - 1) + (uint64_t)im->w * bpb, &b, &o, NULL) || o % (bpb % 4 ? 4 * bpb : bpb)) {
            VRED_ONCE("linear texture serial %llu (format %#x, row %zu) cannot be copied back into its storage: the CPU reads what it held", (unsigned long long)im->serial, im->d.format, row); unref_locked(im); continue; }
        if (!any) { vka_barrier(cb); any = 1; }
        VkBufferImageCopy c = {o, (uint32_t)(row / bpb), h, {im->aspect, 0, 0, 1}, {0, 0, 0}, {im->w, h, 1}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
        im->wb_fence = vka_fence_last();   // (the submission this copy is recorded in, as vka_img_written's last_fence: vka_wbfollow.h)
        static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VLOG("linear texture serial %llu %ux%u format %#x: the GPU's content copied back into its storage", (unsigned long long)im->serial, im->w, h, im->d.format);
        unref_locked(im);
    }
    g_nwb = 0;
    pthread_mutex_unlock(&g_tmu);
}
int mtl_tex_gpu_owned(uint64_t serial) {   // hle_nvn.c nvn_ft_poll: the GPU has written the image (render target, copy or clear destination)
    pthread_mutex_lock(&g_tmu); smap_ent *e = map_get(serial); int r = e && e->im && e->im->gpu_owned; pthread_mutex_unlock(&g_tmu); return r;
}
unsigned mtl_cpu_tex_count(void) {   // live images whose content follows their storage
    pthread_mutex_lock(&g_tmu); unsigned n = 0; for (int i = 0; i < g_nlive; i++) n += g_live[i]->track && !g_live[i]->gpu_owned; pthread_mutex_unlock(&g_tmu); return n;
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { if (uploads) *uploads = g_st_up[ST_TRACK] + g_st_up[ST_FILL] + g_st_up[ST_SPARSE]; if (reuploads) *reuploads = g_reup; }
// The CPU wrote the texture's storage: uploaded at its first use, or again at the next submission unless the GPU has written it since (then the GPU's content wins)
static uint64_t *g_redo; static int g_nredo, g_redocap;
void mtl_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&g_tmu);
    smap_ent *e = map_get(t->serial);
    if (!e) cpuw_take(t->serial, 1);
    else if (e->im && !e->im->gpu_owned && !e->im->cpu_dirty) {
        e->im->cpu_dirty = 1;
        if (g_nredo == g_redocap) { g_redocap = g_redocap ? g_redocap * 2 : 64; g_redo = realloc(g_redo, (size_t)g_redocap * sizeof *g_redo); }
        g_redo[g_nredo++] = t->serial;
    }
    pthread_mutex_unlock(&g_tmu);
}

// ---------- Descriptor tables ----------
// One update-after-bind set: binding 0 the texture pool (sampled images, up to 2^20), 1 the sampler pool (up to 4096), 2 images (storage). A registration writes its slot at the next
// submission. Nothing draws yet, so no command buffer holds the set while it is written
static samp_par *g_sreg; static int g_nsreg;
// NVN's reserved sampler of pool slot 0 (vka.h VKA_SAMP_ZERO): linear min / mag, no mipmaps, clamp to edge on all axes, LOD 0..15
static const samp_par g_samp_zero = {.minf = NVN_MIN_FILTER_LINEAR, .magf = NVN_MAG_FILTER_LINEAR, .ws = NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt = NVN_WRAP_MODE_CLAMP_TO_EDGE,
                                     .wr = NVN_WRAP_MODE_CLAMP_TO_EDGE, .lod_max = 15.0f};
static int *g_sdirty; static int g_nsd, g_sdcap;
static uint8_t *g_sdflag;
static VkDescriptorSetLayout g_dsl; static VkDescriptorPool g_dpool; static VkDescriptorSet g_set;
static uint32_t g_ntex_slots, g_nsamp_slots, g_nimg_slots;
typedef struct { samp_par p; int iv; VkSampler s; } samp_ent; static samp_ent *g_samp; static int g_nsamp;
static VkSampler (*g_sres)[2]; static int g_nsres;   // the sampler of each registered ID ([1]: its integer view twin), found once per registration (sampler_of searches every distinct sampler)
// The view a slot with no image gets: VK_NULL_HANDLE, the null descriptor (robustness2 nullDescriptor, vka_dev.c V.null_desc; a read returns zero). vka_tex_prepare writes
// it only with the pool reads reported (slot_write_keep); otherwise such a write is dropped, as before (the slot keeps what it held)
static VkImageView g_null_view;

// ---------- Texture pool reads (DM_VK_POOL_FB) ----------
// Ported from Banana Native v0.2.9 / v0.3.11 (eden/v0.3.11 vka_tex.c "Texture pool reads"; docs/reference/rendering.md there: "Textures read only through the pool were
// never made" and "Texture pool reads report nothing on Qualcomm's driver"). A shader whose texture handle the translator could not trace to a slot reads the pool (set 1
// binding 0) at an ID the host never sees: agl / gsys lit materials read their lightmaps from handles in material records in a storage buffer (Ocarina of Time's walls in
// Link's house). Until this port write_tex made an image for every registered texture and the prune kept every image a live slot names: at Link's house door 6,022 images,
// 3,436 MiB (POCO F7 Ultra, Qualcomm 891.14), the state upstream measured at 3.0-3.7 GiB against about 1.6 GiB with this feedback, and lmkd kills on 12-16 GB phones.
// Here the GPU reports the IDs it reads: every library module made carries vka_spv_pool_fb's code (vka_shader.c hook), which sets bit id of a 2^20-bit bitmap (set
// VKA_POOL_FB_SET binding VKA_POOL_FB_BINDING, a set of its own) before each pool read. Each submission with a draw or dispatch that reads the pool (vka_draw.c bind_sets:
// draws, dispatches and the task / mesh fallback's compute stages and pulling draws alike) ends with the bitmap's first ceil(registered IDs / 8) bytes copied into a
// host-visible ring slot and then, behind a barrier, the bitmap cleared (vka_tex_pfb_commit); the next vka_tex_prepare after that submission's fence takes the slot
// (pfb_consume_locked). An ID read whose slot holds the null view is written again at this submission with its image made (PFB_WANT: write_tex makes it, as a draw's first
// use would); an ID read whose slot names its image stamps that image used (last_frame, last_fence), so the image cache's LRU prune treats pool reads as uses. An image no
// longer read goes as any other (its slot gets the null view; a later read makes it again, one frame of the null view). Such an eviction waits for every submission
// recorded so far (the slot may be read by one in flight, which no stamp shows), not only for the image's own last use. Memory: the bitmap 128 KiB and the ring
// 8 x 128 KiB of host-visible memory, a byte per ID.
// DM_VK_POOL_FB (restart): 1 (default) on, 0 off: the eager slots of before (every registered texture made, every image a live slot names kept), two descriptor sets a
// pipeline, no module rewritten. DM_VK_EAGER_TEX (restart): unset = eager exactly when the feedback is off; 1 = eager with the feedback on too (upstream's 0.2.8), 0 = never.
// DM_VK_POOL_FB_EARLY=0 (restart): no pixel module gets early depth tests. DM_VK_POOL_FB_BLIND_EAGER (default 1): a library module whose pool reads the rewrite refuses
// (vka_tex_pfb_blind) puts every slot back to eager for the rest of the run, so a texture only that module reads is still made (memory as before the port, logged red);
// 0: upstream's behaviour (that texture reads 0). DM_VK_POOL_FB_DEAD (default 600): that many readbacks with not one bit set do the same (pfb_dead_check_locked: a
// driver that drops the bitmap's stores). Needs null descriptors (the slot of an evicted or unmade image gets one: slot_write_keep), fragmentStoresAndAtomics and update-after-bind storage buffers beyond set 0's (a vertex-pipeline module also
// vertexPipelineStoresAndAtomics, else it is refused). Idea and bitmap layout after Anka Native's texture-use feedback (MIT, copyright 2026 seedds and 2026 EddizEge)
enum { PFB_BYTES = 1 << 17, PFB_RING = 8, PFB_WANT = 1, PFB_NULL = 2, PFB_MADE = 4 };
static _Atomic int g_pfb = -1;   // -1 not decided; 0 off; 1 on (the bitmap and the ring made)
static _Atomic int g_pfb_eager = -1;   // write_tex makes every slot's image and the prune keeps what live slots name (decided with g_pfb)
static _Atomic int g_pfb_blind_eager, g_pfb_blind_new;   // a module's pool reads report nothing: eager from then on; new: every slot to be written again
static vka_span g_pfb_map, g_pfb_ring;
static VkDescriptorSetLayout g_pfb_dsl; static VkDescriptorPool g_pfb_dpool; static VkDescriptorSet g_pfb_set;   // the bitmap's set (made in pfb_avail)
static struct { uint64_t fence; uint32_t n; int busy; } g_pfb_r[PFB_RING];   // (under g_tmu)
static _Atomic int g_pfb_any;   // the submission being recorded has a draw or dispatch that reads the pool
static uint8_t *g_pfb_st;   // (under g_tmu) per texture ID: PFB_WANT, PFB_NULL (its slot was last written with the null view), PFB_MADE
static uint64_t g_pfb_nrb, g_pfb_skip, g_pfb_hits, g_pfb_wants, g_pfb_made, g_pfb_made_b, g_pfb_again, g_pfb_logged;   // (under g_tmu)
static int pfb_avail(void) {
    int v = atomic_load(&g_pfb);
    if (v >= 0) return v;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if ((v = atomic_load(&g_pfb)) >= 0) { pthread_mutex_unlock(&mu); return v; }
    int want = VKA_ENV("DM_VK_POOL_FB", 1) != 0;
    v = want && V.dev && V.uab;
    if (v && !V.fstores) { VRED("texture pool reads: the device has no fragmentStoresAndAtomics: pool reads are not reported (every registered texture is made, as before)"); v = 0; }
    // An evicted image's slot must stop naming it before the image goes (vka_tex_prune_pressure_locked), and a slot whose image is not made must hold a valid descriptor
    // for a pool read: both take a write of the null descriptor (slot_write_keep), which needs robustness2's nullDescriptor
    if (v && !V.null_desc) { VRED("texture pool reads: the device has no null descriptors (robustness2 nullDescriptor): pool reads are not reported (every registered texture is made, as before)"); v = 0; }
    if (v) {
        VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12}; vk.vkGetPhysicalDeviceProperties2(V.pd, &p2);
        if (p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers <= DMSH_VK_STAGE_BINDINGS || p12.maxDescriptorSetUpdateAfterBindStorageBuffers <= DMSH_VK_STAGE_BINDINGS) {
            VRED("texture pool reads: %u / %u update-after-bind storage buffers a stage / a layout leave no room beside set 0's: pool reads are not reported",
                 p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers, p12.maxDescriptorSetUpdateAfterBindStorageBuffers); v = 0; }
        else if (V.props.limits.maxBoundDescriptorSets <= VKA_POOL_FB_SET) { VRED("texture pool reads: %u descriptor sets a pipeline: no room for the bitmap's set; pool reads are not reported", V.props.limits.maxBoundDescriptorSets); v = 0; }
    }
    if (v) {
        VkDeviceSize al = V.sb_align > 256 ? V.sb_align : 256;
        uint8_t *st = calloc(1u << 20, 1);
        if (!st || !vka_static(PFB_BYTES, al, &g_pfb_map) || !vka_static((VkDeviceSize)PFB_BYTES * PFB_RING, al, &g_pfb_ring)) { free(st); VRED("texture pool reads: no memory for the bitmap: not reported"); v = 0; }
        else {
            memset(g_pfb_map.ptr, 0, PFB_BYTES); memset(g_pfb_ring.ptr, 0, (size_t)PFB_BYTES * PFB_RING); g_pfb_st = st;
            // The bitmap's set (set VKA_POOL_FB_SET of every pipeline layout), written once: made here, at device init, before any module or pipeline layout
            VkDescriptorSetLayoutBinding fb = {VKA_POOL_FB_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
            VkDescriptorSetLayoutCreateInfo fli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &fb};
            VkDescriptorPoolSize fps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
            VkDescriptorPoolCreateInfo fpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &fps};
            VkDescriptorSetAllocateInfo fai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &g_pfb_dsl};
            VkResult r;
            if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &fli, NULL, &g_pfb_dsl)) || (r = vk.vkCreateDescriptorPool(V.dev, &fpi, NULL, &g_pfb_dpool)) ||
                ((fai.descriptorPool = g_pfb_dpool), (r = vk.vkAllocateDescriptorSets(V.dev, &fai, &g_pfb_set)))) {
                VRED("texture pool reads: the bitmap's descriptor set: %d; pool reads are not reported", r); g_pfb_set = VK_NULL_HANDLE; v = 0;
            } else {
                VkDescriptorBufferInfo bi = {g_pfb_map.buf, g_pfb_map.off, PFB_BYTES};
                VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_pfb_set, .dstBinding = VKA_POOL_FB_BINDING, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
                vk.vkUpdateDescriptorSets(V.dev, 1, &w, 0, NULL);
            }
        }
    }
    int e = VKA_ENV("DM_VK_EAGER_TEX", -1);
    atomic_store(&g_pfb_eager, e >= 0 ? e != 0 : !v);
    if (v) VLOG("texture pool reads: the GPU reports the texture IDs shaders read through the pool (a bitmap in descriptor set %d); their images are made when read and kept while read%s (DM_VK_POOL_FB=0 off)",
                VKA_POOL_FB_SET, atomic_load(&g_pfb_eager) ? "; every registered texture is made too (DM_VK_EAGER_TEX=1)" : "");
    else VLOG("texture pool reads: not reported (%s): every registered texture's image is made and kept while its pool lives, as before", want ? "the device cannot" : "DM_VK_POOL_FB=0");
    atomic_store(&g_pfb, v);
    pthread_mutex_unlock(&mu);
    return v;
}
static int tex_eager(void) {   // write_tex makes every registered slot's image, and the prune keeps every image a live slot names (the state before the pool reads)
    if (atomic_load_explicit(&g_pfb_eager, memory_order_acquire) < 0) (void)pfb_avail();
    return atomic_load_explicit(&g_pfb_eager, memory_order_relaxed) > 0 || atomic_load_explicit(&g_pfb_blind_eager, memory_order_relaxed);
}
unsigned vka_tex_pfb_flags(void) {
    if (!pfb_avail()) return 0;
    return (V.vstores ? VKA_PFB_VTX_STORES : 0u) | (VKA_ENV("DM_VK_POOL_FB_EARLY", 1) ? VKA_PFB_EARLY : 0u);
}
void vka_tex_pfb_blind(void) {
    if (atomic_load(&g_pfb) <= 0 || !VKA_ENV("DM_VK_POOL_FB_BLIND_EAGER", 1) || atomic_exchange(&g_pfb_blind_eager, 1)) return;
    atomic_store(&g_pfb_blind_new, 1);
    VRED("texture pool reads: a library module's pool reads cannot be reported: every registered texture's image is made and kept again from now on (as before the feedback; "
         "memory as then). DM_VK_POOL_FB_BLIND_EAGER=0 keeps the feedback alone (a texture only that module reads through the pool then reads 0)");
}
void vka_tex_pfb_draw(void) { if (atomic_load_explicit(&g_pfb, memory_order_relaxed) > 0) atomic_store_explicit(&g_pfb_any, 1, memory_order_relaxed); }
void vka_tex_pfb_commit(VkCommandBuffer cb, uint64_t fence) {
    if (!cb || atomic_load(&g_pfb) <= 0 || !atomic_exchange(&g_pfb_any, 0)) return;
    pthread_mutex_lock(&g_tmu);
    uint32_t n = (uint32_t)(((uint64_t)g_ntreg + 31) / 32 * 4), k = PFB_RING; if (n > PFB_BYTES) n = PFB_BYTES;
    for (uint32_t i = 0; i < PFB_RING && k == PFB_RING; i++) if (!g_pfb_r[i].busy) k = i;
    if (!n || k == PFB_RING) { g_pfb_skip++; atomic_store(&g_pfb_any, 1); pthread_mutex_unlock(&g_tmu); return; }   // (every slot waiting: the bits stay for the next submission's copy)
    g_pfb_r[k].fence = fence; g_pfb_r[k].n = n; g_pfb_r[k].busy = 1;
    pthread_mutex_unlock(&g_tmu);
    VkMemoryBarrier b0 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b0, 0, NULL, 0, NULL);
    VkBufferCopy c = {g_pfb_map.off, g_pfb_ring.off + (VkDeviceSize)k * PFB_BYTES, n}; vk.vkCmdCopyBuffer(cb, g_pfb_map.buf, g_pfb_ring.buf, 1, &c);
    // The fill writes what the copy reads (a write-after-read hazard): without this barrier Vulkan lets the two transfers run in any order
    VkMemoryBarrier bm = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &bm, 0, NULL, 0, NULL);
    vk.vkCmdFillBuffer(cb, g_pfb_map.buf, g_pfb_map.off, PFB_BYTES, 0);
    VkMemoryBarrier b1 = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b1, 0, NULL, 0, NULL);
}
// Texture ID id was read through the pool (caller holds g_tmu)
static void pfb_hit(int id, uint64_t frame, uint64_t fence) {
    if (id <= 0 || id >= g_ntreg || (uint32_t)id >= g_ntex_slots) return;
    const treg_ent *e = &g_treg[id];
    if (!e->t.serial || e->img || (e->t.pool && e->pgen && nvn_pool_live(e->t.pool, NULL, NULL) != e->pgen)) return;   // (a storage image's ID is another table; a finalized pool's: null)
    g_pfb_hits++;
    smap_ent *se = map_get(e->t.serial); vka_img *im = se ? se->im : NULL;
    if (se && !im) return;   // (a texture whose format has no image: vka_tex_get gives none again)
    if (im) { if (atomic_load(&im->last_frame) < frame) atomic_store(&im->last_frame, frame); if (atomic_load(&im->last_fence) < fence) atomic_store(&im->last_fence, fence); }
    if (im && !(g_pfb_st[id] & PFB_NULL)) return;   // (its slot names its image)
    if (!(g_pfb_st[id] & PFB_WANT)) { g_pfb_st[id] |= PFB_WANT; g_pfb_wants++; }
    dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id);
}
// The bitmap stays empty (pfb_consume_locked): DM_VK_POOL_FB_DEAD (live, default 600: some 10 s of submissions) readbacks of submissions whose shaders read the pool, and not one bit in any of
// them (counted before pfb_hit's filters: an ID of no live registration counts too). Upstream's earlier Qualcomm symptom was exactly this (every readback 0 IDs, every
// module rewritten with none refused: the stores of the bitmap in set 1 were dropped, "Texture pool reads report nothing on Qualcomm's driver" at eden/v0.3.11), and its
// set-2 fix is not confirmed on a phone there. Without a bit nothing makes a texture only the pool reads (walls drawn black), so every slot goes back to eager, as
// vka_tex_pfb_blind does: the memory of before the feedback, never less than it lit. 0: never (the feedback alone, whatever it reports). Caller holds g_tmu
static uint64_t g_pfb_bits;   // (under g_tmu) bits seen in the readbacks, raw
static void pfb_dead_check_locked(void) {
    if (g_pfb_bits || !g_pfb_nrb || atomic_load_explicit(&g_pfb_blind_eager, memory_order_relaxed)) return;
    int lim = VKA_ENV("DM_VK_POOL_FB_DEAD", 600);
    if (lim <= 0 || g_pfb_nrb < (uint64_t)lim || atomic_exchange(&g_pfb_blind_eager, 1)) return;
    atomic_store(&g_pfb_blind_new, 1);
    VRED("texture pool reads: %llu readbacks of submissions whose shaders read the texture pool, and not one texture ID in them: the GPU does not write the bitmap (descriptor set %d) "
         "on this driver (%s, driver version %#x): every registered texture's image is made and kept again from now on (as before the feedback; memory as then). "
         "DM_VK_POOL_FB=0 turns the feedback off; DM_VK_POOL_FB_DEAD=0 keeps it alone", (unsigned long long)g_pfb_nrb, VKA_POOL_FB_SET, V.props.deviceName, V.props.driverVersion);
}
static void pfb_consume_locked(void) {
    if (atomic_load(&g_pfb) <= 0) return;
    uint64_t done = vka_done(), frame = vka_frame(), fence = vka_fence_last();
    for (int k = 0; k < PFB_RING; k++) {
        if (!g_pfb_r[k].busy || g_pfb_r[k].fence > done) continue;
        const uint32_t *wd = (const uint32_t *)(g_pfb_ring.ptr + (size_t)k * PFB_BYTES);
        // The readback copied out in one memcpy before the scan (after DroidPlay Core c1abc57d, MIT idea): Arm's Mali has no memory type both cached and coherent, so the
        // ring can be uncached host memory, where the scan's 4-byte loads cost ~70 ns each (DroidPlay: 2.3 ms for 33,792 words against 0.33 ms on an Adreno). The slot
        // is not written again until it is reused (its fence is done), so the copy is the readback. DM_VK_POOL_SNAP=0 (live) scans in place, as before
        static uint32_t snap[PFB_BYTES / 4];
        if (g_pfb_r[k].n <= PFB_BYTES && VKA_ENV("DM_VK_POOL_SNAP", 1)) { memcpy(snap, wd, g_pfb_r[k].n & ~3u); wd = snap; }
        for (uint32_t i = 0, nw = g_pfb_r[k].n / 4; i < nw; i++)
            for (uint32_t x = wd[i]; x; x &= x - 1) { g_pfb_bits++; pfb_hit((int)(i * 32 + (uint32_t)__builtin_ctz(x)), frame, fence); }
        g_pfb_r[k].busy = 0; g_pfb_nrb++;
    }
    pfb_dead_check_locked();
    if (g_pfb_nrb >= g_pfb_logged + 1800 || (g_pfb_nrb && !g_pfb_logged)) {
        g_pfb_logged = g_pfb_nrb;
        VLOG("texture pool reads: %llu readbacks (%llu put off, every slot waiting), %llu bits set in them, %llu IDs read of live registrations, %llu slots made for a read (%llu images made, %.1f MiB, %llu of them for an ID made before: evicted and read again)%s",
             (unsigned long long)g_pfb_nrb, (unsigned long long)g_pfb_skip, (unsigned long long)g_pfb_bits, (unsigned long long)g_pfb_hits, (unsigned long long)g_pfb_wants, (unsigned long long)g_pfb_made, g_pfb_made_b / 1048576.0, (unsigned long long)g_pfb_again,
             tex_eager() ? "; slots eager too" : "");
    }
    if (atomic_exchange(&g_pfb_blind_new, 0))   // (vka_tex_pfb_blind: eager from now on) every registered slot written again, its image made
        for (int id = 1; id < g_ntreg; id++) if (g_treg[id].t.serial) dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id);
}

static void bindless_init_once(void) {
    static int done; if (done) return; done = 1;
    if (!V.uab) { VRED("no update-after-bind descriptor indexing: the texture and sampler pools have no tables"); return; }
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12}; vk.vkGetPhysicalDeviceProperties2(V.pd, &p2);
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    // Every pipeline layout holds this set beside set 0, whose combined image samplers and storage images count against the same limits: RESERVE of each is left to set 0
    // (a stage's set-0 range is DMSH_VK_STAGE_BINDINGS bindings; 5 graphics stages)
    enum { RESERVE = 5 * DMSH_VK_STAGE_BINDINGS };
#define HEAD(lim) ((lim) > 2 * RESERVE ? (lim) - RESERVE : (lim) / 2)
    g_ntex_slots = MIN(1u << 20, HEAD(MIN(p12.maxDescriptorSetUpdateAfterBindSampledImages, p12.maxPerStageDescriptorUpdateAfterBindSampledImages)));
    g_nsamp_slots = MIN(4096u, HEAD(MIN(p12.maxDescriptorSetUpdateAfterBindSamplers, p12.maxPerStageDescriptorUpdateAfterBindSamplers)));
    g_nimg_slots = MIN(1u << 16, HEAD(MIN(p12.maxDescriptorSetUpdateAfterBindStorageImages, p12.maxPerStageDescriptorUpdateAfterBindStorageImages)));
#undef HEAD
#undef MIN
    // DM_VK_POOL_SLOTS=<n> (restart only, a diagnostic): the texture table no longer than n, as on a device with lower update-after-bind limits (a Mali-G720: 499,680)
    { uint32_t cap = (uint32_t)VKA_ENV("DM_VK_POOL_SLOTS", 0); if (cap >= 1024 && cap < g_ntex_slots) { g_ntex_slots = cap; VLOG("DM_VK_POOL_SLOTS=%u: the texture table is cut to it", cap); } }
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, g_ntex_slots, VK_SHADER_STAGE_ALL, NULL}, {1, VK_DESCRIPTOR_TYPE_SAMPLER, g_nsamp_slots, VK_SHADER_STAGE_ALL, NULL},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, g_nimg_slots, VK_SHADER_STAGE_ALL, NULL}};
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, bfs[3] = {bf, bf, bf};
    uint32_t nb = g_nimg_slots ? 3 : 2;
    int pfb = pfb_avail();   // (the pool reads' bitmap has a set of its own, made with it: "Texture pool reads")
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = nb, .pBindingFlags = bfs};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = nb, .pBindings = b};
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, g_ntex_slots}, {VK_DESCRIPTOR_TYPE_SAMPLER, g_nsamp_slots}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, g_nimg_slots}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = nb, .pPoolSizes = ps};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
    VkResult r;
    if ((r = vk.vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_dsl)) || (r = vk.vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dpool)) || ((ai.descriptorPool = g_dpool), (r = vk.vkAllocateDescriptorSets(V.dev, &ai, &g_set)))) {
        VRED("bindless set (%u textures, %u samplers, %u images): %d", g_ntex_slots, g_nsamp_slots, g_nimg_slots, r); g_set = VK_NULL_HANDLE; return; }
    VLOG("bindless tables: %u textures, %u samplers, %u images (update after bind)%s", g_ntex_slots, g_nsamp_slots, g_nimg_slots, pfb ? "; the pool reads' bitmap in a set of its own" : "");
}
// Two threads may come here first (the replay's first pool slot, a prewarm thread's first module asking for the tables' sizes): one makes the tables, the other waits
static void bindless_init(void) { static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu); bindless_init_once(); pthread_mutex_unlock(&mu); }
// The tables' sizes (0s without tables), for the modules' pool arrays (vka_shader.c csm_fit)
void vka_bindless_counts(uint32_t *tex, uint32_t *samp, uint32_t *img) { bindless_init(); *tex = g_ntex_slots; *samp = g_nsamp_slots; *img = g_nimg_slots; }
VkDescriptorSet vka_bindless_set(void) { return g_set; }
VkDescriptorSetLayout vka_bindless_layout(void) { return g_dsl; }
VkDescriptorSetLayout vka_pfb_layout(void) { return atomic_load(&g_pfb) > 0 ? g_pfb_dsl : VK_NULL_HANDLE; }
VkDescriptorSet vka_pfb_set(void) { return atomic_load(&g_pfb) > 0 ? g_pfb_set : VK_NULL_HANDLE; }

static void dirty(int **list, int *n, int *cap, uint8_t **flag, int nslots, int id) {
    if (!*flag) *flag = calloc((size_t)nslots, 1);
    if ((*flag)[id]) return;
    (*flag)[id] = 1;
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *list = realloc(*list, (size_t)*cap * sizeof **list); }
    (*list)[(*n)++] = id;
}
static void treg(int id, const tex_t *t, const view_t *v, int img) {
    if (id < 0 || id >= (1 << 20)) return;
    pthread_mutex_lock(&g_tmu);
    if (id >= g_ntreg) { int n = g_ntreg ? g_ntreg : 1024; while (n <= id) n *= 2; g_treg = realloc(g_treg, (size_t)n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (size_t)(n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    if (g_pool_written_cap < g_ntreg) {   // (the pool slot memo: a new ID's slots hold nothing known, so their first write is sent)
        pool_written *p = realloc(g_pool_written, (size_t)g_ntreg * sizeof *p);
        if (p) { memset(p + g_pool_written_cap, 0, (size_t)(g_ntreg - g_pool_written_cap) * sizeof *p); g_pool_written = p; g_pool_written_cap = g_ntreg; }
        else { free(g_pool_written); g_pool_written = NULL; g_pool_written_cap = 0; }   // (every slot written, as before)
    }
    treg_ent *e = &g_treg[id]; e->t = *t; e->has_v = v != NULL; if (v) e->v = *v; e->img = (uint8_t)img; e->pgen = nvn_pool_live(t->pool, NULL, NULL);
    dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id);
    if (!g_tver) g_tver = calloc(1 << 20, sizeof *g_tver);
    if (g_tver) atomic_fetch_add_explicit(&g_tver[id], 1, memory_order_release);
    pthread_mutex_unlock(&g_tmu);
}
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { treg(id, t, v, 0); }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { treg(id, t, v, 1); }
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= 4096 * 4) return;
    pthread_mutex_lock(&g_tmu);
    if (id >= g_nsreg) { int n = g_nsreg ? g_nsreg : 256; while (n <= id) n *= 2; g_sreg = realloc(g_sreg, (size_t)n * sizeof *g_sreg); memset(g_sreg + g_nsreg, 0, (size_t)(n - g_nsreg) * sizeof *g_sreg); g_nsreg = n; }
    if (id < g_nsres) g_sres[id][0] = g_sres[id][1] = VK_NULL_HANDLE;
    g_sreg[id] = *p; dirty(&g_sdirty, &g_nsd, &g_sdcap, &g_sdflag, 4096 * 4, id);
    if (!g_sver) g_sver = calloc(4096 * 4, sizeof *g_sver);
    if (g_sver) atomic_fetch_add_explicit(&g_sver[id], 1, memory_order_release);
    pthread_mutex_unlock(&g_tmu);
}
void vka_samp_pool_seed(int base) {
    static uint8_t seeded[4096 * 4];
    if (base < 0 || base >= 4096 * 4 || seeded[base]) return;
    seeded[base] = 1;
    static const samp_par none;
    pthread_mutex_lock(&g_tmu);
    int taken = base < g_nsreg && memcmp(&g_sreg[base], &none, sizeof none);
    pthread_mutex_unlock(&g_tmu);
    if (!taken) mtl_reg_samp(base, &g_samp_zero);   // (a registration of the game's own at that ID replaces it, as on the GPU)
}
void mtl_set_tex_pool_n(int n) { (void)n; }

static VkSamplerAddressMode wrap(int w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
// iv: the sampler for an integer view (a uint or sint sampled binding, vka_draw.c stage_writes). Integer formats have no SAMPLED_IMAGE_FILTER_LINEAR_BIT: a linear filter or
// mipmap mode over one is invalid (VUID-vkCmdDispatch-magFilter-04553, mipmapMode-04770; the same for draws) and a float border colour reads undefined. Its twin filters
// nearest in both, without anisotropy, with the INT border of the same choice. Maxwell and Ampere filter no integer texture (a TEX of one reads the nearest texel), so the
// twin is the guest's sampling. The border: Vulkan's INT colours hold 0 and 1 where the guest's are raw words (a float 1.0 border reads 0x3f800000 there); the nearest of
// the three (open, no game seen depending on it)
static VkSampler sampler_of(const samp_par *p, int iv) {
    for (int i = 0; i < g_nsamp; i++) if (g_samp[i].iv == iv && !memcmp(&g_samp[i].p, p, sizeof *p)) return g_samp[i].s;
    if (g_nsamp >= (int)g_nsamp_slots) { VRED_ONCE("more than %u distinct samplers", g_nsamp_slots); return VK_NULL_HANDLE; }
    int mip = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
    float aniso = p->aniso > 1 ? (p->aniso < V.props.limits.maxSamplerAnisotropy ? p->aniso : V.props.limits.maxSamplerAnisotropy) : 1;
    float bias = p->lod_bias, bl = V.props.limits.maxSamplerLodBias; if (bias > bl) bias = bl; if (bias < -bl) bias = -bl;
    int white = p->border[0] > 0.5f && p->border[3] > 0.5f, opaque = p->border[3] > 0.5f;
    if (iv) aniso = 1;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr), .mipLodBias = bias,
        .anisotropyEnable = aniso > 1, .maxAnisotropy = aniso, .compareEnable = p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE,
        .compareOp = p->cmp_func >= 1 && p->cmp_func <= 8 ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_NEVER,
        .minLod = p->lod_min, .maxLod = mip ? p->lod_max : (p->lod_min < 0.25f ? 0.25f : p->lod_min),
        .borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : opaque ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    if (iv) {
        if (ci.magFilter || ci.minFilter || ci.mipmapMode) { static _Atomic int nl; if (atomic_fetch_add(&nl, 1) < 4)
            VLOG("an integer view sampled with a filtering sampler (mag %d min %d): its nearest twin", p->magf, p->minf); }
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST; ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.borderColor = white ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : opaque ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    }
    // Reduction (from Banana Native 02d081a2, MIT): a filtered sample takes the min or max of the texels it covers instead of their weighted average (NVN 1 min, 2 max:
    // Vulkan's own numbers), what a depth pyramid or a max-depth target is built with. samplerFilterMinmax guarantees it for the depth formats and R32_SFLOAT among others
    // (Vulkan 1.2 "Required Format Support"). A comparison sampler must keep the weighted average (VUID-VkSamplerCreateInfo-compareEnable-01423): there the comparison
    // wins and the reduction is logged. An integer view's twin filters nearest, where the reduction changes nothing; it is chained all the same
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = (VkSamplerReductionMode)p->reduction};
    if (p->reduction == 1 || p->reduction == 2) {
        if (!V.minmax) VRED_ONCE("sampler reduction mode %d not applied: the device has no samplerFilterMinmax", p->reduction);
        else if (ci.compareEnable) VRED_ONCE("sampler reduction mode %d with a depth comparison: Vulkan allows no reduction there, the comparison is kept", p->reduction);
        else ci.pNext = &red;
    } else if (p->reduction) VRED_ONCE("sampler reduction mode %d not applied: not an NVN reduction mode", p->reduction);
    VkSampler s = VK_NULL_HANDLE;
    if (vk.vkCreateSampler(V.dev, &ci, NULL, &s)) return VK_NULL_HANDLE;
    g_samp = realloc(g_samp, (size_t)(g_nsamp + 1) * sizeof *g_samp); g_samp[g_nsamp++] = (samp_ent){*p, iv, s};
    return s;
}
static VkComponentSwizzle swz1(int n) { return n >= 0 && n <= 5 ? (VkComponentSwizzle)(n + 1) : VK_COMPONENT_SWIZZLE_IDENTITY; }   // NVN ZERO ONE R G B A = Vulkan's minus 1

// The view a registered texture ID stands for: its view_t (levels, layers, format, target, swizzle, depth-stencil mode) over the texture's image. vt: the view type, or -1 for
// the registration's own target; storage: a storage image view (no sRGB decode, no swizzle)
// The attachments of the open render pass (vka_draw.c rp_begin): a sampled view of the same image whose levels start below an attached level and include it is cut to the
// levels below (nvn_mtl.m fb_prepare, "Mip-chain pass sampling the level it renders" in docs/reference/rendering.md). DKB fills its environment cube's mip chain level by level,
// each drawn from the one above through a view of the whole chain; sampling the attached level is a feedback loop Vulkan leaves undefined (on Adreno: NaN texels, which bloom
// spread into white blotches and black speckles over the whole level)
// g_rp_open: a pass is open in the recording command buffer (rp_begin passes its attachments, vka_rp_end NULL; a pass of no attachments is open too), which
// img_storage_upgrade's copy may not run inside
static vka_img *g_fb_im[9]; static uint32_t g_fb_lv[9]; static int g_nfb, g_rp_open;
void vka_tex_feedback(vka_img *const *ims, const uint32_t *levels, int n) { g_rp_open = ims != NULL; g_nfb = n > 9 ? 9 : n; for (int i = 0; i < g_nfb; i++) { g_fb_im[i] = ims[i]; g_fb_lv[i] = levels[i]; } }
static VkImageView view_of(const treg_ent *e, vka_img *im, int vt_want, int storage, VkFormat over) {   // over: a storage view's format in place of the texture's (id_view)
    vka_fmt f; const view_t *v = e->has_v ? &e->v : NULL;
    if (!vka_fmt_of(v && v->set_format ? v->format : e->t.d.format, &f)) f = im->f;
    uint32_t tl = im->base + im->levels, l0 = v && v->set_levels ? v->base_level : 0, nl = v && v->set_levels && v->nlevels ? v->nlevels : tl > l0 ? tl - l0 : 1;
    if (im->vtrack && !storage && l0 < im->vmin) { uint32_t cut = im->vmin - l0; nl = nl > cut ? nl - cut : 1; l0 = im->vmin; }   // a sparse texture: from its wholly mapped levels
    if (l0 < im->base) { uint32_t cut = im->base - l0; nl = nl > cut ? nl - cut : 1; l0 = 0; } else l0 -= im->base;   // texture levels → the image's (a sparse texture's left-out levels)
    uint32_t a0 = v && v->set_layers ? v->min_layer : 0, na = v && v->set_layers && v->nlayers ? v->nlayers : im->layers - a0;
    if (im->d.target == NVN_TEXTURE_TARGET_3D) { a0 = 0; na = 1; }   // a 3D texture's view range names slices (a render target view's), which a 3D view does not take
    if (l0 >= im->levels || a0 >= im->layers) return VK_NULL_HANDLE;
    if (l0 + nl > im->levels) nl = im->levels - l0;
    if (a0 + na > im->layers) na = im->layers - a0;
    if (vt_want >= 0 && !storage) for (int i = 0; i < g_nfb; i++) if (g_fb_im[i] == im && g_fb_lv[i] > l0 && g_fb_lv[i] < l0 + nl) {
        static _Atomic int n; if (!atomic_exchange(&n, 1)) VLOG("a pass samples the texture it renders (serial %llu level %u) through levels %u+%u: sampled through the levels below it", (unsigned long long)im->serial, g_fb_lv[i], l0, nl);
        nl = g_fb_lv[i] - l0; }
    int tgt = v && v->set_target ? v->target : e->t.d.target;
    VkComponentMapping m = {0};
    if (storage) nl = 1;
    else if (v && v->set_swizzle) m = (VkComponentMapping){swz1(v->swz[0]), swz1(v->swz[1]), swz1(v->swz[2]), swz1(v->swz[3])};
    else if (e->t.set_swz) m = (VkComponentMapping){swz1(e->t.swz & 7), swz1(e->t.swz >> 3 & 7), swz1(e->t.swz >> 6 & 7), swz1(e->t.swz >> 9 & 7)};
    else { int fmt = v && v->set_format ? v->format : e->t.d.format; if (fmt == 0x37 || fmt == 0x6D || fmt == 0x6E || fmt == 0x6F || fmt == 0x70 || (fmt >= 0x71 && fmt <= 0x78) || fmt == 0x99 || fmt == 0x9B) m.a = VK_COMPONENT_SWIZZLE_ONE; }
    VkImageViewType vt = vt_want >= 0 ? (VkImageViewType)vt_want : view_type(tgt, na);
    // What the image's type lets the view be (img_create: 3D for a 3D target, 1D for the 1D targets, cube-compatible for square 2D images of whole cubes)
    int img3d = im->d.target == NVN_TEXTURE_TARGET_3D;
    if ((vt == VK_IMAGE_VIEW_TYPE_3D) != img3d || vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) return VK_NULL_HANDLE;   // no 1D images (img_create)
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        if (im->w != im->h || im->layers < 6 || im->layers % 6 || im->samples > 1 || na < 6) return VK_NULL_HANDLE;
        if (vt == VK_IMAGE_VIEW_TYPE_CUBE) na = 6; else na -= na % 6;
    } else if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) na = 1;
    else if (vt == VK_IMAGE_VIEW_TYPE_3D) { a0 = 0; na = 1; }
    VkImageAspectFlags asp = im->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : e->t.stencil || (v && v->set_dsmode && v->dsmode) ? VK_IMAGE_ASPECT_STENCIL_BIT : im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    VkFormat vf = over ? over : e->img || storage ? f.vk : f.view;
    // A storage binding needs a view with storage use (VUID-VkWriteDescriptorSet-descriptorType-00339), which vka_view leaves out in a format without storage support.
    // An image can have storage use that its own format lacks (B10G11R11_UFLOAT on Adreno, through a learned R32_UINT view: img_create_), and a float storage binding of
    // it views it in that format: the binding reads as unbound, as it did while such an image had no storage use, instead of a view without it. The format checked is the
    // one vka_view makes the view in (the image's own when its list lacks the asked one)
    if (storage) {
        pthread_mutex_lock(&g_tmu); VkFormat sf = img_lists(im, vf) ? vf : im->f.vk; pthread_mutex_unlock(&g_tmu);
        if (!(feats(sf) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            static struct { VkFormat a, b; } seen[16]; static int ns; int k = 0;   // (once a pair; the recording thread)
            while (k < ns && !(seen[k].a == im->f.vk && seen[k].b == sf)) k++;
            if (k == ns && ns < 16) { seen[ns].a = im->f.vk; seen[ns++].b = sf;
                VRED("texture serial %llu (VkFormat %d, %ux%u) bound as a shader image in VkFormat %d, which has no storage support: unbound (its writes go nowhere, its reads are zero)", (unsigned long long)im->serial, im->f.vk, im->w, im->h, sf); }
            return VK_NULL_HANDLE;
        }
    }
    return vka_view(im, vf, vt, asp, l0, nl, a0, na, storage ? NULL : &m);
}

// Whether an image remade now would take storage use through a learned view format: its own format has none (B10G11R11_UFLOAT on Adreno), but a format learned for its
// format and size has (R32_UINT, id_view), which img_create_ puts in the remade image's list with storage use. Caller holds g_tmu (g_vlmu after it, as vka_view takes them)
static int vl_storage(const vka_img *im) {
    if (im->f.kind != 0 || compressed(&im->f) || !VKA_ENV("DM_VK_UBWC", 1)) return 0;   // (img_create_ takes learned formats under these only)
    VkFormat x[VL_PER_IMAGE]; pthread_mutex_lock(&g_vlmu); int n = vl_formats(im->f.vk, im->w, im->h, x, VL_PER_IMAGE); pthread_mutex_unlock(&g_vlmu);
    for (int k = 0; k < n; k++) if (feats(x[k]) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return 1;
    return 0;
}
// DM_VK_IMG_UPGRADE=0: no image intent from a binding: made and kept without storage use, the binding reads as unbound (as before); live
static int storage_able(const vka_img *im) { return !im->no_storage && !im->rs && VKA_ENV("DM_VK_IMG_UPGRADE", 1) && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && ((feats(im->f.vk) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || vl_storage(im)); }
// A texture made without NVN's IMAGE flag and then bound to a shader image slot (nvn_mtl.m image_prepare: the binding is the image intent; some engines register a texture with
// RegisterTexture and use its handle in an image slot). Its image was made without storage use (img_create), so the shader's writes went nowhere and what read the texture
// afterwards saw what was there before: DKB's opening area drew purple (a compute pass writing a 1536x864 R11G11B10 target). The image is remade with storage use, its content
// copied, and swapped in place, so every serial and view that shares it follows; the old image goes once the submissions that may use it complete. Not while a thread outside
// recording holds it (the present reads img unlocked): that binding stays unbound and the next one tries again. Caller holds g_tmu. Not inside a render pass either (a copy
// there is invalid, VUID-vkCmdCopyImage-renderpass): vka_tex_id_storage_pending ends the pass first for an image storage_able takes, but an image can become able after
// that answer, when a binding of the same draw learns its integer view format (id_view: a second binding of the image, or of another image of its format and size, in the
// same descriptor writes); that binding stays unbound too, and the next draw's pending check ends the pass and remakes it
static int img_storage_upgrade(vka_img *im, VkCommandBuffer cb) {
    if (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) return 1;
    if (!cb || g_rp_open || !storage_able(im) || im->holds) return 0;
    tex_t t = {0}; t.serial = im->serial; t.mem = im->mem; t.d = im->d; t.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; t.L = im->L;
    vka_img *n = img_create(&t, &im->f, im->base);
    if (!n) return 0;
    if (!(n->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {   // the format's image limits refuse storage at this shape
        VRED("texture serial %llu %ux%u VkFormat %d: bound as a shader image, but the driver has no storage use for it at this shape: its writes go nowhere", (unsigned long long)im->serial, im->w, im->h, im->f.vk);
        g_img_bytes -= n->bytes; img_destroy(n); im->no_storage = 1; return 0; }
    vka_img_barrier(cb, n->img, im->aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c[16]; uint32_t nc = 0;
    for (uint32_t lv = 0; lv < im->levels && nc < 16; lv++) {
        VkImageSubresourceLayers sr = {im->aspect, lv, 0, im->layers};
        c[nc++] = (VkImageCopy){sr, {0, 0, 0}, sr, {0, 0, 0}, {im->w >> lv ? im->w >> lv : 1, im->h >> lv ? im->h >> lv : 1, im->depth >> lv ? im->depth >> lv : 1}};
    }
    vk.vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, n->img, VK_IMAGE_LAYOUT_GENERAL, nc, c);
    vka_barrier(cb);
    VkImage oi = im->img; vka_imem od = im->m; void *ov = im->views; uint64_t ob = im->bytes;
    im->img = n->img; im->m = n->m; im->views = NULL; im->usage = n->usage; im->bytes = n->bytes; im->alt = n->alt; im->any_fmt = n->any_fmt;
    // (and the new image's format list: it takes the formats learned since the old one was made, and vka_view / img_lists read them from im; with the old list a learned
    // R32_UINT storage view fell back to the image's own format, which has no storage use)
    memcpy(im->xfmt, n->xfmt, sizeof im->xfmt); im->nx = n->nx;
    n->img = oi; n->m = od; n->views = ov; n->bytes = ob; g_img_bytes -= ob;
    if (im->holds) { n->zold = im->zold; im->zold = n; }
    else vka_after(vka_fence_last() + 1, img_destroy, n);
    // Views made of the old image are gone with it: the recording memo (g_tgen) and the registered pool slots that name the image are made again
    atomic_fetch_add_explicit(&g_tgen, 1, memory_order_release);
    int named = 0;
    for (int id = 1; id < g_ntreg; id++) { smap_ent *e = g_treg[id].t.serial ? map_get(g_treg[id].t.serial) : NULL; if (e && e->im == im) { dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id); named++; } }
    static _Atomic int k; if (atomic_fetch_add(&k, 1) < 16)
        VLOG("texture serial %llu %ux%u format %#x (VkFormat %d) flags %#x: bound as a shader image without the image flag, remade with storage use (content copied, %d texture IDs rewritten)",
             (unsigned long long)im->serial, im->w, im->h, im->d.format, im->f.vk, im->d.flags, named);
    return 1;
}

// A texture whose pool was finalized since it was registered (or is another pool now, initialized at the same address) is not made or read: the game may have unmapped its
// storage and the pool object (Mario Kart World on the phone, m22 / m23). Its ID gets the null view: sampling a texture of a finalized pool is undefined in NVN, and an ID
// registered again is dirty again. pgen 0: its pool was not live at the registration (registered before the pool's Initialize, or after its Finalize): not dead by this
// test, but storage_mode makes it zero texels while the pool is not live. The caller holds nvn_pool_use from this check until the image is made and uploaded
static int treg_dead(int id, const treg_ent *e) {
    int dead = e->t.pool && e->pgen && nvn_pool_live(e->t.pool, NULL, NULL) != e->pgen;
    if (dead) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VLOG("texture ID %d (serial %llu, storage %p): its pool %p was finalized after the registration: null view", id, (unsigned long long)e->t.serial, (void *)e->t.mem, (const void *)e->t.pool); }
    return dead;
}
static void write_tex(int id, VkCommandBuffer cb, VkWriteDescriptorSet *w, VkDescriptorImageInfo *ii) {   // caller holds g_tmu; it is let go while the image is made
    treg_ent ent = g_treg[id], *e = &ent;
    if (e->img && VKA_ENV("DM_VK_IMG_UPGRADE", 1)) e->t.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;   // an image registration is image intent (nvn_mtl.m image_prepare), with or without the texture's IMAGE flag
    // Eager (tex_eager: the pool reads not reported, DM_VK_POOL_FB=0, or DM_VK_EAGER_TEX=1): the slot's image is made here (and uploaded) when it is not made yet, as
    // before the port. Otherwise only an image already made is named, and a slot the GPU read through the pool while it held the null view (PFB_WANT, "Texture pool reads")
    // is made here as a first use: the image the read asked for
    int want = g_pfb_st && (g_pfb_st[id] & PFB_WANT), eager = want || tex_eager();
    if (want) g_pfb_st[id] &= (uint8_t)~PFB_WANT;
    vka_img *im = NULL;
    if (eager) {
        pthread_mutex_unlock(&g_tmu);
        nvn_pool_use(1);
        g_first_sampled = !e->img;   // (a texture ID in the sampled table: a first use here is a sample, nvn2_sampled_content)
        im = treg_dead(id, e) ? NULL : vka_tex_get(&e->t, cb);
        g_first_sampled = 0;
        nvn_pool_use(0);
        if (im) { pthread_mutex_lock(&g_tmu); im->holds++; pthread_mutex_unlock(&g_tmu); }
    } else {
        smap_ent *se = e->t.serial ? map_get(e->t.serial) : NULL;
        im = se ? se->im : NULL;
        if (im) im->holds++;
        pthread_mutex_unlock(&g_tmu);
    }
    // (img_storage_upgrade runs without nvn_pool_use: it copies image to image and never reads the storage)
    if (im && e->img && im->m.z) { pthread_mutex_lock(&g_tmu); img_own(im, cb); pthread_mutex_unlock(&g_tmu); }   // (shaders write it: memory of its own first)
    if (im && e->img && !(im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) { pthread_mutex_lock(&g_tmu); img_storage_upgrade(im, cb); pthread_mutex_unlock(&g_tmu); }
    VkImageView view = im ? view_of(e, im, -1, 0, VK_FORMAT_UNDEFINED) : VK_NULL_HANDLE;
    if (!view || treg_dead(id, e)) view = g_null_view;
    pthread_mutex_lock(&g_tmu);
    if (g_pfb_st && !e->img) {   // (whether the slot holds the null view: a pool read of it asks for its image, pfb_hit)
        if (view == g_null_view) g_pfb_st[id] |= PFB_NULL; else g_pfb_st[id] &= (uint8_t)~PFB_NULL;
        if (want && im) { g_pfb_made++; g_pfb_made_b += im->bytes; if (g_pfb_st[id] & PFB_MADE) g_pfb_again++; g_pfb_st[id] |= PFB_MADE; }   // (again: an ID whose image a read made before, evicted since)
    }
    im_unhold_locked(im);
    if (im && e->img) { pthread_mutex_unlock(&g_tmu); vka_img_written(im); pthread_mutex_lock(&g_tmu); }   // an image registration: shaders write it (vka_tex_id_view)
    *ii = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_set, .dstBinding = e->img ? 2 : 0, .dstArrayElement = (uint32_t)id, .descriptorCount = 1,
                                .descriptorType = e->img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii};
}
// Whether vka_tex_prepare sends write_tex's slot write. One with a view, always. One with the null view (no image made, an evicted one, a finalized pool's) only with the
// pool reads reported (pfb_avail requires null descriptors for them): the prune evicts images that slots name, which are destroyed once every submission recorded before
// this write is done (vka_tex_prune_pressure_locked), so the slot must stop naming the view first; and a slot whose image is not made yet must hold a valid descriptor
// for the pool read that asks for it. Before 2026-11-12 the port dropped this write as the eager path does: the slot kept naming a destroyed view. Eager (DM_VK_POOL_FB=0)
// drops it as before (there an image a live slot names is never evicted; a finalized pool's slot keeps what it named, as before the port)
static int slot_write_keep(const VkDescriptorImageInfo *ii) { return ii->imageView != VK_NULL_HANDLE || atomic_load_explicit(&g_pfb, memory_order_relaxed) > 0; }

// ---------- Linear textures' diagnostics ----------
// A linear texture (NVN_TEXTURE_FLAGS_LINEAR / LINEAR_RENDER_TARGET), once each: at its first sample, what the sample reads and whether the CPU's writes into its storage can
// reach it; at its first re-upload, that a CPU write was seen. The replay samples every texture from an image of its own, never from its pool memory (placed or not), so the
// CPU's later writes into a linear texture's storage reach the GPU only through the dirty tracker (nvn_dirty.c: tracked range, write protection, re-upload in
// vka_tex_prepare). Zelda: Ocarina of Time rasterizes its text lines with FreeType into R8 linear textures (256x46 / 640x46, flags 0x44) whose storage holds the glyphs
// while its text boxes come out empty on the phone (2026-11-07, open): these lines say which link fails. DM_VK_LINLOG (live): at most that many lines (default 64, 0 = none)
static _Atomic int g_linlog;
static int lin_room(void) { int m = VKA_ENV("DM_VK_LINLOG", 64); return m > 0 && atomic_fetch_add(&g_linlog, 1) < m; }
static int is_linear(const vka_img *im) { return (im->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0; }
static const char *mode_name(int m) { return m == ST_TRACK ? "uploaded from its storage and again when the CPU writes it (tracked)" : m == ST_FILL ? "uploaded once from its storage (later CPU writes are not followed)"
                                            : m == ST_SPARSE ? "read through its sparse pool's page table" : "cleared (GPU content, its storage not read)"; }
static void lin_note_sample(vka_img *im, int id) {   // (caller does not hold g_tmu)
    if (!is_linear(im)) return;
    pthread_mutex_lock(&g_tmu);
    int first = !(im->lin_seen & 1); im->lin_seen |= 1;
    vka_img c = *im;   // (the fields read below, as they are now)
    pthread_mutex_unlock(&g_tmu);
    if (!first || !lin_room()) return;
    uint64_t size = c.L.size ? c.L.size : 1; VkBuffer b; VkDeviceSize o; VkDeviceAddress bda = 0;
    int placed = c.mem && vka_addr((uint64_t)(uintptr_t)c.mem, size, &b, &o, &bda), tracked = c.mem && nvn_dirty_span(c.mem, size) != NULL;
    char why[640] = "no dirty-tracking state (not tracked)"; if (c.mem && tracked) nvn_dirty_debug(c.span, c.mem, size, c.stamp, why, sizeof why);
    uint64_t nz = 0; if (c.mem && c.L.size) for (uint64_t i = 0; i < c.L.size; i++) nz += c.mem[i] != 0;   // (what the upload just read, as the storage holds it now)
    VLOG("linear texture serial %llu %ux%u format %#x flags %#x stride %u: first sample (texture ID %d) reads an image of its own, never the pool memory; its content: %s%s%s; "
         "its storage holds %llu nonzero bytes now; storage %p+%#llx %s (device address %#llx); in a tracked pool range: %s, follows the CPU's writes: %s; %s",
         (unsigned long long)c.serial, c.w, c.h, c.d.format, c.d.flags, c.d.stride, id, mode_name(c.mode), c.gpu_owned ? "; the GPU has written it (render target, clear, copy or shader image): its storage is no longer followed" : "",
         c.cpu_dirty ? "; a CPU write is pending" : "", (unsigned long long)nz, (const void *)c.mem, (unsigned long long)c.L.size, placed ? "in placed memory" : "not in placed memory (anonymous guest memory)",
         (unsigned long long)bda, tracked ? "yes" : "no", c.track && !c.gpu_owned ? "yes" : "no", why);
}
static void lin_note_reup(vka_img *im, size_t lo, size_t hi, int rows) {   // caller holds g_tmu
    if (!is_linear(im) || (im->lin_seen & 2)) return;
    im->lin_seen |= 2;
    if (!lin_room()) return;
    VLOG("linear texture serial %llu %ux%u format %#x: the dirty tracker saw a CPU write into its storage (bytes %#zx-%#zx of %#llx): uploaded again (%s; its first re-upload; %llu re-uploads of tracked textures in all)",
         (unsigned long long)im->serial, im->w, im->h, im->d.format, lo, hi, (unsigned long long)im->L.size, rows ? "the rows written" : "all of it", (unsigned long long)g_reup);
}

// ---------- Per-draw lookups (vka_draw.c) ----------
// A shader's binding names the shape it samples (dim: SPIR-V Dim, array, multisample, sampled type): the view is made in that shape when the image allows it, else none
static int dim_view_type(uint8_t dim) {
    int a = (dim & DMSH_VK_DIM_ARRAY) != 0;
    switch (dim & DMSH_VK_DIM_MASK) {
    case 0: return a ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 1: return a ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    case 2: return VK_IMAGE_VIEW_TYPE_3D;
    case 3: return a ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return -1;
    }
}
static VkImageView id_view(int id, VkCommandBuffer cb, uint8_t dim, int storage, vka_img **imo);
VkImageView vka_tex_id_view(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout) {
    if (layout) *layout = VK_IMAGE_LAYOUT_GENERAL;
    return id_view(id, cb, dim, storage, NULL);
}
static int in_fb(const vka_img *im) { for (int i = 0; i < g_nfb; i++) if (g_fb_im[i] == im) return 1; return 0; }   // (a view of a pass's own target is cut to its other levels: view_of)
// Texture ID id's storage overlaps an inline update deferred in the open pass (vka_exec.c "Inline uniform updates"; Erick's 6e47905): a draw sampling it ends the pass first
// (vka_draw.c ub_hazard), since with upload batches its first use can upload it inside the pass (gw_content would miss a write not in the GPU write list yet). 1 when it
// cannot tell: an unregistered ID, no storage, a sparse texture
int vka_tex_id_overlaps_pending_ub(int id) {
    if (id <= 0) return 1;
    pthread_mutex_lock(&g_tmu);
    int r = 1;
    if (id < g_ntreg && g_treg[id].t.serial) {
        const tex_t *t = &g_treg[id].t;
        if (t->mem && t->L.size && !(t->d.flags & NVN_TEXTURE_FLAGS_SPARSE)) r = vka_ub_pending_overlap((uint64_t)(uintptr_t)t->mem, t->L.size);
    }
    pthread_mutex_unlock(&g_tmu);
    return r;
}
// The image is one of the open pass's attachments, or may share memory with one (bar_hazard only; view_of and the view memo keep in_fb). Two vka_img can name the same
// bytes: sparse- or virtual-pool-bound images (im->vbind / vpool, vka_sparse.c) bind the same physical pages, and textures of other serials can lie over a target's guest
// storage. Guest ranges are compared half-open (adjacent ones do not overlap); storage not known on either side counts as an overlap, so the pass ends. Erick's b6a3bfb
static int in_fb_or_alias(const vka_img *im) {
    if (in_fb(im)) return 1;
    if (!im->mem || !im->L.size) return 1;
    uint64_t a = (uint64_t)(uintptr_t)im->mem;
    for (int i = 0; i < g_nfb; i++) {
        const vka_img *f = g_fb_im[i];
        if (!f || !f->mem || !f->L.size) return 1;
        // Two residency images (vbind: tiles bound to the physical pages behind their mappings, sparse_bind) may share Vulkan memory at different guest addresses, which
        // the guest ranges cannot show: counted as an overlap. (vtrack images hold an uploaded copy of their own, so their guest range is what matters)
        if (im->vbind && f->vbind) return 1;
        uint64_t b = (uint64_t)(uintptr_t)f->mem;
        if (a <= b ? b - a < im->L.size : a - b < f->L.size) return 1;
    }
    return 0;
}
// Texture ID id stands for an image the open pass draws into (any level), or one that may share memory with it (in_fb_or_alias): a draw sampling it after a game barrier
// inside the pass needs the pass ended first (vka_draw.c bar_hazard). A registered ID with no image mapped counts as one too, since what it samples is not known
// (make_images has ended the pass for a new image before bar_hazard looks, so it is rare)
int vka_tex_id_in_pass(int id) {
    if (!g_nfb || id <= 0) return 0;
    pthread_mutex_lock(&g_tmu);
    int r = 0;
    if (id < g_ntreg && g_treg[id].t.serial) { smap_ent *e = map_get(g_treg[id].t.serial); r = !e || !e->im || in_fb_or_alias(e->im); }
    pthread_mutex_unlock(&g_tmu);
    return r;
}
typedef struct { int id; uint8_t dim; uint32_t ver, gen; vka_img *im; VkImageView v; } vmemo;
static vmemo g_vm[2048]; static struct { int id; uint32_t ver; VkSampler s[2]; } g_sm[512];
static inline vmemo *vm_slot(int id, uint8_t dim) { return &g_vm[((unsigned)id * 7u + dim) & 2047]; }
static inline int vm_hit(const vmemo *m, int id, uint8_t dim) {
    return m->id == id && m->dim == dim && g_tver && m->ver == atomic_load_explicit(&g_tver[id], memory_order_acquire) && m->gen == atomic_load_explicit(&g_tgen, memory_order_acquire) && !in_fb(m->im);
}
// vka_tex_id_view for the recording thread (one thread at a time: the replay worker, or the submitting thread inline), with the memo; storage images take the full path
VkImageView vka_tex_id_view_rec(int id, VkCommandBuffer cb, uint8_t dim, int storage, VkImageLayout *layout) {
    if (layout) *layout = VK_IMAGE_LAYOUT_GENERAL;
    if (storage || id <= 0 || id >= (1 << 20) || !VKA_ENV("DM_VK_TEX_MEMO", 1)) return id_view(id, cb, dim, storage, NULL);
    vmemo *m = vm_slot(id, dim);
    if (vm_hit(m, id, dim)) { img_touch(m->im); return m->v; }
    uint32_t gen = atomic_load_explicit(&g_tgen, memory_order_acquire), ver = g_tver ? atomic_load_explicit(&g_tver[id], memory_order_acquire) : 0;
    vka_img *im = NULL; VkImageView v = id_view(id, cb, dim, 0, &im);
    if (v && im && g_tver && !in_fb(im)) *m = (vmemo){id, dim, ver, gen, im, v};
    return v;
}
int vka_tex_id_pending_rec(int id, uint8_t dim) {   // 0 at once when the memo holds the ID (its image was made)
    if (id > 0 && id < (1 << 20) && VKA_ENV("DM_VK_TEX_MEMO", 1) && vm_hit(vm_slot(id, dim), id, dim)) return 0;
    return vka_tex_id_pending(id);
}
VkSampler vka_samp_id_rec(int id, int iv) {
    iv = iv != 0;
    if (id <= 0 || id >= 4096 * 4 || !g_sver || !VKA_ENV("DM_VK_TEX_MEMO", 1)) return vka_samp_id(id, iv);
    uint32_t ver = atomic_load_explicit(&g_sver[id], memory_order_acquire);
    typeof(g_sm[0]) *m = &g_sm[id & 511];
    if (m->id == id && m->ver == ver && m->s[iv]) return m->s[iv];
    if (m->id != id || m->ver != ver) { m->id = id; m->ver = ver; m->s[0] = m->s[1] = VK_NULL_HANDLE; }
    return m->s[iv] = vka_samp_id(id, iv);
}
static VkImageView id_view(int id, VkCommandBuffer cb, uint8_t dim, int storage, vka_img **imo) {
    int writes = wbf_view_writes(storage); storage &= VKA_VIEW_STORAGE;   // (VKA_VIEW_RO: a shader image the shader only reads; Banana Native #276)
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); return VK_NULL_HANDLE; }
    treg_ent e = g_treg[id];
    if (storage && VKA_ENV("DM_VK_IMG_UPGRADE", 1)) e.t.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;   // bound to an image slot: image intent (img_storage_upgrade)
    pthread_mutex_unlock(&g_tmu);
    nvn_pool_use(1); g_first_sampled = !storage; vka_img *im = treg_dead(id, &e) ? NULL : vka_tex_get(&e.t, cb); g_first_sampled = 0; nvn_pool_use(0);
    if (!im) return VK_NULL_HANDLE;
    int vt = dim_view_type(dim);
    if (vt < 0) return VK_NULL_HANDLE;
    if (((dim & DMSH_VK_DIM_MS) != 0) != (im->samples > 1)) return VK_NULL_HANDLE;
    if (storage && im->m.z) { pthread_mutex_lock(&g_tmu); img_own(im, cb); pthread_mutex_unlock(&g_tmu); }   // (a shader image: memory of its own before the draw writes it; vka_tex_id_storage_pending ended the pass)
    if (!storage && im->m.size >= big_min()) { pthread_mutex_lock(&g_tmu); im->nsamp++; pthread_mutex_unlock(&g_tmu); }   // (DM_VK_BIGIMG_MB: bound for sampling)
    // The sampled type must be the image format's class (float / sint / uint): another class is undefined in Vulkan
    int want = dim & DMSH_VK_DIM_SINT ? 2 : dim & DMSH_VK_DIM_UINT ? 1 : 0, have = im->f.kind == 1 ? 1 : im->f.kind == 2 ? 2 : 0;
    // An integer shader image over a float texture of 4-byte texels (agl's upscaler stores its RGBA8 and R11G11B10F output as packed R32_UINT words): an R32_UINT /
    // R32_SINT view, the same bits. The image takes that view once its format list has the format (Learned view formats): the pair is learned at the first such binding,
    // and images made from then on, in this run and the next ones, take it; until then the binding reads as unbound, as before. Learned before the storage upgrade below:
    // when the image's own format has no storage support (B10G11R11_UFLOAT on Adreno) its storage use can only come through this view (storage_able, vl_storage), so the
    // upgrade refused it and returned before the pair was ever learned, and only a hand-written vk_view_formats.txt had it. An image without storage use that learns the
    // pair is remade with the view at once when no pass is open (make_images: the draw's images made before its pass); inside the draw's pass (its descriptor writes, the
    // pass not ended for a remake since vka_tex_id_storage_pending asked storage_able before the pair existed) img_storage_upgrade refuses, the binding stays unbound this
    // time, and the next draw's pending check ends the pass and remakes it
    VkFormat over = VK_FORMAT_UNDEFINED;
    if (storage && want && !have && im->f.kind == 0 && !compressed(&im->f) && vl_bytes(im->f.vk) == 4) {
        over = want == 1 ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SINT;
        pthread_mutex_lock(&g_tmu); int listed = img_lists(im, over); pthread_mutex_unlock(&g_tmu);
        if (!listed) {   // learns the pair (once)
            pthread_mutex_lock(&g_vlmu); int learned = vl_learn(im->f.vk, over, im->w, im->h); pthread_mutex_unlock(&g_vlmu);
            if (learned) VLOG("texture serial %llu (VkFormat %d, %ux%u) bound as an integer shader image: its image has no VkFormat %d view; learned (images of this format and size made from now on, and in the next runs, take it)%s",
                              (unsigned long long)im->serial, im->f.vk, im->w, im->h, over, im->usage & VK_IMAGE_USAGE_STORAGE_BIT ? "; unbound for the life of this image (it has storage use: not remade)"
                              : g_rp_open ? "; unbound this time (a render pass is open), remade with storage use through it at its next binding" : "; remade with storage use through it now");
        }
    }
    if (storage && !(im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) { pthread_mutex_lock(&g_tmu); int ok = img_storage_upgrade(im, cb); pthread_mutex_unlock(&g_tmu); if (!ok) return VK_NULL_HANDLE; }
    // Bound as a shader image: the GPU writes it (nvn_mtl.m mtl_tex_gpu_owned: ShaderWrite usage), so its storage is no longer its content. Without this a CPU write near a
    // tracked image's pages re-uploaded stale storage over what a compute pass wrote (DKB's 1x1 R16F texture, re-uploaded in the level). Only a binding the shader writes
    // (DMSH_VK_BF_RW, or a host pass's output): a shader image only read is not a write (vka_wbfollow.h wbf_view_writes: Mario Tennis Fever's glyph cache)
    if (writes) vka_img_written(im);
    if (!storage && !(im->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return VK_NULL_HANDLE;
    if (over) {   // (the image as it is now: remade above when it had no storage use, with the formats learned by then)
        pthread_mutex_lock(&g_tmu); int listed = img_lists(im, over); pthread_mutex_unlock(&g_tmu);
        if (!listed) return VK_NULL_HANDLE;
        have = want;
    }
    if (want != have && !(im->f.kind >= 3 && want == 0)) return VK_NULL_HANDLE;
    if (!storage) lin_note_sample(im, id);   // (a linear texture's first sample: what it reads, once)
    // Without eager slots a texture a draw binds is made here while its pool slot may still hold the null view (written before the image existed): the slot is written
    // again at the next submission (vka_tex_prepare), so a shader reading the same ID through the pool sees the image without waiting for its read to be reported
    // (upstream eden/v0.3.11 writes the slot here at once, "What each slot of the pool tables holds"; a write at prepare keeps every slot write where it was)
    if (!storage && !e.img && g_pfb_st) { pthread_mutex_lock(&g_tmu); if (g_pfb_st[id] & PFB_NULL) dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id); pthread_mutex_unlock(&g_tmu); }
    if (imo) *imo = im;
    return view_of(&e, im, vt, storage, over);
}
// The SPIR-V Image Format of an integer VkFormat a storage view can have (0: none, Unknown is declared): vka_spv_img_retype writes it in the retyped module, so a read needs no
// STORAGE_READ_WITHOUT_FORMAT support for the format. R64 formats are left out (Int64ImageEXT)
static uint8_t spv_int_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32G32B32A32_UINT: return 30; case VK_FORMAT_R16G16B16A16_UINT: return 31; case VK_FORMAT_R8G8B8A8_UINT: return 32; case VK_FORMAT_R32_UINT: return 33;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 34; case VK_FORMAT_R32G32_UINT: return 35; case VK_FORMAT_R16G16_UINT: return 36; case VK_FORMAT_R8G8_UINT: return 37;
    case VK_FORMAT_R16_UINT: return 38; case VK_FORMAT_R8_UINT: return 39;
    case VK_FORMAT_R32G32B32A32_SINT: return 21; case VK_FORMAT_R16G16B16A16_SINT: return 22; case VK_FORMAT_R8G8B8A8_SINT: return 23; case VK_FORMAT_R32_SINT: return 24;
    case VK_FORMAT_R32G32_SINT: return 25; case VK_FORMAT_R16G16_SINT: return 26; case VK_FORMAT_R8G8_SINT: return 27; case VK_FORMAT_R16_SINT: return 28; case VK_FORMAT_R8_SINT: return 29;
    default: return 0;
    }
}
// Texture ID id's integer class as id_view checks a storage or sampled binding against it (the image's format: 1 unsigned, 2 signed; 0 a float, normalized or depth format, or no
// texture), and *spv_fmt the SPIR-V Image Format of the view a storage binding of it gets (0: none). That view is in the image's own format, the texture's (vka_tex_get
// makes it from that format; same_shape shares an integer image only at the same VkFormat), whatever format the texture view names: img_create gives an integer image no
// other view format, so vka_view falls back to its own. A registration whose view format is of another class than its texture's is not answered (0). For vka_draw.c
// img_retype_of (storage and sampled images bound to integer textures; a sampled one keeps format Unknown, *spv_fmt unused)
int vka_tex_id_int_class(int id, uint8_t *spv_fmt) {
    *spv_fmt = 0;
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); return 0; }
    treg_ent e = g_treg[id];
    pthread_mutex_unlock(&g_tmu);
    vka_fmt tf, vf;
    if (!vka_fmt_of(e.t.d.format, &tf) || (tf.kind != 1 && tf.kind != 2)) return 0;
    if (!vka_fmt_of(e.has_v && e.v.set_format ? e.v.format : e.t.d.format, &vf)) vf = tf;
    if (vf.kind != tf.kind) return 0;
    uint8_t f = spv_int_format(tf.vk);   // (not the view format's: a declared format must be the bound view's, and that view is in tf.vk)
    if ((f >= 30) != (tf.kind == 1)) f = 0;   // (a format table that names another class: Unknown rather than a wrong format)
    *spv_fmt = f;
    return tf.kind;
}
vka_img *vka_tex_id_img(int id, VkCommandBuffer cb) {   // the image of texture ID id (DM_VK_NANSCAN), NULL = none
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); return NULL; }
    treg_ent e = g_treg[id];
    pthread_mutex_unlock(&g_tmu);
    nvn_pool_use(1); vka_img *im = treg_dead(id, &e) ? NULL : vka_tex_get(&e.t, cb); nvn_pool_use(0);
    return im;
}
// DM_VK_NANSCAN_DUMP: level 0 of texture ID id as its storage holds it now (deswizzled), written to DM_TEMP as nan-now-<serial>.blk
void vka_tex_id_dump(int id) {
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); return; }
    treg_ent e = g_treg[id];
    pthread_mutex_unlock(&g_tmu);
    vka_fmt f; if (!e.t.mem || !vka_fmt_of(e.t.d.format, &f)) return;
    uint32_t bx = (e.t.d.width + f.bw - 1) / f.bw, by = (e.t.d.height + f.bh - 1) / f.bh; size_t pitch = (size_t)bx * f.bpb, n = pitch * by;
    uint8_t *buf = malloc(n); if (!buf) return;
    nvn_pool_use(1); int bad = treg_dead(id, &e) || (e.t.pool && !nvn_pool_live(e.t.pool, NULL, NULL)) || nvn_deswizzle(&e.t.d, 0, 0, e.t.mem, buf, pitch); nvn_pool_use(0);   // (a finalized pool's storage is not read)
    if (!bad) {
        char fn[512]; snprintf(fn, sizeof fn, "%s/nan-now-%llu.blk", getenv("DM_TEMP") ? getenv("DM_TEMP") : "/data/local/tmp", (unsigned long long)e.t.serial);
        FILE *fo = fopen(fn, "wb"); if (fo) { fwrite(buf, 1, n, fo); fclose(fo); VLOG("NaN scan: texture ID %d serial %llu storage %p (pool flags %#x, layout %d): level 0 as it is now written to %s", id, (unsigned long long)e.t.serial, (void *)e.t.mem, pool_flags(e.t.pool), e.t.has_layout, fn); }
    }
    free(buf);
}
// DM_VK_NANSCAN: texture ID id's storage now against its last upload: changed with the tracker's knowledge (re-uploaded at the next submission) or without it (a missed write)
void vka_tex_id_check(int id) {
    nvn_pool_use(1); pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); nvn_pool_use(0); return; }
    smap_ent *e = map_get(g_treg[id].t.serial); vka_img *im = e ? e->im : NULL;
    if (im && im->track && !im->gpu_owned && im->mem && im->hash && !img_pool_dead(im)) {
        uint64_t h = mem_hash(im->mem, im->L.size); int dirty = nvn_dirty_since_span(im->span, im->mem, im->L.size, im->stamp);
        static _Atomic int n; if (h != im->hash && atomic_fetch_add(&n, 1) < 32) { char why[600]; nvn_dirty_debug(im->span, im->mem, im->L.size, im->stamp, why, sizeof why);
            {   // the mapping's protection as the kernel has it (diagnostic)
                FILE *f = fopen("/proc/self/maps", "re"); char ln[512]; uintptr_t a = (uintptr_t)im->mem;
                while (f && fgets(ln, sizeof ln, f)) { unsigned long lo, hi; if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi) { size_t k = strlen(why); ln[strcspn(ln, "\n")] = 0; snprintf(why + k, sizeof why - k, "; maps: %s", ln); break; } }
                if (f) fclose(f);
            }
            VLOG("NaN scan: texture ID %d serial %llu storage %p+%#llx changed since its upload; the dirty tracker %s: %s", id, (unsigned long long)im->serial, (const void *)im->mem, (unsigned long long)im->L.size, dirty ? "knows (re-upload pending)" : "MISSED it", why); }
    }
    pthread_mutex_unlock(&g_tmu); nvn_pool_use(0);
}
// What texture ID id is, for a report: its texture, the registration's view and the image (or why there is none)
void vka_tex_id_desc(int id, char *b, size_t n) {
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { snprintf(b, n, "not registered"); pthread_mutex_unlock(&g_tmu); return; }
    treg_ent e = g_treg[id]; smap_ent *m = map_get(e.t.serial); vka_img *im = m ? m->im : NULL;
    pthread_mutex_unlock(&g_tmu);
    const view_t *v = e.has_v ? &e.v : NULL;
    int sw = v && v->set_swizzle ? v->swz[0] | v->swz[1] << 3 | v->swz[2] << 6 | v->swz[3] << 9 : e.t.set_swz ? e.t.swz : -1;   // (NVN channels: 0 zero 1 one 2 R 3 G 4 B 5 A)
    snprintf(b, n, "serial %llu target %d format %#x %ux%ux%u levels %u flags %#x%s; view target %d layers %u+%u format %#x swizzle %d%d%d%d%s; image %s %ux%ux%u layers %u usage %#x [storage %p, image of serial %llu %p]",
             (unsigned long long)e.t.serial, e.t.d.target, e.t.d.format, e.t.d.width, e.t.d.height, e.t.d.depth, e.t.d.levels, e.t.d.flags, e.img ? " (image slot)" : "",
             v && v->set_target ? v->target : -1, v && v->set_layers ? v->min_layer : 0, v && v->set_layers ? v->nlayers : 0, v && v->set_format ? v->format : 0,
             sw < 0 ? 2 : sw & 7, sw < 0 ? 3 : sw >> 3 & 7, sw < 0 ? 4 : sw >> 6 & 7, sw < 0 ? 5 : sw >> 9 & 7, sw < 0 ? " (none set)" : v && v->set_swizzle ? " (view)" : " (texture)",
             im ? "made" : m ? "refused" : "not made", im ? im->w : 0, im ? im->h : 0, im ? im->depth : 0, im ? im->layers : 0, im ? im->usage : 0,
             (const void *)e.t.mem, im ? (unsigned long long)im->serial : 0ull, (void *)im);
}
// Diagnostic (DM_VK_TEXSTAT_FS, vka_draw.c): the image texture ID id names now (a reference, dropped with vka_tex_drop), and whether view v is one of that image's views
vka_img *vka_tex_id_acquire(int id, VkImageView v, int *own_view) {
    pthread_mutex_lock(&g_tmu);
    vka_img *im = NULL; *own_view = 0;
    if (id > 0 && id < g_ntreg && g_treg[id].t.serial) { smap_ent *m = map_get(g_treg[id].t.serial); im = m ? m->im : NULL; }
    if (im) { im->refs++; im->holds++; for (view_ent *e = im->views; e; e = e->next) if (e->v == v) *own_view = 1; }
    pthread_mutex_unlock(&g_tmu);
    return im;
}
void vka_samp_id_desc(int id, char *b, size_t n) {
    pthread_mutex_lock(&g_tmu);
    if (id == VKA_SAMP_ZERO) snprintf(b, n, "pool slot 0, NVN's reserved sampler (linear, no mipmaps, clamp to edge)");
    else if (id <= 0 || id >= g_nsreg) snprintf(b, n, "sampler ID %d not registered", id);
    else { samp_par p = g_sreg[id]; snprintf(b, n, "sampler ID %d: filter %d/%d wrap %d/%d/%d compare mode %d func %d reduction %d lod %g..%g bias %g", id, p.minf, p.magf, p.ws, p.wt, p.wr,
                                             p.cmp_mode, p.cmp_func, p.reduction, p.lod_min, p.lod_max, p.lod_bias); }
    pthread_mutex_unlock(&g_tmu);
}
// 1 when texture ID id's image (or registration) can form a Vulkan cube / cube-array view (square, 6+ layers a multiple of 6, 1 sample, view layers >= 6); 0 otherwise
int vka_tex_id_can_cube(int id) {
    pthread_mutex_lock(&g_tmu);
    int ok = 1;
    if (id > 0 && id < g_ntreg && g_treg[id].t.serial) {
        const treg_ent *e = &g_treg[id]; smap_ent *m = map_get(e->t.serial); const vka_img *im = m ? m->im : NULL;
        uint32_t w = im ? im->w : e->t.d.width, h = im ? im->h : e->t.d.height;
        uint32_t layers = im ? im->layers : (e->t.d.target == NVN_TEXTURE_TARGET_CUBEMAP ? 6 : e->t.d.target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY || e->t.d.target == NVN_TEXTURE_TARGET_2D_ARRAY ? e->t.d.depth : 1);
        uint32_t samples = im ? im->samples : e->t.d.samples;
        uint32_t a0 = e->has_v && e->v.set_layers ? e->v.min_layer : 0;
        uint32_t na = e->has_v && e->v.set_layers && e->v.nlayers ? e->v.nlayers : (layers > a0 ? layers - a0 : 0);
        if (im && im->d.target == NVN_TEXTURE_TARGET_3D) na = 1;
        ok = w == h && layers >= 6 && !(layers % 6) && samples <= 1 && na >= 6;
    }
    pthread_mutex_unlock(&g_tmu);
    return ok;
}
// Texture ID id as a buffer texture (NVN_TEXTURE_TARGET_BUFFER, which has no image: vka_buf_view binds its storage in place): its guest address, its bytes (elements times
// the texel) and the format a texel buffer view of it takes (its view's format when the registration names one; an 8-bit sRGB format as its UNORM twin, the raw bytes).
// 0: not a registered buffer texture, its pool finalized since, or a format with no texel of whole bytes (depth, block-compressed)
int vka_tex_id_buffer(int id, uint64_t *addr, uint64_t *bytes, VkFormat *fmt, uint32_t *texel) {
    pthread_mutex_lock(&g_tmu);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].t.serial) { pthread_mutex_unlock(&g_tmu); return 0; }
    treg_ent e = g_treg[id];
    pthread_mutex_unlock(&g_tmu);
    if (e.t.d.target != NVN_TEXTURE_TARGET_BUFFER || !e.t.mem || treg_dead(id, &e)) return 0;
    vka_fmt f;
    if (!vka_fmt_of(e.has_v && e.v.set_format ? e.v.format : e.t.d.format, &f) || f.kind >= 3 || f.dec || f.bw > 1 || f.bh > 1 || !f.bpb) return 0;
    *addr = (uint64_t)(uintptr_t)e.t.mem; *bytes = (uint64_t)(e.t.d.width ? e.t.d.width : 1) * f.bpb; *fmt = f.vk; *texel = f.bpb;
    return 1;
}
int vka_tex_id_target(int id) {
    pthread_mutex_lock(&g_tmu);
    int t = id > 0 && id < g_ntreg && g_treg[id].t.serial ? (g_treg[id].has_v && g_treg[id].v.set_target ? g_treg[id].v.target : g_treg[id].t.d.target) : -1;
    pthread_mutex_unlock(&g_tmu);
    return t;
}
int vka_tex_id_storage_pending(int id) {
    pthread_mutex_lock(&g_tmu);
    smap_ent *e = id > 0 && id < g_ntreg && g_treg[id].t.serial ? map_get(g_treg[id].t.serial) : NULL;
    int r = e && e->im && ((!(e->im->usage & VK_IMAGE_USAGE_STORAGE_BIT) && storage_able(e->im) && !e->im->holds) || e->im->m.z);   // (a zero-shared image gets memory of its own: outside the pass too)
    pthread_mutex_unlock(&g_tmu);
    return r;
}
int vka_tex_id_pending(int id) {
    pthread_mutex_lock(&g_tmu);
    int r = id > 0 && id < g_ntreg && g_treg[id].t.serial && !map_get(g_treg[id].t.serial);
    pthread_mutex_unlock(&g_tmu);
    return r;
}
VkSampler vka_samp_id(int id, int iv) {
    pthread_mutex_lock(&g_tmu);
    VkSampler s = VK_NULL_HANDLE; iv = iv != 0;
    if (id == VKA_SAMP_ZERO) { static VkSampler z[2]; if (!(s = z[iv])) s = z[iv] = sampler_of(&g_samp_zero, iv); pthread_mutex_unlock(&g_tmu); return s; }
    if (id > 0 && id < g_nsreg) {
        if (id >= g_nsres) { int n = g_nsreg; g_sres = realloc(g_sres, (size_t)n * sizeof *g_sres); memset(g_sres + g_nsres, 0, (size_t)(n - g_nsres) * sizeof *g_sres); g_nsres = n; }
        if (!(s = g_sres[id][iv])) s = g_sres[id][iv] = sampler_of(&g_sreg[id], iv);
    }
    pthread_mutex_unlock(&g_tmu);
    return s;
}
VkSampler vka_samp_default(void) {
    static VkSampler s;
    if (!s) { VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
              vk.vkCreateSampler(V.dev, &ci, NULL, &s); }
    return s;
}

// Diagnostic (live): DM_VK_READWATCH=<KiB>, once per value: the storage pages of every image the GPU wrote (at most that size; -1: any size) are made inaccessible, and
// the first CPU access to each (the game reading what it believes the GPU rendered there, which the replay never writes back unless the texture is linear) is logged
#include <ucontext.h>
#include <sys/syscall.h>
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
enum { RW_N = 8192, RW_HITS = 256 };
static struct { uintptr_t lo, hi; uint64_t serial; uint32_t w, h, fmt; } g_rwr[RW_N]; static _Atomic int g_nrwr;
static struct { uintptr_t a, pc; int w, tid, i; } g_rwh[RW_HITS]; static _Atomic int g_nrwh; static int g_rwh_logged;
static int rw_fault(void *addr, int is_write, void *uc) {
    uintptr_t a = (uintptr_t)addr; int n = atomic_load(&g_nrwr);
    for (int i = 0; i < n; i++) if (a >= g_rwr[i].lo && a < g_rwr[i].hi) {
        int k = atomic_fetch_add(&g_nrwh, 1);
        if (k < RW_HITS) { g_rwh[k].a = a; g_rwh[k].w = is_write; g_rwh[k].i = i; g_rwh[k].tid = (int)syscall(__NR_gettid);
            g_rwh[k].pc = uc ? (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc : 0; }
        uintptr_t pg = a & ~(uintptr_t)4095; syscall(__NR_mprotect, pg, 4096, PROT_READ | PROT_WRITE);
        return 1;
    }
    return 0;
}
static void read_watch(void) {
    static int was, hooked; int kb = VKA_ENV("DM_VK_READWATCH", 0);
    int nh = atomic_load(&g_nrwh); if (nh > RW_HITS) nh = RW_HITS;
    for (; g_rwh_logged < nh; g_rwh_logged++) { typeof(g_rwh[0]) *h = &g_rwh[g_rwh_logged];
        VLOG("read watch: the CPU %s %#lx (pc %#lx, thread %d): storage %#lx-%#lx of texture serial %llu %ux%u format %#x, which the GPU wrote", h->w ? "writes" : "reads", (unsigned long)h->a, (unsigned long)h->pc, h->tid,
             (unsigned long)g_rwr[h->i].lo, (unsigned long)g_rwr[h->i].hi, (unsigned long long)g_rwr[h->i].serial, g_rwr[h->i].w, g_rwr[h->i].h, g_rwr[h->i].fmt); }
    if (kb == was) return;
    was = kb; if (!kb) return;
    if (!hooked) { hooked = 1; dm_fault_hook_register(rw_fault); }
    int n = atomic_load(&g_nrwr); uint64_t bytes = 0;
    for (int i = 0; i < g_nlive && n < RW_N; i++) { vka_img *im = g_live[i];
        if (!im->gpu_owned || !im->mem || im->vpool || img_pool_dead(im) || !im->L.size) continue;
        if (kb > 0 && im->L.size > (uint64_t)kb << 10) continue;
        uintptr_t lo = ((uintptr_t)im->mem + 4095) & ~(uintptr_t)4095, hi = ((uintptr_t)im->mem + im->L.size) & ~(uintptr_t)4095;
        if (hi <= lo) continue;
        g_rwr[n].lo = lo; g_rwr[n].hi = hi; g_rwr[n].serial = im->serial; g_rwr[n].w = im->w; g_rwr[n].h = im->h; g_rwr[n].fmt = im->d.format; n++; bytes += hi - lo;
    }
    int n0 = atomic_load(&g_nrwr); atomic_store(&g_nrwr, n);
    int bad = 0; for (int i = n0; i < n; i++) if (mprotect((void *)g_rwr[i].lo, g_rwr[i].hi - g_rwr[i].lo, PROT_NONE)) bad++;
    VLOG("read watch: %d storages of GPU-written images (%.1f MiB) made inaccessible (%d failed)", n - n0, bytes / 1048576.0, bad);
}
void vka_tex_prepare(VkCommandBuffer cb) {
    bindless_init();
    nvn_pool_use(1);   // the re-uploads below read guest storage: no pool is finalized (and unmapped) under them. Taken before g_tmu, as write_tex does
    pthread_mutex_lock(&g_tmu);
    pfb_consume_locked();   // the texture IDs read through the pool in submissions done: their images stamped used, null slots written again with their images ("Texture pool reads")
    vka_tex_prune_locked();
    read_watch();
    for (int i = 0; i < g_nredo; i++) {   // CPU writes to textures that already have an image (WriteTexels): from now on they follow their storage
        smap_ent *e = map_get(g_redo[i]); vka_img *im = e ? e->im : NULL;
        if (!im || !im->cpu_dirty) continue;
        im->cpu_dirty = 0;
        if (im->gpu_owned || img_pool_dead(im)) continue;
        tex_t t = {0}; t.serial = im->serial; t.d = im->d; t.L = im->L; t.mem = (uint8_t *)im->mem;
        nvn_tex_layout(&t.d, &t.L);
        if (im->vtrack) { sparse_refresh(im, cb); g_reup++; continue; }
        if (!im->track && (im->span = nvn_dirty_span(t.mem, t.L.size))) im->track = 1;
        if (im->track) upload_tracked(im, &t, cb); else upload_fill(im, &t, cb);
        g_reup++;
    }
    g_nredo = 0;
    {   // Diagnostic (live): DM_VK_RETRACK=<n>, once per value: every image whose content comes from its storage and the GPU has not written is uploaded again
        // (1: the tracked ones, 2: those filled once too), 32 a submission
        static int was, cur = -1, nt, nf; int rt = VKA_ENV("DM_VK_RETRACK", 0);
        if (rt != was) { was = rt; cur = rt ? 0 : -1; nt = nf = 0; }
        if (cur >= 0) { int k = 0;
            for (; cur < g_nlive && k < 32; cur++) { vka_img *im = g_live[cur];
                if (im->gpu_owned || !im->mem || im->vpool || im->vtrack || img_pool_dead(im) || !im->from_mem) continue;
                if (!im->track && rt < 2) continue;
                tex_t t = {0}; t.serial = im->serial; t.d = im->d; t.L = im->L; t.mem = (uint8_t *)im->mem;
                if (im->track) { upload_tracked(im, &t, cb); nt++; } else { upload_fill(im, &t, cb); nf++; } k++; }
            if (cur >= g_nlive) { VLOG("DM_VK_RETRACK=%d: %d tracked and %d filled-once images uploaded again from their storage", rt, nt, nf); cur = -1; } }
    }
    // GPU writes whose submission is done: the images over them that did not get their bytes are uploaded again (see gw_t above)
    gml_poll();
    if (vka_nan_on()) watch_scan(cb);
    static uint64_t nprep, kinds[4], ntake; nprep++;
    if (vka_nan_on()) { for (int i = 0; i < g_ngw; i++) if (g_gw[i].fence && g_gw[i].fence <= vka_done()) kinds[g_gw[i].kind]++;
        if (nprep % 600 == 0) VLOG("NaN scan: GPU writes into guest memory done so far: %llu copies, %llu fills, %llu inline updates, %llu stores / texture → buffer copies (%llu pages from raw global memory logs, %llu logs full, %llu without a log); textures re-uploaded after them %llu", (unsigned long long)kinds[0], (unsigned long long)kinds[1], (unsigned long long)kinds[2], (unsigned long long)kinds[3], (unsigned long long)g_gml_pages, (unsigned long long)g_gml_full, (unsigned long long)g_gml_none, (unsigned long long)g_gw_late); }
    (void)ntake;
    if (g_ngw) {
        // The images whose content comes from their storage, by start address, searched for each done write (hundreds a frame, thousands of images)
        static vka_img **by; static int bycap; int nby = 0; uint64_t maxsz = 0, done = vka_done(); int k = 0, any = 0;
        for (int i = 0; i < g_ngw && !any; i++) any = g_gw[i].fence && g_gw[i].fence <= done;
        if (any) {
            if (bycap < g_nlive) { bycap = g_nlive + 256; by = realloc(by, (size_t)bycap * sizeof *by); }
            for (int j = 0; j < g_nlive; j++) { vka_img *im = g_live[j]; if (im->from_mem && !im->gpu_owned && im->mem && !im->vpool && !img_pool_dead(im)) { by[nby++] = im; if (im->L.size > maxsz) maxsz = im->L.size; } }
            qsort(by, (size_t)nby, sizeof *by, img_by_mem);
        }
        for (int i = 0; i < g_ngw; i++) {
            gw_t *w = &g_gw[i];
            if (!w->fence || w->fence > done) { g_gw[k++] = *w; continue; }
            int lo = 0, hi = nby;   // the first image starting at or after the write's end
            while (lo < hi) { int m = (lo + hi) / 2; if ((uint64_t)(uintptr_t)by[m]->mem < w->hi) lo = m + 1; else hi = m; }
            for (int j = lo - 1; j >= 0 && (uint64_t)(uintptr_t)by[j]->mem + maxsz > w->lo; j--) {
                vka_img *im = by[j]; uint64_t a = (uint64_t)(uintptr_t)im->mem;
                if (w->lo >= a + im->L.size || im->gw_seen >= w->seq || im->gpu_owned) continue;
                tex_t t = {0}; t.serial = im->serial; t.d = im->d; t.L = im->L; t.mem = (uint8_t *)im->mem;
                if (im->track) upload_tracked(im, &t, cb); else upload_fill(im, &t, cb);
                g_gw_late++; if (g_gw_late <= 8 || !(g_gw_late & (g_gw_late - 1))) VLOG("textures re-uploaded after a GPU write into their storage: %llu (uploads that applied pending GPU writes: %llu; pages raw global memory stores wrote: %llu)", (unsigned long long)g_gw_late, (unsigned long long)g_gw_over, (unsigned long long)g_gml_pages);
                if (g_gw_late <= 8) VLOG("texture serial %llu %ux%u format %#x: a GPU write into its storage (%s %#llx+%#llx) is done, re-uploaded", (unsigned long long)im->serial, im->w, im->h, im->d.format,
                                          w->kind == GW_COPY ? "copy" : w->kind == GW_FILL ? "fill" : w->kind == GW_UPDATE ? "inline update" : "store", (unsigned long long)w->lo, (unsigned long long)(w->hi - w->lo));
            }
            free(w->data);
        }
        g_ngw = k;
    }
    // Tracked images whose pages the CPU wrote since their last upload (a write to a protected page moves the dirty clock, so nothing is scanned while it stands still)
    // Sparse pools' textures also when a mapping request came since (the page table's generation moved)
    static uint64_t seen, vseen; uint64_t now = nvn_dirty_now(), vnow = nvn_sparse_gen();
    static int g_wbf_wait; static uint64_t g_wbf_up, g_wbf_waits; int g_wbf_on = VKA_ENV("DM_VK_WB_FOLLOW", 1) != 0; uint64_t wbf_done = vka_done();   // g_wbf_wait: a GPU-written linear texture waited for its copy-back in the last scan
    if (now != seen || vnow != vseen || g_wbf_wait) {
        seen = now; vseen = vnow; g_wbf_wait = 0;
        for (int i = 0; i < g_nlive; i++) {
            vka_img *im = g_live[i];
            if (img_pool_dead(im)) continue;
            if (im->vbind && im->mem) { if (nvn_sparse_since(im->vpool, (uint64_t)(uintptr_t)im->mem, im->L.size, im->vgen, im->stamp)) sparse_bind(im, cb, 0); continue; }   // (GPU-written: binds only)
            if (im->vtrack && !im->gpu_owned && im->mem) {
                if (!nvn_sparse_since(im->vpool, (uint64_t)(uintptr_t)im->mem, im->L.size, im->vgen, im->stamp)) continue;
                sparse_refresh(im, cb);
                if (g_reup++ < 8) VLOG("sparse texture serial %llu %ux%u format %#x: its mappings or pages changed, re-uploaded", (unsigned long long)im->serial, im->w, im->h, im->d.format);
                continue;
            }
            if (!im->track || !im->mem) continue;
            // A GPU-written image's content is the GPU's, except a linear one's once its copy-back has run: then the CPU's later writes to the storage are uploaded (vka_wbfollow.h,
            // Banana Native #276: Mario Tennis Fever clears its 8192x2048 R8 glyph cache on the GPU and rasterizes glyphs into it on the CPU; skipped, every text box was blank).
            // While the copy-back is queued or on the GPU the image waits, and the scan runs again at the next prepare even if the CPU writes nothing more (g_wbf_wait)
            if (!wbf_follows(im->gpu_owned, img_wb_kind(im) && g_wbf_on, im->wb_pending, im->wb_fence, wbf_done)) {
                if (g_wbf_on && im->gpu_owned && img_wb_kind(im) && (im->wb_pending || wbf_done < im->wb_fence)) { g_wbf_wait = 1; g_wbf_waits++; }
                continue;
            }
            int wbf = im->gpu_owned;
            uint64_t size = im->L.size;
            size_t lo, hi;
            if (!nvn_dirty_bounds_since(im->span, im->mem, size, im->stamp, &lo, &hi)) continue;
            tex_t t = {0}; t.serial = im->serial; t.d = im->d; t.L = im->L; t.mem = (uint8_t *)im->mem;
            if (wbf) { uint64_t k = ++g_wbf_up; if (k <= 4 || !(k & (k - 1))) VLOG("texture serial %llu %ux%u format %#x: the CPU wrote it after its copy-back, its written part uploaded (%llu so far; %llu scans waited for a copy-back; DM_VK_WB_FOLLOW=0 turns this off)",
                                                                         (unsigned long long)im->serial, im->w, im->h, im->d.format, (unsigned long long)k, (unsigned long long)g_wbf_waits); }
            int rows = VKA_ENV("DM_VK_ROWS", 1);
            // (the rows path's re-uploads were not counted in g_reup until 2026-11-07: the packaged-textures line's "re-uploads on pool content change" left out every
            // partial re-upload of a linear texture, which read as "the CPU's writes were never seen")
            if (rows && upload_rows_tracked(im, &t, cb, lo, hi)) { g_reup++; lin_note_reup(im, lo, hi, 1); continue; }
            upload_tracked(im, &t, cb); lin_note_reup(im, lo, hi, 0);
            if (g_reup++ < 8) VLOG("texture serial %llu %ux%u format %#x flags %#x target %d: its storage changed, re-uploaded", (unsigned long long)im->serial, im->w, im->h, im->d.format, im->d.flags, im->d.target);
        }
    }
    nvn_pool_use(0);   // (write_tex takes it again for each ID, after letting g_tmu go: never twice on one thread)
    if (!g_set) { g_ntd = g_nsd = 0; if (g_tdflag) memset(g_tdflag, 0, 1 << 20); if (g_sdflag) memset(g_sdflag, 0, 4096 * 4); pthread_mutex_unlock(&g_tmu); return; }
    enum { BATCH = 64 }; VkWriteDescriptorSet w[BATCH]; VkDescriptorImageInfo ii[BATCH]; int nw = 0;
    for (int k = 0; k < g_ntd; k++) {
        int id = g_tdirty[k]; g_tdflag[id] = 0;
        if (id >= (int)(g_treg[id].img ? g_nimg_slots : g_ntex_slots)) { VRED_ONCE("texture ID %d beyond the table", id); continue; }
        uint32_t g0 = atomic_load_explicit(&g_vgen, memory_order_acquire);   // (before the view is read: "Pool slot writes" below)
        write_tex(id, cb, &w[nw], &ii[nw]);
        if (!slot_write_keep(w[nw].pImageInfo)) continue;   // (a null-view write only with the pool reads reported: slot_write_keep)
        // Pool slot writes: a slot that already holds this view is not written again. Many IDs are dirtied with what they hold (every registration dirties its ID; a
        // pool read, a remade image or the blind fallback dirty many at once), and each write costs the driver, and rewrites a slot that pending command buffers may
        // read (UPDATE_UNUSED_WHILE_PENDING allows that only for descriptors they do not use dynamically, the same value or not). Correct because a binding's
        // descriptor type is fixed and its layout is always GENERAL (write_tex), so the same (binding, view handle) with no view destroyed since names the same live view:
        // a handle can be recycled only after a vkDestroyImageView, which img_destroy precedes with a bump of g_vgen and then of the handle's bucket g_vgen_h. The memo
        // stores the view's bucket generation b; a later write of the same handle with b unchanged is skipped (an old view's destroy would have bumped its bucket).
        // write_tex lets g_tmu go and may meet a concurrent img_destroy from vka_after, so the view it read may be destroyed while it runs: g_vgen is read before it
        // (g0) and after b (g1), b after write_tex. With g0 == g1 no destroy began in that window, and a destroy of the view after b's read would have bumped g_vgen
        // first (release) and b's acquire would have seen it there, so g1 would differ: the view was live when b was read (its destroy cannot have begun before g0:
        // write_tex found its image in the registry under g_tmu, held since g0, and retiring takes it out under g_tmu first). Otherwise the slot is written and its
        // memo dropped (a dead view stored with its bumped bucket would pass as unchanged once the handle comes back). Keyed per handle bucket, not on g_tgen (Erick's
        // 6e73a21), which moves at every unref, nor on g_vgen alone, which every retired image moves (most frames in a streaming game). The null view (VK_NULL_HANDLE,
        // never destroyed) is skipped the same way; null to a view and a view to null are writes. The memo's pointer is read after write_tex (treg may grow it while
        // g_tmu is let go). This relies on vka_tex_prepare being the only writer of g_set's bindings 0 and 2, and img_destroy the only place pool views are destroyed.
        // The memo is kept with the switch off too, so turning it on live is safe. DM_VK_POOL_SLOT_DEDUP=0 (live, default 1): every dirty slot written, as before
        {
            VkImageView view = ii[nw].imageView; int kind = w[nw].dstBinding == 2;
            uint32_t b = atomic_load_explicit(vgen_h(view), memory_order_acquire), g1 = atomic_load_explicit(&g_vgen, memory_order_acquire);
            pool_written *p = id < g_pool_written_cap && g_pool_written ? &g_pool_written[id] : NULL;
            if (g0 != g1) { if (p) p->valid &= (uint8_t)~(1u << kind); }
            else if (VKA_ENV("DM_VK_POOL_SLOT_DEDUP", 1) && p && (p->valid >> kind & 1) && p->view[kind] == view && p->gen[kind] == b) { g_pool_slot_skip++; continue; }
            else if (p) { p->view[kind] = view; p->gen[kind] = b; p->valid |= (uint8_t)(1u << kind); }
            g_pool_slot_write++; nw++;
        }
        if (nw == BATCH) { vk.vkUpdateDescriptorSets(V.dev, (uint32_t)nw, w, 0, NULL); nw = 0; }
    }
    g_ntd = 0;
    if (nprep % 600 == 0) { static uint64_t w0, s0;   // (the phone log's measure of the pool slot memo's hit rate)
        if (g_pool_slot_write != w0 || g_pool_slot_skip != s0) VLOG("pool slot writes: %llu written, %llu skipped as unchanged (DM_VK_POOL_SLOT_DEDUP), in the last 600 prepares",
                                                                     (unsigned long long)(g_pool_slot_write - w0), (unsigned long long)(g_pool_slot_skip - s0));
        w0 = g_pool_slot_write; s0 = g_pool_slot_skip; }
    for (int k = 0; k < g_nsd; k++) {
        int id = g_sdirty[k]; g_sdflag[id] = 0;
        if (id >= (int)g_nsamp_slots) { VRED_ONCE("sampler ID %d beyond the table (%u)", id, g_nsamp_slots); continue; }
        VkSampler s = sampler_of(&g_sreg[id], 0); if (!s) continue;
        ii[nw] = (VkDescriptorImageInfo){s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_set, .dstBinding = 1, .dstArrayElement = (uint32_t)id, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii[nw]};
        if (++nw == BATCH) { vk.vkUpdateDescriptorSets(V.dev, (uint32_t)nw, w, 0, NULL); nw = 0; }
    }
    g_nsd = 0;
    if (nw) vk.vkUpdateDescriptorSets(V.dev, (uint32_t)nw, w, 0, NULL);
    pthread_mutex_unlock(&g_tmu);
}

// ---------- The frame capture (vka_capture.c, DM_NVN_CAPTURE) and its replayer (vka_replay.c) ----------
// Read under the texture lock: a registry entry (0 = none registered at id; *n = the table's size), a sampler entry, whether the CPU wrote a serial before its image existed,
// and an image a texture would share at its first use (same storage and shape: vka_tex_get), with a reference the caller drops (vka_tex_drop)
int vka_cap_treg(int id, tex_t *t, view_t *v, uint8_t *has_v, uint8_t *img, int *n) {
    pthread_mutex_lock(&g_tmu);
    if (n) *n = g_ntreg;
    int ok = id >= 0 && id < g_ntreg && g_treg[id].t.serial;
    if (ok) { *t = g_treg[id].t; *v = g_treg[id].v; *has_v = g_treg[id].has_v; *img = g_treg[id].img; }
    pthread_mutex_unlock(&g_tmu);
    return ok;
}
int vka_cap_sreg(int id, samp_par *p, int *n) {
    static const samp_par zero;
    pthread_mutex_lock(&g_tmu);
    if (n) *n = g_nsreg;
    int ok = id >= 0 && id < g_nsreg && memcmp(&g_sreg[id], &zero, sizeof zero);
    if (ok) *p = g_sreg[id];
    pthread_mutex_unlock(&g_tmu);
    return ok;
}
int vka_cap_cpuw(uint64_t serial) {
    pthread_mutex_lock(&g_tmu);
    int r = vksm_get(&g_cpuw, serial) != NULL;
    pthread_mutex_unlock(&g_tmu);
    return r;
}
// With the pool reads reported ("Texture pool reads"), a registered texture's slot names its image only once the image is made: one a draw binds, or one whose read of
// the null view a submission reported (PFB_WANT, made at the next vka_tex_prepare). A live frame's slots name the images earlier frames asked for; a replayer that only
// registers the textures starts with the null view in every slot read through the pool alone (lightmaps behind handles in material records), and its first replay reads 0
// there. vka_cap_slot_named: whether texture ID id's slot names its image now (the capture writes it with each registry entry); vka_cap_slot_want: the replayer's ask
// for that image, taken as a reported read is (write_tex makes it at the next vka_tex_prepare, with the same first content as a live first use)
int vka_cap_slot_named(int id) {
    pthread_mutex_lock(&g_tmu);
    int r = 0;
    if (id > 0 && id < g_ntreg && g_treg[id].t.serial && !g_treg[id].img) { smap_ent *se = map_get(g_treg[id].t.serial); r = se && se->im && !(g_pfb_st && (g_pfb_st[id] & PFB_NULL)); }
    pthread_mutex_unlock(&g_tmu);
    return r;
}
int vka_cap_slot_want(int id) {
    if (pfb_avail() <= 0 || tex_eager()) return 0;   // (eager slots: write_tex makes every registered texture's image anyway)
    pthread_mutex_lock(&g_tmu);
    int r = g_pfb_st && id > 0 && id < g_ntreg && g_treg[id].t.serial && !g_treg[id].img;
    if (r) { if (!(g_pfb_st[id] & PFB_WANT)) { g_pfb_st[id] |= PFB_WANT; g_pfb_wants++; } dirty(&g_tdirty, &g_ntd, &g_tdcap, &g_tdflag, 1 << 20, id); }
    pthread_mutex_unlock(&g_tmu);
    return r;
}
vka_img *vka_cap_shared(const tex_t *t) {
    vka_fmt f; vka_img *im = NULL;
    if (!t || !t->mem || !vka_fmt_of(t->d.format, &f)) return NULL;
    pthread_mutex_lock(&g_tmu);
    for (int i = 0; i < g_nlive && !im; i++) if (same_shape(g_live[i], t, &f, 0)) { im = g_live[i]; im->refs++; im->holds++; img_touch(im); }
    pthread_mutex_unlock(&g_tmu);
    return im;
}
#endif
