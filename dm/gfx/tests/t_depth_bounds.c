// The depth bounds resolver (dm/gfx/nvn/nvn_dbnd.h) the Vulkan replay takes for nvnCommandBufferSetDepthBounds (vka_draw.c): what it tests, what passes nothing, and
// that what cannot reject a stored depth (the test off, or bounds covering [0, 1]) leaves the test off. make check_t_depth_bounds
// Ported from Banana Native v0.3.51, 94009e1d (MIT).
#include <math.h>
#include <stdio.h>
#include "../nvn/nvn_dbnd.h"

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } } while (0)

static void expect(int en, float lo, float hi, int kind, float b0, float b1, const char *label) {
    float b[2] = {-7, -7};
    int k = dbnd_resolve(en, lo, hi, b);
    CHECK(k == kind, label);
    CHECK(b[0] == b0 && b[1] == b1, label);
    CHECK(!signbit(b[0]) && !signbit(b[1]), label);   // never -0: Vulkan takes [0, 1]
}

int main(void) {
    expect(0, 0.2f, 0.4f, DBND_OFF, 0, 1, "the test off: (0, 1), whatever the bounds");
    expect(1, 0, 1, DBND_OFF, 0, 1, "(0, 1) rejects no stored depth: off");
    expect(1, -1, 2, DBND_OFF, 0, 1, "bounds past [0, 1] on both sides clamp to (0, 1): off");
    expect(1, 0.25f, 0.75f, DBND_TEST, 0.25f, 0.75f, "bounds inside [0, 1] are tested as given");
    expect(1, -0.5f, 0.5f, DBND_TEST, 0, 0.5f, "a min below 0 clamps to 0");
    expect(1, 0.5f, 3, DBND_TEST, 0.5f, 1, "a max above 1 clamps to 1");
    expect(1, -0.0f, 0.5f, DBND_TEST, 0, 0.5f, "-0 becomes 0");
    expect(1, 0.5f, 0.5f, DBND_TEST, 0.5f, 0.5f, "min equal to max tests that one depth");
    expect(1, 0.6f, 0.4f, DBND_EMPTY, 0, 1, "min above max passes nothing");
    expect(1, 1.5f, 2, DBND_EMPTY, 0, 1, "min above 1 passes nothing");
    expect(1, -2, -1, DBND_EMPTY, 0, 1, "max below 0 passes nothing");
    expect(1, NAN, 0.5f, DBND_EMPTY, 0, 1, "a NaN min passes nothing");
    expect(1, 0.5f, NAN, DBND_EMPTY, 0, 1, "a NaN max passes nothing");
    printf("t_depth_bounds: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
