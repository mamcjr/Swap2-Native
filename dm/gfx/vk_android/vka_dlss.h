// NVN2's DLSS stand-in upscalers (vka_dlss.c, vka_fsr3.c) and the temporal path agl's NDSR slot can take (vka_ndsr.c, DM_VK_NDSR=gsr2). Ported from Banana Native
// (MIT; eden/v0.3.45 9c98a63b: vka_dlss.c, vka_fsr3.c, vka_exec.c dlss_eval, whose declarations sit in its vka.h); here in a header of their own. The choices made from
// numbers only (the GSR2 tune, the fallback order, the sRGB handling of the colour and the output) are plain C below (VKA_DLSS_PURE: the PC tests build them without the host)
#pragma once
#include <math.h>
#include <stdint.h>

// ---------- Plain C ----------
// GSR2's tunes (upstream's G_TUNES): motion -> current weight, motion -> box shrink, history weight scale, 3-pass box tolerance, the motion ramp (lo, hi; hi <= lo = off),
// history weight scale on the ramp (-1 = hscale), anti-flicker fade and box shrink on the ramp, Catmull-Rom history (1 everywhere, 2 along the ramp), the cap of the
// still-pixel box size (upstream 20), velocity rejection (strength, threshold and slope in pixels of a 640x360 grid), the motion of the output pixel's render texel (1)
// in place of the bilinear one (0)
typedef struct { const char *name; float motw, boxmotw, hscale, boxtol3, mlo, mhi, hsmov, afmot, bsmov, hcr, sfcap, vrej, vlo, vk, mvnear; } vka_g_tune;
static const vka_g_tune VKA_G_TUNES[] = {
    {"m1", 0.05f, 0.12f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0,     2, 20, 0,     0,     0,     0},
    {"r3", 0.03f, 0.02f, 1.25f, 0.03f, 0,    0,    -1, 0, 0,     0, 20, 0,     0,     0,     0},   // dev A/B only
    {"up", 0.05f, 0.05f, 1.0f,  0.05f, 0,    0,    -1, 0, 0,     0, 20, 0,     0,     0,     0},
    {"c0", 0.05f, 0.15f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0.5f,  2, 12, 0,     0,     0,     1},   // the clarity steps, DM_VK_GSR2_CLARITY 0..4
    {"c1", 0.05f, 0.20f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 0.75f, 2, 8,  0.25f, 0.5f,  1,     1},
    {"m2", 0.05f, 0.30f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 4,  0.5f,  0.5f,  1,     1},   // c2, the default
    {"c3", 0.05f, 0.50f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 2,  0.9f,  0.5f,  1,     1},
    {"c4", 0.05f, 0.50f, 1.25f, 0.03f, 2.0f, 8.0f, -1, 0, 1,     2, 2,  1.0f,  0.75f, 0.75f, 1},
};
enum { VKA_G_NT = sizeof VKA_G_TUNES / sizeof VKA_G_TUNES[0], VKA_G_CLARITY0 = 3 };
// The row: DM_VK_GSR2_TUNE when it names one (0..7, dev A/B), else the clarity step DM_VK_GSR2_CLARITY (clamped to 0..4, the default 2 = m2); *cl the clamped step
static inline int vka_dlss_tune_index(int tune, int clarity, int *cl) {
    clarity = clarity < 0 ? 0 : clarity > 4 ? 4 : clarity;
    if (cl) *cl = clarity;
    return tune >= 0 && tune < VKA_G_NT ? tune : VKA_G_CLARITY0 + clarity;
}
static inline float vka_dlss_ovr(float v, float o) { return isnan(o) ? v : o; }   // an override switch when set (NAN = unset)
// GSR2 upscale's sample-factor term: (output area / input area)^3 capped at the tune's sfcap
static inline float vka_dlss_gsr2_sf(int iw, int ih, int ow, int oh, float sfcap) {
    float area = (float)ow * oh / ((float)iw * ih), sr = area * area * area;
    return sr < sfcap ? sr : sfcap;
}

// The colour's and the output's sRGB handling (upstream's "The DLSS stand-in ignored sRGB textures"): the replay stores an 8-bit sRGB texture as its UNORM twin and adds
// the decode in its views, and one image may be shared by a texture and its twin, so the textures' NVN formats decide, not the images'. tex_known: the texture's NVN format
// has a Vulkan format (tex_vk, its view format tex_view, tex_srgb); img_vk / img_srgb: the image's. *sfmt: the view format the colour is read through (0: the image's own
// view format: a texture of another format than its image's); *in_srgb, *enc: the colour is sRGB (read decoded) and the output is sRGB (written encoded)
static inline void vka_dlss_srgb_plan(int ctex_known, int ctex_vk, int ctex_view, int ctex_srgb, int cimg_vk, int cimg_srgb,
                                      int otex_known, int otex_vk, int otex_srgb, int oimg_vk, int oimg_srgb, int *sfmt, int *in_srgb, int *enc) {
    if (!ctex_known) { ctex_vk = cimg_vk; ctex_view = 0; ctex_srgb = cimg_srgb; }
    if (!otex_known) { otex_vk = oimg_vk; otex_srgb = oimg_srgb; }
    *sfmt = ctex_vk == cimg_vk ? ctex_view : 0;
    *in_srgb = *sfmt ? ctex_srgb : cimg_srgb;
    *enc = otex_vk == oimg_vk ? otex_srgb : oimg_srgb;
}
// What an evaluation runs, in order (DM_VK_DLSS): 4 FSR 3.1 when it can (fsr3_ok: depth and motion at the origin, the device and sizes fit; vka_fsr3.c may still refuse);
// 3 GSR2 when it has a sampled depth and motion (have_dm); 2 FSR 1; then the bilinear scale (1), through the encoding copy where the two textures differ in sRGB.
// Returns the first mode to try after FSR 3.1 is ruled out (the caller tries 4 itself), 0 = leave the output unwritten (DM_VK_DLSS <= 0)
static inline int vka_dlss_mode_after(int mode, int have_dm) {
    if (mode <= 0) return 0;
    if (mode >= 4) mode = 2;   // FSR 3.1 refused: FSR 1, as upstream
    if (mode == 3 && !have_dm) mode = 2;
    return mode;
}

#ifndef VKA_DLSS_PURE
#include "vka.h"
// The stand-ins (vka_dlss.c): mode 2 = FSR 1 with the jitter taken out, 3 = Snapdragon GSR2 3-pass; writes the output rectangle of d (level dl) from the input rectangle of
// s (level sl), outside any render pass. Returns the mode done (3, or 2 when GSR2 lacks depth / motion), 0 = not done (the caller blits). sfmt: the input texture's view
// format (an sRGB texture's decodes; its image may be shared with the UNORM twin); enc: the output texture is sRGB (stored as its UNORM twin), so the result is written encoded
int vka_dlss_run(VkCommandBuffer cb, const p_dlss *o, int mode, vka_img *s, uint32_t sl, VkFormat sfmt, int sx, int sy, int iw, int ih, vka_img *d, uint32_t dl, int enc, int dx, int dy, int ow, int oh);
int vka_dlss_scale(VkCommandBuffer cb, VkImageView in, int sx, int sy, int iw, int ih, uint32_t tw, uint32_t th, vka_img *d, uint32_t dl, int dx, int dy, int ow, int oh, int enc);   // a scaled copy, sRGB-encoded with enc
int vka_dlss_ready(void);   // the stand-in's pipelines are made (vka_dlss_scale can run)
// vka_fsr3.c: FSR 3.1 for DM_VK_DLSS=4; 0 = not done (the caller falls back to FSR 1)
int vka_dlss_fsr3(VkCommandBuffer cb, vka_img *src, VkFormat sfmt, vka_img *dst, int enc, vka_img *depth, vka_img *motion, const p_dlss_t2t *o);
// GSR2 3-pass from views the caller made (agl's NDSR slot, vka_ndsr.c DM_VK_NDSR=gsr2): colour cv (linear, iw x ih from texel 0), reversed depth dv and motion mv (texels,
// previous minus current, DLSS's convention) at the same size, no jitter; into d's rectangle (level dl). user: who calls (a change of caller starts the history again),
// reset: start the history again. 1 = done, 0 = not done (the caller runs what it ran before)
int vka_dlss_gsr2_views(VkCommandBuffer cb, int user, VkImageView cv, VkImageView dv, VkImageView mv, int iw, int ih, int reset, vka_img *d, uint32_t dl, int enc, int dx, int dy, int ow, int oh);
// The stand-in's compute layout (4 combined samplers at 0..3, 2 storage images at 4, 5, 128 bytes of push constants) for a caller's own pass of that shape
// (vka_ndsr.c's motion pass): one dispatch of pipeline p over w x h in 8x8 groups, then a full barrier. VK_NULL_HANDLE / 0 when the stand-in is not ready
VkPipelineLayout vka_dlss_layout(void);
void vka_dlss_pass(VkCommandBuffer cb, VkPipeline p, const VkImageView in[4], int linear, VkImageView o0, VkImageView o1, const void *pc, uint32_t w, uint32_t h);
// An RGBA16F work image of the stand-in's kind (storage, sampled, blit source, cleared to 0 when made); 2 = made now, 1 = there, 0 = failed. The old one goes once the GPU is past it
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; } vka_dlss_wimg;
int vka_dlss_wimg_get(VkCommandBuffer cb, vka_dlss_wimg *w, uint32_t width, uint32_t height);
#endif
