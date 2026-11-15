// BC blocks decoded on the CPU for a GPU without BC textures (dm/gfx/vk_android/vka_bcdec.h): the format each BC format decodes into, and vka_bc_rows' layout: blocks
// in rows of bx at a pitch of their own, texels out at a pitch of their own, BC4 / BC5 as one and two bytes, BC6H's three halves widened to four with alpha 1.0
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include "../../gfx/vk_android/vka_bcdec.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    VkFormat to; uint8_t tb;
    CHECK(vka_bc_kind(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, &to, &tb) == VKA_BC1 && to == VK_FORMAT_R8G8B8A8_UNORM && tb == 4, "BC1 into RGBA8");
    CHECK(vka_bc_kind(VK_FORMAT_BC4_SNORM_BLOCK, &to, &tb) == VKA_BC4S && to == VK_FORMAT_R8_SNORM && tb == 1, "BC4 SNORM into R8 SNORM");
    CHECK(vka_bc_kind(VK_FORMAT_BC5_UNORM_BLOCK, &to, &tb) == VKA_BC5U && to == VK_FORMAT_R8G8_UNORM && tb == 2, "BC5 into RG8");
    CHECK(vka_bc_kind(VK_FORMAT_BC6H_UFLOAT_BLOCK, &to, &tb) == VKA_BC6U && to == VK_FORMAT_R16G16B16A16_SFLOAT && tb == 8, "BC6H into RGBA16F");
    CHECK(vka_bc_kind(VK_FORMAT_BC7_SRGB_BLOCK, &to, &tb) == VKA_BC7 && to == VK_FORMAT_R8G8B8A8_UNORM && tb == 4, "BC7 into RGBA8");
    CHECK(!vka_bc_kind(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &to, &tb) && !vka_bc_kind(VK_FORMAT_R8G8B8A8_UNORM, &to, &tb), "ASTC and RGBA8 are not decoded");
    CHECK(vka_bc_block_bytes(VKA_BC1) == 8 && vka_bc_block_bytes(VKA_BC4S) == 8 && vka_bc_block_bytes(VKA_BC7) == 16 && vka_bc_block_bytes(VKA_BC5U) == 16, "block sizes");

    {   // BC1: two block rows of three blocks, the blocks 24 bytes apart in a row of 80 (a pitch with padding); block (x, y) is one colour: red, green, blue / white, black, red
        static const uint16_t col[2][3] = {{0xF800, 0x07E0, 0x001F}, {0xFFFF, 0x0000, 0xF800}};
        static const uint8_t want[2][3][3] = {{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}}, {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}}};
        uint8_t src[2 * 80] = {0}, dst[8 * 64]; memset(dst, 0xAA, sizeof dst);
        for (int y = 0; y < 2; y++) for (int x = 0; x < 3; x++) { uint8_t *b = src + y * 80 + x * 8; b[0] = b[2] = (uint8_t)col[y][x]; b[1] = b[3] = (uint8_t)(col[y][x] >> 8); }   // colour0 = colour1, indices 0
        vka_bc_rows(VKA_BC1, src, 80, 3, 2, dst, 64);
        int ok = 1;
        for (int ty = 0; ty < 8; ty++) for (int tx = 0; tx < 12; tx++) { const uint8_t *p = dst + ty * 64 + tx * 4, *w = want[ty / 4][tx / 4]; if (p[0] != w[0] || p[1] != w[1] || p[2] != w[2] || p[3] != 255) ok = 0; }
        CHECK(ok, "BC1 blocks land in their texels, opaque");
        for (int ty = 0; ty < 8; ty++) for (int k = 48; k < 64; k++) if (dst[ty * 64 + k] != 0xAA) { ok = 0; }
        CHECK(ok, "nothing is written past a row's texels");
    }
    {   // BC4: endpoints 200 and 10, all indices 0 (the first endpoint), unsigned; signed: endpoints 100 and -100 (0x9C), indices 0
        uint8_t u[8] = {200, 10, 0, 0, 0, 0, 0, 0}, sg[8] = {100, 0x9C, 0, 0, 0, 0, 0, 0}, d[16];
        vka_bc_rows(VKA_BC4U, u, 8, 1, 1, d, 4); int ok = 1; for (int i = 0; i < 16; i++) ok &= d[i] == 200;
        CHECK(ok, "BC4 UNORM: one byte a texel");
        vka_bc_rows(VKA_BC4S, sg, 8, 1, 1, d, 4); ok = 1; for (int i = 0; i < 16; i++) ok &= (int8_t)d[i] == 100;
        CHECK(ok, "BC4 SNORM: a signed byte a texel");
    }
    {   // BC5: red 50, green 250
        uint8_t b[16] = {50, 0, 0, 0, 0, 0, 0, 0, 250, 0, 0, 0, 0, 0, 0, 0}, d[32]; vka_bc_rows(VKA_BC5U, b, 16, 1, 1, d, 8);
        int ok = 1; for (int i = 0; i < 16; i++) ok &= d[2 * i] == 50 && d[2 * i + 1] == 250;
        CHECK(ok, "BC5: two bytes a texel, red then green");
    }
    {   // BC6H: an all-zero block decodes to black; the fourth half is 1.0, and the rows keep to their pitch
        uint8_t b[16] = {0};
        uint16_t e[4 * 20]; for (int i = 0; i < 80; i++) e[i] = 0xBEEF;
        vka_bc_rows(VKA_BC6U, b, 16, 1, 1, (uint8_t *)e, 20 * 2);
        int ok = 1; for (int r = 0; r < 4; r++) { for (int c = 0; c < 4; c++) ok &= e[r * 20 + 4 * c] == 0 && e[r * 20 + 4 * c + 1] == 0 && e[r * 20 + 4 * c + 2] == 0 && e[r * 20 + 4 * c + 3] == 0x3C00;
                                                  for (int k = 16; k < 20; k++) ok &= e[r * 20 + k] == 0xBEEF; }
        CHECK(ok, "BC6H: RGBA halves, alpha 1.0, inside the row");
    }
    printf("t_android_bcdec: %s\n", fails ? "FAILED" : "ok");
    return fails != 0;
}
