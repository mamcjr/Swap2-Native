// How the Android replay makes a texture-to-texture copy (dm/gfx/vk_android/vka_t2tpath.h, after Banana Native a4a64a03): a plain copy, a blit, or a drawn depth copy
// where the driver has no blit for the formats (the Adreno 830's system driver has no BLIT_DST for its depth formats). Build: clang -I.. t_android_t2tpath.c
#include "../../gfx/vk_android/vka_t2tpath.h"
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    // exact copies are copies whatever the formats' features
    CHECK(vka_t2t_path(1, 3, 3, 0, 0, 0, 0) == VKA_T2T_COPY, "an exact depth copy without blit features is a copy");
    CHECK(vka_t2t_path(1, 1, 1, 1, 1, 0, 0) == VKA_T2T_COPY, "an exact integer, compressed, multisampled copy is a copy");
    // a scaled copy with both blit features is a blit (colour and depth)
    CHECK(vka_t2t_path(0, 0, 0, 0, 0, 1, 1) == VKA_T2T_BLIT, "scaled colour with blit features");
    CHECK(vka_t2t_path(0, 3, 3, 0, 0, 1, 1) == VKA_T2T_BLIT, "scaled depth with blit features");
    CHECK(vka_t2t_path(0, 5, 5, 0, 0, 1, 1) == VKA_T2T_BLIT, "scaled depth-stencil with blit features");
    // no blit destination (or source): a depth destination is drawn, the others are not done
    CHECK(vka_t2t_path(0, 3, 3, 0, 0, 1, 0) == VKA_T2T_DEPTH_DRAW, "depth without BLIT_DST is drawn");
    CHECK(vka_t2t_path(0, 3, 3, 0, 0, 0, 1) == VKA_T2T_DEPTH_DRAW, "depth without BLIT_SRC is drawn");
    CHECK(vka_t2t_path(0, 5, 5, 0, 0, 1, 0) == VKA_T2T_NONE, "depth-stencil without BLIT_DST is not done (no stencil from a fragment shader)");
    CHECK(vka_t2t_path(0, 4, 4, 0, 0, 1, 0) == VKA_T2T_NONE, "stencil without BLIT_DST is not done");
    CHECK(vka_t2t_path(0, 0, 0, 0, 0, 1, 0) == VKA_T2T_NONE, "colour without BLIT_DST is not done (rather than an invalid blit)");
    // what never blits
    for (int k = 1; k <= 2; k++) CHECK(vka_t2t_path(0, k, k, 0, 0, 1, 1) == VKA_T2T_NONE, "integer kind %d is not blitted", k);
    CHECK(vka_t2t_path(0, 0, 3, 0, 0, 1, 1) == VKA_T2T_NONE, "colour into depth is not done");
    CHECK(vka_t2t_path(0, 3, 5, 0, 0, 1, 1) == VKA_T2T_NONE, "depth into depth-stencil is not done");
    CHECK(vka_t2t_path(0, 0, 0, 1, 0, 1, 1) == VKA_T2T_NONE, "compressed is not blitted");
    CHECK(vka_t2t_path(0, 3, 3, 0, 1, 1, 0) == VKA_T2T_NONE, "multisampled depth is not drawn");
    printf("t_android_t2tpath: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail != 0;
}
