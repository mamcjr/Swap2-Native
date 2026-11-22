// SPIR-V rewrites made by the replay at pipeline time (vka_spvpatch.c). Plain C, no Vulkan
#pragma once
#include <stddef.h>
#include <stdint.h>
// Per-primitive fragment values duplicated by the mesh fallback's pulling vertex stage:
// PerPrimitiveEXT becomes Flat and the mesh extension/capability are removed in place.
int vka_spv_mesh_fragment(uint32_t *w, size_t *nw);
// The vertex module w (nw words) with the vec4 float Input variables at the locations of umask retyped as uvec4 and those of smask as ivec4, every load of them bit-cast back
// to float: the shader then sees the integer register bits Maxwell's fetch gives an integer format. A malloc'd copy (*out_nw words), or NULL with the reason in err
uint32_t *vka_spv_vin_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// Subgroup operations and built-ins rewritten as subgroups of one invocation, for a stage the device runs none in (vka_spvpatch.c). A malloc'd copy (*out_nw words,
// *changed = 1), NULL with err empty when the module has none, NULL with the reason in err when it cannot be rewritten
uint32_t *vka_spv_subgroup_single(const uint32_t *w, size_t nw, size_t *out_nw, int *changed, char *err, size_t errn);
// A geometry shader's memory writes, counted (the return; -1 with err on failure) and, with out and a count above 0, the module with each of them made only by the run of
// the shader that first claims its primitive in the storage buffer at set 0 binding (zeroed for each draw, a word per primitive), malloc'd (*out_nw words): vka_spvpatch.c
int vka_spv_gs_claim(const uint32_t *w, size_t nw, uint32_t binding, uint32_t **out, size_t *out_nw, char *err, size_t errn);
// A geometry shader that may end without emitting anything made to emit one degenerate primitive instead (Adreno's binning and rendering passes; vka_spvpatch.c,
// from Banana Native 0749171c). A malloc'd copy (*out_nw words); NULL with err empty: nothing to do (not such a shader); NULL with the reason in err: not rewritten
uint32_t *vka_spv_gs_fill(const uint32_t *w, size_t nw, size_t *out_nw, char *err, size_t errn);
// A pixel shader's float colour outputs at the locations of umask / smask as unsigned / signed integers of their bits (each store bit-cast), for integer attachments: malloc'd
// (*out_nw words), NULL with the reason in err (vka_spvpatch.c)
uint32_t *vka_spv_fout_retype(const uint32_t *w, size_t nw, uint32_t umask, uint32_t smask, size_t *out_nw, char *err, size_t errn);
// An image to retype (vka_spv_img_retype): the variable at descriptor set / binding, its class (1 unsigned, 2 signed integer) and, for a storage image, the SPIR-V Image
// Format to declare (0: keep Unknown; else one of that class: Rgba32i 21 ... R8i 29, Rgba32ui 30 ... R8ui 39). A sampled image takes 0 (it keeps Unknown)
typedef struct { uint32_t set, binding; uint8_t cls, fmt; } vka_spv_imgsel;
// The float storage images of format Unknown and the float sampled images (combined or separate; or arrays of them) at sel (nsel, at most 8) as integer images of that
// class (and format), each read, fetch, sample and gather bit-cast back to float and each written texel bit-cast to the integer vector, for integer textures bound to
// formatted surface operations and texture reads; a depth comparison on one is refused: malloc'd (*out_nw words), NULL with the reason in err (vka_spvpatch.c)
uint32_t *vka_spv_img_retype(const uint32_t *w, size_t nw, const vka_spv_imgsel *sel, int nsel, size_t *out_nw, char *err, size_t errn);
// Texture-use feedback for the texture pool (vka_spvpatch.c vka_spv_pool_fb, ported from Banana Native v0.3.11): before each read of the pool (a one-index access chain
// into the sampled-image array at set `set` binding `tex_binding`) the module sets bit (id & 31) of word id >> 5 of a storage buffer at set `fb_set` binding `fb_binding`,
// id the index's low 20 bits: an atomic OR when a load finds the bit clear. flags: VKA_PFB_VTX_STORES (the device has vertexPipelineStoresAndAtomics: vertex, tessellation
// and geometry modules may take it), VKA_PFB_EARLY (a pixel shader whose tests can give the same result early gets EarlyFragmentTests: its stores would otherwise move its
// depth test after shading). alpha_spec: the SpecId of the alpha test's function (DMSH_VK_SPEC_ALPHA_FUNC). A malloc'd copy (*out_nw words; *sites the reads given code;
// *early 0 no mode added, 1 added and right for every pipeline without alpha to coverage, 2 added and right only with the alpha test off too); NULL with err empty when
// the module reads no pool, NULL with the reason in err when it cannot take the code (the module is then used as it is: its pool reads report nothing)
enum { VKA_PFB_VTX_STORES = 1, VKA_PFB_EARLY = 2 };
// Whether a pixel shader's only effect is its colour outputs: no discard, demote or terminate, no depth, sample mask or stencil export (built-in or execution mode), no
// write to memory (buffers, images, atomics; a store through a pointer of unknown class counts). 1: always; 2: when the alpha test is off (its one discard is the module's
// only one, under the SpecId alpha_spec constant: DMSH_VK_SPEC_ALPHA_FUNC); 0: it has another effect, or the module is not a pixel shader or cannot be read. A pipeline
// whose colour writes are all masked can then leave such a shader out (vka_pso.c "Pixel shaders with nothing to do"): its depth test, depth writes and occlusion counts
// are the rasterizer's alone
int vka_spv_fs_inert(const uint32_t *w, size_t nw, uint32_t alpha_spec);
uint32_t *vka_spv_pool_fb(const uint32_t *w, size_t nw, uint32_t set, uint32_t tex_binding, uint32_t fb_set, uint32_t fb_binding, unsigned flags, uint32_t alpha_spec, size_t *out_nw, int *sites, int *early, char *err, size_t errn);

// A descriptor array at (set, binding) declared with more than len elements gets len (vka_spvpatch.c vka_spv_array_len: a device whose pool table is shorter than the
// translator's 2^20). The rewritten module (malloc), or NULL when nothing changes
uint32_t *vka_spv_array_len(const uint32_t *w, size_t nw, uint32_t set, uint32_t binding, uint32_t len, size_t *out_nw);

// Every subgroup shuffle as a ShuffleXor by the invocation's lane ^ its index, which reads the same lane (vka_spvpatch.c, from DroidPlay Core 8115593e: Qualcomm's V@07xx
// compilers crash on the shuffle). A malloc'd copy (*out_nw words, *sites shuffles rewritten); NULL with err empty when the module has no shuffle, NULL with the reason in err
// when it cannot be rewritten (no SubgroupLocalInvocationId input or 32-bit unsigned type)
uint32_t *vka_spv_shuffle_xor(const uint32_t *w, size_t nw, size_t *out_nw, int *sites, char *err, size_t errn);

// Each subgroup shuffle of lane (SubgroupLocalInvocationId & -32) | k (k a constant) as two broadcasts from the constant lanes k and 32 + k, chosen by bit 32 of the lane
// (vka_spvpatch.c): the same value on a 64-lane subgroup without ir3's loop of rotates. A malloc'd copy (*out_nw words, *sites shuffles rewritten); NULL with err empty when
// no shuffle has that form, NULL with the reason in err when the module cannot be rewritten
uint32_t *vka_spv_shuffle_halves(const uint32_t *w, size_t nw, size_t *out_nw, int *sites, char *err, size_t errn);
