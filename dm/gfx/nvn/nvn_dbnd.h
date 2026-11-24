// Depth bounds test: the bounds a draw runs with, from what the game set (nvnCommandBufferSetDepthBounds, recorded as OP_DEPTH_BOUNDS). The Switch's GPU passes a fragment
// when the depth already stored at its sample lies in [min, max], as GL's EXT_depth_bounds_test and Vulkan define it. The stored depth is always in [0, 1], so a bound outside
// it is the same as the bound clamped, and (0, 1) cannot reject anything; bounds that no stored depth can meet (min above max, min above 1, max below 0, or a NaN, which fails
// every comparison on the GPU) pass nothing. Vulkan takes bounds in [0, 1] only (without VK_EXT_depth_range_unrestricted), so the replay hands it the clamped bounds, or
// (1, 0) for bounds that pass nothing (vka_draw.c). Pure C, shared with upstream nxnative's Metal replay, which takes the same resolver.
// Ported from Banana Native v0.3.51, 94009e1d (MIT; there from seedds/nxnative 5b7dad03b).
#pragma once

enum { DBND_OFF, DBND_TEST, DBND_EMPTY };   // the test is off or cannot reject anything / bounds to test / no fragment passes

// b receives the bounds to test: (0, 1) unless DBND_TEST
static inline int dbnd_resolve(int enable, float lo, float hi, float b[2]) {
    b[0] = 0; b[1] = 1;
    if (!enable) return DBND_OFF;
    if (!(lo <= hi) || !(lo <= 1) || !(hi >= 0)) return DBND_EMPTY;   // written so that a NaN on either side lands here
    b[0] = lo > 0 ? lo : 0; b[1] = hi >= 1 ? 1 : hi > 0 ? hi : 0;   // -0 becomes 0
    return b[0] == 0 && b[1] == 1 ? DBND_OFF : DBND_TEST;
}
