// The Vulkan device of the replay: instance, the one queue, the timeline that fences submissions, the completion thread that runs their CPU-side effects, and scratch memory.
// Feature and extension choices follow the probe of android/spike (results/honor-pnm-n49.md): placed memory maps, buffer device addresses, update-after-bind arrays, display timing
#if defined(__ANDROID__)
#include "vka.h"
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vka_fd_alias.h"
#include "vka_ahb_alias.h"
#include "vka_memreport.h"   // DM_VK_MEMREPORT: the driver's memory by object type (DroidPlay Core 151f1557)
#include "vka_remap_probe.h"   // advertised placed maps that refuse the pools' remaps (Banana Native v0.3.44)
#include "vka_shmcap.h"   // a Qualcomm GPU's real compute shared memory

// The device's facts as init_once found them, for the game's log (said once by vka_mem.c at the first memory pool: init_once runs before that log is open)
static char g_facts[3][900];
void vka_dev_facts_log(void) { for (int i = 0; i < 3; i++) if (g_facts[i][0]) VLOG("%s", g_facts[i]); }

// One placed map of a 64 KiB host-visible allocation at an address reserved for it (vka_dev.c, unadvertised placed maps): 1 when the driver maps it exactly there and
// the memory reads back what was written through it
// n: the allocation's size; adv: the driver advertises placed maps, so this is a diagnostic of what its map does (from Banana Native v0.3.43: the real placements are pools of
// megabytes, and a Mali-G77's PanVK refused them with VK_ERROR_MEMORY_MAP_FAILED while advertising the extension, issue #92), in the pools' memory type and at an address
// aligned as the driver asks (V.placed_align)
static int vka_placed_probe_n(VkDeviceSize n, int adv) {
    VkPhysicalDeviceMemoryProperties mp; vk.vkGetPhysicalDeviceMemoryProperties(V.pd, &mp);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == ~0u; i++)
        if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) type = i;
    if (adv && V.guest_type < mp.memoryTypeCount) type = V.guest_type;   // (the type the pools use)
    if (type == ~0u) return 0;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = n, .memoryTypeIndex = type};
    VkDeviceMemory mem; if (vk.vkAllocateMemory(V.dev, &mai, NULL, &mem)) return 0;
    uintptr_t al = adv && V.placed_align > 4096 ? (uintptr_t)V.placed_align : 4096;
    void *base = mmap(NULL, n + al, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0), *at = base == MAP_FAILED ? base : (void *)(((uintptr_t)base + al - 1) & ~(al - 1));
    int ok = 0; void *q = NULL; VkResult r = VK_ERROR_UNKNOWN;
    if (base != MAP_FAILED) {
        VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = at};
        VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .offset = 0, .size = VK_WHOLE_SIZE};
        r = vk.vkMapMemory2KHR(V.dev, &mi, &q);
        if (!r && q == at) { volatile uint32_t *w = q; w[0] = 0x5eed1234u; w[n / 4 - 1] = 0xc0ffee00u; ok = w[0] == 0x5eed1234u && w[n / 4 - 1] == 0xc0ffee00u; }
        if (!r) { VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem}; vk.vkUnmapMemory2KHR(V.dev, &ui); }
        munmap(base, n + al);
    }
    vk.vkFreeMemory(V.dev, mem, NULL);
    if (adv) { if (ok) VLOG("placed maps: advertised by the driver; a test map of %llu KiB works (vkMapMemory2 %d, asked %p, got %p)", (unsigned long long)(n >> 10), r, at, q);
               else VRED("placed maps: advertised by the driver, but a test map of %llu KiB does not work (vkMapMemory2 %d, asked %p, got %p): the game's memory pools will probably not be placed", (unsigned long long)(n >> 10), r, at, q); }
    else VLOG("placed maps: not advertised by the driver; a placed map %s (vkMapMemory2 %d, asked %p, got %p)%s", ok ? "works" : "does not work", r, at, q,
              ok ? ": used without the extension (DM_VK_PLACED_PROBE=0: off)" : "");
    return ok;
}
static int vka_placed_probe(void) { return vka_placed_probe_n(64 << 10, 0); }

vka_fns vk;
vka_dev V = {.qmu = PTHREAD_MUTEX_INITIALIZER};
static int g_state;   // 0 not tried, 1 up, -1 failed
static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_fault_on;


uint32_t vka_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.memp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.memp.memoryTypes[i].propertyFlags & want) == want && !(V.memp.memoryTypes[i].propertyFlags & avoid)) return i;
    return UINT32_MAX;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }

static void *completion_main(void *arg);
static void vka_host_compile_fns(void);   // (below: the host's floating-point environment)
#include "vka_placedkill.h"
int vka_placed_unused;   // the driver has placed maps and this run leaves them (an alias by choice or by the switch): vka_mem.c's line says which
// The two records of vka_placedkill.h, in the game's temp folder, read and written here once a start: 1 = this run takes the alias in place of placed maps.
// forget: placed maps again and the count cleared (DM_VK_ALIAS=placed). asked: this run's backing was asked for, or the switch is off: it neither counts nor acts
static int placed_kill_check(int forget, int threshold, int asked) {
    const char *t = getenv("DM_TEMP"), *d = getenv("DM_VK_DRIVER"); if (!t || !*t) return 0;
    char key[96] = "system"; if (d && *d && strcmp(d, "system")) { const char *s = strrchr(d, '/'); snprintf(key, sizeof key, "%s", s && s[1] ? s + 1 : d); }
    for (char *c = key; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-' || *c == '_')) *c = '_';
    char pl[700], pm[700], last[200] = "", mine[200] = "", lo[200], mo[200];
    snprintf(pl, sizeof pl, "%s/vk_backing-last.txt", t); snprintf(pm, sizeof pm, "%s/vk_backing-%s.txt", t, key);
    if (forget) remove(pm);
    FILE *f = fopen(pl, "re"); if (f) { size_t n = fread(last, 1, sizeof last - 1, f); last[n] = 0; fclose(f); }
    f = fopen(pm, "re"); if (f) { size_t n = fread(mine, 1, sizeof mine - 1, f); mine[n] = 0; fclose(f); }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    vkpk_result r = vkpk_decide(last, mine, getenv("NX_PREV_EXIT"), key, (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000, threshold, asked, lo, sizeof lo, mo, sizeof mo);
    if ((f = fopen(pl, "we")) != NULL) { fputs(lo, f); fclose(f); }
    if ((f = fopen(pm, "we")) != NULL) { fputs(mo, f); fclose(f); }
    if (r.use_alias)
        VLOG("guest memory: %d run%s of this game on this driver (%s) ended killed from outside in the foreground while its memory was placed Vulkan memory: the exported alias "
             "in its place from now on (%s; DM_VK_ALIAS=placed: placed maps again; DM_VK_PLACED_KILLS=0: never switch)", r.kills, r.kills == 1 ? "" : "s", key, pm);
    else if (r.counted > 0)
        VLOG("guest memory: the run before on this driver (%s) ended killed from outside in the foreground on placed maps (%d in a row, the alias is taken at %d; DM_VK_PLACED_KILLS)", key, r.kills, threshold);
    return r.use_alias;
}
#include "vka_validation.inc"
static int init_once(void) {
    void *lib = vka_driver_open();   // DM_VK_DRIVER: a custom driver through libadrenotools (vka_driver.c); NULL = the system's
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib || !(vk.vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"))) { VRED("no libvulkan.so: %s", dlerror()); return 0; }
    if(!val_open())return 0;
#define G(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(NULL, #f);
    VKA_GLOBAL_FNS(G)
#undef G
    uint32_t api = VK_API_VERSION_1_0; if (vk.vkEnumerateInstanceVersion) vk.vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) { VRED("Vulkan instance %u.%u: the replay needs 1.3", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api)); return 0; }
    uint32_t ne = 0; vk.vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
    VkExtensionProperties *ie = calloc(ne ? ne : 1, sizeof *ie); vk.vkEnumerateInstanceExtensionProperties(NULL, &ne, ie);
    const char *iext[4]; uint32_t niext = 0;
    if (has_ext(ie, ne, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(ie, ne, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) { iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME; iext[niext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME; V.has_android_surface = 1; }
    if(val_layer && has_ext(ie,ne,VK_EXT_DEBUG_REPORT_EXTENSION_NAME))iext[niext++]=VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
    free(ie);
    const char *layer = "VK_LAYER_KHRONOS_validation"; uint32_t nlayer = 0;
    const char *ve = getenv("DM_VK_VALIDATE");
    if (ve && atoi(ve)) {
        uint32_t nl = 0; vk.vkEnumerateInstanceLayerProperties(&nl, NULL); VkLayerProperties *lp = calloc(nl ? nl : 1, sizeof *lp); vk.vkEnumerateInstanceLayerProperties(&nl, lp);
        for (uint32_t i = 0; i < nl; i++) if (!strcmp(lp[i].layerName, layer)) nlayer = 1;
        free(lp); VLOG("DM_VK_VALIDATE: validation layer %s", nlayer ? "on" : "not installed");
    }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .pEngineName = "nxnative", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext,
                                .enabledLayerCount = nlayer, .ppEnabledLayerNames = &layer};
    VkResult r = vk.vkCreateInstance(&ici, NULL, &V.inst);
    if (r) { VRED("vkCreateInstance %d", r); return 0; }
#define I(f) vk.f = (PFN_##f)vk.vkGetInstanceProcAddr(V.inst, #f);
    VKA_INSTANCE_FNS(I)
#undef I
    uint32_t npd = 1; VkPhysicalDevice pd[1];
    if (vk.vkEnumeratePhysicalDevices(V.inst, &npd, pd) < 0 || !npd) { VRED("no Vulkan physical device"); return 0; }
    V.pd = pd[0];
    vk.vkGetPhysicalDeviceProperties(V.pd, &V.props); vk.vkGetPhysicalDeviceMemoryProperties(V.pd, &V.memp);
    {   // a Qualcomm GPU's real compute shared memory (vka_shmcap.h: community Turnip builds advertise 64 KiB the Adreno does not have)
        uint32_t adv = V.props.limits.maxComputeSharedMemorySize, lim = vka_shared_mem_limit(V.props.vendorID, adv);
        if (lim != adv) {
            VLOG("compute shared memory: the driver advertises %u bytes, the Adreno holds %u (past it a workgroup's accesses land at its start): %u used", adv, lim, lim);
            V.props.limits.maxComputeSharedMemorySize = lim;
        }
    }
    if (V.props.apiVersion < VK_API_VERSION_1_3) { VRED("device %s is Vulkan %u.%u: the replay needs 1.3", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion)); return 0; }
    uint32_t nq = 0; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp); vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    V.qf = UINT32_MAX; for (uint32_t i = 0; i < nq && V.qf == UINT32_MAX; i++) if ((qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) V.qf = i;
    { VkPhysicalDeviceFeatures pf; vk.vkGetPhysicalDeviceFeatures(V.pd, &pf); const VkPhysicalDeviceSparseProperties *sp = &V.props.sparseProperties;   // sparse residency: for sparse NVN
      VLOG("sparse: binding %d, residency buffer %d image2D %d image3D %d aliased %d, standard 2D block shape %d, non-resident strict %d, address space %llu MiB, sparse binding on queue family %u: %s; "
           "buffer / image granularity %llu",   // pools' textures (docs/reference/nvn-objects.md "Sparse texture arrays made at their full size")
           pf.sparseBinding, pf.sparseResidencyBuffer, pf.sparseResidencyImage2D, pf.sparseResidencyImage3D, pf.sparseResidencyAliased, sp->residencyStandard2DBlockShape, sp->residencyNonResidentStrict,
           (unsigned long long)(V.props.limits.sparseAddressSpaceSize >> 20), V.qf, V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? "yes" : "no",
           (unsigned long long)V.props.limits.bufferImageGranularity);
      // Sparse residency images (vka_sparse.c) need it all: binding on the replay's queue, 2D residency, the standard block shape (NVN's 64 KB tiles) and strict non-resident reads
      // (an unbound tile reads 0, as an unmapped NVN page). The features are enabled with every other supported one (vkGetPhysicalDeviceFeatures2 below)
      V.sparse_res = pf.sparseBinding && pf.sparseResidencyImage2D && sp->residencyStandard2DBlockShape && sp->residencyNonResidentStrict && V.qf < nq && (qp[V.qf].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT); }
    free(qp);
    if (V.qf == UINT32_MAX) { VRED("no graphics + compute queue family"); return 0; }

    uint32_t nde = 0; vk.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &nde, NULL);
    VkExtensionProperties *de = calloc(nde ? nde : 1, sizeof *de); vk.vkEnumerateDeviceExtensionProperties(V.pd, NULL, &nde, de);
    const char *dext[32]; uint32_t ndext = 0;
    { char q[700]; size_t qn = 0; q[0] = 0;   // the vendor's own extensions (tile properties, render pass transforms ...): what a tiler-specific path could use
      for (uint32_t i = 0; i < nde && qn < sizeof q; i++) if (strstr(de[i].extensionName, "_QCOM_")) qn += (size_t)snprintf(q + qn, sizeof q - qn, " %s", de[i].extensionName + 3);
      if (qn) VLOG("device extensions of the vendor:%s", q); }
    int swap = has_ext(de, nde, VK_KHR_SWAPCHAIN_EXTENSION_NAME), map2 = has_ext(de, nde, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME), placed = map2 && has_ext(de, nde, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
    V.display_timing = swap && has_ext(de, nde, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
    if (swap) dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    V.mem_budget = has_ext(de, nde, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (V.mem_budget) dext[ndext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
    if (map2) dext[ndext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
    if (placed) dext[ndext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    int x_fd = has_ext(de, nde, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (x_fd) dext[ndext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    // Hardware buffers as pool memory (vka_ahb_alias.h): for a driver without placed maps (Mali, Xclipse). Enabled wherever it is, used only when its probe passes below
    int x_ahb = has_ext(de, nde, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) && has_ext(de, nde, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    if (x_ahb) { dext[ndext++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME; dext[ndext++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME; }
    if (V.display_timing) dext[ndext++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    // The draw path (vka_draw.c): each one optional, with a fallback when absent
    int x_push = has_ext(de, nde, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME), x_vin = has_ext(de, nde, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const char *x_rob = has_ext(de, nde, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ? VK_KHR_ROBUSTNESS_2_EXTENSION_NAME : has_ext(de, nde, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) ? VK_EXT_ROBUSTNESS_2_EXTENSION_NAME : NULL;
    const char *x_i8 = has_ext(de, nde, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME : has_ext(de, nde, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME) ? VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME : NULL;
    const char *x_div = has_ext(de, nde, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : has_ext(de, nde, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) ? VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME : NULL;
    if (x_push) dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (x_vin) dext[ndext++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    if (x_rob) dext[ndext++] = x_rob;
    if (x_i8) dext[ndext++] = x_i8;
    if (x_div) dext[ndext++] = x_div;
    int x_gpl = has_ext(de, nde, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(de, nde, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    if (x_gpl) { dext[ndext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME; dext[ndext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME; }
    int mesh_ext = has_ext(de, nde, VK_EXT_MESH_SHADER_EXTENSION_NAME); const char *mesh_env = getenv("DM_VK_MESH");   // restart only: the device's extensions
    // Qualcomm's own driver (driver ID 8) draws NVN2's task and mesh programs through the compute fallback (vka_mesh.inc) even where it has VK_EXT_mesh_shader (the
    // Adreno 840 and 850 packages): its native mesh pipelines drew Ocarina of Time's scene as scattered garbage triangles on an Adreno 850 (891.14), and on the Adreno 840
    // put that game's foliage colour pass behind its own mesh depth prepass (DroidPlay Core f7447509), where the fallback, which every Adreno 830 package runs, draws it
    // right (docs/reference/shaders.md "Native mesh shading on Qualcomm's driver"). DM_VK_MESH=1: native all the same (an A/B); =0: off on every driver; Turnip keeps it
    int mesh_qcom = 0;
    if (mesh_ext && !(mesh_env && (!strcmp(mesh_env, "0") || !strcmp(mesh_env, "1")))) {
        VkPhysicalDeviceDriverProperties dp0 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 pp0 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp0};
        vk.vkGetPhysicalDeviceProperties2(V.pd, &pp0);
        mesh_qcom = dp0.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    }
    int x_mesh = mesh_ext && !(mesh_env && !strcmp(mesh_env, "0")) && !mesh_qcom;
    if (x_mesh) dext[ndext++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
    int x_ph = has_ext(de, nde, "VK_QCOM_queue_perf_hint");
    if (x_ph) dext[ndext++] = "VK_QCOM_queue_perf_hint";
    // DM_VK_PIPE_STATS=1 (diagnostic, restart only): VK_KHR_pipeline_executable_properties, so each compute pipeline logs what the driver says of its code (registers,
    // instructions, waves: vka_pso.c pipe_stats). Off by default: a pipeline asked for its statistics may be compiled again rather than taken from the cache
    const char *ps_env = getenv("DM_VK_PIPE_STATS");
    int x_pexec = ps_env && strcmp(ps_env, "0") && has_ext(de, nde, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
    if (x_pexec) dext[ndext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    const char *fault_env = getenv("DM_VK_DEVICE_FAULT");
    int x_fault = has_ext(de, nde, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) && !(fault_env && !strcmp(fault_env, "0"));
    if (x_fault) dext[ndext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    // DM_VK_VRS_TEST=<w>x<h> (diagnostic, restart only): VK_KHR_fragment_shading_rate's pipeline rate, given to the pipelines of the fragment shaders in DM_VK_VRS_KEYS
    // (vka_pso.c full_pipeline), to measure how much of a pass is its pixel shading before the game's own shading rate image is mapped
    const char *vrs_env = getenv("DM_VK_VRS_TEST");
    int x_fsr = vrs_env && *vrs_env && strcmp(vrs_env, "0") && has_ext(de, nde, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    if (x_fsr) dext[ndext++] = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
    int x_mr = mr_want(de, nde);   // DM_VK_MEMREPORT=1 (vka_memreport.h)
    if (x_mr) dext[ndext++] = VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME;
    free(de);

    // Every feature the device has, but robustness and capture-replay (they cost and nothing needs them); the shader side takes what it needs from the same device
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT fmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesKHR frob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
    VkPhysicalDeviceIndexTypeUint8Features fi8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES};
    VkPhysicalDeviceVertexAttributeDivisorFeatures fdiv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceMeshShaderFeaturesEXT fmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT ffault = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fpexec = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR ffsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    void *chain = placed ? &fmp : NULL;
#define LINK(on, st) if (on) { (st).pNext = chain; chain = &(st); }
    struct { VkStructureType sType; void *pNext; VkBool32 queuePerfHint; } fph = {(VkStructureType)1000302001};   // VkPhysicalDeviceQueuePerfHintFeaturesQCOM (not in the NDK's headers yet)
    LINK(x_vin, fvin) LINK(x_rob, frob) LINK(x_i8, fi8) LINK(x_div, fdiv) LINK(x_ph, fph) LINK(x_gpl, fgpl) LINK(x_mesh, fmesh) LINK(x_fault, ffault) LINK(x_pexec, fpexec) LINK(x_fsr, ffsr)
#undef LINK
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = chain};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);
    ffault.deviceFaultVendorBinary = VK_FALSE;
    g_fault_on = x_fault && ffault.deviceFault;
    // Diagnostic A/B for descriptor-range violations; never enabled by a game profile.
    f2.features.robustBufferAccess &= VKA_ENV("DM_VK_ROBUST_DIAG", 0) != 0; f13.robustImageAccess = VK_FALSE;
    f12.bufferDeviceAddressCaptureReplay = VK_FALSE; f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    frob.robustBufferAccess2 = VK_FALSE; frob.robustImageAccess2 = VK_FALSE;   // only nullDescriptor: unbound slots read a null descriptor
    // Mesh shading: the task and mesh stages only (multiview, shading-rate and query support each need more of the device than the replay enables or uses)
    fmesh.multiviewMeshShader = VK_FALSE; fmesh.primitiveFragmentShadingRateMeshShader = VK_FALSE; fmesh.meshShaderQueries = VK_FALSE;
    if (x_mesh && !fmesh.meshShader) fmesh.taskShader = VK_FALSE;   // (task shading is of no use without mesh shading)
    V.indirect_count = f12.drawIndirectCount && f2.features.multiDrawIndirect;
    V.push_desc = x_push; V.vin_dyn = x_vin && fvin.vertexInputDynamicState; V.null_desc = x_rob && frob.nullDescriptor; V.ext_storage = f2.features.shaderStorageImageExtendedFormats; V.idx8 = x_i8 && fi8.indexTypeUint8;
    V.divisor = x_div && fdiv.vertexAttributeInstanceRateDivisor; V.logic_op = f2.features.logicOp; V.dual_src = f2.features.dualSrcBlend; V.geom = f2.features.geometryShader;
    V.tess = f2.features.tessellationShader; V.depth_clamp = f2.features.depthClamp; V.fill_lines = f2.features.fillModeNonSolid; V.depth_bias_clamp = f2.features.depthBiasClamp;
    V.pexec = x_pexec && fpexec.pipelineExecutableInfo;
    ffsr.primitiveFragmentShadingRate = VK_FALSE; ffsr.attachmentFragmentShadingRate = VK_FALSE;   // (the test sets the pipeline's rate only)
    V.fsr_pipe = x_fsr && ffsr.pipelineFragmentShadingRate;
    if (x_fsr) VLOG("fragment shading rate test (DM_VK_VRS_TEST=%s): pipeline rate %s", vrs_env, V.fsr_pipe ? "on" : "not on this device");
    V.multi_vp = f2.features.multiViewport; V.a2c = 1; V.mesh = x_mesh && fmesh.meshShader; V.task = V.mesh && fmesh.taskShader;
    V.minmax = f12.samplerFilterMinmax;   // (enabled with the rest of f12: NVN's min / max sampler reduction, vka_tex.c sampler_of)
    V.vstores = f2.features.vertexPipelineStoresAndAtomics; V.fstores = f2.features.fragmentStoresAndAtomics;   // (enabled with the rest: texture pool reads, vka_tex.c)
    if (!f12.timelineSemaphore || !f13.dynamicRendering) { VRED("the device lacks timeline semaphores (%d) or dynamic rendering (%d)", f12.timelineSemaphore, f13.dynamicRendering); return 0; }
    // (enabled with the rest above when the device has them) vertex shaders read the draw's base instance from BaseInstance (vk_android_spv.py base_instance), and an indirect draw's firstInstance must be honoured
    if (!f11.shaderDrawParameters || !f2.features.drawIndirectFirstInstance) VRED("the device lacks shaderDrawParameters (%d) or drawIndirectFirstInstance (%d): instanced vertex shaders and indirect draws' first instances will be wrong", f11.shaderDrawParameters, f2.features.drawIndirectFirstInstance);
    V.bda = f12.bufferDeviceAddress; V.uab = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound && f12.runtimeDescriptorArray;
    // (enabled with the rest above when the device has them) the library's modules index the texture and sampler pools and the bindless slot arrays with NonUniform
    // (dmsh_xlate's nonuniform_index answer, docs/reference/shaders.md): such a module declares the array kind's ...ArrayNonUniformIndexing capability, which needs the feature
    if (!f12.shaderSampledImageArrayNonUniformIndexing || !f12.shaderStorageImageArrayNonUniformIndexing || !f12.shaderUniformTexelBufferArrayNonUniformIndexing || !f12.shaderStorageTexelBufferArrayNonUniformIndexing)
        VRED("the device lacks non-uniform indexing of sampled image (%d), storage image (%d), uniform texel buffer (%d) or storage texel buffer (%d) arrays: a module that declares it fails to create",
             f12.shaderSampledImageArrayNonUniformIndexing, f12.shaderStorageImageArrayNonUniformIndexing, f12.shaderUniformTexelBufferArrayNonUniformIndexing, f12.shaderStorageTexelBufferArrayNonUniformIndexing);
    V.placed = placed && fmp.memoryMapPlaced; V.placed_range = placed && fmp.memoryMapRangePlaced; V.unmap_reserve = placed && fmp.memoryUnmapReserve;
    float prio = 1.f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = x_mr ? mr_chain(&f2) : (void *)&f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext};
    void vka_kgsl_before(int has_ext), vka_kgsl_after(int has_hint); vka_kgsl_before(x_ph);   // the kernel contexts the device's queue gets (the KGSL floor below)
    if ((r = vk.vkCreateDevice(V.pd, &dci, NULL, &V.dev))) { VRED("vkCreateDevice %d", r); return 0; }
#define D(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(V.dev, #f);
    VKA_DEVICE_FNS(D)
#undef D
    val_probe();
    if (!vk.vkMapMemory2KHR) vk.vkMapMemory2KHR = (PFN_vkMapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkMapMemory2");   // core in 1.4
    if (!vk.vkUnmapMemory2KHR) vk.vkUnmapMemory2KHR = (PFN_vkUnmapMemory2KHR)vk.vkGetDeviceProcAddr(V.dev, "vkUnmapMemory2");
    if (!vk.vkMapMemory2KHR || !vk.vkUnmapMemory2KHR) V.placed = V.placed_range = 0;
    // An advertised placed map can still refuse the guest pools' remaps (vka_remap_probe.h, from Banana Native v0.3.44: PanVK on a Mali gave VK_ERROR_MEMORY_MAP_FAILED at
    // every pool while advertising VK_EXT_map_memory_placed, so the GPU saw no game memory). The pools' own sequence (map, unmap, placed remap at an aligned reserved address,
    // placed unmap, map) on one 64 KiB buffer of the pools' memory type: when it fails, the run goes on as a driver without placed maps (the exported or hardware buffer alias
    // below, which are tested themselves). DM_VK_REMAP_PROBE=0 (restart) trusts the extension bit, as before
    if (V.placed && VKA_ENV("DM_VK_REMAP_PROBE", 1)) {
        VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   // (the guest type's rule, below)
        uint32_t gt = vka_mem_type(~0u, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (gt == UINT32_MAX) gt = vka_mem_type(~0u, hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
        if (gt == UINT32_MAX) gt = vka_mem_type(~0u, hv, 0);
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 pq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp}; vk.vkGetPhysicalDeviceProperties2(V.pd, &pq);
        char why[160]; int ok = vka_guest_remap_probe(&vk, V.dev, gt, V.bda, pp.minPlacedMemoryMapAlignment ? (size_t)pp.minPlacedMemoryMapAlignment : 4096, why, sizeof why);
        if (ok) VLOG("guest placed memory: %s", why);
        else { VRED("guest placed memory: the driver advertises placed maps, but %s: run as a driver without them (DM_VK_REMAP_PROBE=0: trust the extension)", why); V.placed = V.placed_range = V.unmap_reserve = 0; }
    }
    // DM_VK_ALIAS=fd / ahb (restart only, a diagnostic): run as a driver without placed maps does, on the exported alias or the hardware buffer alias, to test
    // those paths on a phone that has placed maps
    // DM_VK_ALIAS=prefer (the launcher's default since 2026-11-14, a game's Graphics setting "GPU memory sharing"): an alias when the driver has one (the
    // exported one, else the hardware buffer one), placed maps when it has none: unlike fd / ahb it never leaves the GPU without the game's memory
    const char *alias_env = getenv("DM_VK_ALIAS");
    int force_fd = alias_env && !strcmp(alias_env, "fd"), force_ahb = alias_env && !strcmp(alias_env, "ahb"), prefer = alias_env && !strcmp(alias_env, "prefer");
    // A game killed from outside on placed maps twice in a row takes the exported alias from then on (vka_placedkill.h: a Redmagic 11 Air; the cause in that
    // phone's kernel is open). DM_VK_PLACED_KILLS=<n>: the kills in a row that switch (2), 0: never; DM_VK_ALIAS=placed: placed maps again, the count cleared.
    // Its records are written on every start of a driver with placed maps, whatever the run's backing: the next run must know what this one was
    int auto_alias = 0, keep_placed = V.placed, keep_range = V.placed_range, keep_reserve = V.unmap_reserve;
    if (keep_placed) {
        int forget = alias_env && !strcmp(alias_env, "placed"), threshold = VKA_ENV("DM_VK_PLACED_KILLS", 2), asked = (alias_env && !forget) || threshold <= 0 || !x_fd || !vka_fd_enabled();
        if (placed_kill_check(forget, threshold, asked)) auto_alias = 1;
    }
    if (prefer) { auto_alias = 1; VLOG("DM_VK_ALIAS=prefer: an alias of guest memory when this driver has one, placed maps when it has none"); }
    if (force_fd || force_ahb) { V.placed = V.placed_range = V.unmap_reserve = 0; VLOG("DM_VK_ALIAS=%s: placed maps are left unused", alias_env); }
    if (auto_alias) { V.placed = V.placed_range = V.unmap_reserve = 0; force_fd = 1; }   // (force_fd: the exported alias is tried, and the unadvertised placed map only as the fallback below)
    // A driver that has the placed map but does not advertise it (Qualcomm 512.863 on the Adreno 740: the strings and vkMapMemory2 are in the driver, the extension is
    // not reported and cannot be enabled): try one placed map of a small allocation at a reserved address; when it lands there, use placed maps without enabling the
    // extension (whole allocations only, no unmap-reserve). DM_VK_PLACED_PROBE=0 turns the try off
    if (!V.placed && !force_fd && !force_ahb && vk.vkMapMemory2KHR && vk.vkUnmapMemory2KHR) {
        const char *e = getenv("DM_VK_PLACED_PROBE");
        if (!e || strcmp(e, "0")) V.placed = vka_placed_probe();
    }
    if (!V.placed && !force_ahb && x_fd && vka_fd_enabled()) {
        char why[160]; V.fd_alias = vka_fd_probe(&vk, V.pd, V.dev, &V.memp, V.bda, why, sizeof why);
        VLOG("exported guest memory alias: %s (%s; DM_VK_FD_ALIAS=0: off)", V.fd_alias ? "on" : "unavailable", why);
    }
    // The third way to guest-address memory (V.fd_alias 2): a hardware buffer the host maps over the pool's address itself (vka_ahb_alias.h)
    if (!V.placed && !V.fd_alias && x_ahb && vka_ahb_enabled()) {
        char why[240]; int ok = vka_ahb_probe(vk.vkGetDeviceProcAddr, vk.vkGetPhysicalDeviceExternalBufferProperties, V.pd, V.dev, &V.memp, V.qf, V.bda, why, sizeof why);
        if (ok) V.fd_alias = 2;
        VLOG("hardware buffer guest memory alias: %s (%s; DM_VK_AHB_ALIAS=0: off)", ok ? "on" : "unavailable", why);
        vka_mem_backing_note(why);   // (said again at the first pool: these lines come before the game's log file is open)
    } else if (!V.placed && !V.fd_alias) vka_mem_backing_note(!x_ahb ? "the driver lacks VK_ANDROID_external_memory_android_hardware_buffer or VK_EXT_queue_family_foreign" : "off (DM_VK_AHB_ALIAS=0)");
    if (auto_alias && !V.fd_alias) {   // (no alias on this driver: placed maps are better than no GPU memory at all)
        if (keep_placed) { V.placed = keep_placed; V.placed_range = keep_range; V.unmap_reserve = keep_reserve; }
        else if (vk.vkMapMemory2KHR && vk.vkUnmapMemory2KHR) { const char *e = getenv("DM_VK_PLACED_PROBE"); if (!e || strcmp(e, "0")) V.placed = vka_placed_probe(); }
        if (V.placed) { VLOG("guest memory: this driver has no alias to take in place of its placed maps: placed maps this run"); vka_mem_backing_note(""); }
    }
    vka_placed_unused = keep_placed && !V.placed;
    vka_host_compile_fns();
    vka_shader_pfb_hook();   // (over the FPCR wrapper: the module it makes is the rewritten one; vka_shader.c "Texture pool reads")
    vk.vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    if (x_ph && fph.queuePerfHint) V.perf_hint = (void *)vk.vkGetDeviceProcAddr(V.dev, "vkQueueSetPerfHintQCOM");
    VLOG("GPU clock floor (VK_QCOM_queue_perf_hint): %s", V.perf_hint ? "available (DM_VK_GPU_FLOOR)" : x_ph ? "extension without the feature" : "not on this device");
    vka_kgsl_after(V.perf_hint != NULL);
#if defined(__ANDROID__)
    { int vka_floor_adpf(void), vka_floor_kgsl(void); if (!V.perf_hint && !vka_floor_kgsl()) VLOG("GPU clock floor through ADPF: %s", vka_floor_adpf() ? "each frame's span and GPU time reported against the game's shortest period (DM_VK_GPU_FLOOR_ADPF)" : "off (DM_VK_GPU_FLOOR=0 or DM_VK_GPU_FLOOR_ADPF=0)"); }
#endif

    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pmp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = placed ? &pmp : NULL};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p13};
    VkPhysicalDevicePushDescriptorPropertiesKHR ppd = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR, .pNext = &p11};
    VkPhysicalDeviceMaintenance3Properties pm3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = V.push_desc ? (void *)&ppd : (void *)&p11};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT pgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT, .pNext = &pm3};
    VkPhysicalDeviceDriverProperties pdr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = x_gpl ? (void *)&pgpl : (void *)&pm3};
    VkPhysicalDeviceMeshShaderPropertiesEXT pmesh = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT, .pNext = &pdr};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = x_mesh ? (void *)&pmesh : (void *)&pdr};
    vk.vkGetPhysicalDeviceProperties2(V.pd, &p2);
    VLOG("driver: %s (%s), driver ID %d, conformance %u.%u.%u.%u, driver version %#x; %s", pdr.driverName, pdr.driverInfo, pdr.driverID, pdr.conformanceVersion.major,
         pdr.conformanceVersion.minor, pdr.conformanceVersion.subminor, pdr.conformanceVersion.patch, V.props.driverVersion, vka_driver_tag() ? "custom (DM_VK_DRIVER)" : "the system's");
    V.driver_id = (int)pdr.driverID;
    if (vka_driver_tag()) {   // adrenotools hands back the system's driver when the custom one does not load (vka_driverpath.h; Banana Native PR 409): say so in the game's log
        void vka_driver_lib(const char **dir, const char **name); int vka_driver_mapped(void); const char *dir, *name; vka_driver_lib(&dir, &name);
        int m = vka_driver_mapped();
        if (!m) VRED("custom GPU driver %s did not take: %s%s is not mapped in the process, so the driver line above is the phone's own driver (adrenotools fell back; logcat tags adrenotools / hook_impl say why)",
                     vka_driver_tag(), dir ? dir : "", name ? name : "");
        else VLOG("custom GPU driver %s: %s%s %s", vka_driver_tag(), dir ? dir : "", name ? name : "", m > 0 ? "is mapped in the process" : "(/proc/self/maps not read: not checked)");
    }
    { const char *e = getenv("DM_VK_GPL"); int want = !(e && !strcmp(e, "0"));   // restart only: the libraries are made with the device's pipelines
      V.gpl = want && x_gpl && fgpl.graphicsPipelineLibrary && pgpl.graphicsPipelineLibraryFastLinking;
      VLOG("graphics pipeline libraries: %s", V.gpl ? "on (a pipeline the cache lacks is linked from stage libraries, the optimized one is made in the background; DM_VK_GPL=0 off)"
           : !x_gpl ? "not on this device" : !fgpl.graphicsPipelineLibrary ? "extension without the feature" : !pgpl.graphicsPipelineLibraryFastLinking ? "no fast linking" : "off (DM_VK_GPL=0)"); }
    V.placed_align = pmp.minPlacedMemoryMapAlignment ? pmp.minPlacedMemoryMapAlignment : 4096; V.max_alloc = pm3.maxMemoryAllocationSize;
    V.max_push = V.push_desc ? ppd.maxPushDescriptors : 0;
    if (V.mesh && !vk.vkCmdDrawMeshTasksEXT) V.mesh = V.task = 0;
    if (V.mesh) { V.mesh_inv = pmesh.maxMeshWorkGroupInvocations; V.task_inv = pmesh.maxTaskWorkGroupInvocations; V.mesh_out_v = pmesh.maxMeshOutputVertices;
                  V.mesh_out_p = pmesh.maxMeshOutputPrimitives; V.task_payload = pmesh.maxTaskPayloadSize;
                  V.task_groups = pmesh.maxTaskWorkGroupCount[0]; V.mesh_groups = pmesh.maxMeshWorkGroupCount[0]; }
    VLOG("mesh shading (NVN2 task and mesh programs): %s", V.task ? "on" : V.mesh ? "mesh shaders only (no taskShader)" : !mesh_ext ? "not on this device" : mesh_qcom ? "the compute fallback on Qualcomm's driver, which has VK_EXT_mesh_shader (DM_VK_MESH=1: native)" : !x_mesh ? "off (DM_VK_MESH=0)" : "extension without the mesh stage");
    if (V.mesh) VLOG("mesh shading limits: workgroups mesh %u / task %u invocations, mesh output %u vertices / %u primitives / %u components / %u bytes, task payload %u bytes, mesh shared %u bytes",
                     V.mesh_inv, V.task_inv, V.mesh_out_v, V.mesh_out_p, pmesh.maxMeshOutputComponents, pmesh.maxMeshOutputMemorySize, V.task_payload, pmesh.maxMeshSharedMemorySize);
    VLOG("subgroups: size %u, stages %#x (geometry %s, fragment %s, vertex %s), operations %#x, quad operations in all stages %s, required size stages %#x", p11.subgroupSize, p11.subgroupSupportedStages,
         p11.subgroupSupportedStages & VK_SHADER_STAGE_GEOMETRY_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT ? "yes" : "NO", p11.subgroupSupportedStages & VK_SHADER_STAGE_VERTEX_BIT ? "yes" : "NO",
         p11.subgroupSupportedOperations, p11.subgroupQuadOperationsInAllStages ? "yes" : "no", p13.requiredSubgroupSizeStages);
    V.subgroup_stages = p11.subgroupSupportedStages;
    V.subgroup_req_stages = f13.subgroupSizeControl ? p13.requiredSubgroupSizeStages : 0;
    V.subgroup_size = p11.subgroupSize; V.subgroup_min = p13.minSubgroupSize ? p13.minSubgroupSize : p11.subgroupSize; V.subgroup_max = p13.maxSubgroupSize ? p13.maxSubgroupSize : p11.subgroupSize;
    if (!vk.vkCmdPushDescriptorSetKHR) vk.vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vk.vkGetDeviceProcAddr(V.dev, "vkCmdPushDescriptorSet");   // core in 1.4
    if (!vk.vkCmdPushDescriptorSetKHR) V.push_desc = 0, V.max_push = 0;
    if (!vk.vkCmdSetVertexInputEXT) V.vin_dyn = 0;
    vka_enc_install();
    vka_gbar_install();   // on top of the encoder: sees every work command the replay records (vka_gbar.c)
    const VkPhysicalDeviceLimits *lim = &V.props.limits;
    V.ub_align = lim->minUniformBufferOffsetAlignment ? lim->minUniformBufferOffsetAlignment : 256; V.sb_align = lim->minStorageBufferOffsetAlignment ? lim->minStorageBufferOffsetAlignment : 256;
    V.ub_range = lim->maxUniformBufferRange; V.sb_range = lim->maxStorageBufferRange; V.max_vin_bindings = lim->maxVertexInputBindings;
    vka_tex_pfb_flags();   // texture pool reads decided (and their buffers made) here, before any module is made, so no module's hook makes them under a caller's lock

    VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (V.guest_type == UINT32_MAX) V.guest_type = vka_mem_type(~0u, HV, 0);
    V.upload_type = V.guest_type;
    V.local_type = vka_mem_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);

    VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vk.vkCreateSemaphore(V.dev, &sci, NULL, &V.tl))) { VRED("timeline semaphore %d", r); return 0; }
    pthread_t th; if (pthread_create(&th, NULL, completion_main, NULL)) { VRED("cannot start the completion thread"); return 0; }
    pthread_detach(th);
    VLOG("device %s, Vulkan %u.%u.%u, driver %#x; placed maps %s (range %s, alignment %llu), buffer device address %s, update-after-bind %s, display timing %s; guest memory type %u (%#x), max allocation %llu MiB, at most %u allocations",
         V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), V.props.driverVersion,
         V.placed ? "yes" : "no", V.placed_range ? "yes" : "no", (unsigned long long)V.placed_align, V.bda ? "yes" : "no", V.uab ? "yes" : "no", V.display_timing ? "yes" : "no",
         V.guest_type, V.guest_type < 32 ? V.memp.memoryTypes[V.guest_type].propertyFlags : 0, (unsigned long long)(V.max_alloc >> 20), V.props.limits.maxMemoryAllocationCount);   // (images take one allocation each: Mario Kart World ran out of them)
    VLOG("draw path: push descriptors %s (%u), vertex input dynamic state %s, null descriptors %s, 8-bit indices %s, instance divisors %s, logic op %s, dual source %s, geometry %s, tessellation %s; "
         "uniform alignment %llu (range %llu), storage alignment %llu, subgroups %u (%u-%u)",
         V.push_desc ? "yes" : "no", V.max_push, V.vin_dyn ? "yes" : "no", V.null_desc ? "yes" : "no", V.idx8 ? "yes" : "no", V.divisor ? "yes" : "no", V.logic_op ? "yes" : "no",
         V.dual_src ? "yes" : "no", V.geom ? "yes" : "no", V.tess ? "yes" : "no", (unsigned long long)V.ub_align, (unsigned long long)V.ub_range, (unsigned long long)V.sb_align, V.subgroup_size, V.subgroup_min, V.subgroup_max);
    if (!V.placed && !V.fd_alias) VRED("no placed maps, exported alias or hardware buffer alias of guest memory: guest memory stays anonymous and the GPU sees no memory pool");
    if (placed && V.placed && VKA_ENV("DM_VK_PLACED_PROBE", 1)) { vka_placed_probe_n(64 << 10, 1); vka_placed_probe_n(32 << 20, 1); }   // (diagnostic only: V.placed stays as advertised)
    // The same facts kept for the game's log (vka_dev_facts_log): the device is made before the launcher opens that file, so none of the lines above is in it, and a
    // report from a phone nobody here has (a Mali, an Xclipse) starts from them
    snprintf(g_facts[0], sizeof g_facts[0], "device %s, vendor %#x, Vulkan %u.%u.%u, driver ID %d, driver version %#x; placed maps %s, guest memory alias %d, buffer device address %s, update-after-bind %s; "
             "guest memory type %u (%#x), max allocation %llu MiB, at most %u allocations and %u samplers",
             V.props.deviceName, V.props.vendorID, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), V.driver_id, V.props.driverVersion,
             V.placed ? "yes" : "no", V.fd_alias, V.bda ? "yes" : "no", V.uab ? "yes" : "no", V.guest_type, V.guest_type < 32 ? V.memp.memoryTypes[V.guest_type].propertyFlags : 0,
             (unsigned long long)(V.max_alloc >> 20), V.props.limits.maxMemoryAllocationCount, V.props.limits.maxSamplerAllocationCount);
    snprintf(g_facts[1], sizeof g_facts[1], "draw path: push descriptors %s (%u), vertex input dynamic state %s, null descriptors %s, 8-bit indices %s, instance divisors %s, indirect count %s, logic op %s, "
             "dual source %s, geometry %s, tessellation %s, mesh %s, sparse residency %s; uniform alignment %llu (range %llu), storage alignment %llu (range %llu), subgroups %u (%u-%u) in stages %#x",
             V.push_desc ? "yes" : "no", V.max_push, V.vin_dyn ? "yes" : "no", V.null_desc ? "yes" : "no", V.idx8 ? "yes" : "no", V.divisor ? "yes" : "no", V.indirect_count ? "yes" : "no", V.logic_op ? "yes" : "no",
             V.dual_src ? "yes" : "no", V.geom ? "yes" : "no", V.tess ? "yes" : "no", V.task ? "task and mesh" : V.mesh ? "mesh only" : "no", V.sparse_res ? "yes" : "no",
             (unsigned long long)V.ub_align, (unsigned long long)V.ub_range, (unsigned long long)V.sb_align, (unsigned long long)V.sb_range, V.subgroup_size, V.subgroup_min, V.subgroup_max, V.subgroup_stages);
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES}; VkPhysicalDeviceProperties2 pq = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vk.vkGetPhysicalDeviceProperties2(V.pd, &pq);
    snprintf(g_facts[2], sizeof g_facts[2], "features and limits: BC textures %s, ASTC %s, ETC2 %s, vertex stage stores %s, fragment stage stores %s, 64-bit integers %s, 16-bit integers %s, storage images without a format read %s / write %s, "
             "multi draw indirect %s, depth clamp %s; compute shared memory %u bytes, workgroup %u invocations, update-after-bind sampled images %u a stage / %u a set, bound sets %u",
             f2.features.textureCompressionBC ? "yes" : "no", f2.features.textureCompressionASTC_LDR ? "yes" : "no", f2.features.textureCompressionETC2 ? "yes" : "no", V.vstores ? "yes" : "no", V.fstores ? "yes" : "no",
             f2.features.shaderInt64 ? "yes" : "no", f2.features.shaderInt16 ? "yes" : "no", f2.features.shaderStorageImageReadWithoutFormat ? "yes" : "no", f2.features.shaderStorageImageWriteWithoutFormat ? "yes" : "no",
             f2.features.multiDrawIndirect ? "yes" : "no", f2.features.depthClamp ? "yes" : "no", V.props.limits.maxComputeSharedMemorySize, V.props.limits.maxComputeWorkGroupInvocations,
             p12.maxPerStageDescriptorUpdateAfterBindSampledImages, p12.maxDescriptorSetUpdateAfterBindSampledImages, V.props.limits.maxBoundDescriptorSets);
    if (!swap) VRED("no VK_KHR_swapchain: nothing is presented");
    vka_present_init();
    return 1;
}
// The host's floating-point environment. The game's code runs natively with the FPCR its SDK sets (Ocarina of Time's threads: 0x1000000, flush-to-zero), the host's code
// it calls (NVN, HLE) runs on with it, and a thread created there inherits it (Linux copies the creating thread's FP state). So the replay's threads and the driver's
// shader compiler ran with flush-to-zero: Qualcomm 891.11 compiled pipelines whose results differ from the same SPIR-V compiled with FPCR 0 (Ocarina of Time's
// gameplay scene about 2^12 too bright at the bloom pass, 90 times at the tone map's input; nxreplay, a process of its own, compiled them right). Every replay thread
// sets the host's FPCR (0: round to nearest, no flush-to-zero, IEEE NaNs) when it starts, the calls that compile (device creation, shader modules, pipelines:
// vka_host_compile_fns) set it around themselves on any thread, and so does a replay run inline on the game's thread (vka_worker.c, DM_VK_WORKER=0).
// DM_VK_GUEST_FPCR=1 (diagnostic, restart) leaves the inherited FPCR, as before; the pipeline cache then keeps no file (vka_pso.c), so no later run reads its pipelines
static inline uint64_t fpcr_get(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
int vka_guest_fp(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_GUEST_FPCR"); v = e && atoi(e) > 0; if (v) VLOG("DM_VK_GUEST_FPCR=1: the replay's threads and the driver's compiles keep the FPCR they inherit from the game"); } return v; }
uint64_t vka_host_fp(const char *who) {
    uint64_t v = fpcr_get();
    if (v && !vka_guest_fp()) {
        __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)0));
        static _Atomic int n; if (who && atomic_fetch_add(&n, 1) < 4) VLOG("%s: started with the game's FPCR %#llx, the host's (0) set", who, (unsigned long long)v);
    }
    return v;
}
void vka_fp_restore(uint64_t fpcr) { if (fpcr_get() != fpcr) __asm__ volatile("msr fpcr, %0" :: "r"(fpcr)); }
static PFN_vkCreateShaderModule g_real_csm; static PFN_vkCreateGraphicsPipelines g_real_cgp; static PFN_vkCreateComputePipelines g_real_ccp;
static VkResult VKAPI_CALL host_csm(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    uint64_t fp = vka_host_fp(NULL); VkResult r = g_real_csm(d, ci, a, m); vka_fp_restore(fp); return r; }
static VkResult VKAPI_CALL host_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t fp = vka_host_fp(NULL); VkResult r = g_real_cgp(d, c, n, ci, a, p); vka_fp_restore(fp); return r; }
static VkResult VKAPI_CALL host_ccp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t fp = vka_host_fp(NULL); VkResult r = g_real_ccp(d, c, n, ci, a, p); vka_fp_restore(fp); return r; }
static void vka_host_compile_fns(void) {   // the table's compiling calls through the wrappers above (every caller of vk.*: the replay, presents, NaN scan, NDSR)
    g_real_csm = vk.vkCreateShaderModule; g_real_cgp = vk.vkCreateGraphicsPipelines; g_real_ccp = vk.vkCreateComputePipelines;
    if (g_real_csm) vk.vkCreateShaderModule = host_csm;
    if (g_real_cgp) vk.vkCreateGraphicsPipelines = host_cgp;
    if (g_real_ccp) vk.vkCreateComputePipelines = host_ccp;
}
int vka_init(void) {
    if (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE)) return g_state > 0;
    pthread_mutex_lock(&g_init_mu);
    if (!g_state) { uint64_t fp = vka_host_fp(NULL); __atomic_store_n(&g_state, init_once() ? 1 : -1, __ATOMIC_RELEASE); vka_fp_restore(fp); }   // (the driver's threads made here inherit it)
    pthread_mutex_unlock(&g_init_mu);
    return g_state > 0;
}
int vka_ok(void) { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) > 0; }

static VkResult submit_x(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal, VkFence fence);
// Every vka_submit's time waiting for the queue's lock and in the driver's vkQueueSubmit, and the calls (cumulative; the pace cost line, vka_perf.c)
static _Atomic uint64_t g_qs_lock_ns, g_qs_call_ns, g_qs_n;
void vka_submit_stats(uint64_t *lock_ns, uint64_t *call_ns, uint64_t *n) { *lock_ns = g_qs_lock_ns; *call_ns = g_qs_call_ns; *n = g_qs_n; }
// Who else holds the queue lock, and for how long (the pace cost line): the present's submission and vkQueuePresentKHR, and the GPU clock floor
static _Atomic uint64_t g_qh_ns[4];
void vka_qhold_add(int k, uint64_t ns) { g_qh_ns[k & 3] += ns; }
void vka_qhold_stats(uint64_t *v) { for (int i = 0; i < 4; i++) v[i] = g_qh_ns[i]; }
VkResult vka_submit(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal) {
    return submit_x(cb, signal, nwait, wait, wait_vals, stages, bin_signal, VK_NULL_HANDLE);
}
// A submission with a fence of its own (vka_exec.c: a commit no timeline value signals yet, whose command buffer slot is reused once that fence is reached)
VkResult vka_submit_fence(VkCommandBuffer cb, uint64_t signal, VkFence fence) { return submit_x(cb, signal, 0, NULL, NULL, NULL, VK_NULL_HANDLE, fence); }
static VkResult submit_x(VkCommandBuffer cb, uint64_t signal, uint32_t nwait, const VkSemaphore *wait, const uint64_t *wait_vals, const VkPipelineStageFlags *stages, VkSemaphore bin_signal, VkFence fence) {
    VkSemaphore sig[2]; uint64_t sv[2]; uint32_t ns = 0;
    if (signal) { sig[ns] = V.tl; sv[ns++] = signal; }
    if (bin_signal) { sig[ns] = bin_signal; sv[ns++] = 0; }
    // The waits, and one more for the sparse binds queued before it (vks_queue_locked)
    VkSemaphore ws[9]; uint64_t wv[9]; VkPipelineStageFlags wst[9];
    if (nwait > 8) { VRED_ONCE("vka_submit: %u waits, at most 8 taken: the rest are dropped", nwait); nwait = 8; }
    for (uint32_t i = 0; i < nwait; i++) { ws[i] = wait[i]; wv[i] = wait_vals[i]; wst[i] = stages[i]; }
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&V.qmu);
    uint64_t t1 = now_ns();
    vks_queue_locked(&nwait, ws, wv, wst);
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = nwait, .pWaitSemaphoreValues = wv, .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .waitSemaphoreCount = nwait, .pWaitSemaphores = ws, .pWaitDstStageMask = wst,
                       .commandBufferCount = cb ? 1 : 0, .pCommandBuffers = &cb, .signalSemaphoreCount = ns, .pSignalSemaphores = sig};
    VkResult r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, fence);
    g_qs_lock_ns += t1 - t0; g_qs_call_ns += now_ns() - t1; g_qs_n++;
    if (!r && signal > V.tl_queued) __atomic_store_n(&V.tl_queued, signal, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&V.qmu);
    if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("vkQueueSubmit");
    return r;
}
// The queue idle, bounded by the hang time: the timeline up to the last value handed to it (the present's copies wait for no more), then vkQueueWaitIdle for the rest
int vka_queue_idle(void) {
    if (vka_hung()) return 0;
    uint64_t q = vka_fence_queued();
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &V.tl, .pValues = &q};
    VkResult r = q ? vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns()) : VK_SUCCESS;
    if (r) { VRED("queue idle: fence %llu not reached in %.1f s (%d): not waited for", (unsigned long long)q, vka_hang_ns() / 1e9, r); return 0; }
    pthread_mutex_lock(&V.qmu); vk.vkQueueWaitIdle(V.q); pthread_mutex_unlock(&V.qmu);
    return 1;
}
uint64_t vka_fence_queued(void) { return __atomic_load_n(&V.tl_queued, __ATOMIC_ACQUIRE); }

// ---------- Fences ----------
// Fence n is reserved by the submission that signals timeline value n. The completion thread waits for the timeline, then runs that submission's CPU-side effects in order
// and only then publishes n as done: a SyncWait that returns has seen the events and counters of everything before it, as nvn_mtl.m's post_complete does
enum { POST_RING = 1024 };
typedef struct { uint64_t fence; void *ops; int n; void (*run)(void *, int); } post_rec;
static post_rec g_post[POST_RING];
typedef struct after_rec { struct after_rec *next; uint64_t fence; void (*fn)(void *); void *arg; } after_rec;
static after_rec *g_after;
static uint64_t g_fence, g_submitted, g_done;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER, g_scv = PTHREAD_COND_INITIALIZER;

uint64_t vka_fence_last(void) { pthread_mutex_lock(&g_fmu); uint64_t f = g_fence; pthread_mutex_unlock(&g_fmu); return f; }
uint64_t vka_fence_next(void) { pthread_mutex_lock(&g_fmu); uint64_t f = ++g_fence; pthread_mutex_unlock(&g_fmu); return f; }
void vka_fence_post(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    pthread_mutex_lock(&g_fmu);
    while (g_post[fence % POST_RING].fence && g_post[fence % POST_RING].fence != fence) pthread_cond_wait(&g_fcv, &g_fmu);   // that slot's earlier fence has not completed yet
    g_post[fence % POST_RING] = (post_rec){fence, ops, n, run};
    if (fence > g_submitted) g_submitted = fence;
    pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_fmu);
}
void vka_fence_cpu(uint64_t fence, void *ops, int n, void (*run)(void *, int)) {
    if (run) run(ops, n); else free(ops);
    pthread_mutex_lock(&g_fmu);
    if (fence > g_submitted) g_submitted = fence;
    if (fence > g_done) __atomic_store_n(&g_done, fence, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
}
uint64_t vka_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }
int vka_wait_done(uint64_t fence, uint64_t timeout_ns) {
    if (vka_done() >= fence) return 1;
    if (!timeout_ns) return 0;
    vka_worker_cpu_wait(fence);   // a queue the replay parked at a GPU wait (vka_qsched.h) goes on now if it holds this fence back
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_fmu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_fcv, &g_fmu, &dl)) break;
    int ok = g_done >= fence; pthread_mutex_unlock(&g_fmu);
    return ok;
}
void vka_after(uint64_t fence, void (*fn)(void *), void *arg) {
    pthread_mutex_lock(&g_fmu);
    if (g_done >= fence) { pthread_mutex_unlock(&g_fmu); fn(arg); return; }
    after_rec *a = malloc(sizeof *a); *a = (after_rec){g_after, fence, fn, arg}; g_after = a;
    pthread_mutex_unlock(&g_fmu);
}
// ---------- GPU hangs ----------
// A fence that makes no progress for DM_VK_HANG_S seconds (default 6) is a hang: the GPU, or the driver's queue, will not get there. Then nothing more is submitted (mtl_sub_begin
// records no command buffer, vka_submit refuses, the present drops its frames), every fence reads as done with its CPU-side effects run, pool memory the GPU may still read is
// kept instead of freed, and the host exits (dm_host_shutdown) unless DM_VK_HANG_EXIT=0. Before this, the completion thread logged the stall every 2 s and went on waiting, a pool
// given back was freed anyway after 2 s, and the present waited on the queue: on the Adreno 840 phone the system server restarted (a soft reboot) a few seconds later
static _Atomic int g_hung;
int vka_hung(void) { return atomic_load_explicit(&g_hung, memory_order_acquire); }
uint64_t vka_hang_ns(void) {
    double s = VKA_ENVF("DM_VK_HANG_S", 6.0); if (!(s > 0.5)) s = 6.0;
    return (uint64_t)(s * 1e9);
}
static void *hang_exit_main(void *arg) { (void)arg; pthread_setname_np(pthread_self(), "vk-hang"); dm_host_shutdown("GPU hang"); return NULL; }
void vka_exec_hang_report(uint64_t fence);   // vka_exec.c: what the stuck submission held
// Based on BananaNative 0.3.0: request bounded textual fault details, never a vendor binary.
static void fault_report(void) {
    if (!g_fault_on) { VLOG("GPU fault details unavailable (VK_EXT_device_fault absent or disabled)"); return; }
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vk.vkGetDeviceProcAddr(V.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) { VRED("GPU fault: vkGetDeviceFaultInfoEXT missing"); return; }
    VkDeviceFaultCountsEXT n = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult r = get(V.dev, &n, NULL);
    if (r && r != VK_INCOMPLETE) { VRED("GPU fault counts: %d", r); return; }
    if (n.addressInfoCount > 32) n.addressInfoCount = 32;
    if (n.vendorInfoCount > 32) n.vendorInfoCount = 32;
    n.vendorBinarySize = 0;
    VkDeviceFaultAddressInfoEXT ai[32] = {0}; VkDeviceFaultVendorInfoEXT vi[32] = {0};
    VkDeviceFaultInfoEXT fi = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = n.addressInfoCount ? ai : NULL, .pVendorInfos = n.vendorInfoCount ? vi : NULL};
    r = get(V.dev, &n, &fi);
    if (r && r != VK_INCOMPLETE) { VRED("GPU fault details: %d", r); return; }
    VRED("GPU fault: %.*s (%u addresses, %u vendor codes)", (int)sizeof fi.description, fi.description, n.addressInfoCount, n.vendorInfoCount);
    for (uint32_t i = 0; i < n.addressInfoCount && i < 32; i++)
        VRED("GPU fault address %#llx, precision %#llx, type %u", (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision, ai[i].addressType);
    for (uint32_t i = 0; i < n.vendorInfoCount && i < 32; i++)
        VRED("GPU vendor fault %.*s: code %#llx, data %#llx", (int)sizeof vi[i].description, vi[i].description, (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
}
static void hang_declare(uint64_t want, uint64_t at, double s, const char *what);
void vka_device_lost(const char *what) {
    if (vka_hung()) return;
    char message[128]; snprintf(message, sizeof message, "%s returned -4 (device lost)", what);
    uint64_t done = vka_done(); hang_declare(done + 1, done, 0, message);
}
static void hang_declare(uint64_t want, uint64_t at, double s, const char *what) {
    if (atomic_exchange(&g_hung, 1)) return;
    VRED("GPU hang: %s; fence %llu not reached in %.1f s (the timeline at %llu, %llu handed to the queue, %llu reserved): nothing more is submitted, every fence reads as done",
         what, (unsigned long long)want, s, (unsigned long long)at, (unsigned long long)vka_fence_queued(), (unsigned long long)vka_fence_last());
    vka_exec_hang_report(at + 1);
    if (strstr(what, "-4")) fault_report();
    if (!VKA_ENV("DM_VK_HANG_EXIT", 1)) { VRED("GPU hang: DM_VK_HANG_EXIT=0, the game runs on without the GPU"); return; }
    VRED("GPU hang: the host exits (DM_VK_HANG_EXIT=0 runs on without the GPU)");
    pthread_t th; if (!pthread_create(&th, NULL, hang_exit_main, NULL)) pthread_detach(th);
}

// A host wait for the timeline. Test switch DM_VK_TL_POLL=1 (live): it reads the counter every 0.25 ms instead of blocking in vkWaitSemaphores (does a driver's
// blocking wait hold its submissions back? Turnip v32 on KGSL, Ocarina of Time's note)
VkResult vka_tl_wait(VkSemaphore s, uint64_t v, uint64_t timeout_ns) {
    if (vka_hung()) return VK_ERROR_DEVICE_LOST;
    if (!VKA_ENV("DM_VK_TL_POLL", 0)) { VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &s, .pValues = &v}; return vk.vkWaitSemaphores(V.dev, &wi, timeout_ns); }
    uint64_t end = now_ns() + timeout_ns;
    for (;;) {
        uint64_t c = 0; VkResult r = vk.vkGetSemaphoreCounterValue(V.dev, s, &c);
        if (r) return r;
        if (c >= v) return VK_SUCCESS;
        if (now_ns() >= end) return VK_TIMEOUT;
        struct timespec ts = {0, 250000}; nanosleep(&ts, NULL);
    }
}
static void *completion_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-complete"); vka_host_fp("vk-complete"); void vka_perf_thread(int nice); vka_perf_thread(-8);   // vka_perf.c
    uint64_t stall_t0 = 0, stall_at = 0; int stall_logged = 0;
    for (;;) {
        pthread_mutex_lock(&g_fmu);
        while (g_done >= g_submitted) pthread_cond_wait(&g_scv, &g_fmu);
        // Wait for the next fence, not the last one handed to the queue: each fence is published (and its waiters woken) when the GPU reaches it. Waiting for the last one
        // held every earlier fence back until all work queued at that moment had run: Donkey Kong Bananza waits for its previous frame's last fence after queueing two
        // submissions of the next frame, so its wait ended only when those were done too. DM_VK_DONE_LAST=1 (live) waits for the last one, as before, for A/B runs
        uint64_t sub = g_submitted, done = g_done, want = VKA_ENV("DM_VK_DONE_LAST", 0) ? sub : done + 1; pthread_mutex_unlock(&g_fmu);
        uint64_t v = 0;
        if (!vka_hung()) {
            VkResult r = vka_tl_wait(V.tl, want, 500000000ull);
            if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("vkWaitSemaphores");
            if (!vka_hung()) {
                VkResult cr = vk.vkGetSemaphoreCounterValue(V.dev, V.tl, &v);
                if (cr == VK_ERROR_DEVICE_LOST) vka_device_lost("vkGetSemaphoreCounterValue");
            }
            uint64_t t = now_ns();
            if (r == VK_TIMEOUT && v <= done) {   // no progress in this wait
                if (!stall_t0 || stall_at != done) { stall_t0 = t - 500000000ull; stall_at = done; stall_logged = 0; }
                double st = (t - stall_t0) / 1e9;
                if (st >= 2 && !stall_logged) { stall_logged = 1; VRED("GPU: fence %llu not reached after %.1f s (at %llu, %llu handed to the queue)", (unsigned long long)want, st, (unsigned long long)v, (unsigned long long)vka_fence_queued()); }
                if (t - stall_t0 < vka_hang_ns()) continue;
                hang_declare(want, v, st, "no progress");
            } else if (r && r != VK_TIMEOUT) {
                char w[64]; snprintf(w, sizeof w, "vkWaitSemaphores %d (the device is lost)", r);
                hang_declare(want, v, 0, w);
            } else stall_t0 = 0;
        }
        if (vka_hung()) v = sub;   // every fence submitted so far is taken as done: its CPU-side effects run, the guest's waits return
        if (v > sub) v = sub;
        for (uint64_t f = done + 1; f <= v; f++) {
            post_rec *p = &g_post[f % POST_RING];
            if (p->fence == f) { if (p->run) p->run(p->ops, p->n); else free(p->ops); }
        }
        pthread_mutex_lock(&g_fmu);
        for (uint64_t f = done + 1; f <= v; f++) if (g_post[f % POST_RING].fence == f) g_post[f % POST_RING].fence = 0;
        if (v > g_done) __atomic_store_n(&g_done, v, __ATOMIC_RELEASE);
        after_rec *run = NULL, **pp = &g_after;
        while (*pp) { after_rec *a = *pp; if (a->fence <= g_done) { *pp = a->next; a->next = run; run = a; } else pp = &a->next; }
        pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
        while (run) { after_rec *a = run; run = a->next; a->fn(a->arg); free(a); }
    }
    return NULL;
}

// ---------- Scratch memory ----------
// Blocks of host-visible memory handed out linearly. Two owners: the submission being recorded (class 0: the blocks retire at its fence, vka_scratch_retire) and the
// open upload batch (class 1, while vka_scr_up is set on the calling thread: the blocks retire at the batch's value on the upload timeline, vka_up_flush). A block goes
// back to use once that value is reached
typedef struct sblk { struct sblk *next; VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; VkDeviceSize size, used; uint64_t fence; int up; } sblk;
static sblk *g_open[2], *g_busy;
static _Atomic uint64_t g_scratch_bytes; static _Atomic uint32_t g_scratch_n;
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
_Thread_local int vka_scr_up;
static VkSemaphore g_utl;   // the upload timeline: value n = upload batch n has completed
static VkDeviceSize g_ubytes;   // staging taken by the open upload batch (under g_smu)
static uint64_t up_done(void) {   // the upload timeline's value (every batch counts as done after a GPU hang)
    if (vka_hung()) return UINT64_MAX;
    uint64_t v = 0;
    if (g_utl && vk.vkGetSemaphoreCounterValue(V.dev, g_utl, &v) == VK_ERROR_DEVICE_LOST) {
        vka_device_lost("upload vkGetSemaphoreCounterValue"); return UINT64_MAX;
    }
    return v;
}
static int sblk_idle(const sblk *b, uint64_t done, uint64_t udone) { return b->fence <= (b->up ? udone : done); }
static sblk *sblk_new(VkDeviceSize n) {
    sblk *b = calloc(1, sizeof *b); b->size = n;
    // (indirect: an indirect mesh draw's {count, 1, 1} record, its count copied in by the GPU, vka_draw.c)
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &b->buf)) { free(b); return NULL; }
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, b->buf, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    void *p = NULL;
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &b->mem) || vk.vkBindBufferMemory(V.dev, b->buf, b->mem, 0) || vk.vkMapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p)) {
        VRED("scratch block of %llu bytes not allocated", (unsigned long long)n); if (b->mem) vk.vkFreeMemory(V.dev, b->mem, NULL); vk.vkDestroyBuffer(V.dev, b->buf, NULL); free(b); return NULL; }
    b->ptr = p; atomic_fetch_add(&g_scratch_bytes, n); atomic_fetch_add(&g_scratch_n, 1); vka_mem_grew(n); return b;
}
uint64_t vka_scratch_bytes(uint32_t *blocks) { if (blocks) *blocks = atomic_load(&g_scratch_n); return atomic_load(&g_scratch_bytes); }
void vka_mem_grew(uint64_t bytes) {
    static _Atomic uint64_t acc, step = 1;
    uint64_t s = atomic_load_explicit(&step, memory_order_relaxed);
    if (s == 1) { const char *e = getenv("DM_VK_MEMLOG"); s = (e ? strtoull(e, NULL, 10) : 512) << 20; atomic_store_explicit(&step, s, memory_order_relaxed); }
    if (!s || atomic_fetch_add(&acc, bytes) + bytes < s) return;
    atomic_store(&acc, 0);
    int ni; uint32_t ns; uint64_t img = vka_tex_mem_nolock(&ni), scr = vka_scratch_bytes(&ns), pl = vka_mem_placed_nolock(), sp = vks_bytes_nolock();
    VLOG("memory: placed pools %.0f MiB, images %.0f MiB (%d), sparse tile blocks %.0f MiB, scratch %.0f MiB (%u blocks): %.0f MiB", pl / 1048576.0, img / 1048576.0, ni, sp / 1048576.0,
         scr / 1048576.0, ns, (pl + img + sp + scr) / 1048576.0);
}
// Idle blocks (their value reached) larger than the standard 16 MiB, or beyond 64 MiB of idle blocks in all, are freed (under g_smu): at every miss and at every retire.
// Kept, every block stayed for the process's life: Mario Kart World 1.8.0's boot uploads (levels of up to 144 MiB, each in a block of its size) left 1.4 GB of scratch
// before its first frame, and the phone's low-memory killer stopped it
static void sblk_trim(uint64_t done, uint64_t udone) {
    VkDeviceSize idle = 0;
    for (sblk **pp = &g_busy; *pp;) {
        sblk *b = *pp;
        int id = sblk_idle(b, done, udone);
        if (id && (b->size > (16u << 20) || idle + b->size > (64u << 20))) {
            *pp = b->next; vk.vkUnmapMemory(V.dev, b->mem); vk.vkDestroyBuffer(V.dev, b->buf, NULL); vk.vkFreeMemory(V.dev, b->mem, NULL);
            atomic_fetch_sub(&g_scratch_bytes, b->size); atomic_fetch_sub(&g_scratch_n, 1); free(b);
        } else { if (id) idle += b->size; pp = &b->next; }
    }
}
int vka_scratch(VkDeviceSize n, VkDeviceSize align, vka_span *out) {
    if (!align) align = 16;
    int c = vka_scr_up ? 1 : 0;
    pthread_mutex_lock(&g_smu);
    sblk *b = g_open[c];
    VkDeviceSize o = b ? (b->used + align - 1) / align * align : 0;
    if (!b || o + n > b->size) {
        // Best fit among the idle blocks (a 144 MiB block kept for a 1 KB request stays alive), then the idle ones beyond the budget freed
        uint64_t done = vka_done(), udone = up_done(); sblk **best = NULL;
        for (sblk **pp = &g_busy; *pp; pp = &(*pp)->next) if ((*pp)->size >= n && sblk_idle(*pp, done, udone) && (!best || (*pp)->size < (*best)->size)) best = pp;
        b = NULL; if (best) { b = *best; *best = b->next; }
        sblk_trim(done, udone);
        if (!b) b = sblk_new(n > (16u << 20) ? (n + 0xFFFF) & ~(VkDeviceSize)0xFFFF : 16u << 20);
        if (!b) { pthread_mutex_unlock(&g_smu); return 0; }
        b->used = 0; b->up = c; b->next = g_open[c]; g_open[c] = b; o = 0;
    }
    b->used = o + n; *out = (vka_span){b->buf, b->ptr + o, o};
    if (c) g_ubytes += n;
    pthread_mutex_unlock(&g_smu);
    return 1;
}
static void retire(int c, uint64_t value) {
    pthread_mutex_lock(&g_smu);
    while (g_open[c]) { sblk *b = g_open[c]; g_open[c] = b->next; b->fence = value; b->next = g_busy; g_busy = b; }
    if (c) { g_ubytes = 0; sblk_trim(vka_done(), up_done()); }   // (the submission's blocks are trimmed at a miss only: a game that takes more than 16 MiB a frame would make and free them every frame)
    pthread_mutex_unlock(&g_smu);
}
void vka_scratch_retire(uint64_t fence) { retire(0, fence); }

// ---------- Upload batches ----------
// The first content of an image the replay makes (its upload from guest memory, or its clear) is recorded into a command buffer of its own, an upload batch, instead of
// the submission being recorded. The batch goes to the queue when its staging passes DM_VK_UPLOAD_BATCH_MB (live, default 64) and always before the replay's next part
// or submission goes (vka_exec.c: chunk, vka_rp_sub_commit), so the queue runs it before every command that uses the image; it ends with a full barrier, whose second
// scope covers every later submission. Its staging retires when the GPU reaches the batch's value on the upload timeline, not at the fence of the submission it was made
// in: one submission that makes hundreds of images (Mario Kart World 1.8.0 at boot: 29,000 images, 1.5 GB of staging in one submission) no longer holds all of it.
// When the batches in flight hold more than DM_VK_UPLOAD_INFLIGHT_MB (live, default 256) of staging, the replay thread waits for the oldest. Only a new image is safe here:
// nothing recorded earlier in the submission reads it (a re-upload of a live image stays in the submission, in order with the draws before it). DM_VK_UPLOAD_BATCH=0 (live)
// records first contents into the submission, as before
enum { NUP = 16 };
static struct { VkCommandBuffer cb; uint64_t val; } g_ub[NUP];
static VkCommandPool g_upool; static uint64_t g_uval; static int g_ui = -1; static unsigned g_unext;   // g_ui: the open batch's slot (-1: none); replay thread only
static uint64_t g_n_ubatch, g_n_uwait, g_uwait_ns;
static int up_wait(uint64_t v) {   // 1 = the upload timeline reached v
    if (up_done() >= v) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_utl, .pValues = &v};
    uint64_t t0 = now_ns(); VkResult r = vk.vkWaitSemaphores(V.dev, &wi, vka_hang_ns());
    if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("upload vkWaitSemaphores");
    g_n_uwait++; g_uwait_ns += now_ns() - t0;
    if (r && !vka_hung()) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("upload batch %llu not done in %.1f s (%d)", (unsigned long long)v, vka_hang_ns() / 1e9, r); }
    return !r || vka_hung();
}
int vka_up_on(void) { return VKA_ENV("DM_VK_UPLOAD_BATCH", 1) != 0; }
VkCommandBuffer vka_up_begin(void) {
    if (g_ui < 0) {
        static int failed; if (failed) return VK_NULL_HANDLE;
        if (!g_upool) {
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qf};
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NUP};
            VkSemaphoreTypeCreateInfo st = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
            VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
            VkCommandBuffer cbs[NUP]; VkCommandPool p = VK_NULL_HANDLE;
            if (vk.vkCreateCommandPool(V.dev, &pi, NULL, &p) || ((ai.commandPool = p), vk.vkAllocateCommandBuffers(V.dev, &ai, cbs)) || vk.vkCreateSemaphore(V.dev, &sci, NULL, &g_utl)) {
                VRED_ONCE("upload batches not set up: first contents are recorded into the submission"); g_utl = VK_NULL_HANDLE; failed = 1; return VK_NULL_HANDLE; }
            for (int i = 0; i < NUP; i++) g_ub[i].cb = cbs[i];
            g_upool = p;
        }
        if (vka_hung()) return VK_NULL_HANDLE;
        unsigned k = g_unext % NUP;   // (16 slots: the backpressure below keeps them mostly done)
        if (g_ub[k].val && !up_wait(g_ub[k].val)) return VK_NULL_HANDLE;   // (its batch still runs: the caller records into the submission)
        g_unext++;
        vk.vkResetCommandBuffer(g_ub[k].cb, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (vk.vkBeginCommandBuffer(g_ub[k].cb, &bi)) return VK_NULL_HANDLE;
        g_ui = (int)k;
    }
    vka_scr_up = 1;
    return g_ub[g_ui].cb;
}
void vka_up_end(void) { vka_scr_up = 0; }
static int is_batch(VkCommandBuffer cb) { for (int i = 0; i < NUP; i++) if (cb && g_ub[i].cb == cb) return 1; return 0; }
// No batch can be begun (a slot's batch still runs after the hang time): the upload goes on in the submission being recorded, with its staging (the batch before it was sent,
// so its image's first barrier and copies come before; an upload stopped half-way would leave a tracked image with part of its content for good)
static VkCommandBuffer up_fallback(void) { vka_scr_up = 0; return vka_rp_cb(); }
VkCommandBuffer vka_up_cur(VkCommandBuffer cb) {   // the open batch when cb was one (vka_up_point may have sent it), else cb
    if (!is_batch(cb)) return cb;
    if (g_ui >= 0) return g_ub[g_ui].cb;
    VkCommandBuffer n = vka_up_begin(); return n ? n : up_fallback();
}
VkCommandBuffer vka_up_point(VkCommandBuffer cb) {
    if (g_ui < 0 || cb != g_ub[g_ui].cb) return cb;
    pthread_mutex_lock(&g_smu); VkDeviceSize n = g_ubytes; pthread_mutex_unlock(&g_smu);
    double mb = VKA_ENVF("DM_VK_UPLOAD_BATCH_MB", 64.0);
    if ((double)n < mb * 1048576.0) return cb;
    vka_up_flush();
    VkCommandBuffer nb = vka_up_begin(); return nb ? nb : up_fallback();
}
void vka_up_flush(void) {
    if (g_ui < 0) return;
    unsigned k = (unsigned)g_ui; g_ui = -1;
    VkCommandBuffer cb = g_ub[k].cb;
    vka_barrier(cb);   // everything the batch wrote, for every later submission on the queue
    VkResult r = vk.vkEndCommandBuffer(cb);
    uint64_t v = ++g_uval;
    VkSemaphore ws[1]; uint64_t wv[1]; VkPipelineStageFlags wst[1]; uint32_t nw = 0;   // the sparse binds queued before it (vks_queue_locked)
    VkTimelineSemaphoreSubmitInfo ti = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &v};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_utl};
    pthread_mutex_lock(&V.qmu);
    if (!r) {
        vks_queue_locked(&nw, ws, wv, wst);
        ti.waitSemaphoreValueCount = nw; ti.pWaitSemaphoreValues = wv; si.waitSemaphoreCount = nw; si.pWaitSemaphores = ws; si.pWaitDstStageMask = wst;
        r = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE);
    }
    pthread_mutex_unlock(&V.qmu);
    // Nothing on the GPU will signal it: the host does, so later batches and the slot are not held; only once the batch before it is done (a host signal must be below
    // every value still pending on the queue)
    if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("upload vkQueueSubmit");
    if (r && !vka_hung() && (v == 1 || up_wait(v - 1)) && !vka_hung()) { VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_utl, .value = v}; vk.vkSignalSemaphore(V.dev, &ss); }
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 8) VRED("upload batch %llu: %d (its images keep what they held)", (unsigned long long)v, r); }
    g_ub[k].val = v; g_n_ubatch++;
    retire(1, v);
}
// Backpressure: the batches in flight hold at most DM_VK_UPLOAD_INFLIGHT_MB of staging (the oldest is waited for; the GPU runs them in queue order). Called where the
// replay thread holds no lock (vka_tex_get after the table lock, chunk, the commit): the present and other threads that take the image table do not wait for the GPU
void vka_up_throttle(void) {
    if (!g_utl) return;
    double cap = VKA_ENVF("DM_VK_UPLOAD_INFLIGHT_MB", 256.0) * 1048576.0;
    for (;;) {
        uint64_t udone = up_done(), oldest = UINT64_MAX; VkDeviceSize fl = 0;
        pthread_mutex_lock(&g_smu);
        for (sblk *b = g_busy; b; b = b->next) if (b->up && b->fence > udone) { fl += b->size; if (b->fence < oldest) oldest = b->fence; }
        pthread_mutex_unlock(&g_smu);
        if ((double)fl <= cap || oldest == UINT64_MAX || !up_wait(oldest)) break;
    }
}
void vka_up_stats(uint64_t *batches, uint64_t *waits, double *wait_ms) { if (batches) *batches = g_n_ubatch; if (waits) *waits = g_n_uwait; if (wait_ms) *wait_ms = g_uwait_ns / 1e6; }
#endif

#if defined(__ANDROID__)
// DM_VK_GPU_FLOOR=s (live; default 100): the queue's GPU clock floor through VK_QCOM_queue_perf_hint, s % of the highest clock (100: the highest; 0: the driver's default
// governor). The GPU's governor follows its busy time, and the replay keeps the GPU idle while it records the start of a frame, so it clocks down: the same replayed level
// frame took 21-28 ms of GPU time with the clock up and 93-99 ms after two seconds of idle GPU (Adreno 840), and in the live level the GPU finished 23-33 ms after the
// recording at the default governor, 6-10 ms with the floor at 100 (with DM_VK_CHUNK_PASS). The game's frame-rate controller measures frame start to GPU end, so that tail
// decides between its 20 and 30 fps: in one live session (CPU capped at 2.23 GHz) 85 % of the frames at 30 fps with the floor at 100, 0-16 % at 70 or with the
// driver's governor. The cost is power while a frame's GPU work runs; a lower floor or 0 gives it back
// A hint is the queue's state until changed; the kernel takes the highest of all queues' floors
// DM_VK_GPU_FLOOR_AUTO=1 (live; default 0 = the fixed floor): the floor follows what the frame needs, between 0 and DM_VK_GPU_FLOOR. Each frame's span from the start of the replay's
// recording of its first submission to the GPU end of its last (vka_exec.c, vka_floor_span: recording plus the GPU tail) is held against the period the frame aims at: the game's
// shortest period (vka_floor_period_ns), not the period it presents at now. Over the last 30 frames: the 90th percentile within 2 ms of the period raises the floor a step (within
// 0.5 ms: to DM_VK_GPU_FLOOR at once); more than 5 ms below it (DM_VK_GPU_FLOOR_LO; raise: DM_VK_GPU_FLOOR_HI, 2 ms) for 60 frames since the last change lowers it a step. Steps of 25 %
enum { FSPAN = 64 };
static _Atomic uint64_t g_fspan[FSPAN], g_fspan_f[FSPAN];   // per frame (mod FSPAN): its span in ns, and which frame it is
static uint64_t g_floor_frames[5];                            // frames at each step (statistics)
static double g_p90_sum; static uint64_t g_p90_n;
// The period the floor aims at, for every floor (the queue hint's DM_VK_GPU_FLOOR_AUTO=1 and the ADPF floor's reports, vka_perf.c): the game's shortest period, the shortest
// present period it has asked for in this session and at most one 60 Hz vsync (16.7 ms), not the period it presents at now. A game with a dynamic present interval decides
// its interval from how long its frames take: DKB's controller (main+0x204a18) steps from 3 vsyncs (20 fps) back to 2 only when its measure of the frame is below 20 ms. At
// 20 fps the GPU idles a third of the time, its governor lowers the clock, the frame's GPU time grows to 33-36 ms (Adreno 840) and the game stays at 20 fps for good; a floor
// aiming at the current period (50 ms, or the 33.3 ms cap the auto floor had) sees that frame on target and never raises the clock. Aiming at the shortest period keeps the
// clock where the game can step back up. DM_VK_GPU_FLOOR_PERIOD=<ms> (live; default 0 = the shortest period) sets it instead (33.3: the cap before 2026-11-05)
_Atomic int64_t vka_period_min_ns;   // the shortest present period the game asked for (vka_perf.c vka_perf_frame); 0 = none yet
int64_t vka_floor_period_ns(void) {
    double ms = VKA_ENVF("DM_VK_GPU_FLOOR_PERIOD", 0.0);
    if (ms >= 4 && ms <= 200) return (int64_t)(ms * 1e6);
    int64_t m = atomic_load_explicit(&vka_period_min_ns, memory_order_relaxed), v = 16666667;
    return m >= 4000000 && m < v ? m : v;
}
void vka_floor_span(uint64_t frame, uint64_t ns) {           // the completion thread: a submission of frame ended ns after the frame began
    unsigned k = frame % FSPAN;
    if (atomic_load(&g_fspan_f[k]) != frame) { atomic_store(&g_fspan[k], 0); atomic_store(&g_fspan_f[k], frame); }
    if (ns > atomic_load(&g_fspan[k])) atomic_store(&g_fspan[k], ns);
}
uint64_t vka_floor_span_of(uint64_t frame) { unsigned k = frame % FSPAN; return atomic_load(&g_fspan_f[k]) == frame ? atomic_load(&g_fspan[k]) : 0; }   // 0 = not in (vka_perf.c DM_VK_ADPF_SPAN)
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int floor_auto(int top, uint64_t f) {
    static int cur = -1; static uint64_t last_f, changed_f;
    if (cur < 0 || cur > top) cur = top;
    if (f == last_f) return cur;
    last_f = f;
    uint64_t v[30]; int n = 0;
    for (uint64_t g = f > 3 ? f - 3 : 0; g > 0 && g + 34 > f && n < 30; g--) { unsigned k = g % FSPAN; if (atomic_load(&g_fspan_f[k]) == g && atomic_load(&g_fspan[k])) v[n++] = atomic_load(&g_fspan[k]); }
    if (n < 20) return cur;
    qsort(v, n, sizeof *v, cmp_u64);
    double p90 = v[n * 9 / 10] / 1e6, per = vka_floor_period_ns() / 1e6;
    int was = cur;
    g_p90_sum += p90; g_p90_n++;
    double hi = VKA_ENVF("DM_VK_GPU_FLOOR_HI", 2.0), lo = VKA_ENVF("DM_VK_GPU_FLOOR_LO", 5.0);   // ms below the period: raise above per - hi, lower below per - lo
    // A step down that has to be taken back within 60 frames is not tried again for a while (300 frames, doubling up to 4,800 each time it fails again): on the Adreno
    // 840 a floor of 75 instead of 100 added 5-15 ms to the level's span at once, and the controller went up and down every few frames
    static uint64_t hold_until, backoff = 300, lowered_f;
    if (p90 > per - 0.5) cur = top;
    else if (p90 > per - hi) cur = cur + 25 > top ? top : cur + 25;
    else if (p90 < per - lo && f - changed_f >= 60 && f >= hold_until) { cur = cur - 25 < 0 ? 0 : cur - 25; lowered_f = f; }
    if (cur > was && lowered_f && f - lowered_f < 60) { hold_until = f + backoff; backoff = backoff * 2 > 4800 ? 4800 : backoff * 2; lowered_f = 0; }
    if (lowered_f && f - lowered_f > 600) { backoff = 300; lowered_f = 0; }   // a step down that held
    if (cur != was) { changed_f = f; if (VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0)) VLOG("GPU clock floor %d -> %d %%: frame %llu, span p90 %.1f ms of %.1f", was, cur, (unsigned long long)f, p90, per); }
    g_floor_frames[cur / 25 < 5 ? cur / 25 : 4]++;
    return cur;
}
void vka_floor_stats(char *b, size_t n) {   // the replay statistics: frames at each floor step since the last call
    uint64_t t = 0; for (int i = 0; i < 5; i++) t += g_floor_frames[i];
    if (!t) { if (n) b[0] = 0; return; }
    snprintf(b, n, "GPU clock floor (auto): frames at 0/25/50/75/100 %%: %.0f/%.0f/%.0f/%.0f/%.0f %%; recording start to GPU end p90 %.1f ms on average", 100.0 * g_floor_frames[0] / t, 100.0 * g_floor_frames[1] / t, 100.0 * g_floor_frames[2] / t,
             100.0 * g_floor_frames[3] / t, 100.0 * g_floor_frames[4] / t, g_p90_n ? g_p90_sum / g_p90_n : 0.0);
    g_p90_sum = 0; g_p90_n = 0;
    memset(g_floor_frames, 0, sizeof g_floor_frames);
}
// The floor through the kernel driver (KGSL) for a Vulkan driver without VK_QCOM_queue_perf_hint on an Adreno (Turnip): the power constraint of the queue's own kernel context.
// Qualcomm's hint maps onto the same kernel call: IOCTL_KGSL_SETPROPERTY KGSL_PROP_PWR_CONSTRAINT {KGSL_CONSTRAINT_PWRLEVEL, context, level} (graphics-kernel adreno.c
// adreno_set_constraint), level 1 = the highest clock or 2-100 = a floor at that % of it, the hint's two kinds. On GPUs whose clock the GMU decides (gen8, the Adreno 840:
// not host_based_dcvs) the kernel hands it to the GMU at once as the context's rule (adreno_gen8_hwsched.c gen8_hwsched_set_pwrconstraint, H2F_MSG_CONTEXT_RULE), which holds
// the clock while that context's work runs; on host-DCVS GPUs (older) it applies at dispatch of submissions flagged KGSL_CONTEXT_PWR_CONSTRAINT and there only "highest" (1)
// is taken. The rule only reaches the GMU while the context is registered with it (after its first submission; again after the GPU slept), and Turnip does not create its
// context with that flag, so the level is sent again every 30 frames. Which contexts: the kernel lets a file descriptor set the constraint only on contexts it owns
// (kgsl_context_get_owner), so the contexts the driver made for the device's queue are the ids this process's /dev/kgsl-3d0 descriptors own after vkCreateDevice and did
// not own before (Turnip: one per VkQueue, tu_knl_kgsl.cc kgsl_submitqueue_new). The ownership test is the same ioctl with KGSL_CONSTRAINT_NONE, which changes nothing on a
// context without a constraint. Before 2026-11-05 Turnip's floor was the ADPF session (vka_perf.c), which did not raise the GPU clock: 282-646 MHz (mean 380) in DKB's
// level where the system driver ran 1,100-1,200 MHz. DM_VK_GPU_FLOOR_KGSL=0 (restart) leaves it off (the ADPF floor then); DM_VK_GPU_FLOOR and DM_VK_GPU_FLOOR_AUTO work as
// with the queue hint
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
struct kgsl_dc { unsigned int type, context_id; void *data; size_t size; };
struct kgsl_gp { unsigned int type; void *value; size_t sizebytes; };
#define KGSL_IOCTL_SETPROPERTY _IOW(0x09, 0x32, struct kgsl_gp)
enum { KG_FDS = 4, KG_IDS = 1024, KG_CTX = 8 };
static int g_kg_fd[KG_FDS], g_kg_nfd; static uint8_t g_kg_own[KG_FDS][KG_IDS / 8];
static struct { int fd; unsigned id; } g_kg[KG_CTX]; static int g_kg_n, g_kg_host;   // the queue's contexts; host DCVS: only "highest"
static int kg_set(int fd, unsigned id, int type, unsigned level) {
    unsigned lv = level; struct kgsl_dc c = {(unsigned)type, id, &lv, sizeof lv}; struct kgsl_gp g = {0x12, &c, sizeof c};   // KGSL_PROP_PWR_CONSTRAINT
    int r; do r = ioctl(fd, KGSL_IOCTL_SETPROPERTY, &g); while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r < 0 ? -errno : 0;
}
static void kg_scan(int keep_fds) {   // this process's descriptors on /dev/kgsl-3d0 and the context ids each owns
    if (!keep_fds) {
        g_kg_nfd = 0; DIR *d = opendir("/proc/self/fd"); if (!d) return;
        struct dirent *de; char path[64], to[64];
        while ((de = readdir(d)) && g_kg_nfd < KG_FDS) {
            int fd = atoi(de->d_name); if (fd <= 2) continue;
            snprintf(path, sizeof path, "/proc/self/fd/%d", fd); ssize_t n = readlink(path, to, sizeof to - 1); if (n <= 0) continue; to[n] = 0;
            if (!strncmp(to, "/dev/kgsl-3d", 12)) g_kg_fd[g_kg_nfd++] = fd;
        }
        closedir(d);
    }
    for (int i = 0; i < g_kg_nfd; i++) { memset(g_kg_own[i], 0, sizeof g_kg_own[i]); for (unsigned id = 1; id < KG_IDS; id++) if (!kg_set(g_kg_fd[i], id, 0, 0)) g_kg_own[i][id / 8] |= (uint8_t)(1u << (id % 8)); }
}
static uint8_t g_kg_was[KG_FDS][KG_IDS / 8]; static int g_kg_wasfd[KG_FDS], g_kg_wasn, g_kg_tried;
void vka_kgsl_before(int has_ext) {   // (not with the queue hint's extension: its driver's contexts are left alone)
    g_kg_tried = !has_ext && VKA_ENV("DM_VK_GPU_FLOOR_KGSL", 1) != 0;
    if (!g_kg_tried) return;
    kg_scan(0); g_kg_wasn = g_kg_nfd; memcpy(g_kg_wasfd, g_kg_fd, sizeof g_kg_fd); memcpy(g_kg_was, g_kg_own, sizeof g_kg_own);
}
void vka_kgsl_after(int has_hint) {
    if (has_hint) return;   // the queue hint does it
    if (!g_kg_tried) { VLOG("GPU clock floor through KGSL: off (DM_VK_GPU_FLOOR_KGSL=0, or the queue hint's extension without its feature)"); return; }
    kg_scan(0);
    g_kg_n = 0;
    for (int i = 0; i < g_kg_nfd; i++) {
        int w = -1; for (int j = 0; j < g_kg_wasn; j++) if (g_kg_wasfd[j] == g_kg_fd[i]) w = j;
        for (unsigned id = 1; id < KG_IDS && g_kg_n < KG_CTX; id++)
            if ((g_kg_own[i][id / 8] >> (id % 8) & 1) && !(w >= 0 && (g_kg_was[w][id / 8] >> (id % 8) & 1))) { g_kg[g_kg_n].fd = g_kg_fd[i]; g_kg[g_kg_n].id = id; g_kg_n++; }
    }
    if (g_kg_n) {   // the level form the kernel takes: 2-100 % with GMU DCVS, else -EINVAL and only 1 (highest)
        int r = kg_set(g_kg[0].fd, g_kg[0].id, 1, 50); g_kg_host = r == -EINVAL; kg_set(g_kg[0].fd, g_kg[0].id, 0, 0);
    }
    char b[96]; size_t o = 0; b[0] = 0; for (int k = 0; k < g_kg_n && o < sizeof b; k++) o += (size_t)snprintf(b + o, sizeof b - o, "%s%u", k ? "," : "", g_kg[k].id);
    if (g_kg_n) VLOG("GPU clock floor through KGSL: the queue's kernel context%s %s (%d descriptor%s on /dev/kgsl-3d0; %s), DM_VK_GPU_FLOOR (DM_VK_GPU_FLOOR_KGSL)",
                     g_kg_n > 1 ? "s" : "", b, g_kg_nfd, g_kg_nfd == 1 ? "" : "s", g_kg_host ? "host DCVS: highest clock only" : "GMU DCVS: floor in % of the highest clock");
    else VLOG("GPU clock floor through KGSL: %s", g_kg_nfd ? "no kernel context of the device's own (the ADPF floor)" : "no /dev/kgsl-3d0 descriptor (not an Adreno with KGSL; the ADPF floor)");
}
int vka_floor_kgsl(void) { return !V.perf_hint && g_kg_n > 0; }
static VkResult kg_apply(int s) {   // the floor s % (0: none) on the queue's contexts
    int bad = 0;
    for (int k = 0; k < g_kg_n; k++) {
        int r = s <= 0 ? kg_set(g_kg[k].fd, g_kg[k].id, 0, 0) : kg_set(g_kg[k].fd, g_kg[k].id, 1, s >= 100 || g_kg_host ? 1u : (unsigned)(s < 2 ? 2 : s));
        if (r) { bad = r; static int n; if (n++ < 3) VRED("KGSL power constraint %d %% on context %u: %d", s, g_kg[k].id, r); }
    }
    return bad ? VK_ERROR_UNKNOWN : VK_SUCCESS;
}
static _Atomic int g_tail_n;   // DM_VK_GPU_FLOOR_AUTO=2: submissions with draws counted at their commit and not yet completed
// The queue's hint (any thread: the replay's, the completion thread's). tail: the floor is s while a counted submission is pending, else 0, decided under the queue lock, so
// a commit and a completion racing cannot apply their floors in the wrong order (before 2026-11-03 a completion's 0 could land after the next commit's floor)
static void floor_set_(int s, int tail, int again) {   // again: send it even if unchanged (the KGSL rule, see kg_apply)
    static _Atomic int cur = -1;
    if (!tail && !again && atomic_load(&cur) == s) return;
    pthread_mutex_lock(&V.qmu); uint64_t th = now_ns();
    if (tail) s = atomic_load(&g_tail_n) > 0 ? s : 0;
    if (!again && atomic_load(&cur) == s) { pthread_mutex_unlock(&V.qmu); return; }
    VkResult r;
    if (V.perf_hint) {
        struct { VkStructureType sType; void *pNext; uint32_t type, scale; } h = {(VkStructureType)1000302000, NULL, s >= 100 ? 2u : s > 0 ? 3u : 0u, s > 0 && s < 100 ? (uint32_t)s : 0u};
        r = ((VkResult (*)(VkQueue, const void *))V.perf_hint)(V.q, &h);
    } else r = kg_apply(s);
    int was = atomic_exchange(&cur, s);
    vka_qhold_add(2, now_ns() - th);
    pthread_mutex_unlock(&V.qmu);
    if (was < 0 || (was != s && VKA_ENV("DM_VK_GPU_FLOOR_LOG", 0) > 1)) VLOG("GPU clock floor%s: %d %% (%s): %d", V.perf_hint ? "" : " (KGSL)", s, s >= 100 ? "highest" : s ? "scaled" : "default governor", r);
}
// DM_VK_GPU_FLOOR_AUTO=2: the floor only for the tail. The driver's governor keeps the clock while the frame is recorded (its parts reach the GPU as fast as they are
// recorded, so a faster GPU only waits longer); when a submission with draws is handed to the queue the floor goes to DM_VK_GPU_FLOOR, and back to the governor once
// every such submission handed over has completed. A submission is counted when its span op is posted (vka_floor_tail_add) and the count is given back by that op on
// completion, so the two always pair: before 2026-11-03 every completed submission with draws took one off, also those committed before a live switch to mode 2 or whose
// end failed, and the floor could drop with a counted submission still on the GPU
static int floor_mode(void) { return V.perf_hint || g_kg_n ? VKA_ENV("DM_VK_GPU_FLOOR_AUTO", 0) : -1; }
static int floor_top(void) { int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); return s < 0 ? 0 : s > 100 ? 100 : s; }
// The floor without VK_QCOM_queue_perf_hint and without the KGSL constraint above (a driver that is not on KGSL, or DM_VK_GPU_FLOOR_KGSL=0): the ADPF session (vka_perf.c) reports each frame's span (recording start to the GPU end of
// its last submission) with its GPU time against the shortest period, so the system raises the clocks the frame's work runs on when the frame takes longer than the period
// the game could run at. Before 2026-11-05 there was no floor at all there: on Turnip DKB stayed at 20 fps with 33-36 ms of GPU time a frame, as the system driver did with
// DM_VK_GPU_FLOOR=0. It has no level: DM_VK_GPU_FLOOR > 0 turns it on, 0 off, as does DM_VK_GPU_FLOOR_ADPF=0 (live); DM_VK_ADPF_SPAN set explicitly decides the reports. On the Adreno 840 with Turnip it did not raise the GPU clock (282-646 MHz, mean 380; 2026-11-05)
int vka_floor_adpf(void) { return !V.perf_hint && !g_kg_n && floor_top() > 0 && VKA_ENV("DM_VK_GPU_FLOOR_ADPF", 1); }
int vka_floor_tail_add(void) { if (floor_mode() != 2) return 0; atomic_fetch_add(&g_tail_n, 1); return 1; }   // vka_exec.c, a submission with draws: 1 = counted
void vka_floor_commit(int counted) { if (counted) floor_set_(floor_top(), 1, 0); }   // before that submission is handed to the queue
void vka_floor_done(int counted) {     // the completion thread, that submission done
    if (!counted) return;
    atomic_fetch_sub(&g_tail_n, 1);
    if (floor_mode() == 2) floor_set_(floor_top(), 1, 0);
}
void vka_gpu_floor(void) {
    int mode = floor_mode(); if (mode < 0 || mode == 2) return;
    int s = VKA_ENV("DM_VK_GPU_FLOOR", 100); if (s < 0) s = 0; if (s > 100) s = 100;
    uint64_t vka_frame(void);
    uint64_t f = vka_frame();
    if (mode == 1) s = floor_auto(s, f);
    static uint64_t sent_f;   // the KGSL rule again every 30 frames (it is lost when the GMU registers the context again)
    int again = !V.perf_hint && (f >= sent_f + 30 || f < sent_f);
    if (again) sent_f = f;
    floor_set_(s, 0, again);
}
#endif
