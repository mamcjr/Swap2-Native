// The task / mesh compute packing (dm/gfx/vk_android/vka_mesh_pack.h, DM_VK_MESH_PACK) run on a Vulkan device: the selftest's task and mesh fixtures
// (vka_mesh_test_spv.h; no subgroup operations, so the halves are told apart by LocalInvocationIndex and any subgroup size will do, Mesa's lavapipe included) are
// dispatched as the library has them (n workgroups of 32) and packed ((n + 1) / 2 workgroups of 64) on the same inputs, and every word of the payload and output
// buffers must match, for even and odd n (an odd n repeats the last workgroup in the second half). Not part of `make run` (it needs a Vulkan driver):
//   cc -O1 -g -Wall -I.. -o build/t_android_meshpack_lvp t_android_meshpack_lvp.c -lvulkan
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json build/t_android_meshpack_lvp
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../../gfx/vk_android/vka_mesh_pack.h"
#include "../../gfx/vk_android/vka_mesh_test_spv.h"

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)
enum { OUT = 384, PAYLOAD, OFFSETS, INFO, NB = 4, BYTES = 4 << 20 };
static VkDevice dev; static VkPhysicalDevice pd; static VkQueue q; static uint32_t qf;
static VkBuffer buf[NB]; static VkDeviceMemory mem[NB]; static uint32_t *map[NB];

static uint32_t memtype(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return i;
    fprintf(stderr, "no host-visible memory\n"); exit(1);
}
static void init(void) {
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst; CK(vkCreateInstance(&ic, NULL, &inst));
    uint32_t n = 1; VkResult r = vkEnumeratePhysicalDevices(inst, &n, &pd); if (r < 0 || !n) { fprintf(stderr, "no Vulkan device\n"); exit(1); }
    VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(pd, &pr); printf("t_android_meshpack_lvp: device %s\n", pr.deviceName);
    uint32_t nq = 8; VkQueueFamilyProperties qp[8]; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qf = 0; qf < nq && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT); qf++) {}
    float pri = 1; VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &pri};
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc};
    CK(vkCreateDevice(pd, &dc, NULL, &dev)); vkGetDeviceQueue(dev, qf, 0, &q);
    for (int i = 0; i < NB; i++) {
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CK(vkCreateBuffer(dev, &bc, NULL, &buf[i]));
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf[i], &mr);
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits)};
        CK(vkAllocateMemory(dev, &ma, NULL, &mem[i])); CK(vkBindBufferMemory(dev, buf[i], mem[i], 0));
        CK(vkMapMemory(dev, mem[i], 0, BYTES, 0, (void **)&map[i]));
    }
}
// one dispatch of groups workgroups of the module; the buffers as they are before and after
static void run(const uint32_t *w, size_t nw, uint32_t groups) {
    VkDescriptorSetLayoutBinding b[NB]; for (int i = 0; i < NB; i++) b[i] = (VkDescriptorSetLayoutBinding){OUT + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = NB, .pBindings = b};
    VkDescriptorSetLayout sl; CK(vkCreateDescriptorSetLayout(dev, &lc, NULL, &sl));
    VkPipelineLayoutCreateInfo pc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl};
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pc, NULL, &pl));
    VkShaderModuleCreateInfo mc = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = w};
    VkShaderModule mod; CK(vkCreateShaderModule(dev, &mc, NULL, &mod));
    VkComputePipelineCreateInfo cc = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = mod, .pName = "main"}};
    VkPipeline p; CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cc, NULL, &p));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NB};
    VkDescriptorPoolCreateInfo dpc = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpc, NULL, &dp));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &sl};
    VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &da, &ds));
    VkDescriptorBufferInfo bi[NB]; VkWriteDescriptorSet wr[NB];
    for (int i = 0; i < NB; i++) { bi[i] = (VkDescriptorBufferInfo){buf[i], 0, BYTES}; wr[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = OUT + i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]}; }
    vkUpdateDescriptorSets(dev, NB, wr, 0, NULL);
    VkCommandPoolCreateInfo cpc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf};
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpc, NULL, &cp));
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; CK(vkAllocateCommandBuffers(dev, &ca, &cb));
    VkCommandBufferBeginInfo bb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bb));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, groups, 1, 1);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE)); CK(vkQueueWaitIdle(q));
    vkDestroyCommandPool(dev, cp, NULL); vkDestroyDescriptorPool(dev, dp, NULL); vkDestroyPipeline(dev, p, NULL); vkDestroyShaderModule(dev, mod, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL); vkDestroyDescriptorSetLayout(dev, sl, NULL);
}
// the inputs a controller would leave: n workgroups, the task payload pattern, a (task, local group) pair per mesh group
static void inputs(uint32_t n, int task) {
    for (int i = 0; i < NB; i++) memset(map[i], 0, BYTES);
    uint32_t *info = map[INFO - OUT], *pay = map[PAYLOAD - OUT], *off = map[OFFSETS - OUT];
    info[0] = n; info[1] = info[2] = 1; info[4] = n; info[5] = 0; info[10] = n; info[11] = 64; info[13] = 256; info[14] = 3; info[15] = 65535; info[16] = 1; info[19] = task ? 2 : 0;
    for (uint32_t i = 0; i < 64 * 512; i++) pay[i] = (i * 2654435761u) % 7u;
    for (uint32_t g = 0; g < n; g++) { off[2 * g] = g % 5; off[2 * g + 1] = g / 5; }
}
static int same(const char *what, uint32_t n, uint32_t *const want[NB]) {
    for (int i = 0; i < NB; i++) if (memcmp(want[i], map[i], BYTES)) {
        size_t k = 0; while (want[i][k] == map[i][k]) k++;
        fprintf(stderr, "%s n=%u: binding %d word %zu: %#x packed, %#x as built\n", what, n, OUT + i, k, map[i][k], want[i][k]);
        return 0;
    }
    return 1;
}
int main(void) {
    init();
    const struct { const uint32_t *w; size_t bytes; const char *name; int task; } fx[2] = {
        {SPV_MESH_TASK_COMPUTE, sizeof SPV_MESH_TASK_COMPUTE, "task", 1}, {SPV_MESH_MESH_COMPUTE, sizeof SPV_MESH_MESH_COMPUTE, "mesh", 0}};
    static const uint32_t counts[] = {1, 2, 3, 7, 64, 101, 500};
    uint32_t *want[NB]; for (int i = 0; i < NB; i++) want[i] = malloc(BYTES);
    int bad = 0, runs = 0;
    for (int f = 0; f < 2; f++) {
        size_t on = 0; char why[256]; vka_mesh_pack_stats st;
        uint32_t *o = vka_mesh_pack(fx[f].w, fx[f].bytes / 4, INFO, 1, &on, &st, why, sizeof why);
        if (!o) { fprintf(stderr, "%s: not packed: %s\n", fx[f].name, why); return 1; }
        for (unsigned c = 0; c < sizeof counts / sizeof *counts; c++) {
            uint32_t n = counts[c];
            inputs(n, fx[f].task); run(fx[f].w, fx[f].bytes / 4, n);
            for (int i = 0; i < NB; i++) memcpy(want[i], map[i], BYTES);
            if (n == 3) {   // (the test means something only if the program wrote there)
                size_t nz = 0; const uint32_t *o0 = want[fx[f].task ? PAYLOAD - OUT : 0];
                for (size_t k = 0; k < BYTES / 4; k++) nz += o0[k] != 0;
                printf("t_android_meshpack_lvp: %s n=3 as built: %zu nonzero words in its %s buffer\n", fx[f].name, nz, fx[f].task ? "payload" : "output");
            }
            inputs(n, fx[f].task); run(o, on, (n + 1) / 2);
            if (!same(fx[f].name, n, want)) bad++;
            runs++;
        }
        free(o);
    }
    printf("t_android_meshpack_lvp: %d of %d dispatches packed gave every word as built\n", runs - bad, runs);
    if (bad) return 1;
    printf("t_android_meshpack_lvp: ok\n");
    return 0;
}
