// From Banana Native v0.3.44 (f03e6671, MIT), unchanged but for this line: used at the replay's start (vka_dev.c, DM_VK_REMAP_PROBE).
// Whether guest pools can really be placed: the map / unmap / placed-remap sequence the pools use (vka_mem.c), run on one 64 KiB buffer, not just the extension bit.
// A driver can advertise VK_EXT_map_memory_placed and still refuse every placed remap of a guest buffer (PanVK on Mali: vkMapMemory2 -5, VK_ERROR_MEMORY_MAP_FAILED,
// at each pool; docs/reference/memory.md, Advertised placed maps reject guest-buffer remaps). Used by the readiness check (vka_readiness.c) and at the replay's start
// (vka_dev.c). The includer has vka_fns and vka_guest_usage (vka_fd_alias.h); the caller enables placed maps on dev when the driver advertises them
#pragma once
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>

// 1 when a buffer with the guest usage, in memory type type, keeps its bytes through an ordinary map, a placed remap at an aligned reserved address, the placed unmap
// and another ordinary map. why: the step that failed and its VkResult, or "passed"
static int vka_guest_remap_probe(vka_fns *f, VkDevice dev, uint32_t type, int bda, size_t align, char *why, size_t whyn) {
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    void *reservation = MAP_FAILED, *mapped = NULL; size_t reserved = 0;
    int ok = 0, placed = 0; VkResult r = VK_ERROR_FEATURE_NOT_PRESENT; const char *phase = "entry points";
    if (!f->vkMapMemory2KHR || !f->vkUnmapMemory2KHR || !f->vkCreateBuffer || !f->vkAllocateMemory || !f->vkMapMemory || type == UINT32_MAX) goto done;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536, .usage = vka_guest_usage(bda), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    phase = "create buffer"; if ((r = f->vkCreateBuffer(dev, &bi, NULL, &buf))) goto done;
    VkMemoryRequirements mr; f->vkGetBufferMemoryRequirements(dev, buf, &mr);
    phase = "memory type"; if (!(mr.memoryTypeBits & (1u << type))) { r = VK_ERROR_FEATURE_NOT_PRESENT; goto done; }
    VkMemoryAllocateFlagsInfo af = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = bda ? &af : NULL, .allocationSize = mr.size, .memoryTypeIndex = type};
    phase = "allocate"; if ((r = f->vkAllocateMemory(dev, &ai, NULL, &mem))) goto done;
    phase = "bind buffer"; if ((r = f->vkBindBufferMemory(dev, buf, mem, 0))) goto done;
    phase = "initial map"; if ((r = f->vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped)) || !mapped) goto done;
    volatile uint32_t *w = mapped; size_t last = (size_t)mr.size / sizeof *w - 1;
    w[0] = 0x5eed1234; w[last] = 0xc0ffee00;
    f->vkUnmapMemory(dev, mem); mapped = NULL;
    if (align < 4096) align = 4096;
    reserved = (size_t)mr.size + align;
    reservation = mmap(NULL, reserved, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    phase = "reserve address"; if (reservation == MAP_FAILED) { r = VK_ERROR_MEMORY_MAP_FAILED; goto done; }
    uintptr_t a = (uintptr_t)reservation, at = a + (align - a % align) % align;
    VkMemoryMapPlacedInfoEXT pi = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)at};
    VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = mem, .size = VK_WHOLE_SIZE};
    phase = "placed remap"; if ((r = f->vkMapMemory2KHR(dev, &mi, &mapped))) goto done;
    placed = 1;
    phase = "exact address"; if (mapped != (void *)at) goto done;
    w = mapped;
    phase = "preserved bytes"; if (w[0] != 0x5eed1234 || w[last] != 0xc0ffee00) goto done;
    w[0] = 0xc0ffee00; w[last] = 0x5eed1234;
    VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem};
    phase = "placed unmap"; if ((r = f->vkUnmapMemory2KHR(dev, &ui))) goto done;
    mapped = NULL; placed = 0;
    phase = "readback map"; if ((r = f->vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &mapped)) || !mapped) goto done;
    w = mapped; phase = "readback bytes"; ok = w[0] == 0xc0ffee00 && w[last] == 0x5eed1234;
done:
    if (mapped) {
        if (placed) { VkMemoryUnmapInfoKHR ui2 = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = mem}; f->vkUnmapMemory2KHR(dev, &ui2); }
        else f->vkUnmapMemory(dev, mem);
    }
    if (buf) f->vkDestroyBuffer(dev, buf, NULL);
    if (mem) f->vkFreeMemory(dev, mem, NULL);
    if (reservation != MAP_FAILED) munmap(reservation, reserved);
    snprintf(why, whyn, "%s guest-buffer map/remap (%s, %d)", ok ? "passed" : "failed", phase, r);
    return ok;
}
