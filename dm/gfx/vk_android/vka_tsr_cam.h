// Camera motion from depth for a temporal upscaler in agl's NDSR slot (vka_ndsr.c, DM_VK_NDSR=gsr2; docs/ANALISE-SGSR2-FSR31-FRAMEGEN-2026-11-16.md, step B). Plain C
// (the PC test t_android_tsr_cam.c builds it): the camera from a game's scene constant block, the reprojection prev_VP * inverse(VP) in double (in float the far
// field trembled: NX2navi's lesson), and a CPU reference of the motion pass (fsr/ndsr_tsr.comp) for the tests.
// The block's layout is a game's (a build's): rows of four floats, the view as three rows (right, up, back; x_view = row . (x, y, z, 1)) and the projection as four
// (clip = P x_view), as NX2navi and Lichii describe Ocarina of Time's 0x3500-byte scene context block (vka_ndsr.c k_tsr_cam: gated by the main module's build ID)
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct { double m[4][4]; } vka_m4;   // row-major: m[row][column], column vectors (y = M x)

static inline void vka_m4_mul(const vka_m4 *a, const vka_m4 *b, vka_m4 *o) {
    vka_m4 r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a->m[i][k] * b->m[k][j]; r.m[i][j] = s; }
    *o = r;
}
// Gauss-Jordan with partial pivoting; 0 when the matrix is singular (a pivot below 1e-300 times its column's scale)
static inline int vka_m4_inv(const vka_m4 *a, vka_m4 *o) {
    double w[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { w[i][j] = a->m[i][j]; w[i][j + 4] = i == j; }
    for (int c = 0; c < 4; c++) {
        int p = c; double best = fabs(w[c][c]), scale = 0;
        for (int r = 0; r < 4; r++) { double v = fabs(w[r][c]); if (v > scale) scale = v; }
        for (int r = c + 1; r < 4; r++) if (fabs(w[r][c]) > best) { best = fabs(w[r][c]); p = r; }
        if (!(best > 1e-300) || !(best > 1e-12 * scale)) return 0;
        if (p != c) for (int j = 0; j < 8; j++) { double t = w[c][j]; w[c][j] = w[p][j]; w[p][j] = t; }
        double inv = 1.0 / w[c][c];
        for (int j = 0; j < 8; j++) w[c][j] *= inv;
        for (int r = 0; r < 4; r++) if (r != c && w[r][c] != 0) { double f = w[r][c]; for (int j = 0; j < 8; j++) w[r][j] -= f * w[c][j]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) o->m[i][j] = w[i][j + 4];
    return 1;
}
// The view (3 rows from vrow, with (0, 0, 0, 1) below) and the projection (4 rows from prow) of a block of floats; each row is four floats at 16 * row bytes
static inline void vka_tsr_view_proj(const float *blk, int vrow, int prow, vka_m4 *view, vka_m4 *proj) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        view->m[i][j] = i < 3 ? (double)blk[(vrow + i) * 4 + j] : (double)(j == 3);
        proj->m[i][j] = (double)blk[(prow + i) * 4 + j];
    }
}
// Every value finite and the view-projection invertible: a camera the motion can use
static inline int vka_tsr_cam_ok(const float *blk, int vrow, int prow) {
    for (int i = 0; i < 3 * 4; i++) if (!isfinite(blk[vrow * 4 + i])) return 0;
    for (int i = 0; i < 4 * 4; i++) if (!isfinite(blk[prow * 4 + i])) return 0;
    vka_m4 v, p, vp, inv; vka_tsr_view_proj(blk, vrow, prow, &v, &p); vka_m4_mul(&p, &v, &vp);
    return vka_m4_inv(&vp, &inv);
}
// The device depth (0..1 as stored) of a point d units in front of the camera (view z = -d, the camera looks along -z); zero_one: NVN's depth mode near = 0
// (nvnDeviceSetDepthMode), else GL's near = -w (window depth = (ndc + 1) / 2)
static inline double vka_tsr_depth_at(const vka_m4 *proj, double d, int zero_one) {
    double z = proj->m[2][2] * -d + proj->m[2][3], w = proj->m[3][2] * -d + proj->m[3][3];
    double ndc = w != 0 ? z / w : 0;
    return zero_one ? ndc : 0.5 * ndc + 0.5;
}
// Reversed depth (near stored larger than far): what GSR2 reads (nearest = max). 1 reversed, 0 not
static inline int vka_tsr_reversed(const vka_m4 *proj, int zero_one) { return vka_tsr_depth_at(proj, 1.0, zero_one) > vka_tsr_depth_at(proj, 100.0, zero_one); }
// The reprojection from this frame's clip space to the previous frame's: prev_VP * inverse(VP), in double, written as 16 floats (row-major) for the motion pass. The pass
// multiplies (NDC x, NDC y, device depth, 1): with GL's depth mode (zero_one 0) the depth's NDC 2z - 1 is folded in here, in double (in float, 2z - 1 of a reversed depth
// near 0 kept too few bits: 0.13 texels off at 20000 units). 0 = VP singular
static inline int vka_tsr_reproj(const vka_m4 *vp, const vka_m4 *prev_vp, int zero_one, float out[16]) {
    vka_m4 inv, r;
    if (!vka_m4_inv(vp, &inv)) return 0;
    vka_m4_mul(prev_vp, &inv, &r);
    if (!zero_one) for (int i = 0; i < 4; i++) { r.m[i][3] -= r.m[i][2]; r.m[i][2] *= 2; }   // r (x, y, 2z - 1, 1) = r' (x, y, z, 1)
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (float)r.m[i][j];
    return 1;
}
// A cut (a new scene, a teleport, a camera switch), where the history must start again: the camera moved more than tmax world units, or turned by more than amax radians
// about any axis, between the two views (their rows' differences). Measured on the view matrices, not the projections (a zoom is not a cut)
static inline int vka_tsr_cut(const vka_m4 *view, const vka_m4 *prev_view, double tmax, double amax) {
    // camera positions: -R^T t
    double c0[3] = {0}, c1[3] = {0};
    for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) { c0[j] -= view->m[i][j] * view->m[i][3]; c1[j] -= prev_view->m[i][j] * prev_view->m[i][3]; }
    double dt = sqrt((c0[0] - c1[0]) * (c0[0] - c1[0]) + (c0[1] - c1[1]) * (c0[1] - c1[1]) + (c0[2] - c1[2]) * (c0[2] - c1[2]));
    if (!(dt <= tmax)) return 1;
    for (int i = 0; i < 3; i++) {   // the angle between each axis row now and before (the rows are unit length for a rigid view; normalized against a scale)
        double a = 0, n0 = 0, n1 = 0;
        for (int j = 0; j < 3; j++) { a += view->m[i][j] * prev_view->m[i][j]; n0 += view->m[i][j] * view->m[i][j]; n1 += prev_view->m[i][j] * prev_view->m[i][j]; }
        if (!(n0 > 0 && n1 > 0)) return 1;
        double c = a / sqrt(n0 * n1); c = c > 1 ? 1 : c < -1 ? -1 : c;
        if (!(acos(c) <= amax)) return 1;
    }
    return 0;
}
// The camera position of a view (for the diagnostic log)
static inline void vka_tsr_cam_pos(const vka_m4 *view, double out[3]) {
    for (int j = 0; j < 3; j++) { out[j] = 0; for (int i = 0; i < 3; i++) out[j] -= view->m[i][j] * view->m[i][3]; }
}

// The motion pass's push constants (fsr/ndsr_tsr.comp, 128 bytes): the reprojection (vka_tsr_reproj, the depth mode folded in), the render size, the pass (0 colour,
// 1 motion and depth) and the conventions (zero_one is only logged by the pass: kept for its layout)
typedef struct { float r[16]; int32_t size[2], pass, yup, zero_one, rev, sqrt_in, pad[9]; } vka_tsr_pc;
_Static_assert(sizeof(vka_tsr_pc) == 128, "vka_tsr_pc");
// The CPU reference of the motion pass at texel (x, y) of a w x h target with device depth z: the motion in texels, previous minus current (DLSS's convention: the
// texel this point was at last frame, minus where it is), as fsr/ndsr_tsr.comp computes it in float, with r from vka_tsr_reproj (the depth mode folded in). yup: NDC y
// up at the top row (NVN's upper-left window origin, as D3D / Metal); else y down (Vulkan). 0 = the point maps behind the previous camera (w <= 0): no motion
static inline int vka_tsr_motion_ref(const float r[16], int x, int y, int w, int h, float z, int yup, float out[2]) {
    float u = ((float)x + 0.5f) / (float)w, v = ((float)y + 0.5f) / (float)h;
    float c[4] = {2.0f * u - 1.0f, yup ? 1.0f - 2.0f * v : 2.0f * v - 1.0f, z, 1.0f}, p[4];
    for (int i = 0; i < 4; i++) p[i] = r[i * 4 + 0] * c[0] + r[i * 4 + 1] * c[1] + r[i * 4 + 2] * c[2] + r[i * 4 + 3] * c[3];
    if (!(p[3] > 1e-8f)) { out[0] = out[1] = 0; return 0; }
    float px = p[0] / p[3], py = p[1] / p[3];
    float pu = 0.5f * px + 0.5f, pv = yup ? 0.5f - 0.5f * py : 0.5f * py + 0.5f;
    out[0] = (pu - u) * (float)w; out[1] = (pv - v) * (float)h;
    return 1;
}
