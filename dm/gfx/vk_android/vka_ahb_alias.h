// Guest-address memory on drivers without placed maps: an AHardwareBuffer the GPU and the CPU share (ARM Mali, Samsung Xclipse: neither has VK_EXT_map_memory_placed,
// and an OPAQUE_FD export, vka_fd_alias.h, is not something the CPU may map there). A BLOB buffer with GPU_DATA_BUFFER use is imported as the memory of a VkBuffer
// (VK_ANDROID_external_memory_android_hardware_buffer, which every Android Vulkan driver has), and the dma-buf behind it is mapped by the host itself over the pool's own
// address with MAP_FIXED, as the exported alias is: the replay keeps one model (a pool's bytes are the GPU's bytes, vka_mem.c), only what backs a segment differs.
// The NDK gives no call for the buffer's descriptor: it is taken from the handle AHardwareBuffer_sendHandleToUnixSocket writes (SCM_RIGHTS), and the descriptor that
// holds the data is the one whose map shows what the buffer's own lock shows (a handle may carry others, a metadata region for one).
// Nothing is assumed of the driver: the probe makes one such buffer, checks the two maps against each other and mprotect on the alias, then has the GPU copy from it and into
// it several times (a GPU that does not see the CPU's writes at once, or a CPU that reads a stale line after the GPU wrote, fails here and the path stays off).
// DM_VK_AHB_ALIAS=0: off. The probe's sentence is in the log and in the readiness facts (memory.ahbAliasProbe)
#pragma once
#include <android/hardware_buffer.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

static int vka_ahb_enabled(void) { const char *e = getenv("DM_VK_AHB_ALIAS"); return !e || strcmp(e, "0"); }

// The device calls this path needs, loaded by name (the readiness check runs it on a device of its own, without the replay's table)
typedef struct {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID props;
    PFN_vkCreateBuffer create_buffer; PFN_vkDestroyBuffer destroy_buffer; PFN_vkGetBufferMemoryRequirements buffer_reqs; PFN_vkBindBufferMemory bind;
    PFN_vkAllocateMemory alloc; PFN_vkFreeMemory free_; PFN_vkMapMemory map; PFN_vkUnmapMemory unmap;
    PFN_vkCreateCommandPool create_pool; PFN_vkDestroyCommandPool destroy_pool; PFN_vkAllocateCommandBuffers alloc_cb;
    PFN_vkBeginCommandBuffer begin; PFN_vkEndCommandBuffer end; PFN_vkCmdCopyBuffer copy; PFN_vkGetDeviceQueue get_queue; PFN_vkQueueSubmit submit; PFN_vkQueueWaitIdle wait_idle;
} vka_ahb_fns;
static int vka_ahb_load(PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, vka_ahb_fns *f) {
#define L(m, name) if (!(f->m = (PFN_##name)gdpa(dev, #name))) return 0;
    L(props, vkGetAndroidHardwareBufferPropertiesANDROID) L(create_buffer, vkCreateBuffer) L(destroy_buffer, vkDestroyBuffer) L(buffer_reqs, vkGetBufferMemoryRequirements) L(bind, vkBindBufferMemory)
    L(alloc, vkAllocateMemory) L(free_, vkFreeMemory) L(map, vkMapMemory) L(unmap, vkUnmapMemory)
    L(create_pool, vkCreateCommandPool) L(destroy_pool, vkDestroyCommandPool) L(alloc_cb, vkAllocateCommandBuffers)
    L(begin, vkBeginCommandBuffer) L(end, vkEndCommandBuffer) L(copy, vkCmdCopyBuffer) L(get_queue, vkGetDeviceQueue) L(submit, vkQueueSubmit) L(wait_idle, vkQueueWaitIdle)
#undef L
    return 1;
}

// The descriptors of a buffer's handle (the caller closes them). -1: the handle could not be read
static int vka_ahb_handle_fds(AHardwareBuffer *b, int *fds, int max) {
    int sv[2], n = -1; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(b, sv[0])) {
        char data[4096]; union { char c[CMSG_SPACE(sizeof(int) * 128)]; struct cmsghdr align; } ctl;
        struct iovec io = {data, sizeof data}; struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl.c, .msg_controllen = sizeof ctl.c};
        ssize_t r; do r = recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); while (r < 0 && errno == EINTR);
        if (r >= 0) { n = 0;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)), got[128]; memcpy(got, CMSG_DATA(c), (size_t)k * sizeof(int));
                for (int i = 0; i < k; i++) { if (n < max) fds[n++] = got[i]; else close(got[i]); }
            } }
    }
    close(sv[0]); close(sv[1]); return n;
}
// The descriptor of the buffer's data (owned by the caller), -1 with the reason in why. The buffer's lock gives the address gralloc maps it at: the data descriptor is
// the one whose own shared map shows a mark written there, and shows it change
static int vka_ahb_data_fd(AHardwareBuffer *b, size_t n, char *why, size_t whyn) {
    void *lk = NULL; int fds[16], nf, hit = -1;
    if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &lk) || !lk) { snprintf(why, whyn, "the buffer could not be locked"); return -1; }
    volatile uint64_t *m0 = lk, *m1 = (volatile uint64_t *)((char *)lk + n - 8);
    uint64_t a = 0x5eed1234abcd0000ull ^ (uint64_t)(uintptr_t)lk ^ (uint64_t)n, c = ~a;
    *m0 = a; *m1 = c;
    if ((nf = vka_ahb_handle_fds(b, fds, 16)) < 0) snprintf(why, whyn, "the buffer's handle could not be read (errno %d)", errno);
    else if (!nf) snprintf(why, whyn, "the buffer's handle has no descriptor");
    else {
        for (int i = 0; i < nf && hit < 0; i++) {
            void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0); if (p == MAP_FAILED) continue;
            volatile uint64_t *q0 = p, *q1 = (volatile uint64_t *)((char *)p + n - 8);
            if (*q0 == a && *q1 == c) { *m0 = a + 1; if (*q0 == a + 1) { *q1 = c + 1; if (*m1 == c + 1) hit = i; } }
            munmap(p, n);
        }
        if (hit < 0) snprintf(why, whyn, "none of the handle's %d descriptors maps the buffer's data", nf);
        for (int i = 0; i < nf; i++) if (i != hit) close(fds[i]);
    }
    *m0 = 0; *m1 = 0;
    AHardwareBuffer_unlock(b, NULL);
    return hit < 0 ? -1 : fds[hit];
}

// One buffer of n bytes: its VkBuffer and memory (the guest usage, vka_fd_alias.h vka_guest_usage), the data descriptor and the host's own map of it (not the driver's:
// the memory is never mapped through Vulkan, so the caller unmaps `mapped` with munmap). The reference on the buffer is dropped once the driver has imported it
static VkResult vka_ahb_create(const vka_ahb_fns *f, PFN_vkGetPhysicalDeviceExternalBufferProperties ext_props, VkPhysicalDevice pd, VkDevice dev,
                               VkDeviceSize n, int bda, vka_fd_memory *out, char *why, size_t whyn) {
    *out = (vka_fd_memory){.fd = -1}; why[0] = 0;
    if (!n || n > 0x7fffffffu) { snprintf(why, whyn, "a size no BLOB buffer has (%llu bytes)", (unsigned long long)n); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkBufferUsageFlags usage = vka_guest_usage(bda);
    if (ext_props) {
        VkPhysicalDeviceExternalBufferInfo qi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = usage, .handleType = ht};
        VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES}; ext_props(pd, &qi, &ep);
        if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            snprintf(why, whyn, "the driver imports no hardware buffer for the guest's buffer usage %#x (features %#x)", usage, ep.externalMemoryProperties.externalMemoryFeatures);
            return VK_ERROR_FEATURE_NOT_PRESENT; }
    }
    AHardwareBuffer_Desc d = {.width = (uint32_t)n, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
                              .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN};
    AHardwareBuffer *b = NULL; int ar = AHardwareBuffer_allocate(&d, &b);
    if (ar || !b) { snprintf(why, whyn, "AHardwareBuffer_allocate of %llu bytes failed (%d)", (unsigned long long)n, ar); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if ((out->fd = vka_ahb_data_fd(b, (size_t)n, why, whyn)) < 0) goto fail;
    VkExternalMemoryBufferCreateInfo ei = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ei, .size = n, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if ((r = f->create_buffer(dev, &bi, NULL, &out->buf))) { snprintf(why, whyn, "vkCreateBuffer for a hardware buffer failed (%d)", r); goto fail; }
    VkAndroidHardwareBufferPropertiesANDROID hp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    if ((r = f->props(dev, b, &hp)) || !hp.memoryTypeBits || hp.allocationSize < n) {
        snprintf(why, whyn, "the driver's properties of the buffer: result %d, memory types %#x, %llu bytes for %llu", r, hp.memoryTypeBits, (unsigned long long)hp.allocationSize, (unsigned long long)n);
        if (!r) r = VK_ERROR_FEATURE_NOT_PRESENT; goto fail; }
    VkMemoryAllocateFlagsInfo af = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkImportAndroidHardwareBufferInfoANDROID im = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = bda ? &af : NULL, .buffer = b};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = hp.allocationSize, .memoryTypeIndex = (uint32_t)__builtin_ctz(hp.memoryTypeBits)};
    if ((r = f->alloc(dev, &ai, NULL, &out->mem))) { snprintf(why, whyn, "the import failed (vkAllocateMemory %d%s)", r, bda ? ", with a device address" : ""); goto fail; }
    if ((r = f->bind(dev, out->buf, out->mem, 0))) { snprintf(why, whyn, "vkBindBufferMemory on the imported memory failed (%d)", r); goto fail; }
    out->mapped = mmap(NULL, (size_t)n, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
    if (out->mapped == MAP_FAILED) { out->mapped = NULL; snprintf(why, whyn, "the data descriptor's map failed (errno %d)", errno); r = VK_ERROR_MEMORY_MAP_FAILED; goto fail; }
    AHardwareBuffer_release(b);
    return VK_SUCCESS;
fail:
    if (out->fd >= 0) close(out->fd);
    if (out->buf) f->destroy_buffer(dev, out->buf, NULL);
    if (out->mem) f->free_(dev, out->mem, NULL);
    *out = (vka_fd_memory){.fd = -1};
    AHardwareBuffer_release(b);
    return r;
}
static void vka_ahb_destroy(const vka_ahb_fns *f, VkDevice dev, vka_fd_memory *m, size_t n) {
    if (m->mapped) munmap(m->mapped, n);
    if (m->fd >= 0) close(m->fd);
    if (m->buf) f->destroy_buffer(dev, m->buf, NULL);
    if (m->mem) f->free_(dev, m->mem, NULL);
    *m = (vka_fd_memory){.fd = -1};
}

// The probe: 1 when a buffer made as above is a true alias (the fixed map and the first map show each other's writes, a page takes mprotect) and the GPU and the CPU
// see each other's writes at once, four rounds each way through a plain host-visible buffer on queue family qf. why: the sentence for the log and the readiness facts
static int vka_ahb_probe(PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetPhysicalDeviceExternalBufferProperties ext_props, VkPhysicalDevice pd, VkDevice dev,
                         const VkPhysicalDeviceMemoryProperties *mp, uint32_t qf, int bda, char *why, size_t whyn) {
    enum { N = 65536, W = N / 4 };
    vka_ahb_fns f; if (!vka_ahb_load(gdpa, dev, &f)) { snprintf(why, whyn, "a device call is missing"); return 0; }
    vka_fd_memory m; char cw[200];
    if (vka_ahb_create(&f, ext_props, pd, dev, N, bda, &m, cw, sizeof cw)) { snprintf(why, whyn, "%s", cw); return 0; }
    int ok = 0; void *at = mmap(NULL, N, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), *a = MAP_FAILED;
    VkBuffer pb = VK_NULL_HANDLE; VkDeviceMemory pm = VK_NULL_HANDLE; VkCommandPool pool = VK_NULL_HANDLE; void *pp = NULL;
    if (at == MAP_FAILED) { snprintf(why, whyn, "alias reservation failed (errno %d)", errno); goto done; }
    a = mmap(at, N, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, m.fd, 0);
    if (a == MAP_FAILED) { snprintf(why, whyn, "the fixed map of the data descriptor failed (errno %d)", errno); goto done; }
    volatile uint32_t *first = m.mapped, *alias = a;
    for (size_t i = 0; i < W; i++) first[i] = (uint32_t)i ^ 0x5eed1234;
    for (size_t i = 0; i < W; i++) if (alias[i] != ((uint32_t)i ^ 0x5eed1234)) { snprintf(why, whyn, "the fixed map does not show the first map's writes"); goto done; }
    for (size_t i = 0; i < W; i++) alias[i] = (uint32_t)i ^ 0xc0ffee00;
    if (mprotect((char *)a + 4096, 4096, PROT_READ) || mprotect((char *)a + 4096, 4096, PROT_READ | PROT_WRITE)) { snprintf(why, whyn, "mprotect on the alias failed (errno %d)", errno); goto done; }
    for (size_t i = 0; i < W; i++) if (first[i] != ((uint32_t)i ^ 0xc0ffee00)) { snprintf(why, whyn, "the first map does not show the fixed map's writes"); goto done; }
    // The GPU: a plain host-visible buffer on the other side of each copy
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements mr; uint32_t type = UINT32_MAX; const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (f.create_buffer(dev, &bi, NULL, &pb)) { snprintf(why, whyn, "the GPU check's buffer could not be made"); goto done; }
    f.buffer_reqs(dev, pb, &mr);
    for (uint32_t i = 0; i < mp->memoryTypeCount && type == UINT32_MAX; i++) if ((mr.memoryTypeBits >> i & 1) && (mp->memoryTypes[i].propertyFlags & hv) == hv) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || f.alloc(dev, &ai, NULL, &pm) || f.bind(dev, pb, pm, 0) || f.map(dev, pm, 0, VK_WHOLE_SIZE, 0, &pp) || !pp) { snprintf(why, whyn, "the GPU check's memory could not be made"); goto done; }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf};
    VkCommandBuffer cb = VK_NULL_HANDLE; VkQueue q = VK_NULL_HANDLE; f.get_queue(dev, qf, 0, &q);
    if (!q || f.create_pool(dev, &pci, NULL, &pool)) { snprintf(why, whyn, "the GPU check's command pool could not be made"); goto done; }
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (f.alloc_cb(dev, &cai, &cb)) { snprintf(why, whyn, "the GPU check's command buffer could not be made"); goto done; }
    volatile uint32_t *plain = pp;
    for (int round = 0; round < 8; round++) {   // even rounds: the CPU writes the alias, the GPU copies it out; odd: the CPU writes the plain buffer, the GPU copies it into the alias
        int to_gpu = !(round & 1); uint32_t key = 0x9e3779b9u * (uint32_t)(round + 1);
        volatile uint32_t *src = to_gpu ? alias : plain, *dst = to_gpu ? plain : alias;
        for (size_t i = 0; i < W; i++) src[i] = (uint32_t)i * 2654435761u ^ key;
        VkCommandBufferBeginInfo bg = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VkBufferCopy c = {0, 0, N}; VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        if (f.begin(cb, &bg)) { snprintf(why, whyn, "the GPU check could not record"); goto done; }
        f.copy(cb, to_gpu ? m.buf : pb, to_gpu ? pb : m.buf, 1, &c);
        VkResult r = f.end(cb); if (!r) r = f.submit(q, 1, &si, VK_NULL_HANDLE); if (!r) r = f.wait_idle(q);
        if (r) { snprintf(why, whyn, "the GPU check's copy failed (%d)", r); goto done; }
        for (size_t i = 0; i < W; i++) if (dst[i] != ((uint32_t)i * 2654435761u ^ key)) {
            snprintf(why, whyn, to_gpu ? "the GPU does not see the CPU's writes to the alias (round %d, word %zu)" : "the CPU does not see the GPU's writes through the alias (round %d, word %zu)", round, i);
            goto done; }
    }
    ok = 1; snprintf(why, whyn, "passed: fixed alias both ways, mprotect, and 8 GPU copies%s", bda ? ", with a device address" : "");
done:
    if (pool) f.destroy_pool(dev, pool, NULL);
    if (pp) f.unmap(dev, pm);
    if (pb) f.destroy_buffer(dev, pb, NULL);
    if (pm) f.free_(dev, pm, NULL);
    if (a != MAP_FAILED) munmap(a, N); else if (at != MAP_FAILED) munmap(at, N);
    vka_ahb_destroy(&f, dev, &m, N);
    return ok;
}
