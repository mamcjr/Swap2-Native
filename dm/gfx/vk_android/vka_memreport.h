// DM_VK_MEMREPORT=1 (restart, a diagnostic): where the process's graphics memory goes, the replay's allocations and the driver's own alike. Ported from DroidPlay Core
// (MIT; dm/android/gfx_vk_memreport.inc, 151f1557) to vka_dev.c, the full report only (DroidPlay's light mode, the per-pipeline counts its heavy-pipeline eviction reads,
// is not here). With VK_EXT_device_memory_report the driver reports every memory object it allocates or frees with the Vulkan object it belongs to (an image, a pipeline,
// a command buffer, a descriptor pool...). A line goes out each time the live total passes another 256 MB up, at a failed allocation, and at an allocation 10 s or more after
// the last line, with the live bytes and counts by object type. Why: a Mali-G720 tablet ran out of memory and lost the GPU in Ocarina of Time, and DroidPlay found a
// Qualcomm pipeline's scratch block (12-24 MiB for a shader that spills a few bytes) behind most of its 2.3 GB of pipeline memory: this says which kind of object holds it.
// Included once, by vka_dev.c (vk, V, VLOG)
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

enum { MR_TYPES = 27, MR_CAP = 1 << 18 };   // VkObjectType 0-25, then everything else; slots for live memory objects (a power of two)
typedef struct { uint64_t id, size; uint8_t type; } mr_ent;
static mr_ent *g_mr_tab;
static uint32_t g_mr_n, g_mr_full;
static int64_t g_mr_bytes[MR_TYPES], g_mr_count[MR_TYPES], g_mr_total, g_mr_next = 256ll << 20;
static uint64_t g_mr_failed, g_mr_imported;
static struct timespec g_mr_last;
static pthread_mutex_t g_mr_mu = PTHREAD_MUTEX_INITIALIZER;

static const char *const g_mr_names[MR_TYPES] = {"unknown", "instance", "physical device", "device", "queue", "semaphore", "command buffer", "fence", "device memory", "buffer",
    "image", "event", "query pool", "buffer view", "image view", "shader module", "pipeline cache", "pipeline layout", "render pass", "pipeline", "set layout", "sampler",
    "descriptor pool", "descriptor set", "framebuffer", "command pool", "other"};

static uint32_t mr_slot(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 46) & (MR_CAP - 1); }
static void mr_log(const char *why) {   // under g_mr_mu
    char s[1024]; size_t n = 0; s[0] = 0;
    for (int i = 0; i < MR_TYPES; i++) if (g_mr_count[i] && n < sizeof s)
        n += (size_t)snprintf(s + n, sizeof s - n, "%s%s %.1f MB (%lld)", n ? ", " : "", g_mr_names[i], g_mr_bytes[i] / 1048576.0, (long long)g_mr_count[i]);
    VLOG("memory report (%s): %.1f MB live in %u memory objects: %s; %llu allocations failed, %llu imports, %u frees of objects not seen (cumulative; DM_VK_MEMREPORT)", why,
         g_mr_total / 1048576.0, g_mr_n, s, (unsigned long long)g_mr_failed, (unsigned long long)g_mr_imported, g_mr_full);
    clock_gettime(CLOCK_MONOTONIC, &g_mr_last);
}
static void VKAPI_PTR mr_cb(const VkDeviceMemoryReportCallbackDataEXT *d, void *user) {
    (void)user;
    uint64_t id = d->memoryObjectId ? d->memoryObjectId : 1;   // 0 marks a free slot
    pthread_mutex_lock(&g_mr_mu);
    if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT) { g_mr_failed++; mr_log("an allocation failed"); }
    else if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT || d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT) {
        if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT) g_mr_imported++;
        uint8_t t = d->objectType < MR_TYPES - 1 ? (uint8_t)d->objectType : MR_TYPES - 1;
        if (g_mr_n < MR_CAP - MR_CAP / 8) {
            uint32_t i = mr_slot(id); while (g_mr_tab[i].id) i = (i + 1) & (MR_CAP - 1);
            g_mr_tab[i] = (mr_ent){id, d->size, t}; g_mr_n++;
            g_mr_bytes[t] += (int64_t)d->size; g_mr_count[t]++; g_mr_total += (int64_t)d->size;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (g_mr_total >= g_mr_next) { while (g_mr_next <= g_mr_total) g_mr_next += 256ll << 20; mr_log("passed another 256 MB"); }
        else if (now.tv_sec - g_mr_last.tv_sec >= 10) mr_log("10 s");
    } else {   // a free or an unimport: the object's slot goes, and the run after it shifts back so lookups stay unbroken (linear probing)
        uint32_t i = mr_slot(id);
        while (g_mr_tab[i].id && g_mr_tab[i].id != id) i = (i + 1) & (MR_CAP - 1);
        if (!g_mr_tab[i].id) g_mr_full++;
        else {
            mr_ent e = g_mr_tab[i]; g_mr_bytes[e.type] -= (int64_t)e.size; g_mr_count[e.type]--; g_mr_total -= (int64_t)e.size; g_mr_n--;
            for (uint32_t j = (i + 1) & (MR_CAP - 1); g_mr_tab[j].id; j = (j + 1) & (MR_CAP - 1)) {
                uint32_t h = mr_slot(g_mr_tab[j].id);
                if (((j - h) & (MR_CAP - 1)) >= ((j - i) & (MR_CAP - 1))) { g_mr_tab[i] = g_mr_tab[j]; i = j; }
            }
            g_mr_tab[i].id = 0;
            if (g_mr_total + (512ll << 20) < g_mr_next) g_mr_next -= 256ll << 20;   // a drop, then growth again, is reported again
        }
    }
    pthread_mutex_unlock(&g_mr_mu);
}
static VkDeviceDeviceMemoryReportCreateInfoEXT g_mr_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT, .pfnUserCallback = mr_cb};
static VkPhysicalDeviceDeviceMemoryReportFeaturesEXT g_mr_feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT};
// At device creation: 1 when DM_VK_MEMREPORT=1 and the driver has the extension and its feature (then enable VK_EXT_device_memory_report and chain mr_chain's structs)
static int mr_want(const VkExtensionProperties *de, uint32_t nd) {
    const char *e = getenv("DM_VK_MEMREPORT");
    if (!e || *e != '1') return 0;
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_mr_feat};
    int has = 0; for (uint32_t i = 0; i < nd && !has; i++) has = !strcmp(de[i].extensionName, VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME);
    if (has) vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    if (!g_mr_feat.deviceMemoryReport) { VLOG("DM_VK_MEMREPORT=1, but the driver has no VK_EXT_device_memory_report"); return 0; }
    g_mr_feat.pNext = NULL;
    if (!(g_mr_tab = calloc(MR_CAP, sizeof *g_mr_tab))) return 0;
    return 1;
}
static void *mr_chain(void *next) { g_mr_ci.pNext = next; g_mr_feat.pNext = &g_mr_ci; return &g_mr_feat; }
