// From Banana Native v0.3.43 (af1ed596, community PR 409; MIT), unchanged but for this line.
// Whether a custom driver is what runs (vka_dev.c, after device creation), free of Vulkan so the Mac's unit tests (dm/host/tests/t_android_driverpath.c) can check it.
// DM_VK_DRIVER loads a custom driver through libadrenotools (vka_driver.c), and a library comes back even when the custom driver did not take: on an Android 13 AYN
// Thor Max (Adreno 740) three Qualcomm driver packages ran the phone's own driver (its build, date and version in the [vk] driver line) while the host said
// "custom (DM_VK_DRIVER)", and the crashes they gave were the system driver's (docs/reference/rendering.md "A Qualcomm custom driver silently falls back").
// The device's command addresses cannot tell: Android's loader answers vkGetDeviceProcAddr for core commands with trampolines of its own (libvulkan.so, which
// adrenotools loads from the system too). So the process's maps are read: the custom driver took when its library file is mapped.
#ifndef VKA_DRIVERPATH_H
#define VKA_DRIVERPATH_H
#include <stddef.h>
#include <string.h>

// 1 when a line of /proc/self/maps (n bytes, no newline) maps the library <folder>/<name>: its path ends with "/<folder>/<name>". The folder is matched by its last
// component only, since the kernel writes the path it resolved (the launcher's /data/user/0/<app>/files/gpu_drivers/X is /data/data/<app>/files/gpu_drivers/X there)
static inline int vkdp_line_maps(const char *line, size_t n, const char *folder, const char *name) {
    if (!line || !folder || !name || !*name) return 0;
    const char *p = memchr(line, '/', n); if (!p) return 0;   // the path is the line's first '/' (the address fields have none)
    size_t pl = n - (size_t)(p - line), nl = strlen(name), fl = strlen(folder);
    while (pl && (p[pl - 1] == ' ' || p[pl - 1] == '\r')) pl--;
    if (pl >= 10 && !memcmp(p + pl - 10, " (deleted)", 10)) pl -= 10;
    if (pl < nl + 1 || memcmp(p + pl - nl, name, nl) || p[pl - nl - 1] != '/') return 0;
    if (!fl) return 1;
    size_t dl = pl - nl - 1;   // the folder part's end
    return dl >= fl + 1 && !memcmp(p + dl - fl, folder, fl) && p[dl - fl - 1] == '/';
}
// The last component of a folder path (trailing '/' ignored) into out (cap bytes); "" when there is none
static inline void vkdp_last(const char *dir, char *out, size_t cap) {
    size_t l = dir ? strlen(dir) : 0; while (l && dir[l - 1] == '/') l--;
    size_t s = l; while (s && dir[s - 1] != '/') s--;
    size_t k = l - s < cap - 1 ? l - s : cap - 1; if (cap) { if (k) memcpy(out, dir + s, k); out[k] = 0; }
}
#endif
