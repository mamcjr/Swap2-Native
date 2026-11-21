// NVN2's DLSS stand-in's choices made from numbers (dm/gfx/vk_android/vka_dlss.h; vka_dlss.c, vka_exec.c dlss_eval, ported from Banana Native eden/v0.3.45): the GSR2 tune
// row a switch pair picks, the override rule, the upscale pass's sample-factor cap, the fallback order of the modes, and the sRGB handling of the colour and the output
// by the textures' NVN formats (Star Fox: an RGBA8 sRGB colour into an RGBA16F output, blitted raw before, too bright)
#include <stdio.h>
#include <string.h>
#define VKA_DLSS_PURE
#include "../../gfx/vk_android/vka_dlss.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// VkFormat numbers (vulkan_core.h)
enum { R8G8B8A8_UNORM = 37, R8G8B8A8_SRGB = 43, R16G16B16A16_SFLOAT = 97, B10G11R11_UFLOAT = 122, R32_UINT = 98 };

static void tunes(void) {
    static const char *steps[5] = {"c0", "c1", "m2", "c3", "c4"};
    int cl = -1;
    CHECK(VKA_G_NT == 8, "%d rows", VKA_G_NT);
    CHECK(!strcmp(VKA_G_TUNES[vka_dlss_tune_index(-1, 2, &cl)].name, "m2") && cl == 2, "the default is m2 (clarity 2)");
    for (int c = 0; c < 5; c++) CHECK(!strcmp(VKA_G_TUNES[vka_dlss_tune_index(-1, c, NULL)].name, steps[c]), "clarity %d is %s", c, steps[c]);
    CHECK(vka_dlss_tune_index(-1, 9, &cl) == VKA_G_CLARITY0 + 4 && cl == 4, "clarity clamped to 4");
    CHECK(vka_dlss_tune_index(-1, -3, &cl) == VKA_G_CLARITY0 && cl == 0, "clarity clamped to 0");
    CHECK(vka_dlss_tune_index(0, 2, NULL) == 0 && !strcmp(VKA_G_TUNES[0].name, "m1"), "DM_VK_GSR2_TUNE=0 forces m1");
    CHECK(vka_dlss_tune_index(7, 0, NULL) == 7, "DM_VK_GSR2_TUNE=7 forces c4");
    CHECK(vka_dlss_tune_index(8, 1, NULL) == VKA_G_CLARITY0 + 1, "a tune past the table: the clarity step");
    // m2 (upstream's measured default): velocity rejection at half strength, nearest-texel motion, still-pixel box cap 4
    const vka_g_tune *m2 = &VKA_G_TUNES[VKA_G_CLARITY0 + 2];
    CHECK(m2->vrej == 0.5f && m2->mvnear == 1 && m2->sfcap == 4 && m2->boxmotw == 0.30f, "m2's fields");
    CHECK(vka_dlss_ovr(1.5f, NAN) == 1.5f && vka_dlss_ovr(1.5f, 0.25f) == 0.25f, "an override only when set");
}

static void sample_factor(void) {
    // 768x432 -> 1920x1080: the area ratio 6.25, cubed 244: capped at the tune's sfcap
    CHECK(vka_dlss_gsr2_sf(768, 432, 1920, 1080, 4) == 4, "capped");
    CHECK(vka_dlss_gsr2_sf(768, 432, 1920, 1080, 20) == 20, "upstream's cap");
    float one = vka_dlss_gsr2_sf(768, 432, 768, 432, 4);
    CHECK(one > 0.999f && one < 1.001f, "1:1 (DLAA, Star Fox's resized network): %g", one);
    float q = vka_dlss_gsr2_sf(960, 540, 1152, 648, 20);   // 1.2x linear: 1.44^3 = 2.99
    CHECK(q > 2.98f && q < 3.0f, "1.2x: %g", q);
}

static void modes(void) {
    CHECK(vka_dlss_mode_after(0, 1) == 0 && vka_dlss_mode_after(-1, 1) == 0, "0: the output unwritten");
    CHECK(vka_dlss_mode_after(1, 1) == 1, "1: the bilinear scale");
    CHECK(vka_dlss_mode_after(2, 0) == 2 && vka_dlss_mode_after(3, 1) == 3, "FSR 1 and GSR2 as asked");
    CHECK(vka_dlss_mode_after(3, 0) == 2, "GSR2 without depth or motion: FSR 1");
    CHECK(vka_dlss_mode_after(4, 1) == 2 && vka_dlss_mode_after(4, 0) == 2, "FSR 3.1 refused: FSR 1");
}

static void srgb(void) {
    int sf, in, enc;
    // Star Fox (log 2026-11-18: NVN 0x38 -> 0x29): RGBA8 sRGB colour, stored as RGBA8 UNORM with an sRGB view; RGBA16F output
    vka_dlss_srgb_plan(1, R8G8B8A8_UNORM, R8G8B8A8_SRGB, 1, R8G8B8A8_UNORM, 1, 1, R16G16B16A16_SFLOAT, 0, R16G16B16A16_SFLOAT, 0, &sf, &in, &enc);
    CHECK(sf == R8G8B8A8_SRGB && in == 1 && enc == 0, "Star Fox: read through the decoding view (%d %d %d)", sf, in, enc);
    CHECK(in != enc, "Star Fox: no raw blit (the old stand-in's too-bright picture)");
    // Fire Emblem Fortune's Weave: both linear (its picture was right in every mode upstream)
    vka_dlss_srgb_plan(1, R16G16B16A16_SFLOAT, R16G16B16A16_SFLOAT, 0, R16G16B16A16_SFLOAT, 0, 1, R16G16B16A16_SFLOAT, 0, R16G16B16A16_SFLOAT, 0, &sf, &in, &enc);
    CHECK(sf == R16G16B16A16_SFLOAT && !in && !enc, "linear to linear (%d %d %d)", sf, in, enc);
    // the UNORM texture over an image an sRGB texture made (one image, two textures): the texture decides, not the image
    vka_dlss_srgb_plan(1, R8G8B8A8_UNORM, R8G8B8A8_UNORM, 0, R8G8B8A8_UNORM, 1, 1, R8G8B8A8_UNORM, 1, R8G8B8A8_UNORM, 0, &sf, &in, &enc);
    CHECK(sf == R8G8B8A8_UNORM && !in && enc == 1, "a UNORM colour, an sRGB output texture: written encoded (%d %d %d)", sf, in, enc);
    // a texture of another format than its image's (a learned view): the image's own view and sRGB-ness
    vka_dlss_srgb_plan(1, R32_UINT, R32_UINT, 0, R8G8B8A8_UNORM, 1, 1, B10G11R11_UFLOAT, 0, B10G11R11_UFLOAT, 0, &sf, &in, &enc);
    CHECK(sf == 0 && in == 1 && !enc, "another format: the image's (%d %d %d)", sf, in, enc);
    // an NVN format with no Vulkan format: the images'
    vka_dlss_srgb_plan(0, 0, 0, 0, R8G8B8A8_UNORM, 1, 0, 0, 0, R8G8B8A8_UNORM, 1, &sf, &in, &enc);
    CHECK(sf == 0 && in == 1 && enc == 1, "unknown NVN formats: the images' (%d %d %d)", sf, in, enc);
}

int main(void) {
    tunes(); sample_factor(); modes(); srgb();
    if (fails) { printf("t_android_dlss: %d failure(s)\n", fails); return 1; }
    printf("t_android_dlss: ok\n");
    return 0;
}
