// A scaled or mirrored depth copy drawn instead of blitted (vka_t2tpath.h VKA_T2T_DEPTH_DRAW), included by vka_exec.c copy_t2t. Ported from Banana Native a4a64a03 (MIT,
// vka_exec.c t2t_depth_draw) to this replay's images and views: per layer, a full-viewport triangle over the destination rectangle (the present's rot.vert, its texture
// coordinates an affine map onto the source rectangle, so a mirror is a negative scale) whose fragment shader (vka_depthcopy_spv.h) samples the source level nearest, as
// the blit did, and writes gl_FragDepth with the depth test ALWAYS. Push descriptors; one pipeline per depth format. Mario Tennis Fever fills its 7168x7168 shadow map's
// level 1 with a scaled copy of level 0; blitted on a driver without BLIT_DST for D32 (the Adreno 830's system driver) that level stayed unwritten and far shadows were lost.
// DM_VK_DEPTH_COPY_DRAW (live): 1 (default) a depth copy is drawn where the driver cannot blit it; 2 every scaled depth copy is drawn (an A/B on a driver that blits); 0 never
// (such a copy is then not done, with a red line, rather than an invalid blit)
#pragma once
#include "vka.h"
#include "vka_present_rot.h"
#include "vka_depthcopy_spv.h"
#include <stdatomic.h>

static struct { VkFormat f; VkPipeline p; } g_dcp[8]; static int g_ndcp, g_dcp_broken;
static VkPipelineLayout g_dcp_lay; static VkDescriptorSetLayout g_dcp_dsl; static VkShaderModule g_dcp_vs, g_dcp_fs; static VkSampler g_dcp_smp;
static VkPipeline vka_dcp_pipeline(VkFormat f) {
    for (int i = 0; i < g_ndcp; i++) if (g_dcp[i].f == f) return g_dcp[i].p;
    if (g_ndcp == 8 || g_dcp_broken || !V.push_desc) return VK_NULL_HANDLE;
    VkResult r = 0;
    if (!g_dcp_lay) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b};
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)};
        VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_rot_vert, .pCode = vka_rot_vert};
        VkShaderModuleCreateInfo mf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_depthcopy_frag, .pCode = vka_depthcopy_frag};
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (!(r = vk.vkCreateDescriptorSetLayout(V.dev, &dl, NULL, &g_dcp_dsl))) {
            VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dcp_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
            r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_dcp_lay);
        }
        if (!r) r = vk.vkCreateShaderModule(V.dev, &mv, NULL, &g_dcp_vs);
        if (!r) r = vk.vkCreateShaderModule(V.dev, &mf, NULL, &g_dcp_fs);
        if (!r) r = vk.vkCreateSampler(V.dev, &si, NULL, &g_dcp_smp);
        if (r) { VRED("scaled depth copy: layout / shaders / sampler %d", r); g_dcp_broken = 1; return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_dcp_vs, .pName = "main"},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_dcp_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = f};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_dcp_lay};
    VkPipeline p = VK_NULL_HANDLE;
    if ((r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p))) { VRED_ONCE("scaled depth copy pipeline (format %d): %d", f, r); return VK_NULL_HANDLE; }
    g_dcp[g_ndcp++].f = f; g_dcp[g_ndcp - 1].p = p;
    return p;
}
// ss, ds: the copy's subresources (one level each, layerCount layers); x0..y1: the source rectangle (swapped for a mirror); dr: the destination rectangle.
// 1 = drawn (the caller marks the destination written and ends with its barrier), 0 = not done (logged)
static int vka_t2t_depth_draw(VkCommandBuffer cb, vka_img *s, vka_img *d, VkImageSubresourceLayers ss, VkImageSubresourceLayers ds, int x0, int y0, int x1, int y1, VkRect2D dr) {
    if (!(s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(d->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || s->depth > 1 || d->depth > 1 || ss.layerCount != ds.layerCount) {
        VRED_ONCE("scaled depth copy (serial %llu -> %llu): the source cannot be sampled, the destination cannot be a depth attachment, or a 3D / layer-count mismatch: not done",
                  (unsigned long long)s->serial, (unsigned long long)d->serial); return 0; }
    if (s->img == d->img && ss.mipLevel == ds.mipLevel) { VRED_ONCE("scaled depth copy within one level of one image: not done"); return 0; }
    VkPipeline p = vka_dcp_pipeline(d->f.vk);
    if (!p) { VRED_ONCE("scaled depth copy (format %d): no pipeline (push descriptors: %d): not done", d->f.vk, V.push_desc); return 0; }
    uint32_t sw = s->w >> ss.mipLevel ? s->w >> ss.mipLevel : 1, sh = s->h >> ss.mipLevel ? s->h >> ss.mipLevel : 1;
    float pc[6] = {(float)(x1 - x0) / sw, 0, 0, (float)(y1 - y0) / sh, (float)x0 / sw, (float)y0 / sh};   // uv = m * p + t, p = 0..1 over the viewport
    VkViewport v = {(float)dr.offset.x, (float)dr.offset.y, (float)dr.extent.width, (float)dr.extent.height, 0, 1};
    vka_barrier(cb);   // the source's writes before its reads in the fragment stage
    for (uint32_t i = 0; i < ds.layerCount; i++) {
        VkImageView sv = vka_view(s, s->f.view ? s->f.view : s->f.vk, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, ss.mipLevel, 1, ss.baseArrayLayer + i, 1, NULL);
        VkImageView dv = vka_view(d, d->f.vk, VK_IMAGE_VIEW_TYPE_2D, d->aspect, ds.mipLevel, 1, ds.baseArrayLayer + i, 1, NULL);
        if (!sv || !dv) { vka_dyn_invalidate(); return i > 0; }
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = dr, .layerCount = 1, .pDepthAttachment = &a};
        VkDescriptorImageInfo ii = {g_dcp_smp, sv, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
        vk.vkCmdBeginRendering(cb, &ri);
        vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vk.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dcp_lay, 0, 1, &wr);
        vk.vkCmdPushConstants(cb, g_dcp_lay, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        vk.vkCmdSetViewport(cb, 0, 1, &v); vk.vkCmdSetScissor(cb, 0, 1, &dr);
        vk.vkCmdDraw(cb, 3, 1, 0, 0);
        vk.vkCmdEndRendering(cb);
    }
    vka_dyn_invalidate();   // its pipeline replaced the draws' state
    static _Atomic int n; if (!atomic_exchange(&n, 1)) VLOG("scaled depth copy drawn (format %d, blit %s): serial %llu level %u -> serial %llu level %u, %ux%u (DM_VK_DEPTH_COPY_DRAW)", d->f.vk,
        VKA_ENV("DM_VK_DEPTH_COPY_DRAW", 1) >= 2 ? "possible, drawn by DM_VK_DEPTH_COPY_DRAW=2" : "not possible on this driver",
        (unsigned long long)s->serial, ss.mipLevel, (unsigned long long)d->serial, ds.mipLevel, dr.extent.width, dr.extent.height);
    return 1;
}
