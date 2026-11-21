// The task / mesh compute packing (dm/gfx/vk_android/vka_mesh_pack.h, DM_VK_MESH_PACK): the selftest's task and mesh fixtures (vka_mesh_test_spv.h) are rewritten to a
// 64-invocation workgroup with their workgroup arrays doubled and every builtin load replaced, a module with a guest atomic or another local size is left as it is, and
// each result passes spirv-val (when it is on the PATH). DM_MESHPACK_DIR=<folder of <key>_5.spv> adds a library's task / mesh computes (dm/gfx/tools: extract them with
// vk_android_delta.Library), each rewritten module validated, with a count of what was left and why.
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../gfx/vk_android/vka_mesh_pack.h"
#include "../../gfx/vk_android/vka_mesh_test_spv.h"

enum { INFO = 387 };
static int g_val = -1;   // spirv-val on the PATH
static int validate(const uint32_t *w, size_t n, const char *name) {
    if (g_val < 0) g_val = system("spirv-val --version >/dev/null 2>&1") == 0;   // NOLINT(bugprone-command-processor): the test runs spirv-val from the PATH when there is one, a fixed command
    if (!g_val) return 1;
    char p[256]; snprintf(p, sizeof p, "%s/meshpack_%d.spv", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    FILE *f = fopen(p, "wb"); assert(f); fwrite(w, 4, n, f); fclose(f);
    char cmd[512]; snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s >/dev/null 2>&1", p);
    int ok = system(cmd) == 0;   // NOLINT(bugprone-command-processor): spirv-val on the test's own temporary file
    if (!ok) { snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s 2>&1 | head -5", p); fprintf(stderr, "spirv-val refused %s:\n", name); if (system(cmd)) {} }   // NOLINT(bugprone-command-processor): the same check again, its first lines shown
    remove(p);
    return ok;
}
static unsigned count_op(const uint32_t *w, size_t n, uint32_t op) {
    unsigned c = 0; for (size_t i = 5; i < n; i += w[i] >> 16) { if (!(w[i] >> 16)) break; if ((w[i] & 0xffff) == op) c++; }
    return c;
}
static int local_x(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n; i += w[i] >> 16) { if (!(w[i] >> 16)) break; if ((w[i] & 0xffff) == 16 && w[i + 2] == 17) return (int)w[i + 3]; }
    return -1;
}
// loads of a builtin left in the output (there should be none of LocalInvocationIndex / WorkgroupId other than the rewrite's own raw ones)
static void fixture(const uint32_t *w, size_t bytes, const char *name) {
    size_t n = bytes / 4, on = 0; char why[256]; vka_mesh_pack_stats st;
    assert(local_x(w, n) == 32);
    uint32_t *o = vka_mesh_pack(w, n, INFO, 1, &on, &st, why, sizeof why);
    if (!o) { fprintf(stderr, "%s: not packed: %s\n", name, why); assert(0); }
    assert(local_x(o, on) == 64);
    for (size_t i = 5; i < on; i += o[i] >> 16) assert(!((o[i] & 0xffff) == 71 && o[i + 2] == 11 && o[i + 3] == 25));   // no WorkgroupSize left to override it
    assert(count_op(o, on, 224) == count_op(w, n, 224));   // the barriers stay
    assert(o[3] > w[3]);
    assert(st.loads > 0);
    assert(validate(o, on, name));
    printf("t_android_meshpack: %s packed: %u workgroup bytes, %u chains and %u builtin loads rewritten%s, %zu -> %zu words\n", name, st.wg_bytes, st.chains, st.loads,
           st.made_lane ? " (a local index of its own)" : "", n, on);
    // refusals: another local size; an atomic on a storage buffer
    uint32_t *c = malloc(bytes); memcpy(c, w, bytes);
    for (size_t i = 5; i < n; i += c[i] >> 16) if ((c[i] & 0xffff) == 16 && c[i + 2] == 17) c[i + 3] = 64;
    assert(!vka_mesh_pack(c, n, INFO, 1, &on, &st, why, sizeof why) && strstr(why, "local size"));
    free(c); free(o);
}
static void library(const char *dir) {
    DIR *d = opendir(dir); if (!d) { fprintf(stderr, "DM_MESHPACK_DIR %s: not a folder\n", dir); exit(1); }
    struct dirent *e; unsigned packed = 0, left = 0, bad = 0, n_reason = 0, atom = 0; char reasons[32][128]; unsigned rc[32] = {0};
    uint64_t wmax = 0;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 6 || strcmp(e->d_name + L - 6, "_5.spv")) continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long nb = ftell(f); fseek(f, 0, SEEK_SET);
        uint32_t *w = malloc((size_t)nb); if (fread(w, 1, (size_t)nb, f) != (size_t)nb) { fclose(f); free(w); continue; } fclose(f);
        size_t on = 0; char why[256]; vka_mesh_pack_stats st;
        uint32_t *o = vka_mesh_pack(w, (size_t)nb / 4, INFO, 1, &on, &st, why, sizeof why);
        if (!o) {
            left++;
            char r[128]; snprintf(r, sizeof r, "%.120s", why); char *k = strstr(r, "(storage class"); if (k) *k = 0;
            unsigned j = 0; while (j < n_reason && strcmp(reasons[j], r)) j++;
            if (j == n_reason && n_reason < 32) { snprintf(reasons[n_reason], sizeof reasons[0], "%.120s", r); n_reason++; }
            if (j < 32) rc[j]++;
        } else {
            if (st.atomics) { size_t on2; char why2[256]; assert(!vka_mesh_pack(w, (size_t)nb / 4, INFO, 0, &on2, NULL, why2, sizeof why2) && strstr(why2, "atomic")); }   // (guest_adds 0: left)
            packed++; atom += st.atomics > 0; if (st.wg_bytes > wmax) wmax = st.wg_bytes;
            if (local_x(o, on) != 64 || !validate(o, on, e->d_name)) bad++;
            free(o);
        }
        free(w);
    }
    closedir(d);
    printf("t_android_meshpack: %s: %u packed (%s; %u with guest atomic adds), %u left as they are, %u invalid; the most workgroup memory packed: %llu bytes\n", dir, packed,
           g_val > 0 ? "each passes spirv-val" : "spirv-val not on the PATH", atom, left, bad, (unsigned long long)wmax);
    for (unsigned j = 0; j < n_reason; j++) printf("  left: %u x %s\n", rc[j], reasons[j]);
    assert(!bad);
}
int main(void) {
    fixture(SPV_MESH_TASK_COMPUTE, sizeof SPV_MESH_TASK_COMPUTE, "selftest task");
    fixture(SPV_MESH_MESH_COMPUTE, sizeof SPV_MESH_MESH_COMPUTE, "selftest mesh");
    const char *dir = getenv("DM_MESHPACK_DIR");
    if (dir && *dir) library(dir);
    printf("t_android_meshpack: ok\n");
    return 0;
}
