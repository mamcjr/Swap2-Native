// BC1-BC7 textures on a GPU without them (textureCompressionBC off: every ARM Mali and Samsung Xclipse; vka_tex.c fmt_of_slow). The image is made in an uncompressed
// format and the blocks are decoded on the CPU where they were copied to the GPU as they are: BC1 / BC2 / BC3 / BC7 into RGBA8 (their sRGB forms through the image's
// sRGB view, as for an RGBA8 texture), BC4 into R8 and BC5 into RG8 (UNORM or SNORM, which sample as the compressed formats do: 0 in the channels they lack, 1 in alpha),
// BC6H into RGBA16F with alpha 1. A vka_fmt keeps the blocks' own layout (bw, bh, bpb: what the game's storage and its copies are made of) and says the decoded form in
// dec and tb (the texel's bytes). The decoder is bcdec (dm/third_party/bcdec, MIT or public domain), the one DroidPlay Core uses for the same purpose.
// Cost: the decoded image takes 4 (BC7, BC3, BC5) to 8 (BC1, BC4, BC6H) times the blocks' memory, and the decode is CPU time at each upload
#pragma once
#include <stdint.h>
#include <string.h>
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"

enum { VKA_BC1 = 1, VKA_BC2, VKA_BC3, VKA_BC4U, VKA_BC4S, VKA_BC5U, VKA_BC5S, VKA_BC6U, VKA_BC6S, VKA_BC7 };

// The decode a BC VkFormat takes (0 = not BC) with the image's format and its texel's bytes
static inline int vka_bc_kind(VkFormat f, VkFormat *to, uint8_t *tb) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: *to = VK_FORMAT_R8G8B8A8_UNORM; *tb = 4; return VKA_BC1;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: *to = VK_FORMAT_R8G8B8A8_UNORM; *tb = 4; return VKA_BC2;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: *to = VK_FORMAT_R8G8B8A8_UNORM; *tb = 4; return VKA_BC3;
    case VK_FORMAT_BC4_UNORM_BLOCK: *to = VK_FORMAT_R8_UNORM; *tb = 1; return VKA_BC4U;
    case VK_FORMAT_BC4_SNORM_BLOCK: *to = VK_FORMAT_R8_SNORM; *tb = 1; return VKA_BC4S;
    case VK_FORMAT_BC5_UNORM_BLOCK: *to = VK_FORMAT_R8G8_UNORM; *tb = 2; return VKA_BC5U;
    case VK_FORMAT_BC5_SNORM_BLOCK: *to = VK_FORMAT_R8G8_SNORM; *tb = 2; return VKA_BC5S;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: *to = VK_FORMAT_R16G16B16A16_SFLOAT; *tb = 8; return VKA_BC6U;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: *to = VK_FORMAT_R16G16B16A16_SFLOAT; *tb = 8; return VKA_BC6S;
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: *to = VK_FORMAT_R8G8B8A8_UNORM; *tb = 4; return VKA_BC7;
    default: return 0;
    }
}
static inline uint32_t vka_bc_block_bytes(int dec) { return dec == VKA_BC1 || dec == VKA_BC4U || dec == VKA_BC4S ? 8 : 16; }
static inline uint32_t vka_bc_texel_bytes(int dec) { return dec == VKA_BC4U || dec == VKA_BC4S ? 1 : dec == VKA_BC5U || dec == VKA_BC5S ? 2 : dec == VKA_BC6U || dec == VKA_BC6S ? 8 : 4; }

// Block rows [0, rows) of bx blocks, src_pitch bytes a block row, into texels: 4 * rows rows of 4 * bx texels, dst_pitch bytes a texel row (at least 4 * bx texels)
static inline void vka_bc_rows(int dec, const uint8_t *src, size_t src_pitch, uint32_t bx, uint32_t rows, uint8_t *dst, size_t dst_pitch) {
    const uint32_t bb = vka_bc_block_bytes(dec), tb = vka_bc_texel_bytes(dec);
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t *s = src + (size_t)y * src_pitch; uint8_t *d = dst + (size_t)y * 4 * dst_pitch;
        for (uint32_t x = 0; x < bx; x++, s += bb, d += 4 * tb) {
            switch (dec) {
            case VKA_BC1: bcdec_bc1(s, d, (int)dst_pitch); break;
            case VKA_BC2: bcdec_bc2(s, d, (int)dst_pitch); break;
            case VKA_BC3: bcdec_bc3(s, d, (int)dst_pitch); break;
            case VKA_BC7: bcdec_bc7(s, d, (int)dst_pitch); break;
            case VKA_BC4U: bcdec_bc4(s, d, (int)dst_pitch, 0); break;
            case VKA_BC4S: bcdec_bc4(s, d, (int)dst_pitch, 1); break;
            case VKA_BC5U: bcdec_bc5(s, d, (int)dst_pitch, 0); break;
            case VKA_BC5S: bcdec_bc5(s, d, (int)dst_pitch, 1); break;
            case VKA_BC6U: case VKA_BC6S: {   // three halves a texel from the decoder; the image has four (alpha 1.0)
                uint16_t h[4 * 4 * 3]; bcdec_bc6h_half(s, h, 4 * 3, dec == VKA_BC6S);
                for (int r = 0; r < 4; r++) { uint16_t *o = (uint16_t *)(void *)(d + (size_t)r * dst_pitch);
                    for (int c = 0; c < 4; c++) { o[4 * c] = h[(r * 4 + c) * 3]; o[4 * c + 1] = h[(r * 4 + c) * 3 + 1]; o[4 * c + 2] = h[(r * 4 + c) * 3 + 2]; o[4 * c + 3] = 0x3C00; } }
                break; }
            default: break;
            }
        }
    }
}
