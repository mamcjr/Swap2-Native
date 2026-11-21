// How the replay makes a texture-to-texture copy (vka_exec.c copy_t2t), after Banana Native a4a64a03 (MIT): a plain copy, a blit, or a draw for a scaled or mirrored depth
// copy the driver cannot blit. Plain C, no Vulkan: t_android_t2tpath.c checks the rule
#pragma once

enum { VKA_T2T_NONE = 0, VKA_T2T_COPY = 1, VKA_T2T_BLIT = 2, VKA_T2T_DEPTH_DRAW = 3 };
// exact: a plain copy is possible (same size, no mirror, same texel blocks and samples); skind, dkind: vka_fmt.kind of the source and the destination (0 float / unorm /
// snorm, 1 uint, 2 sint, 3 depth, 4 stencil, 5 depth-stencil); compressed: either is block-compressed; ms: either is multisampled; blit_src, blit_dst: the source's VkFormat
// has VK_FORMAT_FEATURE_BLIT_SRC_BIT and the destination's VK_FORMAT_FEATURE_BLIT_DST_BIT (optimal tiling).
// A blit without both is invalid (VUID-vkCmdBlitImage-srcImage-01999, dstImage-02000), and Vulkan requires neither for depth formats: the Adreno 830's system driver has no
// BLIT_DST for D16, D32, D24S8 or D32S8 (Banana Native a4a64a03), and Mali and Xclipse drivers need not have them either. A depth destination is then drawn (a fragment
// shader writing the sampled depth); a stencil or depth-stencil one is not done (a fragment shader writes no stencil), nor is a colour one (no game seen needs it)
static inline int vka_t2t_path(int exact, int skind, int dkind, int compressed, int ms, int blit_src, int blit_dst) {
    if (exact) return VKA_T2T_COPY;
    if (skind != dkind || (skind && skind < 3) || compressed || ms) return VKA_T2T_NONE;
    if (!blit_src || !blit_dst) return dkind == 3 ? VKA_T2T_DEPTH_DRAW : VKA_T2T_NONE;
    return VKA_T2T_BLIT;
}
