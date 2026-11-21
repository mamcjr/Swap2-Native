// Pipelines that killed the process on this driver (vka_pso.c deadly_*; the <cache file>.deadly list), free of Vulkan so the unit test
// (dm/host/tests/t_android_deadly.c) can check it: which build in flight a crash records, how a record matches a later build, the line format the crash handler
// writes, and the list's stamp (the shader library, the driver, the host build).
// A record is a pair of shader keys: the pre-rasterization (vertex or mesh) or compute shader, and the fragment shader. A zero in a record matches any shader there:
// (A, 0) refuses every pipeline whose pre-rasterization or compute shader is A, (0, B) every one whose fragment shader is B, (A, B) only pipelines and parts made of
// both. The build the crashing thread was in is recorded as its exact pair, whatever its length (the driver crashed compiling those shaders: DroidPlay Core 1ae945d2,
// Ocarina of Time's first compute pipeline on an Adreno 735, at every start); a build of another thread is recorded only once it had run DM_VK_DEADLY_MS, as its
// first shader alone (vka_pso.c's rule since 2026-11-12: a long compile that took the memory maps or the memory, which any pipeline of that shader would again).
#ifndef VKA_DEADLY_H
#define VKA_DEADLY_H
#include <stdint.h>
#include <string.h>

typedef struct { uint64_t pre, fs; } vkdl_rec;

// Whether a build of shaders (pre, fs) is one of the n records. A build of no shader (a vertex input or fragment output part, a probe) matches none
static inline int vkdl_match(const vkdl_rec *r, int n, uint64_t pre, uint64_t fs) {
    if (!pre && !fs) return 0;
    for (int i = 0; i < n; i++) {
        if (!r[i].pre && !r[i].fs) continue;
        if ((!r[i].pre || r[i].pre == pre) && (!r[i].fs || r[i].fs == fs)) return 1;
    }
    return 0;
}

// The record a crash leaves for one build in flight: 1 with it in *out, 0 for none. self: the build is the crashing thread's; age_ns: how long it had run
static inline int vkdl_choose(int self, uint64_t age_ns, uint64_t min_ns, uint64_t pre, uint64_t fs, vkdl_rec *out) {
    if (!pre && !fs) return 0;
    if (self) { out->pre = pre; out->fs = fs; return 1; }
    if (age_ns < min_ns) return 0;
    out->pre = pre; out->fs = pre ? 0 : fs;
    return 1;
}

// A record's line, "<pre> <fs>\n" in hex (34 bytes, no terminator): plain stores, as the crash handler writes it
static inline void vkdl_line(const vkdl_rec *r, char b[34]) {
    static const char hx[] = "0123456789abcdef";
    for (int j = 0; j < 16; j++) { b[j] = hx[(r->pre >> (60 - 4 * j)) & 15]; b[17 + j] = hx[(r->fs >> (60 - 4 * j)) & 15]; }
    b[16] = ' '; b[33] = '\n';
}

// A line of the list read back: 1 with the record in *r. Lines that are not two hex keys (the stamp, comments) are not records
static inline int vkdl_parse(const char *ln, vkdl_rec *r) {
    uint64_t v[2] = {0, 0}; int k = 0, d = 0;
    for (const char *p = ln; ; p++) {
        char c = *p; int x = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (x >= 0) { if (k > 1 || d >= 16) return 0; v[k] = (v[k] << 4) | (uint64_t)x; d++; continue; }
        if (c == ' ' && d == 16 && k == 0) { k = 1; d = 0; continue; }
        if ((c == '\n' || c == '\r' || !c) && d == 16 && k == 1) break;
        return 0;
    }
    r->pre = v[0]; r->fs = v[1];
    return r->pre || r->fs;
}

// The list's stamp: the shader library's id, the driver (vendor, device, version and pipeline cache UUID: a driver update changes them) and the host build (its commit,
// DM_BUILD_COMMIT): a list made under another is dropped, so an update of any of them gives its pipelines another try
static inline uint64_t vkdl_mix(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static inline uint64_t vkdl_stamp(uint64_t lib, uint32_t vendor, uint32_t device, uint32_t drv, const uint8_t uuid[16], const char *host) {
    uint64_t h = vkdl_mix(1469598103934665603ull, "VKDL2", 5);
    h = vkdl_mix(h, &lib, sizeof lib); h = vkdl_mix(h, &vendor, sizeof vendor); h = vkdl_mix(h, &device, sizeof device); h = vkdl_mix(h, &drv, sizeof drv);
    if (uuid) h = vkdl_mix(h, uuid, 16);
    h = vkdl_mix(h, "|", 1); if (host) h = vkdl_mix(h, host, strlen(host));
    return h ? h : 1;
}
#endif
