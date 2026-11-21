// PC test of the temporal path's motion pass (vka_ndsr.c DM_VK_NDSR=gsr2, fsr/ndsr_tsr.comp, vka_ndsr_tsr_spv.h) on any Vulkan driver, made for Mesa's lavapipe
// (dm/tools/tests/test_ndsr_tsr.py builds and runs it in Linux or WSL). The shader's matrix is the CPU's reprojection (vka_tsr_cam.h vka_tsr_reproj) in push constants,
// read as a column-major mat4: this checks that the GPU's motion is vka_tsr_motion_ref's (the CPU reference the unit test t_android_tsr_cam.c checks against both
// cameras), for a camera that walks and turns, both window origins and both depth modes; the reversed depth it writes; and pass 0's colour (squared for a sqrt input).
// The depth is an R32_SFLOAT image here (the replay binds the scene's D32F through its depth aspect; the shader reads .x either way).
// Exit 0 = all passed. Build: gcc -O1 -I dm/third_party/vulkan-headers/include -I dm/gfx/vk_android dm/gfx/vk_android/selftest/tsr_lvp.c -ldl -lm
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vka_tsr_cam.h"
#include "vka_ndsr_tsr_spv.h"

#define FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkCreateShaderModule) X(vkCreateComputePipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkResetDescriptorPool)
#define DECL(n) static PFN_##n n;
FNS(DECL)
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static VkPhysicalDeviceMemoryProperties memp; static VkCommandPool pool;
static VkDescriptorSetLayout dsl; static VkPipelineLayout pl; static VkDescriptorPool dpool; static VkSampler smp; static VkPipeline pipe;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_) { printf("FAIL: %s = %d\n", #x, r_); exit(2); } } while (0)

static float half(uint16_t h) { uint32_t s = h >> 15, e = h >> 10 & 31, m = h & 1023; float v = e == 31 ? (m ? NAN : INFINITY) : e ? ldexpf(1 + m / 1024.0f, (int)e - 15) : ldexpf((float)m, -24); return s ? -v : v; }
static uint16_t to_half(float f) {   // round to nearest (finite, normal range: the test's colours)
    if (f == 0) return 0;
    int e; float m = frexpf(fabsf(f), &e);   // f = m 2^e, m in [0.5, 1)
    int he = e - 1 + 15; uint32_t hm = (uint32_t)lroundf((m * 2 - 1) * 1024);
    if (hm == 1024) { hm = 0; he++; }
    return (uint16_t)((f < 0) << 15 | he << 10 | hm);
}
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++) if ((bits >> i & 1) && (memp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL: no memory type\n"); exit(2);
}
static VkBuffer buffer(VkDeviceSize n, VkBufferUsageFlags use, void **map) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = use}; VkBuffer b; VK(vkCreateBuffer(dev, &bi, NULL, &b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m)); VK(vkBindBufferMemory(dev, b, m, 0)); VK(vkMapMemory(dev, m, 0, n, 0, map));
    return b;
}
static VkImage image(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags use, VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use};
    VkImage im; VK(vkCreateImage(dev, &ci, NULL, &im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0)};
    VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m)); VK(vkBindImageMemory(dev, im, m, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK(vkCreateImageView(dev, &vi, NULL, view));
    return im;
}
static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static int init(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW); if (!lib) { printf("SKIP: no libvulkan.so.1\n"); return 0; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci_ = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app}; VkInstance inst;
    if (ci_(&ii, NULL, &inst)) { printf("SKIP: no Vulkan instance\n"); return 0; }
#define LOAD(n) n = (PFN_##n)gipa(inst, #n); if (!n) { printf("FAIL: no %s\n", #n); exit(2); }
    FNS(LOAD)
    uint32_t n = 1; if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("SKIP: no Vulkan device\n"); return 0; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s\n", pp.deviceName);
    vkGetPhysicalDeviceMemoryProperties(pd, &memp);
    uint32_t nq = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qi = 0; while (qi < nq && !(qf[qi].queueFlags & VK_QUEUE_COMPUTE_BIT)) qi++;
    float prio = 1; VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
    VK(vkCreateDevice(pd, &dc, NULL, &dev)); vkGetDeviceQueue(dev, qi, 0, &q);
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qi};
    VK(vkCreateCommandPool(dev, &pc, NULL, &pool));
    // The DLSS stand-in's layout (vka_dlss.c dl_ready): combined samplers 0..3, storage images 4 and 5, 128 bytes of push constants
    VkDescriptorSetLayoutBinding lb[6];
    for (uint32_t i = 0; i < 6; i++) lb[i] = (VkDescriptorSetLayoutBinding){i, i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6, .pBindings = lb}; VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK(vkCreatePipelineLayout(dev, &li, NULL, &pl));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps}; VK(vkCreateDescriptorPool(dev, &dp, NULL, &dpool));
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VK(vkCreateSampler(dev, &sc, NULL, &smp));
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_ndsr_tsr, .pCode = vka_ndsr_tsr}; VkShaderModule m; VK(vkCreateShaderModule(dev, &mi, NULL, &m));
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}};
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe));
    VkShaderModuleCreateInfo mn = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_ndsr_noop, .pCode = vka_ndsr_noop}; VkShaderModule m2; VK(vkCreateShaderModule(dev, &mn, NULL, &m2));
    VkComputePipelineCreateInfo pcn = pci; pcn.stage.module = m2; VkPipeline pn; VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pcn, NULL, &pn));   // (the no-op builds)
    return 1;
}

enum { W = 160, H = 90 };
// One dispatch of pass `pass` over W x H: depth (W*H floats), colour (W*H*4 floats); returns the two outputs as W*H*4 floats each
static void run(const vka_tsr_pc *pc, const float *depth, const float *colour, float *o0, float *o1) {
    void *dz, *dc, *r0, *r1;
    VkBuffer bz = buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &dz), bc = buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &dc);
    VkBuffer b0 = buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &r0), b1 = buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &r1);
    memcpy(dz, depth, W * H * 4);
    for (int i = 0; i < W * H * 4; i++) ((uint16_t *)dc)[i] = to_half(colour[i]);
    VkImageView vz, vc, v0, v1;
    VkImage iz = image(VK_FORMAT_R32_SFLOAT, W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vz);
    VkImage ic = image(VK_FORMAT_R16G16B16A16_SFLOAT, W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vc);
    VkImage i0 = image(VK_FORMAT_R16G16B16A16_SFLOAT, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &v0);
    VkImage i1 = image(VK_FORMAT_R16G16B16A16_SFLOAT, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &v1);
    VK(vkResetDescriptorPool(dev, dpool, 0));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl}; VkDescriptorSet ds; VK(vkAllocateDescriptorSets(dev, &da, &ds));
    VkDescriptorImageInfo ii[6] = {{smp, vz, VK_IMAGE_LAYOUT_GENERAL}, {smp, vc, VK_IMAGE_LAYOUT_GENERAL}, {smp, vz, VK_IMAGE_LAYOUT_GENERAL}, {smp, vz, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, v0, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, v1, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[6];
    for (int i = 0; i < 6; i++) wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                                              .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(dev, 6, wr, 0, NULL);
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VK(vkBeginCommandBuffer(cb, &bi));
    VkImage all[4] = {iz, ic, i0, i1};
    for (int i = 0; i < 4; i++) barrier(cb, all[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy rg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyBufferToImage(cb, bz, iz, VK_IMAGE_LAYOUT_GENERAL, 1, &rg); vkCmdCopyBufferToImage(cb, bc, ic, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    barrier(cb, iz, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); barrier(cb, ic, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128, pc);
    vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
    barrier(cb, i0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); barrier(cb, i1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    vkCmdCopyImageToBuffer(cb, i0, VK_IMAGE_LAYOUT_GENERAL, b0, 1, &rg); vkCmdCopyImageToBuffer(cb, i1, VK_IMAGE_LAYOUT_GENERAL, b1, 1, &rg);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo su = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb}; VK(vkQueueSubmit(q, 1, &su, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(q));
    for (int i = 0; i < W * H * 4; i++) { o0[i] = half(((uint16_t *)r0)[i]); o1[i] = half(((uint16_t *)r1)[i]); }
}

// A camera as the unit test makes one (t_android_tsr_cam.c look / persp)
static void look(double px, double py, double pz, double yaw, double pitch, vka_m4 *v) {
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    double r[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) v->m[i][j] = r[j][i]; v->m[i][3] = -(r[0][i] * px + r[1][i] * py + r[2][i] * pz); }
    v->m[3][0] = v->m[3][1] = v->m[3][2] = 0; v->m[3][3] = 1;
}
static void persp_rev(double fovy, double aspect, double n, int zero_one, vka_m4 *p) {   // reversed infinite
    double t = 1.0 / tan(0.5 * fovy);
    memset(p, 0, sizeof *p); p->m[0][0] = t / aspect; p->m[1][1] = t; p->m[3][2] = -1;
    if (zero_one) p->m[2][3] = n; else { p->m[2][2] = 1; p->m[2][3] = 2 * n; }
}

int main(void) {
    if (!init()) return 0;
    static float depth[W * H], colour[W * H * 4], o0[W * H * 4], o1[W * H * 4];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   // a floor, a wall, the sky (depth 0)
        int i = y * W + x; double d = y > H / 2 ? 2.0 + 400.0 / (y - H / 2 + 1) : x < W / 3 ? 12.0 + x : 0;
        depth[i] = d ? (float)(0.25 / d) : 0.0f;   // reversed, near 0.25, zero_one (rewritten below for GL's mode)
        for (int c = 0; c < 3; c++) colour[4 * i + c] = 0.1f + 0.8f * (float)((x * 7 + y * 3 + c * 11) % 97) / 97;
        colour[4 * i + 3] = 1;
    }
    // pass 0: the colour, squared for a sqrt input
    for (int sq = 0; sq < 2; sq++) {
        vka_tsr_pc pc; memset(&pc, 0, sizeof pc); pc.size[0] = W; pc.size[1] = H; pc.pass = 0; pc.sqrt_in = sq;
        run(&pc, depth, colour, o0, o1);
        double e = 0; for (int i = 0; i < W * H; i++) for (int c = 0; c < 3; c++) { float v = half(to_half(colour[4 * i + c])); e = fmax(e, fabs(o0[4 * i + c] - (sq ? v * v : v))); }
        CHECK(e < 2e-3, "pass 0 (sqrt %d): colour off by %g", sq, e);
    }
    // pass 1: the motion against the CPU reference, for each depth mode and window origin
    for (int zo = 0; zo < 2; zo++) for (int yup = 0; yup < 2; yup++) {
        float dz[W * H]; for (int i = 0; i < W * H; i++) dz[i] = zo ? depth[i] : depth[i];   // (reversed window depth n / d in both modes: GL's NDC is 2z - 1 of the same stored value)
        vka_m4 p, v0, v1, vp0, vp1; persp_rev(1.0, (double)W / H, 0.25, zo, &p);
        look(1500, 40, -2200, 0.4, -0.1, &v0); look(1507, 41, -2195, 0.43, -0.09, &v1);   // a step and a small turn
        vka_m4_mul(&p, &v0, &vp0); vka_m4_mul(&p, &v1, &vp1);
        vka_tsr_pc pc; memset(&pc, 0, sizeof pc);
        CHECK(vka_tsr_reproj(&vp1, &vp0, zo, pc.r), "reprojection");
        pc.size[0] = W; pc.size[1] = H; pc.pass = 1; pc.yup = yup; pc.zero_one = zo; pc.rev = vka_tsr_reversed(&p, zo);
        CHECK(pc.rev == 1, "the projection is reversed (zero_one %d)", zo);
        run(&pc, dz, colour, o0, o1);
        double e = 0, emax = 0, dmax = 0; int moved = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * W + x; float m[2];
            if (!vka_tsr_motion_ref(pc.r, x, y, W, H, dz[i], yup, m)) { m[0] = m[1] = 0; }
            double mag = fmax(fabs(m[0]), fabs(m[1])), d = fmax(fabs(o0[4 * i] - m[0]), fabs(o0[4 * i + 1] - m[1]));
            e = d - mag / 1024; if (e > emax) emax = e;   // (the RGBA16F store rounds to 11 bits: half an ulp is mag / 2048; two of them allowed)
            moved += mag > 0.5;
            dmax = fmax(dmax, fabs(o1[4 * i] - dz[i]));
        }
        CHECK(emax < 0.01, "zero_one %d yup %d: GPU motion off the CPU reference by %.4f texels beyond the store's rounding", zo, yup, emax);
        CHECK(moved > W * H / 2, "zero_one %d yup %d: the camera moved %d texels by more than half a texel", zo, yup, moved);
        CHECK(dmax < 1e-3, "zero_one %d yup %d: reversed depth written off by %g", zo, yup, dmax);
        printf("zero_one %d yup %d: worst %.4f texels, %d of %d texels moving\n", zo, yup, emax, moved, W * H);
    }
    // a standard depth: written reversed (1 - z)
    {
        vka_tsr_pc pc; memset(&pc, 0, sizeof pc); for (int i = 0; i < 16; i++) pc.r[i] = i % 5 == 0;
        pc.size[0] = W; pc.size[1] = H; pc.pass = 1; pc.yup = 1; pc.zero_one = 1; pc.rev = 0;
        run(&pc, depth, colour, o0, o1);
        double dmax = 0, mmax = 0; for (int i = 0; i < W * H; i++) { dmax = fmax(dmax, fabs(o1[4 * i] - (1 - depth[i]))); mmax = fmax(mmax, fmax(fabs(o0[4 * i]), fabs(o0[4 * i + 1]))); }
        CHECK(dmax < 1e-3 && mmax < 1e-3, "identity: depth %g, motion %g", dmax, mmax);
    }
    printf(fails ? "tsr_lvp: %d failure(s)\n" : "tsr_lvp: ok\n", fails);
    return fails != 0;
}
