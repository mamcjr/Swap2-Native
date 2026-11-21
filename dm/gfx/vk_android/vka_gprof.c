// GPU time by step (DM_VK_GPUPROF=n, live): one frame in every n (n >= 1) gets a GPU timestamp after each full barrier the replay records (vka_barrier) and at the start of
// each submission and of each part of one (vka_exec.c chunk). Every full barrier waits for all earlier work, so the time between two timestamps is the GPU time of what was
// recorded between them: one render pass, or the clears / copies / uploads of one op. Once the frame's last submission is done the log gets one "gpu prof" block:
// the frame's GPU time by kind (passes, transfers, barriers with nothing between them) and the steps that cost most, each with the recording site of the barrier that
// ended it (file:line), the draws in it and the pass's attachments. Gaps between submissions and parts (the GPU waiting for recording) are left out.
// It costs a stall per profiled frame (timestamps), nothing when off (one cached switch read per submission)
//
// DM_VK_GPUPROF_DRAWS=1 (with DM_VK_GPUPROF, live; after the erickmendes22 Swap2-Native fork's per-draw marks, vka_gprof.c on ocarina-v11-ios-perf-from-v101 2b098ec5 /
// 4ed24d0e, MIT, made general: every pass, every game): on the profiled frame a timestamp also right after each pass begins and after each draw in it, so each draw's
// interval is the GPU time between the end of the one before and its own end. The report then ranks the 32 most expensive draws with their shader keys (vertex, pixel,
// mesh, task), the pass they are in, and, for a mesh draw made by the compute fallback (vka_mesh.inc), the fallback's compute steps since the draw before (the task and mesh
// programs run as compute, and the fallback's own count / control dispatches, all ended by mesh_barrier) added to it; then the same time summed by pixel shader and by mesh
// shader. Caveat: a tiler (Adreno in GMEM mode, Mali) runs a pass bin by bin, so a timestamp inside a pass may be written per bin, or move the pass to direct rendering; the
// intervals then rank draws, they are not their isolated cost, and a pass with many draws may run slower while timed. Ordinary frames record nothing
#if defined(__ANDROID__)
#include "vka.h"
#include <stdlib.h>
#include <string.h>

// 8192 timestamps: a frame of many draws timed one by one (the erickmendes22 fork found 2048 too few for Ocarina of Time's scene even without them)
enum { NQ = 8192, DESC = 256, SHADERS = 64, PASSES = 1024, PDESC = 96 };
// kind: 0 barrier, 1 submission start, 2 part start, 3 commit, 4 a draw's end (DM_VK_GPUPROF_DRAWS), 5 a pass's begin (the same).
// ckey: a compute step's first program (vka_gp_compute), the one a mesh draw's fallback ran; a draw mark's shaders, units and instances; pass: its pass (g_pdesc index)
typedef struct { const char *file; int line; int kind; uint64_t draws, passes, ops; uint64_t ckey, vs, fs, mesh, task; uint32_t units, inst; int pass; char desc[DESC]; } gp_mark;
static gp_mark g_m[NQ]; static int g_n;
static int g_draws_on, g_in_pass, g_npass; static char g_pdesc[PASSES][PDESC];   // per-draw marks this frame; the pass being drawn is timed; the passes' descriptions
static VkQueryPool g_qp; static int g_ok = -1; static double g_tick_ns;
static PFN_vkCmdWriteTimestamp g_write; static PFN_vkCmdResetQueryPool g_reset; static PFN_vkGetQueryPoolResults g_get;
static int g_state;                     // 0 idle, 1 recording the profiled frame, 2 waiting for its last fence
static uint64_t g_frame, g_last_frame, g_fence; static VkCommandBuffer g_cb;
static char g_pass[DESC]; static int g_pass_new;   // the last pass opened (vka_gp_pass), taken by the next mark
static uint64_t g_sub_frame; static int g_sub_frame_set;
// Which shaders a timed step ran (after the erickmendes22 Ocarina fork's attribution): a pass's draws (its first draw's vertex / mesh / task shaders and size, its three
// most used pixel shaders) and a compute step's dispatch (its shader and grid). Kept only while a frame is timed
static struct { uint64_t key; unsigned count; } g_fs[SHADERS];
static unsigned g_pass_draws, g_fs_over; static int g_nfs;
static uint64_t g_first_vs, g_first_mesh, g_first_task; static uint32_t g_first_units, g_first_instances; static int g_first_indirect;
static char g_compute[DESC]; static int g_compute_new; static uint64_t g_ckey;
static void mark(VkCommandBuffer cb, int kind, const char *file, int line);
void vka_gp_draw(uint64_t vertex, uint64_t fragment, uint64_t mesh, uint64_t task, uint32_t units, uint32_t instances, int indirect) {
    if (!vka_gp_active) return;
    if (!g_pass_draws) { g_first_vs = vertex; g_first_mesh = mesh; g_first_task = task; g_first_units = units; g_first_instances = instances; g_first_indirect = indirect != 0; }
    g_pass_draws++;
    if (g_draws_on && g_in_pass && g_cb && g_n < NQ - 64) {   // (room kept for the frame's own barrier marks)
        mark(g_cb, 4, "draw", 0);
        gp_mark *m = &g_m[g_n - 1]; m->vs = vertex; m->fs = fragment; m->mesh = mesh; m->task = task; m->units = units; m->inst = instances;
    }
    for (int i = 0; i < g_nfs; i++) if (g_fs[i].key == fragment) { g_fs[i].count++; return; }
    if (g_nfs < SHADERS) { g_fs[g_nfs].key = fragment; g_fs[g_nfs++].count = 1; } else g_fs_over++;
}
// vka_draw.c rp_begin, right after vkCmdBeginRendering: the pass's first draw is timed from here, not from the barrier before it (its loads and clears are this interval)
void vka_gp_pass_begun(VkCommandBuffer cb) {
    if (!vka_gp_active || !g_draws_on || cb != g_cb || g_n >= NQ - 64) return;
    if (g_npass < PASSES) snprintf(g_pdesc[g_npass], PDESC, "%s", g_pass);
    mark(cb, 5, "pass begin", 0); g_m[g_n - 1].pass = g_npass < PASSES ? g_npass : -1; g_npass++;
    g_in_pass = 1;
}
// Dispatches in a row have no full barrier between them ("Dispatch order", vka_draw.c), so one timed step can hold several: each is listed (up to the description's
// length; the count is always right), the first ones first. Before 2026-11-16 a step showed only its last dispatch, and Ocarina of Time's run of dispatches ending in
// one AGL kernel of a single workgroup read as 3-4.5 ms of that kernel
static unsigned g_ncompute;
void vka_gp_compute(uint64_t key, uint32_t x, uint32_t y, uint32_t z, int indirect) {
    if (!vka_gp_active) return;
    if (!g_compute_new) { g_ncompute = 0; g_compute[0] = 0; g_ckey = key; }
    g_ncompute++;
    size_t used = strlen(g_compute);
    if (used < DESC - 48) snprintf(g_compute + used, DESC - used, "%s%016llx %ux%ux%u%s", used ? ", " : "", (unsigned long long)key, x, y, z, indirect ? " (indirect)" : "");
    g_compute_new = 1;
}
int vka_gp_active;                      // marks are taken (read by vka_barrier)

void vka_gp_sub_frame(uint64_t f) { g_sub_frame = f; g_sub_frame_set = 1; }   // the replay worker: the game frame of the submission it replays next
// The timestamp tick measured against the host clock where the driver's timestampPeriod is off (vka_exec.c tick_sample, Banana Native 5e4212d1); this weak default (0:
// the driver's period) stands until vka_exec.c has it
__attribute__((weak)) double vka_ts_tick_ns(void) { return 0; }
static double tick_now(void) { double t = vka_ts_tick_ns(); return t > 0 ? t : g_tick_ns; }
static int init(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    g_write = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); g_reset = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    g_get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NQ};
    if (!g_write || !g_reset || !g_get || !create || create(V.dev, &ci, NULL, &g_qp) || V.props.limits.timestampPeriod <= 0) { VRED("gpu prof: no timestamp query pool"); return 0; }
    g_tick_ns = V.props.limits.timestampPeriod; g_ok = 1;
    return 1;
}
static void mark(VkCommandBuffer cb, int kind, const char *file, int line) {
    if (g_n >= NQ) return;
    gp_mark *m = &g_m[g_n]; m->file = file; m->line = line; m->kind = kind;
    m->ckey = m->vs = m->fs = m->mesh = m->task = 0; m->units = m->inst = 0; m->pass = g_in_pass && g_npass && g_npass <= PASSES ? g_npass - 1 : -1;
    uint64_t sk, di, pi; vka_draw_stats(&m->draws, &sk, &di, &pi); (void)sk; (void)di; (void)pi;
    extern uint64_t vka_n_rp; m->passes = vka_n_rp; uint64_t subs, rec; vka_exec_counts(&m->ops, &subs, &rec);
    if (kind >= 4) m->desc[0] = 0;   // a draw's or a pass begin's mark: the pass's description and shaders stay for the mark that ends it
    else if (g_pass_new) {   // the pass that ends here: its shaders
        size_t used = (size_t)snprintf(m->desc, DESC, "%s", g_pass);
        if (g_pass_draws && used < DESC) {
            used += (size_t)snprintf(m->desc + used, DESC - used, " | %u draws", g_pass_draws);
            if (used < DESC)
                used += (size_t)snprintf(m->desc + used, DESC - used, ", first: %s %u inst %u%s vs %016llx%s", g_first_mesh ? "groups" : "vertices", g_first_units, g_first_instances,
                                         g_first_indirect ? " indirect" : "", (unsigned long long)g_first_vs, g_first_mesh ? "" : "");
            if (g_first_mesh && used < DESC) used += (size_t)snprintf(m->desc + used, DESC - used, " mesh %016llx task %016llx", (unsigned long long)g_first_mesh, (unsigned long long)g_first_task);
            unsigned char picked[SHADERS] = {0};
            for (int rank = 0; rank < 3 && used < DESC; rank++) {
                int best = -1;
                for (int j = 0; j < g_nfs; j++) if (!picked[j] && (best < 0 || g_fs[j].count > g_fs[best].count)) best = j;
                if (best < 0) break;
                picked[best] = 1;
                used += (size_t)snprintf(m->desc + used, DESC - used, " fs %016llx x%u", (unsigned long long)g_fs[best].key, g_fs[best].count);
            }
            if (g_fs_over && used < DESC) snprintf(m->desc + used, DESC - used, " (+%u more)", g_fs_over);
        }
        g_pass_new = 0; g_pass_draws = 0; g_nfs = 0; g_fs_over = 0; g_compute_new = 0; g_in_pass = 0;
    } else if (g_compute_new) { snprintf(m->desc, DESC, "compute x%u: %s", g_ncompute, g_compute); g_compute_new = 0; m->ckey = g_ckey; }
    else m->desc[0] = 0;
    if (kind >= 1 && kind <= 3) g_in_pass = 0;   // (a submission or part boundary: no pass spans it)
    vka_enc_sync();
    g_write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, (uint32_t)g_n);
    g_n++;
}
void vka_gp_mark(VkCommandBuffer cb, const char *file, int line) { if (cb == g_cb) mark(cb, 0, file, line); }
void vka_gp_pass(const char *desc) { if (vka_gp_active) { snprintf(g_pass, DESC, "%s", desc); g_pass_new = 1; g_pass_draws = 0; g_nfs = 0; g_fs_over = 0; g_in_pass = 0; } }
void vka_gp_part(VkCommandBuffer cb) { if (!vka_gp_active) return; g_cb = cb; mark(cb, 2, "part", 0); }

typedef struct { int i; double ms; } step;
static int by_ms(const void *a, const void *b) { double d = ((const step *)b)->ms - ((const step *)a)->ms; return d > 0 ? 1 : d < 0 ? -1 : 0; }
// DM_VK_GPUPROF_DRAWS: the frame's draws by GPU time. A draw's time: the interval its own mark ends (from the pass begin or the draw before in its pass) and, for a mesh draw
// the fallback made, every compute step since the previous draw that ran its task or mesh program (ckey) or ended at the fallback's mesh_barrier (vka_mesh.inc)
typedef struct { int i; double ms, cs; } dstep;
static int by_dms(const void *a, const void *b) { double d = (((const dstep *)b)->ms + ((const dstep *)b)->cs) - (((const dstep *)a)->ms + ((const dstep *)a)->cs); return d > 0 ? 1 : d < 0 ? -1 : 0; }
typedef struct { uint64_t k; int n; double ms; } dsum;
static void dsum_add(dsum *a, int *na, uint64_t k, double ms) {
    int j = 0; while (j < *na && a[j].k != k) j++;
    if (j == *na) { if (*na == 256) return; a[(*na)++] = (dsum){k, 0, 0}; }
    a[j].n++; a[j].ms += ms;
}
static void draws_report(const uint64_t *t) {
    static dstep d[NQ]; int nd = 0, prev_draw = 0; double raster = 0, mesh_cs = 0;
    for (int i = 1; i < g_n; i++) {
        if (g_m[i].kind != 4) continue;
        const gp_mark *m = &g_m[i], *a = &g_m[i - 1];
        double ms = a->kind == 1 || a->kind == 2 || t[i] <= t[i - 1] ? 0 : (t[i] - t[i - 1]) * g_tick_ns / 1e6, cs = 0;
        // The fallback's own draw is the pulling vertex stage (the mesh program's key in the vertex slot, mesh and task slots empty: vka_draw.c passes sh[0], sh[7], sh[6]),
        // so every draw looks back for the compute steps since the previous draw (found 2026-11-20 on the phone: 0.00 ms attributed in Zelda's porch while mesh draws ran)
        for (int j = i - 1; j > prev_draw; j--) {
            const gp_mark *b = &g_m[j];
            if (b->kind != 0 || g_m[j - 1].kind == 1 || g_m[j - 1].kind == 2 || t[j] <= t[j - 1]) continue;
            int fallback = (b->ckey && (b->ckey == m->mesh || b->ckey == m->task || b->ckey == m->vs)) || (b->file && !strcmp(b->file, "vka_mesh.inc(compute)"));
            if (fallback) cs += (t[j] - t[j - 1]) * g_tick_ns / 1e6;
        }
        d[nd++] = (dstep){i, ms, cs}; raster += ms; mesh_cs += cs; prev_draw = i;
    }
    if (!nd) { VLOG("gpu prof draws: frame %llu: no draw timed (no pass opened while the frame was timed)", (unsigned long long)g_frame); return; }
    VLOG("gpu prof draws: frame %llu: %d draws in %d passes timed one by one, %.2f ms in their passes, %.2f ms of mesh fallback compute attributed to them%s; on a tiler the "
         "intervals rank draws, they are not their isolated cost", (unsigned long long)g_frame, nd, g_npass, raster, mesh_cs, g_n >= NQ - 64 ? " (timestamps ran out: the frame's later draws are not timed)" : "");
    static dsum byfs[256], bymesh[256]; int nfs = 0, nmesh = 0;   // by pixel shader and by mesh shader (the vertex shader where there is none): count and time
    for (int k = 0; k < nd; k++) {
        const gp_mark *m = &g_m[d[k].i];
        dsum_add(byfs, &nfs, m->fs, d[k].ms + d[k].cs); dsum_add(bymesh, &nmesh, m->mesh ? m->mesh : m->vs, d[k].ms + d[k].cs);
    }
    qsort(d, (size_t)nd, sizeof *d, by_dms);
    for (int k = 0; k < nd && k < 32; k++) {
        const gp_mark *m = &g_m[d[k].i];
        VLOG("gpu prof draw #%d: %.3f ms (%.3f ms of it mesh fallback compute): vs %016llx fs %016llx mesh %016llx task %016llx, %u %s x%u; pass %d %s", k + 1, d[k].ms + d[k].cs,
             d[k].cs, (unsigned long long)m->vs, (unsigned long long)m->fs, (unsigned long long)m->mesh, (unsigned long long)m->task, m->units,
             m->mesh ? "groups" : "vertices", m->inst ? m->inst : 1, m->pass, m->pass >= 0 && m->pass < PASSES ? g_pdesc[m->pass] : "?");
    }
    char line[1400];
    for (int w = 0; w < 2; w++) {
        dsum *a = w ? bymesh : byfs; int na = w ? nmesh : nfs; size_t ln = 0; line[0] = 0;
        for (int r = 0; r < 12 && r < na; r++) {   // the 12 most expensive, by selection
            int b = r; for (int j = r + 1; j < na; j++) if (a[j].ms > a[b].ms) b = j;
            dsum x = a[r]; a[r] = a[b]; a[b] = x;
            if (ln < sizeof line) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %016llx %dx %.2fms;", (unsigned long long)a[r].k, a[r].n, a[r].ms);
        }
        VLOG("gpu prof draws by %s:%s", w ? "mesh shader (vertex shader where there is none)" : "pixel shader", line);
    }
}
static void report(void) {
    static uint64_t t[NQ];
    g_tick_ns = tick_now();   // (the measured tick once it is in)
    if (g_get(V.dev, g_qp, 0, (uint32_t)g_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT)) { VLOG("gpu prof: frame %llu: results not ready", (unsigned long long)g_frame); return; }
    static step s[NQ]; int ns = 0; double tot = 0, pass = 0, xfer = 0, bare = 0, first = 0; int npass = 0;
    for (int i = 1; i < g_n; i++) {
        if (g_m[i].kind == 1 || g_m[i].kind == 2) continue;   // the gap before a submission or part: the GPU waited for recording
        double ms = t[i] > t[i - 1] ? (t[i] - t[i - 1]) * g_tick_ns / 1e6 : 0;
        tot += ms; s[ns++] = (step){i, ms};
        if (g_m[i].kind >= 4 || g_m[i - 1].kind >= 4) pass += ms;   // (inside a pass timed draw by draw)
        else if (g_m[i].passes > g_m[i - 1].passes) { pass += ms; npass++; } else if (g_m[i].ops > g_m[i - 1].ops || g_m[i].draws > g_m[i - 1].draws) xfer += ms; else bare += ms;
        if (g_m[i - 1].kind == 1) first += ms;
    }
    VLOG("gpu prof: frame %llu: %d timestamps, %.2f ms GPU: passes %.2f ms (%d), clears / copies / dispatches %.2f ms, steps with nothing recorded (barriers, uploads, writebacks) %.2f ms",
         (unsigned long long)g_frame, g_n, tot, pass, npass, xfer, bare);
    if (VKA_ENV("DM_VK_GPUPROF_LIST", 0))   // every step in order
        for (int k = 0; k < ns; k++) { const gp_mark *a = &g_m[s[k].i - 1], *b = &g_m[s[k].i];
            VLOG("gpu prof: step #%d %.3f ms: %s:%d, %llu draws, %llu ops%s%s", s[k].i, s[k].ms, b->file, b->line, (unsigned long long)(b->draws - a->draws),
                 (unsigned long long)(b->ops - a->ops), b->desc[0] ? "; pass " : "", b->desc); }
    if (g_draws_on) draws_report(t);
    qsort(s, (size_t)ns, sizeof *s, by_ms);
    for (int k = 0; k < ns && k < 16; k++) {
        const gp_mark *a = &g_m[s[k].i - 1], *b = &g_m[s[k].i];
        VLOG("gpu prof:  #%d %.3f ms: %s:%d, %llu draws, %llu passes, %llu ops%s%s", s[k].i, s[k].ms, b->file, b->line, (unsigned long long)(b->draws - a->draws),
             (unsigned long long)(b->passes - a->passes), (unsigned long long)(b->ops - a->ops), b->desc[0] ? "; pass " : "", b->desc);
    }
    // by barrier site: count and time
    struct { const char *f; int l; int n; double ms; } site[64]; int nsite = 0;
    for (int k = 0; k < ns; k++) { const gp_mark *b = &g_m[s[k].i]; int j = 0;
        while (j < nsite && !(site[j].l == b->line && site[j].f == b->file)) j++;
        if (j == nsite) { if (nsite == 64) continue; site[nsite++] = (typeof(site[0])){b->file, b->line, 0, 0}; }
        site[j].n++; site[j].ms += s[k].ms; }
    char line[1200]; size_t ln = 0; line[0] = 0;
    for (int j = 0; j < nsite && ln < sizeof line; j++) ln += (size_t)snprintf(line + ln, sizeof line - ln, " %s:%d %dx %.2fms;", site[j].f, site[j].l, site[j].n, site[j].ms);
    VLOG("gpu prof: by site:%s", line);
}
// mtl_sub_begin, after the command buffer begins: arms the profile at a new frame, reports a finished one
void vka_gp_sub_begin(VkCommandBuffer cb, uint64_t frame) {
    if (g_sub_frame_set) frame = g_sub_frame;
    if (g_state == 2 && vka_done() >= g_fence) { report(); g_state = 0; }
    if (g_state == 1 && frame != g_frame) { g_state = 2; vka_gp_active = 0; g_cb = VK_NULL_HANDLE; }
    if (g_state == 2 && vka_done() >= g_fence) { report(); g_state = 0; }
    if (g_state == 0) {
        int every = VKA_ENV("DM_VK_GPUPROF", 0);
        if (every <= 0 || frame == g_frame || (g_last_frame && frame < g_last_frame + (uint64_t)every) || !init()) return;
        g_state = 1; g_frame = g_last_frame = frame; g_n = 0; vka_gp_active = 1; g_pass_new = 0; g_compute_new = 0; g_pass_draws = 0; g_nfs = 0; g_fs_over = 0;
        g_draws_on = VKA_ENV("DM_VK_GPUPROF_DRAWS", 0) != 0; g_in_pass = 0; g_npass = 0;
        g_reset(cb, g_qp, 0, NQ);
    }
    if (g_state == 1) { g_cb = cb; mark(cb, 1, "submission", 0); }
}
void vka_gp_commit(VkCommandBuffer cb, uint64_t fence) { if (g_state == 1 && cb == g_cb) { mark(cb, 3, "commit", 0); g_fence = fence; } }
#endif
