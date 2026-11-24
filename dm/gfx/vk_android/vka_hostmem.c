// The driver's host memory and the replay's live Vulkan objects, counted (vka_hostmem.h; docs/reference/memory.md "Heap blocks and driver maps that grow at each area
// change (Android)"). vka_dev.c makes the instance and the device with vka_hostmem_callbacks() (NULL unless DM_VK_HOSTMEM=1) and calls vka_hostmem_install() right after it
// loads the device's entry points, before any object is made and before the other wrappers (FPCR, the shader module hook), which then call through these.
// The maps thread (vka_mem.c) logs both lines with its memory maps line: vka_hostmem_log
#if defined(__ANDROID__)
#include "vka.h"
#include "vka_hostmem.h"
#include <malloc.h>

static vkhm_stats g_hm;
static vko_stats g_vo;
static int g_hm_on = -1;

static void *VKAPI_CALL hm_alloc(void *u, size_t size, size_t align, VkSystemAllocationScope sc) { (void)u; return vkhm_alloc(&g_hm, size, align, (int)sc); }
static void *VKAPI_CALL hm_realloc(void *u, void *p, size_t size, size_t align, VkSystemAllocationScope sc) { (void)u; return vkhm_realloc(&g_hm, p, size, align, (int)sc); }
static void VKAPI_CALL hm_free(void *u, void *p) { (void)u; vkhm_free(&g_hm, p); }
static const VkAllocationCallbacks g_cb = {.pfnAllocation = hm_alloc, .pfnReallocation = hm_realloc, .pfnFree = hm_free};

// The allocator for vkCreateInstance and vkCreateDevice: DM_VK_HOSTMEM=1 (restart only, read once at the instance), else NULL (the driver's own, as before)
const VkAllocationCallbacks *vka_hostmem_callbacks(void) {
    if (g_hm_on < 0) { g_hm_on = VKA_ENV("DM_VK_HOSTMEM", 0) != 0; if (g_hm_on) VLOG("DM_VK_HOSTMEM: the driver's host memory goes through the replay's allocator and is counted by scope and size"); }
    return g_hm_on ? &g_cb : NULL;
}

// ---------- The objects: a wrapper per create and destroy entry point, counting what succeeded ----------
#define NEXT(f) static PFN_##f nx_##f;
NEXT(vkCreateImage) NEXT(vkDestroyImage) NEXT(vkCreateImageView) NEXT(vkDestroyImageView) NEXT(vkCreateBuffer) NEXT(vkDestroyBuffer) NEXT(vkCreateBufferView)
NEXT(vkDestroyBufferView) NEXT(vkCreateSampler) NEXT(vkDestroySampler) NEXT(vkCreateGraphicsPipelines) NEXT(vkCreateComputePipelines) NEXT(vkDestroyPipeline)
NEXT(vkCreateShaderModule) NEXT(vkDestroyShaderModule) NEXT(vkCreateDescriptorPool) NEXT(vkDestroyDescriptorPool) NEXT(vkAllocateCommandBuffers) NEXT(vkAllocateMemory)
NEXT(vkFreeMemory)
#undef NEXT
// One made object of kind k (a create entry point with a single output)
#define ONE(f, k, T, CI) \
    static VkResult VKAPI_CALL w_##f(VkDevice d, const CI *ci, const VkAllocationCallbacks *a, T *o) { VkResult r = nx_##f(d, ci, a, o); if (!r) vko_made(&g_vo, k, 1); return r; }
#define GONE(f, k, T) static void VKAPI_CALL w_##f(VkDevice d, T h, const VkAllocationCallbacks *a) { if (h) vko_gone(&g_vo, k); nx_##f(d, h, a); }
ONE(vkCreateImage, VKO_IMAGE, VkImage, VkImageCreateInfo)                 GONE(vkDestroyImage, VKO_IMAGE, VkImage)
ONE(vkCreateImageView, VKO_VIEW, VkImageView, VkImageViewCreateInfo)      GONE(vkDestroyImageView, VKO_VIEW, VkImageView)
ONE(vkCreateBuffer, VKO_BUFFER, VkBuffer, VkBufferCreateInfo)             GONE(vkDestroyBuffer, VKO_BUFFER, VkBuffer)
ONE(vkCreateBufferView, VKO_BVIEW, VkBufferView, VkBufferViewCreateInfo)  GONE(vkDestroyBufferView, VKO_BVIEW, VkBufferView)
ONE(vkCreateSampler, VKO_SAMPLER, VkSampler, VkSamplerCreateInfo)         GONE(vkDestroySampler, VKO_SAMPLER, VkSampler)
ONE(vkCreateShaderModule, VKO_SHADER, VkShaderModule, VkShaderModuleCreateInfo) GONE(vkDestroyShaderModule, VKO_SHADER, VkShaderModule)
ONE(vkCreateDescriptorPool, VKO_DPOOL, VkDescriptorPool, VkDescriptorPoolCreateInfo) GONE(vkDestroyDescriptorPool, VKO_DPOOL, VkDescriptorPool)
ONE(vkAllocateMemory, VKO_MEMORY, VkDeviceMemory, VkMemoryAllocateInfo)  GONE(vkFreeMemory, VKO_MEMORY, VkDeviceMemory)
GONE(vkDestroyPipeline, VKO_PIPELINE, VkPipeline)
#undef ONE
#undef GONE
static void pipes_made(uint32_t n, const VkPipeline *p) { int64_t k = 0; for (uint32_t i = 0; i < n; i++) k += p[i] != VK_NULL_HANDLE; vko_made(&g_vo, VKO_PIPELINE, k); }
static VkResult VKAPI_CALL w_vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    VkResult r = nx_vkCreateGraphicsPipelines(d, c, n, ci, a, p); if (r >= 0) pipes_made(n, p); return r; }   // (a failed one is VK_NULL_HANDLE; PIPELINE_COMPILE_REQUIRED is > 0)
static VkResult VKAPI_CALL w_vkCreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    VkResult r = nx_vkCreateComputePipelines(d, c, n, ci, a, p); if (r >= 0) pipes_made(n, p); return r; }
static VkResult VKAPI_CALL w_vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *o) {
    VkResult r = nx_vkAllocateCommandBuffers(d, ai, o); if (!r) vko_made(&g_vo, VKO_CMDBUF, ai->commandBufferCount); return r; }

void vka_hostmem_install(void) {   // vka_dev.c, once, right after the device's entry points are loaded
    if (!VKA_ENV("DM_VK_OBJCOUNT", 1)) return;
#define W(f) if (vk.f) { nx_##f = vk.f; vk.f = w_##f; }
    W(vkCreateImage) W(vkDestroyImage) W(vkCreateImageView) W(vkDestroyImageView) W(vkCreateBuffer) W(vkDestroyBuffer) W(vkCreateBufferView) W(vkDestroyBufferView)
    W(vkCreateSampler) W(vkDestroySampler) W(vkCreateGraphicsPipelines) W(vkCreateComputePipelines) W(vkDestroyPipeline) W(vkCreateShaderModule) W(vkDestroyShaderModule)
    W(vkCreateDescriptorPool) W(vkDestroyDescriptorPool) W(vkAllocateCommandBuffers) W(vkAllocateMemory) W(vkFreeMemory)
#undef W
}

// The lines: the objects (when counted), the driver's host memory (DM_VK_HOSTMEM=1), and malloc's own total for the whole process (host, driver, its compiler)
void vka_hostmem_log(void) {
    char b[1600];
    if (nx_vkCreateImage) { vko_line(&g_vo, b, sizeof b); VLOG("%s", b); }
    if (g_hm_on > 0) { vkhm_line(&g_hm, b, sizeof b); VLOG("%s", b); }
    struct mallinfo mi = mallinfo();
    VLOG("malloc in use: %.1f MiB in all of the process (host, driver, the driver's compiler; mallinfo), %.1f MiB mapped by the allocator",
         mi.uordblks / 1048576.0, mi.hblkhd / 1048576.0);
}
#endif
