// Camera motion from depth for the temporal path in agl's NDSR slot (dm/gfx/vk_android/vka_tsr_cam.h; vka_ndsr.c DM_VK_NDSR=gsr2, fsr/ndsr_tsr.comp): the 4x4 inverse,
// the camera read from a block laid out as Ocarina of Time's (view rows 0-2, projection rows 7-10 of a 0x3500-byte block), reversed depth, the reprojection
// prev_VP * inverse(VP) against projecting the same world point with both cameras (near, far, the sky of a reversed infinite projection, world coordinates in the
// thousands), both depth modes and both window origins, and the cut rule
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../gfx/vk_android/vka_tsr_cam.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static uint32_t rs = 0x9e3779b9u;
static double rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (rs & 0xffffff) / (double)0x1000000; }   // [0, 1)

static void inverse(void) {
    for (int t = 0; t < 200; t++) {
        vka_m4 a, inv, p;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) a.m[i][j] = (rnd() - 0.5) * 4 + (i == j ? 3 : 0);
        CHECK(vka_m4_inv(&a, &inv), "invertible %d", t);
        vka_m4_mul(&a, &inv, &p);
        double e = 0; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) e = fmax(e, fabs(p.m[i][j] - (i == j)));
        CHECK(e < 1e-10, "A inverse(A) = I: %g", e);
    }
    vka_m4 s = {{{1, 2, 3, 4}, {2, 4, 6, 8}, {0, 1, 0, 1}, {1, 0, 1, 0}}}, o;
    CHECK(!vka_m4_inv(&s, &o), "singular");
}

// A camera: at pos, turned by yaw about y then pitch about x; looks along -z. Rows right, up, back with the translation in the fourth column
static void look(double px, double py, double pz, double yaw, double pitch, vka_m4 *v) {
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    // camera axes in the world: R = Ry(yaw) Rx(pitch); the view's rows are its columns (R^T)
    double r[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};   // columns: right, up, back
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) v->m[i][j] = r[j][i];
        v->m[i][3] = -(r[0][i] * px + r[1][i] * py + r[2][i] * pz);
    }
    v->m[3][0] = v->m[3][1] = v->m[3][2] = 0; v->m[3][3] = 1;
}
// Perspective: reversed infinite (z = near / w, near = 0..1 depth 1 at the near plane) or standard (near 0, far 1); zero_one: device depth 0..1 (else GL's -1..1 NDC)
static void persp(double fovy, double aspect, double n, double f, int reversed, int zero_one, vka_m4 *p) {
    double t = 1.0 / tan(0.5 * fovy);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) p->m[i][j] = 0;
    p->m[0][0] = t / aspect; p->m[1][1] = t; p->m[3][2] = -1;
    if (reversed) { p->m[2][2] = 0; p->m[2][3] = n; if (!zero_one) { p->m[2][2] = 1; p->m[2][3] = 2 * n; } }   // ndc z = n / d (or 1 + 2n / -... ): near 1, far 0 / -1
    else if (zero_one) { p->m[2][2] = f / (n - f); p->m[2][3] = n * f / (n - f); }
    else { p->m[2][2] = (f + n) / (n - f); p->m[2][3] = 2 * f * n / (n - f); }
}
static void to_block(const vka_m4 *v, const vka_m4 *p, float *blk) {   // Ocarina's layout: rows 0-2 the view, 7-10 the projection
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) blk[i * 4 + j] = (float)v->m[i][j];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) blk[(7 + i) * 4 + j] = (float)p->m[i][j];
}
static void apply(const vka_m4 *m, const double x[4], double y[4]) { for (int i = 0; i < 4; i++) { y[i] = 0; for (int k = 0; k < 4; k++) y[i] += m->m[i][k] * x[k]; } }

// One camera move: for texels across a W x H target and depths from near to the sky, the motion of vka_tsr_motion_ref (float, as the shader) against the world point
// those give projected with the previous camera (double). Returns the largest error in texels
static double move_err(const vka_m4 *v0, const vka_m4 *v1, const vka_m4 *p, int zero_one, int yup, int W, int H, int *n_out) {
    float blk0[0x3500 / 4] = {0}, blk1[0x3500 / 4] = {0};
    to_block(v0, p, blk0); to_block(v1, p, blk1);   // (the matrices as the game's floats: what the host reads)
    vka_m4 bv0, bp0, bv1, bp1, vp0, vp1, inv1;
    vka_tsr_view_proj(blk0, 0, 7, &bv0, &bp0); vka_tsr_view_proj(blk1, 0, 7, &bv1, &bp1);
    vka_m4_mul(&bp0, &bv0, &vp0); vka_m4_mul(&bp1, &bv1, &vp1);
    float r[16]; if (!vka_tsr_reproj(&vp1, &vp0, zero_one, r)) return 1e9;
    vka_m4_inv(&vp1, &inv1);
    double worst = 0; int n = 0;
    static const double depths[] = {0.5, 3, 40, 900, 20000};
    for (int k = 0; k < 64; k++) {
        int x = (int)(rnd() * W), y = (int)(rnd() * H);
        for (size_t di = 0; di < sizeof depths / sizeof *depths; di++) {
            double z = vka_tsr_depth_at(&bp1, depths[di], zero_one);
            float zf = (float)z;   // a D32F texel
            double u = (x + 0.5) / W, vv = (y + 0.5) / H;
            double c[4] = {2 * u - 1, yup ? 1 - 2 * vv : 2 * vv - 1, zero_one ? zf : 2.0 * zf - 1, 1}, wp[4], q[4];
            apply(&inv1, c, wp); apply(&vp0, wp, q);
            if (!(q[3] > 1e-6)) continue;   // behind the previous camera
            double pu = 0.5 * q[0] / q[3] + 0.5, pv = yup ? 0.5 - 0.5 * q[1] / q[3] : 0.5 * q[1] / q[3] + 0.5;
            double ex = (pu - u) * W, ey = (pv - vv) * H;
            float m[2]; if (!vka_tsr_motion_ref(r, x, y, W, H, zf, yup, m)) continue;
            double e = fmax(fabs(m[0] - ex), fabs(m[1] - ey));
            // relative to the motion's size for the large ones (a float's 24 bits)
            double mag = fmax(fabs(ex), fabs(ey)); if (mag > 100) e /= mag / 100;
            if (e > worst) worst = e;
            n++;
        }
    }
    *n_out = n;
    return worst;
}

static void reprojection(void) {
    const int W = 976, H = 549;   // Ocarina's porch scene size
    for (int zo = 0; zo < 2; zo++) for (int rev = 0; rev < 2; rev++) for (int yup = 0; yup < 2; yup++) {
        vka_m4 p; persp(1.0, (double)W / H, 0.25, 30000, rev, zo, &p);
        CHECK(vka_tsr_reversed(&p, zo) == rev, "reversed %d (zero_one %d)", rev, zo);
        for (int t = 0; t < 40; t++) {
            // world coordinates in the thousands (a field), a walk of up to 20 units and a turn of up to 0.2 rad per frame
            double x = (rnd() - 0.5) * 8000, y = rnd() * 500, z = (rnd() - 0.5) * 8000, yaw = rnd() * 6.28, pitch = (rnd() - 0.5) * 0.8;
            vka_m4 v0, v1; look(x, y, z, yaw, pitch, &v0);
            look(x + (rnd() - 0.5) * 40, y + (rnd() - 0.5) * 10, z + (rnd() - 0.5) * 40, yaw + (rnd() - 0.5) * 0.4, pitch + (rnd() - 0.5) * 0.2, &v1);
            int n = 0; double e = move_err(&v0, &v1, &p, zo, yup, W, H, &n);
            // (a standard depth's far field: its depth near 1 cancels in the float sums of the pass, up to 0.2 texels at 20000 units of a 30000 far plane, by compiler;
            // reversed depth, as NVN games store it, stays under 0.05)
            double tol = rev ? 0.05 : 0.25;
            CHECK(n > 0 && e < tol, "zero_one %d reversed %d yup %d move %d: %d points, worst error %.4f texels", zo, rev, yup, t, n, e);
        }
        // a still camera: no motion anywhere
        vka_m4 v; look(1234.5, 60, -4321, 1.1, 0.1, &v);
        int n = 0; double e = move_err(&v, &v, &p, zo, yup, W, H, &n);
        CHECK(e < 1e-3, "a still camera: %.6f texels", e);
    }
    // the sky of a reversed infinite projection (depth 0): a pure turn moves it by the turn alone
    {
        vka_m4 p, v0, v1; persp(1.0, 16.0 / 9, 0.25, 0, 1, 1, &p);
        look(100, 20, 300, 0.5, 0, &v0); look(100, 20, 300, 0.5 + 0.02, 0, &v1);
        float blk0[64] = {0}, blk1[64] = {0}; to_block(&v0, &p, blk0); to_block(&v1, &p, blk1);
        vka_m4 a, b, c, d, vp0, vp1; vka_tsr_view_proj(blk0, 0, 7, &a, &b); vka_tsr_view_proj(blk1, 0, 7, &c, &d); vka_m4_mul(&b, &a, &vp0); vka_m4_mul(&d, &c, &vp1);
        float r[16]; CHECK(vka_tsr_reproj(&vp1, &vp0, 1, r), "reprojection");
        float m[2]; int ok = vka_tsr_motion_ref(r, 488, 274, 976, 549, 0.0f, 1, m);
        // the camera turned left by 0.02 rad (yaw up): the direction at the centre now was 0.02 rad left of the previous camera's centre, so it was further left: motion -x
        double expect = -0.5 * 976 * tan(0.02) / (tan(0.5) * 16.0 / 9);   // NDC x = (t / aspect) tan(angle), t = 1 / tan(fovy / 2); half the width per NDC unit
        CHECK(ok && fabs(m[0] - expect) < 0.05 && fabs(m[1]) < 0.05, "sky under a turn: %.3f %.3f (expected %.3f 0)", m[0], m[1], expect);
    }
}

static void cuts(void) {
    vka_m4 a, b;
    look(10, 0, 10, 0.3, 0, &a); look(12, 0, 11, 0.35, 0.01, &b);
    CHECK(!vka_tsr_cut(&b, &a, 500, 0.6), "a walk and a small turn: no cut");
    look(4000, 0, 10, 0.3, 0, &b);
    CHECK(vka_tsr_cut(&b, &a, 500, 0.6), "a teleport: a cut");
    look(10, 0, 10, 0.3 + 1.57, 0, &b);
    CHECK(vka_tsr_cut(&b, &a, 500, 0.6), "a 90 degree turn: a cut");
    vka_m4 p0, p1; persp(1.0, 1.7, 0.25, 1000, 1, 1, &p0); persp(0.5, 1.7, 0.25, 1000, 1, 1, &p1);
    CHECK(!vka_tsr_cut(&a, &a, 500, 0.6), "a zoom (the projection alone): no cut");
    double c[3]; look(1, 2, 3, 0.7, 0.2, &a); vka_tsr_cam_pos(&a, c);
    CHECK(fabs(c[0] - 1) < 1e-9 && fabs(c[1] - 2) < 1e-9 && fabs(c[2] - 3) < 1e-9, "camera position %g %g %g", c[0], c[1], c[2]);
    float blk[64] = {0}; to_block(&a, &p0, blk);
    CHECK(vka_tsr_cam_ok(blk, 0, 7), "a camera block is usable");
    float zero[64] = {0}; CHECK(!vka_tsr_cam_ok(zero, 0, 7), "zeros are no camera");
    blk[7 * 4 + 1] = NAN; CHECK(!vka_tsr_cam_ok(blk, 0, 7), "a NaN is no camera");
}

int main(void) {
    inverse(); reprojection(); cuts();
    if (fails) { printf("t_android_tsr_cam: %d failure(s)\n", fails); return 1; }
    printf("t_android_tsr_cam: ok\n");
    return 0;
}
