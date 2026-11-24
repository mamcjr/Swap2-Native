/* Host check for vka_quad_indirect_spv.h: validates the embedded compute
   SPIR-V header (and runs spirv-val if present), the shared quad index pattern,
   and the host reference for indirect DrawArraysIndirect -> DrawIndexedIndirect.
   Ported from Banana Native v0.3.51, 3ce1aadf (MIT). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vka_quad_indirect_spv.h"

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    abort(); } } while (0)

enum {
    SPV_MAGIC = 0x07230203u,
    SPV_VER_1_3 = 0x00010300u,
    SPV_OP_ENTRY_POINT = 15u,
    SPV_OP_EXECUTION_MODE = 16u,
    SPV_EXEC_GL_COMPUTE = 5u,
    SPV_MODE_LOCAL_SIZE = 17u
};

static int find_spirv_val(void) {
    int rc = system("command -v spirv-val >/dev/null 2>&1");   // NOLINT(bugprone-command-processor): a fixed command, spirv-val looked up on the PATH
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static int run_spirv_val(const uint32_t *words, size_t n_words) {
    assert(words != NULL && n_words > 5u);
    if (!find_spirv_val()) return 0;

    char path[] = "/tmp/t_quad_ind_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);

    size_t want = n_words * sizeof(uint32_t);
    ssize_t wrote = write(fd, words, want);
    int close_rc = close(fd);
    CHECK(wrote == (ssize_t)want && close_rc == 0);

    char cmd[128];
    int n = snprintf(cmd, sizeof cmd, "spirv-val --target-env vulkan1.3 %s", path);
    CHECK(n > 0 && (size_t)n < sizeof cmd);

    int rc = system(cmd);   // NOLINT(bugprone-command-processor): spirv-val on the test's own temporary file
    int rm_rc = unlink(path);
    CHECK(rm_rc == 0);
    CHECK(rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
    return 1;
}

static void check_spirv(int *validated) {
    assert(validated != NULL);
    const size_t n = sizeof quad_indirect_spv / sizeof quad_indirect_spv[0];
    CHECK(n == 287u);
    CHECK(quad_indirect_spv[0] == SPV_MAGIC);
    CHECK(quad_indirect_spv[1] == SPV_VER_1_3);
    CHECK(quad_indirect_spv[4] == 0u);

    int has_compute = 0;
    int has_local_111 = 0;
    for (size_t i = 5u; i < n;) {
        uint32_t w = quad_indirect_spv[i];
        uint32_t wc = w >> 16u;
        uint32_t op = w & 0xffffu;
        CHECK(wc >= 1u && i + wc <= n);
        if (op == SPV_OP_ENTRY_POINT && wc >= 2u && quad_indirect_spv[i + 1u] == SPV_EXEC_GL_COMPUTE)
            has_compute = 1;
        if (op == SPV_OP_EXECUTION_MODE && wc >= 6u && quad_indirect_spv[i + 2u] == SPV_MODE_LOCAL_SIZE) {
            if (quad_indirect_spv[i + 3u] == 1u && quad_indirect_spv[i + 4u] == 1u && quad_indirect_spv[i + 5u] == 1u)
                has_local_111 = 1;
        }
        i += wc;
    }
    CHECK(has_compute == 1 && has_local_111 == 1);
    *validated = run_spirv_val(quad_indirect_spv, n);
}

static void check_fill_indices(void) {
    enum { TEST_QUADS = 8u, GUARD = 2u };
    uint32_t buf[GUARD + TEST_QUADS * 6u + GUARD];
    const uint32_t sentinel = 0xDEADBEEFu;
    for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) buf[i] = sentinel;

    vka_qdx_fill_indices(buf + GUARD, 0u);
    for (size_t i = 0; i < sizeof buf / sizeof buf[0]; i++) CHECK(buf[i] == sentinel);

    vka_qdx_fill_indices(buf + GUARD, TEST_QUADS);
    CHECK(buf[0] == sentinel && buf[1] == sentinel);
    CHECK(buf[GUARD + TEST_QUADS * 6u] == sentinel);
    CHECK(buf[GUARD + TEST_QUADS * 6u + 1u] == sentinel);

    static const uint32_t pat[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    for (uint32_t q = 0; q < TEST_QUADS; q++) {
        for (uint32_t k = 0; k < 6u; k++)
            CHECK(buf[GUARD + q * 6u + k] == q * 4u + pat[k]);
    }
}

static void check_convert_host(void) {
    uint32_t out[5] = {0};

    vka_qdx_convert_host(0u, 1u, 0u, 0u, 65536u, out);
    CHECK(out[0] == 0u && out[1] == 1u && out[2] == 0u && out[3] == 0u && out[4] == 0u);

    vka_qdx_convert_host(4u, 2u, 0u, 0u, 65536u, out);
    CHECK(out[0] == 6u && out[1] == 2u && out[2] == 0u && out[3] == 0u && out[4] == 0u);

    /* Trailing partial vertices (< 4) drop and non-zero offsets survive. */
    vka_qdx_convert_host(11u, 3u, 12u, 5u, 65536u, out);
    CHECK(out[0] == 12u && out[1] == 3u && out[2] == 0u && out[3] == 12u && out[4] == 5u);

    /* Large vertexCount clamps cleanly to max_quads. */
    vka_qdx_convert_host(400000u, 7u, 16u, 9u, 65536u, out);
    CHECK(out[0] == 65536u * 6u && out[1] == 7u && out[2] == 0u && out[3] == 16u && out[4] == 9u);
}

int main(void) {
    int validated = 0;
    check_spirv(&validated);
    check_fill_indices();
    check_convert_host();
    printf("t_android_quad_ind: ok%s\n", validated ? " (spirv-val)" : " (no spirv-val: structure only)");
    return 0;
}
