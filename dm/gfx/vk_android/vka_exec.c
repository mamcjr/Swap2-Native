// The command replay: hle_nvn.c records the game's NVN commands into the game's own command memory (nvn_obj.h records); a submission replays its handles here into one
// Vulkan command buffer (mtl_sub_begin → mtl_exec per handle → mtl_sub_commit), as nvn_mtl.m does into Metal. This milestone covers what a first frame needs: render targets,
// colour / depth-stencil / texture clears, buffer and texture copies, inline updates and fills, barriers, events, counters and fences. Draws and dispatches log once and are skipped
// until the shader side lands. Every transfer op is followed by a full memory barrier: correct, not fast
#if defined(__ANDROID__)
#include "vka.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_timestamp.h"
#include "vka_present_rot.h"
#include "vka_qsched.h"
#include "vka_ubq.h"   // inline uniform updates' ranges and batch rule (t_android_ubq.c)
#include "vka_rcix.h"   // render conditions: the newest report in flight by address (t_android_rcix.c)
#include "vka_oqbatch.h"   // occlusion queries: a completed list's reads and resets in batches (t_android_rcix.c)
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t serial; p_set_rt rt; int has_rt; p_scissor sc; int has_sc; int64_t copy_stride, copy_img_stride; int rc_skip; vka_dstate d; } qstate;   // what a queue keeps between submissions (d: the draw state, vka_draw.c)
static qstate S;
enum { NQS = 8 };
static qstate g_qs[NQS]; static int g_nqs;
enum { NSLOT = 8 };
enum { NCHUNK = 32 };
static struct { VkCommandBuffer cb; uint64_t fence; VkCommandBuffer x[NCHUNK]; VkFence vf; int vf_on; } g_slot[NSLOT];   // vf: set when its commit had no timeline signal (vf_on)   // x: the command buffers a submission goes on in once it hands its first part to the queue (chunk)
static unsigned g_slot_i;
static int g_chunk_draws = -1, g_nx; static uint64_t g_chunk_d0;   // chunk(): draws a part, parts this submission, draws at its part's start
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;   // the submission being recorded (NULL without a device)
VkCommandBuffer vka_rp_cb(void) { return g_cb; }
typedef struct { int kind; uint32_t mode, v, type; uint64_t addr, q0, q1; uint32_t ord, fr; uint64_t k1, k2; uint32_t ent; } pop;   // kind 1 event, 2 counter report, 3 frame span, 4 counted samples-passed report, 5 occlusion queries given back unread (q0..q1: their sequence numbers), 6 a samples-passed report this frame does not count: the report address's last count again (DM_VK_OQ_REUSE)
static pop *g_post; static int g_npost, g_postcap;
static uint64_t g_frame, g_stat_ops, g_stat_draws, g_stat_skipped;
static int g_origin_ll = 1, g_queue_origin_ll = 1, g_depth_zero;
_Atomic int vka_synclog_n;   // DM_VK_SYNCLOG: lines left (mtl_wait)
#define g_synclog vka_synclog_n
static double ms_now(void) { return (now_ns() % 1000000000000ull) / 1e6; }
// ---------- Render conditions (profile render_cond) ----------
// SetRenderEnableConditional(mode, rec) is evaluated here at replay, on the CPU, from the records in memory, as nvn_mtl.m does (rc_value there). A report is written to memory
// only when its submission completes (post_run), so what memory holds is not yet the answer while the report is still in flight:
// - a timestamp report puts 0 in the counter field (the time goes in the second field): one still in flight reads 0 (Tears of the Kingdom's conditional clears);
// - a samples-passed report (an occlusion query) is counted on the GPU (Occlusion queries, below): such a report is kept by fence like a timestamp one (kind 3), reads
//   unknown while its submission is in flight, and from memory (the real count) once it is done. A report that is not counted (DM_VK_OCCLUSION=0, no query pool, no
//   samples-passed reset before it: post_run writes a constant 1) and every other non-timestamp report put their address in a set that reads unknown for good, and a
//   condition on it keeps drawing. Before 2026-11-06 the Vulkan port had no such set: RENDER_IF_NOT_EQUAL over a begin / end pair of occlusion reports compared 1 == 1 (or
//   0 == 0 before their first write) and skipped exactly the objects the game tested for visibility; until 2026-11-07 no samples-passed report was counted;
// - drawing an unknown block is right for occlusion culling (it only saves work) but not for a conditional clear, where a clear that should have been skipped shows. That is the
//   cost on every unknown path: the list overflowed, an address outside guest RAM, and an address that once held an occlusion report (the set is never emptied) and now holds
//   timestamp reports. Before 2026-11-06 a report past the list's end read memory instead, which is the answer only once its submission is done;
// - the reports in flight are kept by fence, of every submission not done yet (vka_done), not only the one being recorded: a report of an earlier submission still on the GPU
//   used to be invisible, and the condition read memory one or more submissions old. Before 2026-11-06 only the timestamp reports of the submission being recorded were
//   kept, at most 64, and tracking stopped silently past that.
// Over RC_PEND_MAX reports in flight, every condition whose reports the list does not hold reads unknown (draws) until the submission that overflowed is done, and says so once.
// rc_known finds the newest entry at an address through an index (vka_rcix.h: every entry appended is put in it, the newest wins; rc_prune rebuilds it in order when it
// removed entries). Until 2026-11-12 it scanned the whole list from its end for each of a condition's two counters (O(reports in flight); Ocarina of Time evaluates over
// 1,000 conditions a frame with up to about 7,000 in flight). Same answers: the index holds exactly the entry the reverse scan stopped at
typedef struct { uint64_t addr, fence; int kind; } rc_rep;   // kind 1 timestamp (counter 0), 2 another counter (unknown), 3 a counted samples-passed report (unknown in flight); fence 0 = the submission being recorded
enum { RC_PEND_MAX = 32768 };   // (counted samples-passed reports take entries: Ocarina of Time makes hundreds a frame)
_Static_assert(RCIX_CAP >= 2 * RC_PEND_MAX, "the render-condition index must stay at most half full (its probes end at an empty slot)");
_Static_assert(offsetof(rc_rep, addr) == 0, "rcix_rebuild reads each entry's address as its first field");
static rc_rep *g_rcp; static int g_nrcp, g_rcpcap, g_rc_over_cur; static uint64_t g_rc_over;   // g_rc_over: the fence of a submission that overflowed the list
static rcix g_rcix;   // the newest entry of g_rcp by address (the replay thread: rc_note, rc_prune and rc_known all run there)
static uint64_t *g_rc_occ; static size_t g_rc_occ_cap, g_rc_occ_n;   // addresses ever written by a non-timestamp report (open addressing, grows at half full)
// Per-frame counts: SetRenderEnable(true / false); the conditional ones by result (skipped / drawn by the comparison, drawn as unknown: an uncounted occlusion report, another
// report in flight, the list overflowed, not in guest RAM, a counted occlusion report in flight); and of the compared ones, those that took a timestamp report in flight as 0
enum { RCS_ON, RCS_OFF, RCS_SKIP, RCS_DRAW, RCS_OCC, RCS_PEND, RCS_OVER, RCS_NOMEM, RCS_TS, RCS_OQ, RCS_N };
static uint64_t g_rc_stat[RCS_N]; static int g_rcp_max;
static int rc_occ_has(uint64_t a) {
    if (!g_rc_occ_cap) return 0;
    for (size_t i = (size_t)(a >> 4) & (g_rc_occ_cap - 1);; i = (i + 1) & (g_rc_occ_cap - 1)) { if (g_rc_occ[i] == a) return 1; if (!g_rc_occ[i]) return 0; }
}
static void rc_occ_add(uint64_t a) {
    if (!a || rc_occ_has(a)) return;
    if (2 * (g_rc_occ_n + 1) > g_rc_occ_cap) {
        size_t nc = g_rc_occ_cap ? 2 * g_rc_occ_cap : 4096; uint64_t *n = calloc(nc, sizeof *n);
        if (!n) { VRED_ONCE("render conditions: the occlusion report set cannot grow past %zu addresses: conditions on new ones are compared from memory", g_rc_occ_n); return; }
        for (size_t i = 0; i < g_rc_occ_cap; i++) if (g_rc_occ[i]) { size_t j = (size_t)(g_rc_occ[i] >> 4) & (nc - 1); while (n[j]) j = (j + 1) & (nc - 1); n[j] = g_rc_occ[i]; }
        free(g_rc_occ); g_rc_occ = n; g_rc_occ_cap = nc;
    }
    size_t j = (size_t)(a >> 4) & (g_rc_occ_cap - 1); while (g_rc_occ[j]) j = (j + 1) & (g_rc_occ_cap - 1);
    g_rc_occ[j] = a; g_rc_occ_n++;
}
// A report recorded in the submission being recorded. A non-timestamp one goes in the occlusion set, which rc_known looks at before the list, so it takes a list entry only
// when the set could not hold it; a second report at an address this submission already reported replaces that entry (as nvn_mtl.m's rc_note; the last RC_MERGE entries are
// searched). Before both, every occlusion report took an entry no condition read, and a game with many of them filled the list and overflowed it. A counted samples-passed
// report (kind 3) skips the set: it takes an entry, and its address reads from memory once its submission is done
enum { RC_MERGE = 64 };
static void rc_note(uint64_t a, int kind) {
    if (kind == 2) { rc_occ_add(a); if (rc_occ_has(a)) return; }
    for (int i = g_nrcp - 1, k = 0; i >= 0 && !g_rcp[i].fence && k < RC_MERGE; i--, k++) if (g_rcp[i].addr == a) { g_rcp[i].kind = kind; return; }
    if (g_nrcp >= RC_PEND_MAX) {
        if (!g_rc_over_cur) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("render conditions: more than %d reports in flight: conditions on reports the list does not hold draw until this submission is done", RC_PEND_MAX); }
        g_rc_over_cur = 1; return;
    }
    if (g_nrcp == g_rcpcap) { int nc = g_rcpcap ? 2 * g_rcpcap : 256; rc_rep *n = realloc(g_rcp, (size_t)nc * sizeof *n); if (!n) { g_rc_over_cur = 1; return; } g_rcp = n; g_rcpcap = nc; }
    g_rcp[g_nrcp++] = (rc_rep){a, 0, kind};
    rcix_put(&g_rcix, a, (uint32_t)(g_nrcp - 1));   // (the merge above changes the newest entry in place: the index already points at it)
    if (g_nrcp > g_rcp_max) g_rcp_max = g_nrcp;
}
static void rc_prune(void) {   // the reports whose submissions are done are in memory now (post_run runs before the fence reads as done)
    uint64_t done = vka_done(); int j = 0, n0 = g_nrcp;
    for (int i = 0; i < g_nrcp; i++) if (!g_rcp[i].fence || g_rcp[i].fence > done) g_rcp[j++] = g_rcp[i];
    g_nrcp = j;
    if (j != n0) rcix_rebuild(&g_rcix, g_rcp, sizeof *g_rcp, g_nrcp);   // the compaction keeps the order but moves entries (nothing removed: the positions hold)
    if (g_rc_over && g_rc_over <= done) g_rc_over = 0;
}
static void rc_commit(uint64_t fence) {   // the submission's reports take its fence
    for (int i = g_nrcp - 1; i >= 0 && !g_rcp[i].fence; i--) g_rcp[i].fence = fence;
    if (g_rc_over_cur) { g_rc_over = fence; g_rc_over_cur = 0; }
}
// A counter at a: 1 = known (*v: the value; a timestamp report in flight counts 0, else what memory holds now); 0 = unknown (*why: the RCS_ reason)
static int rc_known(uint64_t a, uint64_t *v, int *why) {
    if (rc_occ_has(a)) { *why = RCS_OCC; return 0; }
    uint64_t done = vka_done();
    int i = rcix_find(&g_rcix, a);   // the newest report at a decides: when it is done, memory holds it (below, as the old scan's break)
    if (i >= 0 && (!g_rcp[i].fence || g_rcp[i].fence > done)) {
        if (g_rcp[i].kind != 1) { *why = g_rcp[i].kind == 3 ? RCS_OQ : RCS_PEND; return 0; }
        *v = 0; *why = RCS_TS; return 1;
    }
    if (g_rc_over_cur || (g_rc_over && g_rc_over > done)) { *why = RCS_OVER; return 0; }
    if (!vka_addr(a, 8, NULL, NULL, NULL)) { *why = RCS_NOMEM; return 0; }
    memcpy(v, (const void *)(uintptr_t)a, 8); *why = RCS_DRAW; return 1;
}
uint64_t vka_frame(void);
static int rc_eval(const p_render_cond *rc) {   // 1 = the block that begins here is not drawn
    if (rc->mode >= 2) { g_rc_stat[rc->mode == 2 ? RCS_ON : RCS_OFF]++; return rc->mode == 3; }
    uint64_t x = 0, y = 0; int wx = RCS_DRAW, wy = RCS_DRAW;
    int kx = rc_known(rc->addr, &x, &wx), ky = rc_known(rc->addr + 16, &y, &wy);
    int skip = kx && ky && (rc->mode ? x == y : x != y);
    g_rc_stat[!kx ? wx : !ky ? wy : skip ? RCS_SKIP : RCS_DRAW]++;
    {   // Diagnostic (live through ctl, or nxreplay's environment): DM_VK_RC_LOG=<n>, once per value: the first n conditional evaluations from the next frame on
        static double was; static int left; static uint64_t fr = ~0ull; double n = VKA_ENV("DM_VK_RC_LOG", 0);
        if (n != was && vka_frame() != fr) { was = n; left = (int)n; fr = vka_frame(); }
        if (left > 0) { left--;
            VLOG("rc: frame %llu mode %d at %#llx: %s %#llx (why %d), +16 %s %#llx (why %d) -> %s", (unsigned long long)vka_frame(), rc->mode, (unsigned long long)rc->addr,
                 kx ? "known" : "unknown", (unsigned long long)x, wx, ky ? "known" : "unknown", (unsigned long long)y, wy, !kx || !ky ? "drawn (unknown)" : skip ? "skipped" : "drawn"); }
    }
    if (kx && ky && (wx == RCS_TS || wy == RCS_TS)) g_rc_stat[RCS_TS]++;
    return skip;
}
// Every 300 frames with the replay statistics (DM_VK_STATS): what the render conditions did a frame (no line while the game sets none)
static void rc_stats(char *b, size_t n, double frames) {
    static uint64_t s0[RCS_N]; uint64_t d[RCS_N], all = 0;
    for (int i = 0; i < RCS_N; i++) { d[i] = g_rc_stat[i] - s0[i]; s0[i] = g_rc_stat[i]; all += d[i]; }
    b[0] = 0;
    if (all && frames > 0)
        snprintf(b, n, "render conditions per frame: SetRenderEnable %.1f true, %.1f false; conditional %.1f skipped and %.1f drawn by the comparison (%.1f of them with a timestamp report "
                 "in flight as 0), drawn as unknown: %.1f on an uncounted occlusion report, %.1f on a counted occlusion report in flight, %.1f on another report in flight, %.1f with the list "
                 "overflowed, %.1f not in guest RAM; reports in flight up to %d",
                 d[RCS_ON] / frames, d[RCS_OFF] / frames, d[RCS_SKIP] / frames, d[RCS_DRAW] / frames, d[RCS_TS] / frames, d[RCS_OCC] / frames, d[RCS_OQ] / frames, d[RCS_PEND] / frames,
                 d[RCS_OVER] / frames, d[RCS_NOMEM] / frames, g_rcp_max);
    if (frames > 0 && g_rcix.lookups) VLOG("render condition index: %.1f lookups/frame, %.2f probes/lookup, %.1f found/frame (capacity %u)", g_rcix.lookups / frames,
                                          (double)g_rcix.probes / (double)g_rcix.lookups, g_rcix.found / frames, RCIX_CAP);
    g_rcix.lookups = g_rcix.probes = g_rcix.found = 0;
    g_rcp_max = g_nrcp;
}
static void oq_stats(char *b, size_t n, double frames);   // (Occlusion queries)

int vka_origin_ll(void) { return __atomic_load_n(&g_origin_ll, __ATOMIC_RELAXED); }
int vka_queue_origin_ll(void) { return g_queue_origin_ll; }
int vka_depth_zero(void) { return g_depth_zero; }
void mtl_set_origin_lower_left(int on) { if (__atomic_exchange_n(&g_origin_ll, on != 0, __ATOMIC_RELAXED) != (on != 0)) VLOG("window origin: %s", on ? "lower left (the present flips the picture)" : "upper left"); }
void vka_rp_set_queue_origin(int on) { g_queue_origin_ll = on != 0; }
void mtl_set_depth_zero(int on) { g_depth_zero = on != 0; }
// Replay statistics every 300 frames (DM_VK_STATS=0: none): what a frame costs the GPU in passes and barriers (each pass loads and stores its attachments on a tiler), and the
// CPU per draw (recording: mtl_sub_begin to vkEndCommandBuffer)
extern uint64_t vka_n_rp, vka_n_bar, vka_n_transfer_bar, vka_n_attachment_bar; static uint64_t g_n_ub, g_n_sub, g_rec_ns, g_n_pclear, g_n_chunks;
static void ub_stats(char *b, size_t n, double frames);   // "Inline uniform updates"
static _Atomic uint64_t g_fstart[64], g_fstart_f[64];   // when the replay began recording each frame's first submission (its number mod 64): the adaptive floor's spans
static uint64_t g_job_frame; static int g_job_frame_set;   // the frame of the submission the worker replays (vka_exec_sub_frame)
void vka_exec_sub_frame(uint64_t f) { g_job_frame = f; g_job_frame_set = 1; }
uint64_t vka_frame_start_ns(uint64_t f) { unsigned k = f % 64; return atomic_load(&g_fstart_f[k]) == f ? atomic_load(&g_fstart[k]) : 0; }   // now_ns; 0 = not known (vka_perf.c)
void mtl_set_frame(uint64_t f) {
    g_frame = f; vka_ctl_frame(f);
    { if (atomic_load_explicit(&vka_synclog_n, memory_order_relaxed) > 0) VLOG("sync frame %llu begins at %.2f ms", (unsigned long long)f, (now_ns() % 1000000000000ull) / 1e6); }
    static uint64_t f0, d0, rp0, b0, ub0, s0, r0, o0, pc0, ch0, tb0, ab0;
    int on = VKA_ENV("DM_VK_STATS", 1) != 0;
    if (on && f - f0 >= 300) {
        double n = (double)(f - f0);
        if (f0) VLOG("replay up to frame %llu: per frame %.0f draws, %.0f render passes, %.0f clears / copies (%.0f in a pass), %.0f full barriers, %.0f transfer-write barriers, %.0f attachment-write barriers, %.0f inline uniform updates, %.1f submissions (%.1f parts); recording %.2f ms a frame (%.2f us a draw)",
                     (unsigned long long)f, (g_stat_draws - d0) / n, (vka_n_rp - rp0) / n, (g_stat_ops - o0) / n, (g_n_pclear - pc0) / n, (vka_n_bar - b0) / n, (vka_n_transfer_bar - tb0) / n, (vka_n_attachment_bar - ab0) / n, (g_n_ub - ub0) / n, (g_n_sub - s0) / n, (g_n_sub - s0 + g_n_chunks - ch0) / n, (g_rec_ns - r0) / n / 1e6,
                     g_stat_draws > d0 ? (g_rec_ns - r0) / 1e3 / (double)(g_stat_draws - d0) : 0.0);
        { char pb[1024]; void vka_draw_prof(char *b, size_t n, double draws); vka_draw_prof(pb, sizeof pb, (double)(g_stat_draws - d0)); if (pb[0]) VLOG("draw path per draw (us): %s", pb);
          vka_enc_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb);
          vka_floor_stats(pb, sizeof pb); if (pb[0]) VLOG("%s", pb);
          vka_rp_bar_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb);
          vka_disp_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb);
          { void vka_mesh_stats(char *b, size_t n, double frames); vka_mesh_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb); }
          vka_gbar_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb);
          { char ub[640]; ub_stats(ub, sizeof ub, n); if (ub[0]) VLOG("%s", ub); }
          rc_stats(pb, sizeof pb, n); if (pb[0]) VLOG("%s", pb);
          { char ob[1024]; oq_stats(ob, sizeof ob, n); if (ob[0]) VLOG("%s", ob); }
          { void vka_drv_memo_stats(char *b, size_t n); vka_drv_memo_stats(pb, sizeof pb); if (pb[0]) VLOG("%s", pb); } }
        f0 = f; d0 = g_stat_draws; rp0 = vka_n_rp; b0 = vka_n_bar; ub0 = g_n_ub; s0 = g_n_sub; r0 = g_rec_ns; o0 = g_stat_ops; pc0 = g_n_pclear; ch0 = g_n_chunks; tb0 = vka_n_transfer_bar; ab0 = vka_n_attachment_bar;
    }
}
uint64_t vka_frame(void) { return g_frame; }
void vka_exec_counts(uint64_t *ops, uint64_t *subs, uint64_t *rec_ns) { *ops = g_stat_ops; *subs = g_n_sub; *rec_ns = g_rec_ns; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { if (passes) *passes = g_stat_ops; if (draws) *draws = g_stat_draws; }
void mtl_prepare(void) { vka_init(); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

void vka_rp_queue_activate(uint64_t serial) {
    if (S.serial == serial) return;
    int i = 0; while (i < g_nqs && g_qs[i].serial != S.serial) i++;
    if (S.serial) { if (i == g_nqs && g_nqs < NQS) g_nqs++; if (i < NQS) g_qs[i] = S; }
    for (i = 0; i < g_nqs; i++) if (g_qs[i].serial == serial) { S = g_qs[i]; return; }
    memset(&S, 0, sizeof S); S.serial = serial;
}
void vka_rp_queue_forget(uint64_t serial) {
    for (int i = 0; i < g_nqs; i++) if (g_qs[i].serial == serial) { g_qs[i] = g_qs[--g_nqs]; break; }
    if (S.serial == serial) memset(&S, 0, sizeof S);
}

static void post(pop o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, (size_t)g_postcap * sizeof *g_post); }
    g_post[g_npost++] = o;
}

// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN's samples-passed counter is cumulative: ResetCounter(SAMPLES_PASSED) zeroes it and starts the count, every draw after it adds the samples that pass, and
// ReportCounter(SAMPLES_PASSED) writes the count at that point ({count, ticks}) once the GPU gets there; two reports with no reset between keep counting (the second holds
// the first's samples too), as nvn_mtl.m's visibility slots. On Vulkan, after a reset the first draw of a render pass begins an occlusion query (PRECISE when the device
// has occlusionQueryPrecise, which vka_dev.c enables with the device's other features); the query ends at the pass end (vka_rp_end), at the next report and at the next
// reset. The queries since the previous report (or reset) are the report's segment: the completion thread, after the report's fence, adds the segment's results to the
// running count (from 0 after a reset), writes it into guest memory and gives the queries back (vkResetQueryPool with hostQueryReset, else vkCmdResetQueryPool at a later
// submission's start: oq_refill). Nothing waits for the GPU: a result not available after its fence counts 1 sample, and so does a draw made while counting that got no
// query (the ring was full), that the replay skipped (no pipeline, no shader, DrawTexture) or that was dropped with no command buffer (a GPU hang): what is not known reads
// as visible. A report with no command buffer writes 1 and leaves its segment to the next counted report (cumulative: its count includes them). The queries are a ring of OQ_N by sequence
// number, allocated by the replay thread and given back by the completion thread in the same order (Ocarina of Time makes hundreds of reports a frame). One count for
// every queue, as nvn_mtl.m's. Until 2026-11-07 every samples-passed report wrote a constant 1; DM_VK_OCCLUSION=0 (read once, at the first samples-passed reset or report)
// still does (A/B), and so does a report before any samples-passed reset. The replay statistics (DM_VK_STATS) print "occlusion queries per frame: ..."
// Reads and resets by list (2026-11-12, vka_oqbatch.h): when the ranges of a completed submission's reports and given-back segments follow one another, post_run reads
// them all in reads of up to OQ_BATCH queries before the first report (oq_batch_init; a wide read that fails is read again query by query), each report sums its own part
// in the original order, and the whole range is reset (host reset) or freed (command reset) once after the last report (oq_batch_release), to the same value the last
// report gave back before. A list with a gap, out of order or over the ring takes the report-by-report path, unchanged. Results are the same, except after a failed wide
// read (not a lost device): a query whose own read then succeeds gives its count, where the report-by-report path counts its whole read 1 a query; g_oq_mu is held over the
// whole list, so its running count is never interleaved with a vka_fence_cpu post_run on another thread. DM_VK_OQ_BATCH=0 (read once, with the pool) keeps the
// report-by-report path; "occlusion report batching per frame: ..." counts the reads, the resets and the give-backs out of order. Open (not made worse here): a carried
// list (carry_take) whose fence is above the signal stays behind while later lists give back the queries after it, so g_oq_ready can pass slots it has not read yet and the
// replay may begin a query that was not reset; the root fix is in-order give-back tracking (the lowest sequence not given back yet), not the max of the two
enum { OQ_N = 32768, OQ_RB = 256, OQ_BATCH = 1024 };   // queries in the ring; results read at a time (report by report; by list)
static struct { VkQueryPool pool; int precise, host; PFN_vkCmdBeginQuery begin; PFN_vkCmdEndQuery end; PFN_vkCmdResetQueryPool creset; PFN_vkResetQueryPool hreset; PFN_vkGetQueryPoolResults get; } OQ;
static int g_oq = -1;                      // 1 counted; 0 the constant 1 (DM_VK_OCCLUSION=0, no device or no query pool); -1 not decided yet
static int g_oq_count, g_oq_fresh, g_oq_open;   // a samples-passed reset came (draws count); a reset since the last report (the next starts from 0); a query is active in the open pass
static uint32_t g_oq_lost;                 // draws of the segment that count 1 sample (no query, or skipped by the replay)
static uint64_t g_oq_head, g_oq_seg0;      // the next query's sequence number; the segment's first
static _Atomic uint64_t g_oq_ready, g_oq_freed;   // queries below ready are reset and may begin; (command reset) the completion thread has read every query below freed
static uint64_t g_oq_acc; static pthread_mutex_t g_oq_mu = PTHREAD_MUTEX_INITIALIZER;   // the running count (the completion thread; vka_fence_cpu may run a submission's effects on another)
static uint64_t g_oqs_reset, g_oqs_rep, g_oqs_nores, g_oqs_q, g_oqs_full, g_oqs_skip, g_oqs_fly;
static uint64_t g_oqp_cur, g_oqs_passes, g_oqs_pmax, g_oqs_din;   // the queries' pattern (oq_stats): queries in the open pass, passes that held any, the most in one, draws counted inside a query
// Occlusion reuse (DM_VK_OQ_REUSE=<n>, read once at the first samples-passed reset; default 1 = every report counted). Off by default since 2026-11-15 19:00: with 3,
// Ocarina of Time drew light effects over Link (bright white streaks on his back by Mido, 2 of 4 screenshots; none with 1, with the new or the old library): a light
// whose visibility the game measures with a query is drawn over everything while it reads visible, and a repeated "visible" keeps it drawn for up to n - 1 frames
// after Link stepped in front of it. The check's "visible while hidden" answers (about 11 a frame) are not harmless for such effects. Ocarina of Time resets and reports around every object it draws: 2,322
// reports a frame by Kokiri Forest's scarecrow, all 186 passes holding queries, up to 597 in one, 2,952 draws inside them (2026-11-15). On the Adreno 830 those queries
// cost about 10 ms of a 52 ms frame (every report written 1 instead: 41.7 ms; not precise: no change): a tiler sums a query bin by bin, and Qualcomm advises few of them
// in a binned pass. With n, a report whose object was counted lately is counted again only every n frames; in between its draws begin no query and the report writes
// that object's last count: its visibility is measured every n frames and repeated in between, n - 1 frames late at most on top of the replay's own delay.
// The object is named by content (vka_draw: the first draw after the reset or the report before; k1 its shaders, counts, index and first vertex buffer, k2 the bytes
// of its vertex stage's uniform buffers): the game's report addresses come from a ring that moves every frame, and the order of its reports shifts as objects come and
// go (docs/RESULTADOS-ETAPA1-2026-11-15.md, seventh step). By the scarecrow 2,256 of 2,317 reports a frame have a key of their own, 92 % of those come back in the next
// three frames, and of those 0.1-0.2 % change between zero and non-zero (the camera still). A key not seen before, or seen twice in one of the last 8 frames, is counted
// every frame; each key's phase (its high bits) spreads the counted frames evenly
static int g_oq_reuse = -1, g_oq_skip, g_oq_rcheck; static uint64_t g_oq_kres, g_oq_kframe, g_oqs_reused, g_oqs_rnew, g_oqs_rdup, g_oqs_rhid;
// Only a visible object's count is repeated: one whose last count was 0 is counted every frame, so a repeated answer is never "hidden" for an object that came into
// view (it would vanish for up to n - 1 frames); the only stale answer left is "visible" for one that went out of view, a draw the game culls a few frames late.
// With the camera turning by the scarecrow, repeating zeros too gave ~2 % of the repeated answers hidden while visible (2026-11-15). DM_VK_OQ_REUSE_ZERO=1 repeats zeros too
static int g_oq_rzero;
// DM_VK_OQ_REUSE_CHECK=1 (diagnostic): the reused reports are counted all the same and compared with the count they repeat (kind 7: the real count is summed, the
// repeated one written), so the stats say how often the game got a stale answer: hidden while it was visible (an object drawn late) or visible while it was hidden
static _Atomic uint64_t g_oqs_rchk, g_oqs_rlate, g_oqs_rextra;
// The objects' last counts: the replay thread finds and ages the entries (key, seen, dup), the completion thread writes val and valid for the counted reports whose
// entry still holds their key (an entry not seen for 64 frames is taken by a new key)
enum { OQK_N = 16384, OQK_PROBE = 32, OQK_AGE = 64, OQK_DUP = 8 };
typedef struct { _Atomic uint64_t key; uint64_t seen, dupf; _Atomic uint64_t val; _Atomic uint32_t valid; } oqk_ent;   // seen, dupf: replayed frame + 1
static oqk_ent g_oqk[OQK_N];
static uint64_t oqk_key(uint64_t k1, uint64_t k2) { uint64_t k = k1 ^ (k2 * 0x9E3779B97F4A7C15ull); return k ? k : 1; }
static uint32_t oqk_find(uint64_t key, uint64_t f1) {   // the entry's index + 1, 0 = none (the probe is full of live keys: counted)
    uint32_t i0 = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 50) & (OQK_N - 1), old = 0;
    for (uint32_t k = 0; k < OQK_PROBE; k++) {
        uint32_t i = (i0 + k) & (OQK_N - 1); uint64_t ek = atomic_load_explicit(&g_oqk[i].key, memory_order_relaxed);
        if (ek == key) return i + 1;
        if (!old && (!ek || f1 - g_oqk[i].seen > OQK_AGE)) old = i + 1;
    }
    if (!old) return 0;
    oqk_ent *e = &g_oqk[old - 1];
    atomic_store_explicit(&e->valid, 0, memory_order_relaxed); atomic_store_explicit(&e->key, key, memory_order_release); e->seen = 0; e->dupf = 0;
    return old;
}
// A report's content keys (vka_oq_draw): the first draw after its reset or the report before
static uint64_t g_oq_sk1, g_oq_sk2; static uint32_t g_oq_sent; static int g_oq_sk_set, g_oq_keyon = -1;
int vka_oq_key_wanted(void) {   // vka_draw: whether this draw's content keys are wanted (the first draw of a counted segment, with the reuse or the trace on)
    if (g_oq_keyon < 0) g_oq_keyon = VKA_ENV("DM_VK_OQ_TRACE", 0) > 0;
    return g_oq_count && g_cb && !g_oq_sk_set && (g_oq_keyon || g_oq_reuse > 1);
}
static _Atomic uint64_t g_oqs_unav, g_oqs_zero;   // statistics (oq_stats)
static int g_oq_batch_on;                  // DM_VK_OQ_BATCH (read once in oq_on, before g_oq is 1)
static uint64_t g_oqb_pairs[2 * OQ_N];     // a list's {result, availability} pairs (g_oq_mu)
static uint64_t g_oq_gb_end;               // the highest sequence given back so far (g_oq_mu; the out-of-order diagnostic only)
static _Atomic uint64_t g_oq_get_calls, g_oq_reset_calls, g_oq_prefetched, g_oq_ooo;   // statistics (oq_stats): result reads, host resets, queries read by list, give-backs out of order
typedef struct { uint64_t first, last; int on; } oq_batch;   // a completed list's queries [first, last), read into g_oqb_pairs (on)
static int oq_on(void) {
    if (g_oq >= 0) return g_oq;
    g_oq = 0;
    if (!VKA_ENV("DM_VK_OCCLUSION", 1)) { VLOG("occlusion queries: off (DM_VK_OCCLUSION=0): every samples-passed report writes 1"); return 0; }
    if (!vka_ok()) { VRED("occlusion queries: no device: every samples-passed report writes 1"); return 0; }
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vk.vkGetPhysicalDeviceFeatures2(V.pd, &f2);   // (the device was made with every feature it has: vka_dev.c)
    #define OQF(f, n) OQ.f = (PFN_##n)vk.vkGetDeviceProcAddr(V.dev, #n);
    OQF(begin, vkCmdBeginQuery) OQF(end, vkCmdEndQuery) OQF(creset, vkCmdResetQueryPool) OQF(hreset, vkResetQueryPool) OQF(get, vkGetQueryPoolResults)
    #undef OQF
    PFN_vkCreateQueryPool create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool");
    // DM_VK_OQ_PRECISE=0 (read once, with the pool; default 1): the queries without VK_QUERY_CONTROL_PRECISE_BIT, for an A/B of what precise counting costs on a tiler
    // (a non-precise result is only zero or not zero: the game's counts become any non-zero number)
    OQ.precise = f2.features.occlusionQueryPrecise != 0 && VKA_ENV("DM_VK_OQ_PRECISE", 1); OQ.host = f12.hostQueryReset && OQ.hreset;
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ_N};
    VkResult r = create && OQ.begin && OQ.end && OQ.creset && OQ.get ? create(V.dev, &ci, NULL, &OQ.pool) : VK_ERROR_FEATURE_NOT_PRESENT;
    if (r) { OQ.pool = VK_NULL_HANDLE; VRED("occlusion queries: no query pool (%d): every samples-passed report writes 1", r); return 0; }
    if (OQ.host) { OQ.hreset(V.dev, OQ.pool, 0, OQ_N); atomic_store(&g_oq_ready, OQ_N); }   // (else oq_refill resets them at the next submission's start)
    g_oq_batch_on = VKA_ENV("DM_VK_OQ_BATCH", 1) != 0;
    VLOG("occlusion queries: counted on the GPU (%s; queries given back by %s; results read and given back %s; DM_VK_OCCLUSION=0: the constant 1)", OQ.precise ? "precise counts" : "not precise: a nonzero count for any sample",
         OQ.host ? "host reset" : "a reset at a submission's start", g_oq_batch_on ? "by completed submission when its ranges follow one another (DM_VK_OQ_BATCH=0: by report)" : "by report (DM_VK_OQ_BATCH=0)");
    return g_oq = 1;
}
typedef struct { VkCommandBuffer cb; uint32_t q, end; } oq_arg;
static void oq_cmd(const void *a) { oq_arg w; memcpy(&w, a, sizeof w); if (w.end) OQ.end(w.cb, OQ.pool, w.q); else OQ.begin(w.cb, OQ.pool, w.q, OQ.precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0); }
static void oq_rec(uint32_t q, uint32_t end) { oq_arg w = {g_cb, q, end}; if (!vka_enc_call(oq_cmd, &w, sizeof w)) oq_cmd(&w); }   // in order with the encoder's calls
static void oq_close(void) { if (g_oq_open) { g_oq_open = 0; oq_rec((uint32_t)((g_oq_head - 1) % OQ_N), 1); } }
void vka_oq_pass_end(void) {   // vka_rp_end: the pass's query ends before the pass
    oq_close();
    if (g_oqp_cur) { g_oqs_passes++; if (g_oqp_cur > g_oqs_pmax) g_oqs_pmax = g_oqp_cur; g_oqp_cur = 0; }
}
void vka_oq_draw(uint64_t k1, uint64_t k2) {   // vka_draw: a draw about to be recorded in the open pass; while counting, the pass's first draw (or the first after a report) begins a query
    if (g_oq_count && !g_oq_sk_set) {
        g_oq_sk1 = k1; g_oq_sk2 = k2; g_oq_sk_set = 1; g_oq_sent = 0; g_oq_skip = 0;
        if (g_oq_reuse > 1 && k1) {   // this report's object: repeated when it has a count, is not doubled lately and it is not its frame to be counted
            uint64_t key = oqk_key(k1, k2), f1 = (g_job_frame_set ? g_job_frame : g_frame) + 1;
            if ((g_oq_sent = oqk_find(key, f1))) {
                oqk_ent *e = &g_oqk[g_oq_sent - 1];
                if (e->seen == f1) e->dupf = f1;
                e->seen = f1;
                int dup = e->dupf && f1 - e->dupf < OQK_DUP, has = atomic_load_explicit(&e->valid, memory_order_acquire) != 0;
                int vis = has && (g_oq_rzero || atomic_load_explicit(&e->val, memory_order_relaxed) != 0);
                g_oq_skip = has && !dup && vis && (f1 + (key >> 40)) % (uint64_t)g_oq_reuse != 0;
                if (!has) g_oqs_rnew++; else if (dup) g_oqs_rdup++; else if (!vis) g_oqs_rhid++;
            } else g_oqs_rnew++;
        }
    }
    if (g_oq_count && g_oq_open && g_cb) g_oqs_din++;
    if (!g_oq_count || g_oq_open || !g_cb || (g_oq_skip && !g_oq_rcheck)) return;
    uint64_t ready = atomic_load_explicit(&g_oq_ready, memory_order_acquire);
    if (g_oq_head >= ready) { g_oq_lost++; g_oqs_full++; return; }   // the ring is full (or not reset yet): this draw counts 1
    uint64_t base = OQ.host ? ready - OQ_N : atomic_load_explicit(&g_oq_freed, memory_order_relaxed);
    if (g_oq_head + 1 - base > g_oqs_fly) g_oqs_fly = g_oq_head + 1 - base;
    oq_rec((uint32_t)(g_oq_head % OQ_N), 0); g_oq_head++; g_oq_open = 1; g_oqs_q++; g_oqp_cur++; g_oqs_din++;
}
static uint64_t oq_sk0(void) { uint64_t s = 0; if (g_oq_count) vka_draw_stats(NULL, &s, NULL, NULL); return s; }   // around a draw: one the replay skipped counts 1
static void oq_sk1(uint64_t s0) { if (!g_oq_count) return; uint64_t s = 0; vka_draw_stats(NULL, &s, NULL, NULL); if (s > s0) { g_oq_lost++; g_oqs_skip++; } }
static void oq_reset(void) {   // OP_RESET_COUNTER(SAMPLES_PASSED)
    g_oqs_reset++;
    if (!oq_on()) return;
    oq_close();
    if (g_oq_head > g_oq_seg0) post((pop){5, 0, 0, 0, 0, g_oq_seg0, g_oq_head});   // the queries since the last report: no report reads them
    g_oq_seg0 = g_oq_head; g_oq_count = 1; g_oq_fresh = 1; g_oq_lost = 0; g_oq_sk_set = 0; g_oq_sk1 = g_oq_sk2 = 0; g_oq_sent = 0; g_oq_skip = 0;
    if (g_oq_reuse < 0) { g_oq_reuse = VKA_ENV("DM_VK_OQ_REUSE", 1); if (g_oq_reuse < 1) g_oq_reuse = 1; if (g_oq_reuse > 1) { g_oq_rcheck = VKA_ENV("DM_VK_OQ_REUSE_CHECK", 0); g_oq_rzero = VKA_ENV("DM_VK_OQ_REUSE_ZERO", 0); VLOG("occlusion queries: an object seen before counted every %d frames, its last count repeated in between (DM_VK_OQ_REUSE)%s", g_oq_reuse, g_oq_rcheck ? "; the repeated counts checked against real ones (DM_VK_OQ_REUSE_CHECK)" : ""); } }
    uint64_t f = g_job_frame_set ? g_job_frame : g_frame; if (f != g_oq_kframe) { g_oq_kframe = f; g_oq_kres = 0; }   // (the frame being replayed: g_frame is the game's, ahead of it with the worker)
    g_oq_kres++;
}
static int oq_report(uint64_t ad, uint32_t t) {   // OP_REPORT_COUNTER(SAMPLES_PASSED): 1 = counted (its segment goes to the completion thread), 0 = the constant 1
    g_oqs_rep++;
    if (!oq_on()) return 0;
    if (!g_oq_count) { g_oqs_nores++; return 0; }
    // No command buffer (a GPU hang: vka_rp_sub_begin; its effects run at once on the CPU, vka_fence_cpu): the constant 1, not an empty segment's 0. Its segment stays open for
    // the next counted report, whose fence is later than those of the submissions that began its queries (the draws dropped meanwhile count 1: OP_DRAW)
    if (!g_cb) return 0;
    if (g_oq_skip && !g_oq_rcheck) {   // not counted this frame (DM_VK_OQ_REUSE): the object's last count, in order with the other reports
        g_oqs_reused++; post((pop){6, 0, 0, t, ad, 0, 0, .ord = (uint32_t)(g_oq_kres - 1), .k1 = g_oq_sk1, .k2 = g_oq_sk2, .ent = g_oq_sent});
        g_oq_seg0 = g_oq_head; g_oq_fresh = 0; g_oq_lost = 0; g_oq_sk_set = 0; g_oq_sent = 0; g_oq_skip = 0;
        return 1;
    }
    // Qualcomm's 891.x miscounts consecutive occlusion queries kept in one render pass: Ocarina of Time's cave walls and Death Mountain Trail rocks vanished on 891.14
    // (upstream Banana Native 881cf986, #210, a game profile key there; checked by A/B captures on an Odin 3, Adreno 830). The pass is ended after a report whose query
    // was active in it, so the next query begins in a new one; the count and its fence take the usual path. Opt-in (DM_VK_OQ_SPLIT_PASS=1, Qualcomm's driver only; 2 any
    // driver), as upstream made it on 2026-11-13: on an Adreno 840 (0x8034a02c) the split took a frame's GPU time from 36-38 ms to 88 ms. On the Adreno 830 with 891.14
    // (Ocarina of Time, Link's house door, 2026-11-13) it cost nothing measurable (25.5 fps both ways); the cave and trail holes were not checked here
    int sp = VKA_ENV("DM_VK_OQ_SPLIT_PASS", 0), split = g_oq_open && (sp >= 2 || (sp == 1 && V.driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY));
    oq_close();   // (draws after it begin a new query)
    if (g_oq_skip) g_oqs_reused++;
    post((pop){g_oq_skip ? 7 : 4, (uint32_t)g_oq_fresh, g_oq_lost, t, ad, g_oq_seg0, g_oq_head, .ord = (uint32_t)(g_oq_kres - 1), .fr = (uint32_t)g_oq_kframe, .k1 = g_oq_sk1, .k2 = g_oq_sk2, .ent = g_oq_sent});
    g_oq_seg0 = g_oq_head; g_oq_sk_set = 0; g_oq_sk1 = g_oq_sk2 = 0; g_oq_sent = 0; g_oq_skip = 0; g_oq_fresh = 0; g_oq_lost = 0;
    if (split) vka_rp_end();
    return 1;
}
static void oq_refill(VkCommandBuffer cb) {   // (no hostQueryReset) at a submission's start, before its first query: the queries the completion thread has read, reset
    if (g_oq != 1 || OQ.host) return;
    uint64_t f = atomic_load_explicit(&g_oq_freed, memory_order_acquire) + OQ_N, r = atomic_load_explicit(&g_oq_ready, memory_order_relaxed);
    for (uint64_t q = r; q < f;) { uint32_t s = (uint32_t)(q % OQ_N), k = (uint32_t)(f - q < OQ_N - s ? f - q : OQ_N - s); OQ.creset(cb, OQ.pool, s, k); q += k; }
    if (f > r) atomic_store_explicit(&g_oq_ready, f, memory_order_release);
}
// The out-of-order diagnostic (g_oq_mu): a give-back that starts below the highest one so far (a carried list taken after later ones). What is stored is not changed
static void oq_gb_note(uint64_t q0, uint64_t q1) {
    if (q0 < g_oq_gb_end) atomic_fetch_add_explicit(&g_oq_ooo, 1, memory_order_relaxed);
    if (q1 > g_oq_gb_end) g_oq_gb_end = q1;
}
// A completed list's queries read at once (post_run, g_oq_mu held, before the first report): only when its ranges follow one another within the ring (vka_oqbatch.h)
static void oq_batch_init(oq_batch *b, const pop *ops, int n) {
    memset(b, 0, sizeof *b);
    if (g_oq != 1 || !OQ.pool || !g_oq_batch_on) return;
    oqb_plan p = {0};
    for (int i = 0; i < n; i++) if (ops[i].kind == 4 || ops[i].kind == 5 || ops[i].kind == 7) oqb_add(&p, ops[i].q0, ops[i].q1);   // (a given-back segment is read unused: harmless after its fence, and keeps the list contiguous)
    if (!oqb_ok(&p, OQ_N)) return;
    for (uint64_t q = p.first; q < p.last;) {
        uint32_t s = (uint32_t)(q % OQ_N), k = oqb_chunk(q, p.last, OQ_N, OQ_BATCH);
        uint64_t *dst = &g_oqb_pairs[2 * (size_t)(q - p.first)];
        memset(dst, 0, 16 * (size_t)k);
        VkResult e = OQ.get(V.dev, OQ.pool, s, k, 16 * (size_t)k, dst, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);   // (no wait: VK_NOT_READY leaves the unavailable ones 0)
        atomic_fetch_add_explicit(&g_oq_get_calls, 1, memory_order_relaxed);
        // A failed wide read, again by query (6b00c2a): a query read alone keeps its result, one that fails counts 1 as before. Not on a lost device (a GPU hang: vka_dev.c
        // runs every pending post_run, and each read would fail again, up to OQ_N driver calls a list with g_oq_mu held): the chunk stays unavailable, 1 a query, as the
        // report-by-report path counts it; and the retry stops at the first single read that loses the device
        if (e < 0) memset(dst, 0, 16 * (size_t)k);
        if (e < 0 && e != VK_ERROR_DEVICE_LOST && !vka_hung()) for (uint32_t j = 0; j < k; j++) {
            uint64_t *one = &dst[2 * j];
            VkResult e1 = OQ.get(V.dev, OQ.pool, s + j, 1, 16, one, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            atomic_fetch_add_explicit(&g_oq_get_calls, 1, memory_order_relaxed);
            if (e1 < 0) { one[0] = one[1] = 0; if (e1 == VK_ERROR_DEVICE_LOST) break; }
        }
        q += k;
    }
    b->first = p.first; b->last = p.last; b->on = 1;
    atomic_fetch_add_explicit(&g_oq_prefetched, p.last - p.first, memory_order_relaxed);
}
// The list's queries given back once, after its last report (g_oq_mu held): the value the last report stored before
static void oq_batch_release(const oq_batch *b) {
    if (!b->on) return;
    oq_gb_note(b->first, b->last);
    if (OQ.host) {
        for (uint64_t q = b->first; q < b->last;) { uint32_t k = oqb_chunk(q, b->last, OQ_N, OQ_N); OQ.hreset(V.dev, OQ.pool, (uint32_t)(q % OQ_N), k); atomic_fetch_add_explicit(&g_oq_reset_calls, 1, memory_order_relaxed); q += k; }
        atomic_store_explicit(&g_oq_ready, b->last + OQ_N, memory_order_release);
    } else atomic_store_explicit(&g_oq_freed, b->last, memory_order_release);
}
static void oq_done(const pop *o, uint64_t ticks, const oq_batch *b) {   // the completion thread, after the fence of the submission that recorded o (post_run holds g_oq_mu)
    if (o->kind == 6) { const oqk_ent *e = &g_oqk[o->ent - 1]; g_oq_acc = atomic_load_explicit(&e->key, memory_order_relaxed) == oqk_key(o->k1, o->k2) && atomic_load(&e->valid) ? atomic_load(&e->val) : 1; uint64_t rec[2] = {g_oq_acc, ticks}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec); return; }
    if (o->kind == 4 || o->kind == 7) {
        uint64_t sum = o->v, r[2 * OQ_RB];   // (the draws that count 1)
        if (b->on && o->q0 >= b->first && o->q1 <= b->last && o->q1 > o->q0) {   // read with the list (oq_batch_init)
            uint64_t un = 0; sum += oqb_sum(&g_oqb_pairs[2 * (size_t)(o->q0 - b->first)], (size_t)(o->q1 - o->q0), &un);
            if (un) atomic_fetch_add_explicit(&g_oqs_unav, un, memory_order_relaxed);
        } else for (uint64_t q = o->q0; q < o->q1;) {
            uint32_t s = (uint32_t)(q % OQ_N), k = (uint32_t)(o->q1 - q < OQ_N - s ? o->q1 - q : OQ_N - s); if (k > OQ_RB) k = OQ_RB;
            memset(r, 0, 16 * (size_t)k);
            VkResult e = OQ.get(V.dev, OQ.pool, s, k, 16 * (size_t)k, r, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);   // (no wait: VK_NOT_READY leaves the unavailable ones 0)
            atomic_fetch_add_explicit(&g_oq_get_calls, 1, memory_order_relaxed);
            for (uint32_t i = 0; i < k; i++) if (e < 0 || !r[2 * i + 1]) { sum++; atomic_fetch_add_explicit(&g_oqs_unav, 1, memory_order_relaxed); } else sum += r[2 * i];
            q += k;
        }
        if (o->mode) g_oq_acc = 0;
        g_oq_acc += sum;
        if (!g_oq_acc) atomic_fetch_add_explicit(&g_oqs_zero, 1, memory_order_relaxed);
        uint64_t rec[2] = {g_oq_acc, ticks};
        {   // DM_VK_OQ_TRACE=<n> (diagnostic): the first DM_VK_OQ_TRACE_N (48) reports of 4 frames in a row, every n frames with counted reports, as counted (place, address, queries, count)
            static int tf = -2; if (tf == -2) tf = VKA_ENV("DM_VK_OQ_TRACE", -1);
            static uint32_t lastfr = ~0u, seen; if (o->fr != lastfr) { lastfr = o->fr; seen++; }   // (frames with counted reports, in completion order)
            static int tn = -2; if (tn == -2) tn = VKA_ENV("DM_VK_OQ_TRACE_N", 48);
            if (tf > 0 && seen % (uint32_t)tf < 4 && o->ord < (uint32_t)tn) VLOG("oq trace frame %u place %u addr %llx fresh %u queries %llu count %llu k1 %016llx k2 %016llx", o->fr, o->ord, (unsigned long long)o->addr, o->mode, (unsigned long long)(o->q1 - o->q0), (unsigned long long)g_oq_acc, (unsigned long long)o->k1, (unsigned long long)o->k2);
        }
        if (o->kind == 7) {   // DM_VK_OQ_REUSE_CHECK: the repeated count written, compared with the real one
            const oqk_ent *e = o->ent ? &g_oqk[o->ent - 1] : NULL;
            uint64_t old = e && atomic_load_explicit(&e->key, memory_order_relaxed) == oqk_key(o->k1, o->k2) && atomic_load(&e->valid) ? atomic_load(&e->val) : 1; rec[0] = old;
            atomic_fetch_add_explicit(&g_oqs_rchk, 1, memory_order_relaxed);
            if (!old && g_oq_acc) atomic_fetch_add_explicit(&g_oqs_rlate, 1, memory_order_relaxed);
            if (old && !g_oq_acc) atomic_fetch_add_explicit(&g_oqs_rextra, 1, memory_order_relaxed);
        } else if (o->ent) {   // a counted object: its count for the frames that repeat it (in its entry, if a new key has not taken it meanwhile)
            oqk_ent *e = &g_oqk[o->ent - 1];
            if (atomic_load_explicit(&e->key, memory_order_acquire) == oqk_key(o->k1, o->k2)) { atomic_store(&e->val, g_oq_acc); atomic_store_explicit(&e->valid, 1, memory_order_release); }
        }
        memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
    }
    if (!b->on && o->q1 > o->q0) {   // given back, in order (with the list: oq_batch_release, after the last report)
        oq_gb_note(o->q0, o->q1);
        if (OQ.host) {
            for (uint64_t q = o->q0; q < o->q1;) { uint32_t s = (uint32_t)(q % OQ_N), k = (uint32_t)(o->q1 - q < OQ_N - s ? o->q1 - q : OQ_N - s); OQ.hreset(V.dev, OQ.pool, s, k); atomic_fetch_add_explicit(&g_oq_reset_calls, 1, memory_order_relaxed); q += k; }
            atomic_store_explicit(&g_oq_ready, o->q1 + OQ_N, memory_order_release);
        } else atomic_store_explicit(&g_oq_freed, o->q1, memory_order_release);
    }
}
static void oq_stats(char *b, size_t n, double frames) {   // every 300 frames with the replay statistics (no line while the game resets and reports none)
    static uint64_t r0, p0, nr0, q0, f0, s0, u0, z0;
    uint64_t un = atomic_load(&g_oqs_unav), ze = atomic_load(&g_oqs_zero);
    b[0] = 0;
    if (frames > 0 && (g_oqs_reset != r0 || g_oqs_rep != p0)) {
        if (g_oq != 1) snprintf(b, n, "occlusion queries per frame (not counted: %s): %.1f samples-passed resets, %.1f reports, each written 1", g_oq < 0 ? "no reset or report reached the GPU path" : "DM_VK_OCCLUSION=0 or no query pool",
                                (g_oqs_reset - r0) / frames, (g_oqs_rep - p0) / frames);
        else snprintf(b, n, "occlusion queries per frame (counted, %s, %s): %.1f samples-passed resets, %.1f reports (%.1f before any reset, written 1; %.1f counted read 0 samples), %.1f queries "
                      "begun, %.1f draws counted 1 without a query (%.1f with the ring full, %.1f skipped by the replay), %.1f results not available after their fence (counted 1); queries in flight up to %llu of %d",
                      OQ.precise ? "precise" : "not precise", OQ.host ? "host reset" : "command reset", (g_oqs_reset - r0) / frames, (g_oqs_rep - p0) / frames, (g_oqs_nores - nr0) / frames, (ze - z0) / frames,
                      (g_oqs_q - q0) / frames, (g_oqs_full - f0 + g_oqs_skip - s0) / frames, (g_oqs_full - f0) / frames, (g_oqs_skip - s0) / frames, (un - u0) / frames, (unsigned long long)g_oqs_fly, OQ_N);
    }
    { static uint64_t ps0, d0, ru0, rn0, rd0, rh0, ck0, lt0, ex0; size_t o = strlen(b);
      if (g_oq == 1 && frames > 0 && o + 120 < n) snprintf(b + o, n - o, "; %.1f passes held queries (at most %llu in one), %.1f draws inside queries, %.1f reports given their object's last count (DM_VK_OQ_REUSE; %.1f objects counted as new, %.1f as doubled, %.1f as hidden)", (g_oqs_passes - ps0) / frames, (unsigned long long)g_oqs_pmax, (g_oqs_din - d0) / frames, (g_oqs_reused - ru0) / frames, (g_oqs_rnew - rn0) / frames, (g_oqs_rdup - rd0) / frames, (g_oqs_rhid - rh0) / frames);
      ps0 = g_oqs_passes; d0 = g_oqs_din; ru0 = g_oqs_reused; rn0 = g_oqs_rnew; rd0 = g_oqs_rdup; rh0 = g_oqs_rhid; g_oqs_pmax = 0;
      uint64_t ck = atomic_load(&g_oqs_rchk), lt = atomic_load(&g_oqs_rlate), ex = atomic_load(&g_oqs_rextra);
      if (ck > ck0) { o = strlen(b); if (o + 140 < n) snprintf(b + o, n - o, " (checked %.1f: %.1f hidden while visible, %.1f visible while hidden)", (ck - ck0) / frames, (lt - lt0) / frames, (ex - ex0) / frames); }
      ck0 = ck; lt0 = lt; ex0 = ex; }
    r0 = g_oqs_reset; p0 = g_oqs_rep; nr0 = g_oqs_nores; q0 = g_oqs_q; f0 = g_oqs_full; s0 = g_oqs_skip; u0 = un; z0 = ze; g_oqs_fly = 0;
    {   // by list or by report (oq_batch_init): the driver calls of the completion thread
        static uint64_t gc0, rs0, pf0, oo0;
        uint64_t gc = atomic_load(&g_oq_get_calls), rs = atomic_load(&g_oq_reset_calls), pf = atomic_load(&g_oq_prefetched), oo = atomic_load(&g_oq_ooo);
        if (frames > 0 && (gc != gc0 || rs != rs0 || oo != oo0))
            VLOG("occlusion report batching per frame (%s): %.1f queries read by list, %.1f result reads, %.1f host resets, %.1f give-backs out of order", g_oq_batch_on ? "by list" : "DM_VK_OQ_BATCH=0: by report",
                 (pf - pf0) / frames, (gc - gc0) / frames, (rs - rs0) / frames, (oo - oo0) / frames);
        gc0 = gc; rs0 = rs; pf0 = pf; oo0 = oo;
    }
}
uint64_t guest_ns(void);
// Counter timestamps by segment (profile gpu_timestamps segment; DM_NVN_GPU_TS=segment / submission overrides it), as nvn_mtl.m's: a timestamp report also writes a GPU
// timestamp where it is recorded (TOP_OF_PIPE for TIMESTAMP_TOP, else BOTTOM_OF_PIPE), and once the submission is done each report gets the completion time less the GPU time
// from it to the submission's last report, so two reports of one submission bracket the GPU work between them (a game's dynamic resolution and frame pacing read that).
// Without it every report of a submission reads the same time (the completion)
enum { SEGQ = 128 };   // queries per submission slot
static VkQueryPool g_sqp; static int g_sq_n, g_sq_base, g_seg = -1;
static PFN_vkGetQueryPoolResults g_sq_get; static double g_sq_ns;
// Segment timestamps are read when a submission's effects run (post_run), which for a report carried to a later signal (carry_take) can be after its slot has been reused
// and its queries reset: a frame cut into more submissions than NSLOT slots (the GPU waits across queues park and cut it, vka_worker.c DM_VK_EVENT_WAIT). A slot keeps the
// results of its last SHN uses (sh_save, at the reuse, once its work is done); a report knows its slot's use (pop.q0). From upstream Banana Native v0.3.14 (849d2fb3), where
// Ocarina of Time's dynamic resolution read 0 ms of GPU time without it. g_sh_use[slot][j] is 0 while g_sh[slot][j] is rewritten (a seqlock: sh_saved checks it again after
// reading); g_slot_use and g_slot_nq are the replay thread's, g_slot_use also read by the completion thread
enum { SHN = 4 };
static _Atomic uint64_t g_sh[NSLOT][SHN][SEGQ]; static _Atomic uint32_t g_sh_use[NSLOT][SHN], g_slot_use[NSLOT]; static int g_slot_nq[NSLOT];
static int seg_ts(void) {
    if (g_seg < 0) { const char *e = getenv("DM_NVN_GPU_TS"); g_seg = e && *e ? !strcmp(e, "segment") : DM_GAME_GPU_TS_SEGMENT;
                     VLOG("counter timestamps: %s (profile key gpu_timestamps; DM_NVN_GPU_TS=segment / submission)", g_seg ? "GPU time of their place in the submission" : "one time for the whole submission"); }
    return g_seg;
}
static int sh_saved(unsigned slot, unsigned idx, uint32_t use, uint64_t *t) {   // 1: the slot's use `use` was saved, *t its query idx
    for (int j = 0; j < SHN; j++) if (atomic_load_explicit(&g_sh_use[slot][j], memory_order_acquire) == use) {
        uint64_t v = atomic_load_explicit(&g_sh[slot][j][idx], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&g_sh_use[slot][j], memory_order_relaxed) == use) { *t = v; return 1; }
    }
    return 0;
}
// A report's GPU time (qi: its query, use: its slot's use; 0 = none): the slot's saved results of that use, else its query while the slot has not been reused. A slot
// reused between the check and the read (its reset may have run on the GPU) was saved first (sh_save comes before g_slot_use moves), so the saved results are looked at again
static uint64_t sh_get(uint32_t qi, uint32_t use) {
    unsigned slot = qi / SEGQ, idx = qi % SEGQ; uint64_t t = 0;
    if (slot >= NSLOT) return 0;
    if (use && sh_saved(slot, idx, use, &t)) return t;
    if (!use || atomic_load(&g_slot_use[slot]) == use) {
        int r = g_sq_get(V.dev, g_sqp, qi, 1, 8, &t, 8, VK_QUERY_RESULT_64_BIT);
        uint64_t s; if (use && sh_saved(slot, idx, use, &s)) return s;   // (reused meanwhile)
        return r ? 0 : t;
    }
    return 0;
}
static void post_run(void *ops_, int n) {   // the completion thread, in submission order, before the fence reads as done
    pop *ops = ops_;
    uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
    uint64_t ticks = nvn_timestamp_from_ns(g);
    uint64_t gq[SEGQ], tmax = 0; int nq = 0;   // segment timestamps: each report's GPU time, the latest of them
    for (int i = 0; i < n && g_sqp; i++) if (ops[i].kind == 2 && ops[i].v) {
        uint64_t t = sh_get(ops[i].v - 1, (uint32_t)ops[i].q0);
        if (!t) { ops[i].v = 0; continue; }
        if (nq < SEGQ) { gq[nq++] = t; if (t > tmax) tmax = t; } else ops[i].v = 0;
    }
    if (atomic_load_explicit(&g_synclog, memory_order_relaxed) > 0) for (int i = 0; i < n; i++) if (ops[i].kind == 2 && ops[i].type == NVN_COUNTER_TYPE_TIMESTAMP) {
        VLOG("sync done: %d reports written at %.2f ms (the submission's GPU work done)", n, ms_now()); break; }
    // DM_VK_TS_LOG=<submissions> (from upstream v0.3.14): what the game reads from its timestamp reports, for that many submissions with reports: the completion time (ms on
    // the guest clock, mod 100 s) and each report's type, address and time relative to the completion (negative: earlier; "completion": no segment time), as written below
    static _Atomic int tslog = -1; int tsl = atomic_load(&tslog); if (tsl < 0) { tsl = VKA_ENV("DM_VK_TS_LOG", 0); atomic_store(&tslog, tsl); }
    if (tsl > 0) { char b[700], lb[160]; int k = 0, nt = 0, shown = 0; uint64_t first = 0, last = 0; lb[0] = 0;
        for (int i = 0, q = 0; i < n; i++) if (ops[i].kind == 2) {
            uint64_t tk = ticks; double gms = -1;
            if (ops[i].v && g_sqp && q < nq) { gms = (double)(tmax - gq[q]) * g_sq_ns / 1e6; uint64_t back = (uint64_t)((double)(tmax - gq[q]) * g_sq_ns); tk = nvn_timestamp_from_ns(g > back ? g - back : 0); }
            q += ops[i].v ? 1 : 0;
            if (ops[i].type != NVN_COUNTER_TYPE_TIMESTAMP && ops[i].type != NVN_COUNTER_TYPE_TIMESTAMP_TOP) continue;
            uint64_t ns = nvn_timestamp_to_ns(tk); if (!nt) first = ns; last = ns; nt++;
            char one[160]; snprintf(one, sizeof one, " [%s %#llx %+.2f%s slot %d use %u gpu-%.2f]", ops[i].type == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? "top" : "bot", (unsigned long long)ops[i].addr,
                                    ((double)ns - (double)nvn_timestamp_to_ns(ticks)) / 1e6, ops[i].v ? "" : " completion", ops[i].v ? (int)((ops[i].v - 1) / SEGQ) : -1, (unsigned)ops[i].q0, gms);
            if (shown < 4) { shown++; k += snprintf(b + k, sizeof b - (size_t)k, "%s", one); } snprintf(lb, sizeof lb, "%s", one); }
        if (nt && atomic_fetch_sub(&tslog, 1) > 0) VLOG("ts log: submission completes at %.2f ms, %d timestamp reports spanning %.2f ms (%s):%s ... last%s", fmod((double)g / 1e6, 100000.0), nt,
                                                        (double)(last - first) / 1e6, g_sqp ? "segment" : "submission", b, lb); }
    // Occlusion queries: g_oq_mu over the whole list (its running count in one piece against a vka_fence_cpu post_run on another thread; g_oqb_pairs). No other path
    // takes it, and nothing the loop calls takes a lock that leads back to it. It nests one lock under it, also with DM_VK_OQ_BATCH=0 (where it used to be held inside each
    // oq_done only): kind 3's vka_floor_done in floor mode 2 takes V.qmu (floor_set_), and post_run never runs with V.qmu held, so the order is always g_oq_mu -> V.qmu
    int has_oq = 0; for (int i = 0; i < n && !has_oq; i++) has_oq = ops[i].kind >= 4 && ops[i].kind <= 7;
    oq_batch oqb = {0};
    if (has_oq) { pthread_mutex_lock(&g_oq_mu); oq_batch_init(&oqb, ops, n); }
    for (int i = 0, q = 0; i < n; i++) {
        if (ops[i].kind == 3) {   // the frame's span to here (the adaptive GPU clock floor)
            uint64_t f = ops[i].addr; unsigned k = f % 64;
            vka_floor_done(ops[i].v);
            if (atomic_load(&g_fstart_f[k]) == f) { uint64_t t0 = atomic_load(&g_fstart[k]), t1 = now_ns(); if (t1 > t0) vka_floor_span(f, t1 - t0); }
            continue;
        }
        if (ops[i].kind == 2 && ops[i].v) {   // a segment timestamp: the completion time less the GPU time from this report to the last
            uint64_t back = (uint64_t)((double)(tmax - gq[q++]) * g_sq_ns); uint64_t tk = nvn_timestamp_from_ns(g > back ? g - back : 0), tt = ops[i].type;
            uint64_t rec[2] = {DM_GAME_RENDER_COND ? 0 : tk, tk}; (void)tt;
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec); continue;
        }
        if (ops[i].kind >= 4 && ops[i].kind <= 7) { oq_done(&ops[i], ticks, &oqb); continue; }   // a counted samples-passed report, queries given back (Occlusion queries)
        if (ops[i].kind == 1) { volatile uint32_t *v = (volatile uint32_t *)(uintptr_t)ops[i].addr; if (ops[i].mode == NVN_EVENT_SIGNAL_MODE_ADD) *v += ops[i].v; else *v = ops[i].v; }
        else {
            // A timestamp report's counter: the time, or 0 under render_cond (as on the GPU, nvn_mtl.m rc_value). Samples passed not counted (oq_report: DM_VK_OCCLUSION=0, no
            // query pool, no reset before it): 1, so occlusion culling keeps everything visible
            uint64_t t = ops[i].type, rec[2] = {t == NVN_COUNTER_TYPE_TIMESTAMP || t == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? (DM_GAME_RENDER_COND ? 0 : ticks) : t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1 : 0, ticks};
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec);
        }
    }
    if (has_oq) { oq_batch_release(&oqb); pthread_mutex_unlock(&g_oq_mu); }   // (after every report of the list read its part)
    free(ops);
}

// ---------- GPU time ----------
// A timestamp at the top and the bottom of each part of a submission (chunk: a submission goes to the queue in up to NCHUNK + 1 parts, two queries per part in the slot's
// range); once the slot's fence is done the parts' busy times go to the frame the submission was recorded in, which hle_nvn.c reads 8 frames later (mtl_gpu_ms: the
// [longframe] lines, the frame statistics). The queue runs one part after another, so the sum is the frame's GPU busy time. Busy is counted per part: with one pair for the
// whole submission (before 2026-11-03) the GPU's idle time between parts, while the replay thread was still recording the next one (a pipeline compiled, a texture
// deswizzled, the encoder held up), counted as GPU time, and frames stalled on the CPU looked GPU-bound in the [longframe] lines (200-980 ms "GPU" spikes with no new
// pipeline). The span (first part's top to the last part's bottom) is kept beside it: span minus busy is the time the GPU waited for the recording (vka_gpu_span_ms).
// Without timestamps on the queue the frame reads -1 (no GPU time) instead of 0
static struct { PFN_vkCreateQueryPool create; PFN_vkCmdResetQueryPool reset; PFN_vkCmdWriteTimestamp write; PFN_vkGetQueryPoolResults get; } qf;
static VkQueryPool g_qp; static int g_ts_ok = -1; static double g_ts_ns;   // nanoseconds per tick
enum { GRING = 64 };
static struct { _Atomic uint64_t frame, ns, t0, t1, span; _Atomic int pending; } g_gpu[GRING];
static uint64_t g_slot_frame[8]; static uint8_t g_slot_ts[8]; static uint32_t g_slot_parts[8];
enum { QPS = 2 * (NCHUNK + 1) };   // queries a slot: a top and a bottom per part
static int ts_init(void) {
    if (g_ts_ok >= 0) return g_ts_ok;
    g_ts_ok = 0;
    uint32_t nq = 0; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vk.vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &nq, qp);
    if (V.qf >= nq || !qp[V.qf].timestampValidBits || V.props.limits.timestampPeriod <= 0) { VRED("GPU time: the queue has no timestamps"); return 0; }
    qf.create = (PFN_vkCreateQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCreateQueryPool"); qf.reset = (PFN_vkCmdResetQueryPool)vk.vkGetDeviceProcAddr(V.dev, "vkCmdResetQueryPool");
    qf.write = (PFN_vkCmdWriteTimestamp)vk.vkGetDeviceProcAddr(V.dev, "vkCmdWriteTimestamp"); qf.get = (PFN_vkGetQueryPoolResults)vk.vkGetDeviceProcAddr(V.dev, "vkGetQueryPoolResults");
    VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NSLOT * QPS};
    if (!qf.create || !qf.reset || !qf.write || !qf.get || qf.create(V.dev, &ci, NULL, &g_qp)) { VRED("GPU time: no timestamp query pool"); return 0; }
    g_ts_ns = V.props.limits.timestampPeriod; g_ts_ok = 1;
    VLOG("GPU time: timestamps at %.2f ns a tick", g_ts_ns);
    return 1;
}
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); atomic_store(&g_gpu[k].span, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_slot_done(unsigned slot) {   // the slot's fence is done: its two timestamps are there
    if (!g_slot_ts[slot]) return;
    g_slot_ts[slot] = 0;
    uint32_t np = g_slot_parts[slot] ? g_slot_parts[slot] : 1; if (np > NCHUNK + 1) np = NCHUNK + 1;
    uint64_t f = g_slot_frame[slot], t[QPS]; unsigned k = f % GRING;
    int ok = !qf.get(V.dev, g_qp, slot * QPS, 2 * np, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT);
    if (atomic_load(&g_gpu[k].frame) != f) return;
    if (ok && t[2 * np - 1] > t[0]) {
        // Busy: each part from its top (or the previous part's bottom, when the part's top was written while the previous part still ran) to its bottom
        uint64_t busy = 0, prev = 0;
        for (uint32_t i = 0; i < np; i++) { uint64_t a = t[2 * i] > prev ? t[2 * i] : prev, b = t[2 * i + 1]; if (b > a) busy += b - a; if (b > prev) prev = b; }
        uint64_t s0 = (uint64_t)(t[0] * g_ts_ns), s1 = (uint64_t)(t[2 * np - 1] * g_ts_ns);
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(busy * g_ts_ns)); atomic_fetch_add(&g_gpu[k].span, s1 - s0);
        if (s0 < atomic_load(&g_gpu[k].t0)) atomic_store(&g_gpu[k].t0, s0);
        if (s1 > atomic_load(&g_gpu[k].t1)) atomic_store(&g_gpu[k].t1, s1);
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
// The submissions' spans of frame f (first part's start to last part's end, summed over its submissions): above mtl_gpu_ms by the time the GPU waited for the recording; -1 = not in yet
double vka_gpu_span_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].span) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; if (t0) *t0 = 0; if (t1) *t1 = 0; return 0; }   // GPU ticks are not on the host clock (no calibrated timestamps yet)

void vka_perf_thread(int nice);   // vka_perf.c
// What each recent submission held, by fence, for the [RED] line of a GPU hang (vka_dev.c hang_declare)
enum { NHREC = 16 };
static struct { uint64_t fence, frame, queue, draws, ops, skipped, t_ns; } g_hrec[NHREC];
static uint64_t g_sub_draws0, g_sub_ops0, g_sub_skip0, g_sub_t0;
void vka_exec_hang_report(uint64_t fence) {
    for (int i = 0; i < NHREC; i++) if (g_hrec[i].fence == fence || g_hrec[i].fence == fence + 1) {
        VRED("GPU hang: submission %llu (frame %llu, queue serial %llu): %llu draws, %llu clears / copies, %llu skipped, recorded in %.2f ms", (unsigned long long)g_hrec[i].fence,
             (unsigned long long)g_hrec[i].frame, (unsigned long long)g_hrec[i].queue, (unsigned long long)g_hrec[i].draws, (unsigned long long)g_hrec[i].ops,
             (unsigned long long)g_hrec[i].skipped, g_hrec[i].t_ns / 1e6); }
}

// ---------- Inline uniform updates (OP_UPDATE_UB) ----------
// The Vulkan form of nvn_mtl.m's ubd_* (DM_NVN_UB_DEFER; docs/reference/rendering.md, "Inline uniform updates on the Vulkan replay"), after Erick's Ocarina fork (0220a33 to
// 36c9eba), for every game. Each update used to be one vkCmdCopyBuffer per placed run and a full barrier, and one inside a render pass ended the pass. Three steps:
//   - the barrier after the copies is TRANSFER_WRITE → ALL_COMMANDS (vka_transfer_write_barrier, DM_VK_UB_SCOPED);
//   - updates in a row (with only state records between, ub_state_only) go into one vkCmdCopyBuffer of disjoint regions and one barrier (DM_VK_UB_BATCH, live, default 1):
//     an overlapping destination, another buffer pair or a full batch flushes first (the last write wins), and every record that may record GPU work flushes before it;
//   - an update inside an open pass is kept (DM_VK_UB_DEFER, live, default 0 until the phone A/B): its bytes copied into scratch now (the record's memory may be reused),
//     its copy recorded at the pass end after the pass's barrier (vka_ub_flush_pass from vka_rp_end), and the pass's later draws that bind an overlapping uniform range
//     read a shadow of the new bytes (vka_draw.c ubo_desc); a draw that reads the range any other way ends the pass first (vka_draw.c ub_hazard).
// The batch is non-empty only while no pass is open (a kept update stays in g_ub_pending; one that is not ends the pass first), and the kept ones only while one is
enum { UB_BATCH_MAX = 64, UB_DEFER_MAX = 128, UB_DEFER_BYTES = 65536 };
_Static_assert(sizeof(ubq_region) == sizeof(VkBufferCopy) && offsetof(ubq_region, so) == offsetof(VkBufferCopy, srcOffset) && offsetof(ubq_region, dof) == offsetof(VkBufferCopy, dstOffset)
               && offsetof(ubq_region, size) == offsetof(VkBufferCopy, size), "ubq_region is VkBufferCopy's layout");
typedef struct { VkBuffer src, dst; ubq_region r[UB_BATCH_MAX]; unsigned n; } ub_batch;   // (the encoder copies the regions: the array is reused at once)
static ub_batch g_inline_ub;
typedef struct { uint64_t addr, size; vka_span s; } ub_pass_write;   // s: the bytes, in scratch until the submission's fence: the shadows' source and the copy's
static ub_pass_write g_ub_pending[UB_DEFER_MAX];
static ubq_write g_ub_w[UB_DEFER_MAX];   // the same list as vka_ubq.h reads it (addr, size, the scratch bytes)
static int g_ub_np;
static uint64_t g_ub_lo, g_ub_hi, g_ub_seq = 1;   // the kept writes' bounding range (the common miss in one compare); seq: changes with every deferral and flush
static uint64_t g_ub_copy_cmds, g_ub_copy_regions, g_ub_deferred, g_ub_shadows, g_ub_cpu_reads, g_ub_flushed, g_ub_hazard_ends, g_ub_bound_ends;
enum { UBR_FULL = VKA_UBR_N, UBR_SIZE, UBR_SCRATCH, UBR_ALL };
static uint64_t g_ub_refused[UBR_ALL];   // updates in an open pass not kept, by reason (VKA_UBR_*, then the list full, too big, no scratch)
static void ub_flush(VkCommandBuffer cb) {
    ub_batch *b = &g_inline_ub;
    if (!b->n || !cb) { b->n = 0; return; }
    vk.vkCmdCopyBuffer(cb, b->src, b->dst, b->n, (const VkBufferCopy *)b->r);
    vka_transfer_write_barrier(cb);
    g_ub_copy_cmds++; b->n = 0;
}
static void ub_add(VkCommandBuffer cb, VkBuffer sb, VkDeviceSize so, VkBuffer db, VkDeviceSize dof, VkDeviceSize k) {
    ub_batch *b = &g_inline_ub;   // (src is scratch, dst guest RAM: one copy's regions never alias)
    if (ubq_must_flush(b->r, b->n, UB_BATCH_MAX, b->src == sb && b->dst == db, dof, k)) ub_flush(cb);
    if (!b->n) { b->src = sb; b->dst = db; }
    b->r[b->n++] = (ubq_region){so, dof, k}; g_ub_copy_regions++;
}
static void ub_copy(VkCommandBuffer cb, const vka_span *s, uint64_t dst, uint64_t n) {   // scratch bytes into guest RAM, run by placed run, staged in the batch
    for (uint64_t done = 0; done < n;) {
        VkBuffer b; VkDeviceSize o; uint64_t k = vka_addr_run(dst + done, n - done, &b, &o);
        if (!k) { VRED_ONCE("inline buffer update at %#llx is not in placed guest RAM", (unsigned long long)(dst + done)); return; }
        ub_add(cb, s->buf, s->off + done, b, o, k); done += k;
    }
}
// The state records: they change only the replay's CPU-side state and record nothing into the command buffer (the second switch of exec does nothing for them; SET_RT may
// end a pass, which no batch is staged across). A batch stays staged across them (Erick's b938294); anything else flushes it first
static int ub_state_only(uint32_t op) {
    switch (op) {
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
    case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_TF_BIND: case OP_TF_STATE:
    case OP_PRIM_RESTART: case OP_RAST_DISCARD: case OP_SCISSOR: case OP_COPY_STRIDE: case OP_SET_RT: return 1;
    default: return 0;
    }
}
// The list's own flow records (exec's third switch): they move the read position in the same command buffer. END at depth 0 returns to the submission's code, which may
// record (the commit, a part's end) or replay the next list after other work: the batch goes out there
static int ub_list_flow(uint32_t op, int depth) { return op == OP_BEGIN || op == OP_JUMP || op == OP_CALL || (op == OP_END && depth > 0); }
int vka_ub_active(void) { return g_ub_np != 0; }
uint64_t vka_ub_seq(void) { return g_ub_seq; }
void vka_ub_hazard_end(void) { g_ub_hazard_ends++; }
int vka_ub_pending_overlap(uint64_t addr, uint64_t size) { return ubq_any(g_ub_w, g_ub_np, g_ub_lo, g_ub_hi, addr, size); }
void vka_ub_overlay(uint64_t addr, void *out, uint64_t size) { g_ub_shadows++; ubq_overlay(g_ub_w, g_ub_np, addr, out, size); }   // in list order: the last write wins
// A CPU read of guest memory for this draw (vka_draw.c cpu_read: the descriptor walk's handles in a uniform buffer, a uniform copy) with the kept updates over it, so the
// walk and the shader's shadow see the same bytes; DM_VK_UB_CPU_OVERLAY=0 (live): the CPU's bytes alone, as before (the walk then resolved texture handles from the bytes
// before the update while the shader read the constants after it)
void vka_ub_cpu_overlay(uint64_t addr, void *out, uint64_t size) {
    if (!g_ub_np || !ubq_any(g_ub_w, g_ub_np, g_ub_lo, g_ub_hi, addr, size) || !VKA_ENV("DM_VK_UB_CPU_OVERLAY", 1)) return;
    g_ub_cpu_reads++; ubq_overlay(g_ub_w, g_ub_np, addr, out, size);
}
// The kept updates' copies, at the pass end (vka_rp_end: after vkCmdEndRendering and after its barrier, full or attachment-scoped, either of which orders the pass's reads
// of these ranges in place before the copies overwrite them: the scoped one's source stages take in every stage of the pass, vka_draw.c "Pass-end barriers"; the copies publish themselves through the batch's transfer barrier, so no second full barrier follows: Erick's 8538623, here by Vulkan's
// dependency rules for every game). Their GPU writes join the write list here, at their GPU place, before the commit stamps its fence (vka_tex_gw_commit)
void vka_ub_flush_pass(VkCommandBuffer cb) {
    if (!g_ub_np) return;
    if (cb != g_cb) VRED_ONCE("inline uniform updates kept in a pass flushed into another command buffer than the replay's (the pass's own is used)");
    if (g_inline_ub.n) { VRED_ONCE("inline uniform batch staged while a pass was open: flushed at the pass end"); ub_flush(cb); }
    for (int i = 0; i < g_ub_np; i++) { const ub_pass_write *w = &g_ub_pending[i]; vka_tex_gw_update(w->addr, w->size, w->s.ptr); ub_copy(cb, &w->s, w->addr, w->size); }
    ub_flush(cb);
    g_ub_flushed += (uint64_t)g_ub_np; g_ub_np = 0; g_ub_seq++; g_ub_lo = g_ub_hi = 0;
}
static int ub_defer_try(uint64_t d, uint64_t n, const void *data) {   // 1 = kept for the pass end
    vka_span s; if (!vka_scratch(n, 16, &s)) { g_ub_refused[UBR_SCRATCH]++; return 0; }
    memcpy(s.ptr, data, (size_t)n);
    ubq_bound(&g_ub_lo, &g_ub_hi, !g_ub_np, d, n);
    g_ub_w[g_ub_np] = (ubq_write){d, n, s.ptr};
    g_ub_pending[g_ub_np++] = (ub_pass_write){d, n, s};
    g_ub_deferred++; g_ub_seq++;
    return 1;
}
static void ub_stats(char *b, size_t n, double f) {   // the replay statistics' line (empty without updates)
    static uint64_t r0, c0, d0, s0, cr0, fl0, h0, bd0, rf0[UBR_ALL];
    uint64_t r = g_ub_copy_regions - r0, c = g_ub_copy_cmds - c0;
    char rf[160]; int k = 0; static const char *nm[UBR_ALL] = {"", "no pass", "pass wrote memory", "game barrier in the pass", "over a target", "clears waiting", "list full", "over 64 KB", "no scratch"};
    rf[0] = 0;
    for (int i = 1; i < UBR_ALL; i++) { uint64_t v = g_ub_refused[i] - rf0[i]; rf0[i] = g_ub_refused[i]; if (v && k < (int)sizeof rf - 40) k += snprintf(rf + k, sizeof rf - (size_t)k, "%s%s %.1f", k ? ", " : "", nm[i], v / f); }
    if (!r && !c && g_ub_deferred == d0) { if (n) b[0] = 0; }
    else snprintf(b, n, "inline uniform updates per frame: %.1f staged regions in %.1f vkCmdCopyBuffer (DM_VK_UB_BATCH); kept in a pass %.1f, shadows %.1f, CPU reads over them %.1f, flushed %.1f, passes ended by a draw's hazard %.1f / by a boundary %.1f (DM_VK_UB_DEFER)%s%s",
                  r / f, c / f, (g_ub_deferred - d0) / f, (g_ub_shadows - s0) / f, (g_ub_cpu_reads - cr0) / f, (g_ub_flushed - fl0) / f, (g_ub_hazard_ends - h0) / f, (g_ub_bound_ends - bd0) / f, rf[0] ? "; not kept: " : "", rf);
    r0 = g_ub_copy_regions; c0 = g_ub_copy_cmds; d0 = g_ub_deferred; s0 = g_ub_shadows; cr0 = g_ub_cpu_reads; fl0 = g_ub_flushed; h0 = g_ub_hazard_ends; bd0 = g_ub_bound_ends;
}

// The slot's last use is done (vka_rp_sub_begin waited for it): its segment timestamps are kept before this use resets its queries ("Counter timestamps by segment")
static void sh_save(unsigned slot) {
    uint32_t use = atomic_load(&g_slot_use[slot]); int nq = g_slot_nq[slot];
    if (!g_sqp || !use || nq <= 0) return;
    int j = (int)(use % SHN);
    atomic_store(&g_sh_use[slot][j], 0); atomic_thread_fence(memory_order_release);   // (a reader of the old use sees 0 before any new value)
    for (int i = 0; i < nq && i < SEGQ; i++) {
        uint64_t t = 0; if (g_sq_get(V.dev, g_sqp, (uint32_t)(slot * SEGQ + (unsigned)i), 1, 8, &t, 8, VK_QUERY_RESULT_64_BIT)) t = 0;
        atomic_store_explicit(&g_sh[slot][j][i], t, memory_order_relaxed);
    }
    atomic_store_explicit(&g_sh_use[slot][j], use, memory_order_release); g_slot_nq[slot] = 0;
}
static uint64_t g_t_commit, g_t_cm_enc, g_t_cm_up, g_t_cm_sub;
void vka_rp_sub_begin(void) {
    vka_perf_thread(0); vka_ctl_sub();
    { uint64_t f = g_job_frame_set ? g_job_frame : g_frame; if (atomic_load(&g_fstart_f[f % 64]) != f) { atomic_store(&g_fstart[f % 64], now_ns()); atomic_store(&g_fstart_f[f % 64], f); } }
    g_npost = 0; g_cb = VK_NULL_HANDLE; rc_prune();
    // The last submission's commit flushed its inline updates (sub_commit: the pass end, then the batch): any left would be copies into a command buffer already submitted
    if (g_ub_np || g_inline_ub.n) { VRED_ONCE("inline uniform updates left over from the last submission (%d kept, %u staged): dropped", g_ub_np, g_inline_ub.n); g_ub_np = 0; g_inline_ub.n = 0; g_ub_seq++; }
    if (!vka_init()) return;
    vka_gpu_floor();
    if (vka_hung()) { VRED_ONCE("GPU hang: submissions from now on run their effects without the GPU"); return; }   // no command buffer: mtl_sub_commit takes the CPU path
    g_sub_draws0 = g_stat_draws; g_sub_ops0 = g_stat_ops; g_sub_skip0 = g_stat_skipped; g_sub_t0 = now_ns(); g_n_sub++;
    if (!g_pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf};
        VkCommandBuffer cbs[NSLOT];
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = NSLOT};
        if (vk.vkCreateCommandPool(V.dev, &pi, NULL, &g_pool) || ((ai.commandPool = g_pool), vk.vkAllocateCommandBuffers(V.dev, &ai, cbs))) { VRED("submission command buffers not made"); g_pool = VK_NULL_HANDLE; return; }
        for (int i = 0; i < NSLOT; i++) g_slot[i].cb = cbs[i];
    }
    unsigned k = g_slot_i++ % NSLOT;
    // A slot whose last commit went without a timeline signal (handed over while a lower fence was parked at a GPU wait, vka_qsched.h) waits for its own fence: its
    // timeline value comes only with a later commit, which this thread makes (waiting for it would wait forever), and is reached only once all work up to that later
    // commit is done (waiting for that tied the recording to the GPU: on the Honor Ocarina of Time's submission start took 20 ms a frame instead of 1.5, 2026-11-09)
    if (g_slot[k].vf_on) {
        if (vk.vkWaitForFences(V.dev, 1, &g_slot[k].vf, VK_TRUE, vka_hang_ns())) VRED_ONCE("a command buffer slot's fence was not reached in the hang time: reused anyway");
        vk.vkResetFences(V.dev, 1, &g_slot[k].vf); g_slot[k].vf_on = 0;
        if (g_slot_ts[k]) gpu_slot_done(k);   // its GPU times are there (its timeline fence may come later)
    } else if (g_slot[k].fence) vka_wait_done(g_slot[k].fence, UINT64_MAX);
    sh_save(k);   // the slot's segment timestamps, kept for reports whose effects run later (carried to a signal), before this use resets them
    uint64_t done = vka_done();
    for (unsigned i = 0; i < NSLOT; i++) if (g_slot_ts[i] && g_slot[i].fence <= done) gpu_slot_done(i);
    g_cb = g_slot[k].cb;
    vk.vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(g_cb, &bi);
    atomic_store(&vka_disp_pend, VK_NULL_HANDLE);   // (a barrier a skip or hang path left pending never matches a reused slot's command buffer; vka_draw.c "Dispatch order")
    vka_gbar_cb(g_cb);   // the game's barriers between dispatches (vka_gbar.c) follow this command buffer
    if (ts_init()) {
        qf.reset(g_cb, g_qp, k * QPS, QPS); qf.write(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qp, k * QPS);
        g_slot_frame[k] = g_frame; g_slot_ts[k] = 1; g_slot_parts[k] = 1; gpu_tag_begin(g_frame);
        g_sq_n = 0; g_sq_base = (int)k * SEGQ; g_slot_nq[k] = 0; if (atomic_fetch_add(&g_slot_use[k], 1) == UINT32_MAX) atomic_store(&g_slot_use[k], 1);   // (use 0 = none)
        if (seg_ts() && !g_sqp) {
            VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = NSLOT * SEGQ};
            if (qf.create(V.dev, &ci, NULL, &g_sqp)) { g_sqp = VK_NULL_HANDLE; g_seg = 0; VRED("segment timestamps: no query pool: one time for the whole submission"); }
            g_sq_get = qf.get; g_sq_ns = g_ts_ns;
        }
        if (g_sqp) qf.reset(g_cb, g_sqp, (uint32_t)g_sq_base, SEGQ);
    }
    oq_refill(g_cb);   // (direct, before the encoder starts: no pass is open and no query of this submission has begun)
    vka_gp_sub_begin(g_cb, g_frame);
    vka_enc_start(g_cb);
    vka_barrier(g_cb);   // the host's writes to guest memory before this submission (made available by the submit), and earlier submissions' writes
    vka_tex_prepare(g_cb);
    vka_barrier(g_cb);   // the re-uploads of CPU-written textures before the submission's draws read them
    vka_draw_sub_begin(g_cb, k);
    { void vka_pso_pause(int on); vka_pso_pause(0); }   // the game submits: resumed (mtl_drain paused the pipeline threads)
    g_chunk_draws = VKA_ENV("DM_VK_CHUNK", 600);   // live: from the next submission
    g_nx = 0; g_chunk_d0 = g_stat_draws;
}

// A long submission goes to the queue in parts, so the GPU starts on its first passes while the rest is recorded: before the first pass after DM_VK_CHUNK draws (default
// 600; 0 = one part) of the part in progress, the part is ended and submitted without a signal, and the submission goes on in the slot's next command buffer. The parts run
// in order on the one queue, and the barriers recorded in a part cover what later parts do (submission order); only the last part signals the submission's fence.
// On the Adreno 840 phone, Donkey Kong Bananza's level recorded ~45 ms a frame before its GPU work (~30 ms) began, and the game waited for that frame's fence (@sync_ms ~64)
typedef struct { VkCommandBuffer cb; uint32_t q; } part_arg;   // q: the part's bottom (part_submit) or top (part_begin) timestamp query, UINT32_MAX = none
static void part_submit(const void *a) {   // the part ends and goes to the queue (on the encoder thread when it is on)
    part_arg pa; memcpy(&pa, a, sizeof pa); VkCommandBuffer cb = pa.cb;
    if (pa.q != UINT32_MAX) qf.write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, pa.q);
    VkResult r = vk.vkEndCommandBuffer(cb);
    if (!r) r = vka_submit(cb, 0, 0, NULL, NULL, NULL, VK_NULL_HANDLE);
    if (r) { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 4) VRED("part of a submission: %d", r); }
}
static void part_begin(const void *a) {
    part_arg pa; memcpy(&pa, a, sizeof pa); VkCommandBuffer cb = pa.cb;
    vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    if (pa.q != UINT32_MAX) qf.write(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qp, pa.q);
}
static void chunk(void) {
    unsigned k = (g_slot_i - 1) % NSLOT;
    if (g_nx >= NCHUNK) return;
    vka_disp_settle(g_cb);   // a barrier a dispatch left pending is keyed by its command buffer: recorded in this part, where it covers the parts after it (vka_draw.c "Dispatch order")
    if (g_inline_ub.n) { VRED_ONCE("inline uniform batch still staged at a part's end: flushed there"); ub_flush(g_cb); }   // (exec flushes before every draw: "Inline uniform updates")
    if (!g_slot[k].x[g_nx]) {
        vka_enc_sync();   // the pool: not while the encoder thread records into one of its command buffers
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vk.vkAllocateCommandBuffers(V.dev, &ai, &g_slot[k].x[g_nx])) { g_slot[k].x[g_nx] = VK_NULL_HANDLE; g_nx = NCHUNK; return; }
    }
    VkCommandBuffer next = g_slot[k].x[g_nx++];
    // The part's bottom and the next part's top timestamps (the slot's queries were reset at the submission's start, in its first part)
    uint32_t part = g_slot_parts[k]; int ts = g_slot_ts[k] && part < NCHUNK + 1;
    part_arg pe = {g_cb, ts ? k * QPS + 2 * (part - 1) + 1 : UINT32_MAX}, pb = {next, ts ? k * QPS + 2 * part : UINT32_MAX};
    if (ts) g_slot_parts[k] = part + 1;
    vka_gbar_settle(g_cb);   // a game barrier left pending (vka_gbar.c), in this part: it covers the parts after it
    vka_up_flush(); vka_up_throttle();   // the upload batch goes first: the part may use what it made (vka_dev.c "Upload batches")
    // With the encoder the part's end, its submit and the next part's reset and begin are made on its thread, in order with the calls before: the replay thread goes on
    if (vka_enc_call(part_submit, &pe, sizeof pe)) { vka_enc_call(part_begin, &pb, sizeof pb); vka_enc_switch(next); }
    else { part_submit(&pe); part_begin(&pb); }
    g_cb = next; vka_gbar_cb(g_cb);
    vka_draw_cb_switch(g_cb); vka_gp_part(g_cb);
    g_chunk_d0 = g_stat_draws; g_n_chunks++;
}
// DM_VK_CHUNK_PASS=n (live; default 600, 0 = off): a pass that has had n draws in the part in progress is also ended there and the part submitted; the next draw opens it again over the
// same targets (loadOp LOAD, after the pass end's barrier), so its contents and the draws' order are kept. A pass of 1,700 draws whose GPU work (14-18 ms) only began after
// all of it was recorded then starts on the GPU while the rest is recorded. Donkey Kong Bananza's level (Adreno 840, DM_VK_GPUPROF): the extra splits cost no measurable GPU
// time (20.2-20.5 ms a frame with them, 20.8-21.0 without); with the GPU clock floor the GPU finished 6-10 ms after recording instead of 10-33
static inline void chunk_check(void) {
    if (!g_cb) return;
    uint64_t in = g_stat_draws - g_chunk_d0;
    if (g_chunk_draws > 0 && in >= (uint64_t)g_chunk_draws && vka_rp_split_ok(&S.rt, S.has_rt)) { vka_rp_end(); chunk(); return; }
    int cp = VKA_ENV("DM_VK_CHUNK_PASS", 600);
    if (cp > 0 && in >= (uint64_t)cp && vka_rp_open_for(&S.rt, S.has_rt)) { vka_rp_end(); chunk(); }
}
static uint64_t g_cur_fence;   // the fence of the submission being replayed, reserved before it (vka_worker.c); 0 = reserved at its commit
// Commits without a signal (vka_qsched.h qs_commit: a part cut at a wait, or a submission replayed while a lower fence is parked): their completion effects wait here, in
// the order they were recorded, and go with the first signal that covers their fence, so they land only after their GPU work
typedef struct { uint64_t fence; pop *ops; int n; } carry_t;
enum { NCARRY = 64 };
static carry_t g_carry[NCARRY]; static int g_ncarry;
static pop *carry_take(uint64_t fence, uint64_t signal, pop *ops, int *n) {
    if (!signal) {
        // A counter report's segment timestamp (kind 2, v: its query in the slot's range, q0: the slot's use) is read when the effects run, which can be after the slot is
        // reused (it waits for its own fence, not the timeline): sh_save keeps the slot's results at its reuse (upstream Banana Native v0.3.14). Before 2026-11-13 such a
        // report took the completion time, the same for every report carried to one signal, so a game reading the GPU time between two of them read 0 ms (Ocarina of
        // Time's dynamic resolution never lowered the scene upstream). DM_VK_TS_CARRY=0 (live): the completion time, as before
        if (!VKA_ENV("DM_VK_TS_CARRY", 1)) for (int i = 0; i < *n; i++) if (ops[i].kind == 2) ops[i].v = 0;
        if (g_ncarry < NCARRY) { g_carry[g_ncarry++] = (carry_t){fence, ops, *n}; *n = 0; return NULL; }
        VRED_ONCE("completion effects of more than %d unsignalled commits: dropped (events and counter reports not written)", NCARRY);   // (vka_qsched.h keeps it to QS_DEFER_MAX)
        free(ops); *n = 0; return NULL;
    }
    int tot = *n, k = 0;
    for (int i = 0; i < g_ncarry; i++) if (g_carry[i].fence <= signal) tot += g_carry[i].n;
    if (tot == *n) return ops;
    pop *all = malloc((size_t)tot * sizeof *all); int m = 0;
    for (int i = 0; i < g_ncarry; i++) {
        if (g_carry[i].fence <= signal) { if (g_carry[i].n) memcpy(all + m, g_carry[i].ops, (size_t)g_carry[i].n * sizeof *all); m += g_carry[i].n; free(g_carry[i].ops); }
        else g_carry[k++] = g_carry[i];
    }
    g_ncarry = k;
    if (*n) memcpy(all + m, ops, (size_t)*n * sizeof *all);
    free(ops); *n = tot; return all;
}
static uint64_t sub_commit(uint64_t reserved, int has_sig, uint64_t signal);
uint64_t vka_rp_sub_commit(uint64_t reserved) { uint64_t t0 = now_ns(), f = sub_commit(reserved, 0, 0); g_t_commit += now_ns() - t0; return f; }
// The worker's commit with qs_commit's timeline value (0: no signal: its effects wait for the commit that signals past its fence)
uint64_t vka_rp_sub_commit_sig(uint64_t reserved, uint64_t signal) { uint64_t t0 = now_ns(), f = sub_commit(reserved, 1, signal); g_t_commit += now_ns() - t0; return f; }
static uint64_t sub_commit(uint64_t reserved, int has_sig, uint64_t signal) {
    uint64_t fence = reserved ? reserved : vka_fence_next(); g_cur_fence = 0;
    if (!has_sig) signal = fence;
    rc_commit(fence);
    int tail = 0;   // counted for the tail floor (DM_VK_GPU_FLOOR_AUTO=2): given back by the span op when it completes
    if (g_cb && g_stat_draws > g_sub_draws0) { int vka_floor_tail_add(void); tail = vka_floor_tail_add(); post((pop){3, 0, (uint32_t)tail, 0, g_job_frame_set ? g_job_frame : g_frame}); }   // the frame's span at the end of this submission with draws (post_run)
    pop *ops = g_npost ? malloc((size_t)g_npost * sizeof *ops) : NULL; if (ops) memcpy(ops, g_post, (size_t)g_npost * sizeof *ops);
    int n = g_npost; g_npost = 0;
    ops = carry_take(fence, signal, ops, &n);
    if (!g_cb) { if (signal) vka_fence_cpu(signal, ops, n, post_run); else free(ops); return fence; }
    vka_rp_end();   // (the inline updates kept in the pass are copied there: "Inline uniform updates")
    if (g_inline_ub.n) { VRED_ONCE("inline uniform batch still staged at the submission's end: flushed there"); ub_flush(g_cb); }   // before the write-back and the HOST barrier
    vka_disp_settle(g_cb);   // the submission's last dispatches before the steps below and the next submission's (vka_draw.c "Dispatch order")
    vka_tex_writeback(g_cb);   // linear textures the GPU wrote, back into their storage for the CPU
    vka_tex_pfb_commit(g_cb, fence);   // the texture IDs its shaders read through the pool: copied out for the host (the barrier below) and cleared (vka_tex.c "Texture pool reads")
    VkMemoryBarrier mb ={VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT};
    vk.vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    uint64_t tc0 = now_ns();
    vka_enc_stop();   // every call made: the command buffer is complete
    g_t_cm_enc += now_ns() - tc0;
    unsigned k = (g_slot_i - 1) % NSLOT;
    if (g_slot_ts[k]) qf.write(g_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qp, k * QPS + 2 * (g_slot_parts[k] - 1) + 1);
    vka_gp_commit(g_cb, fence);
    VkResult r = vk.vkEndCommandBuffer(g_cb);
    g_rec_ns += now_ns() - g_sub_t0;
    g_hrec[fence % NHREC] = (typeof(g_hrec[0])){fence, g_frame, S.serial, g_stat_draws - g_sub_draws0, g_stat_ops - g_sub_ops0, g_stat_skipped - g_sub_skip0, now_ns() - g_sub_t0};
    vka_tex_gw_commit(fence);
    vka_floor_commit(tail);
    tc0 = now_ns();
    vka_up_flush(); vka_up_throttle();   // before the submission that may use what it made
    uint64_t tc1 = now_ns(); g_t_cm_up += tc1 - tc0;
    unsigned ks = (g_slot_i - 1) % NSLOT;
    if (!signal && !g_slot[ks].vf) { VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; if (vk.vkCreateFence(V.dev, &fi, NULL, &g_slot[ks].vf)) g_slot[ks].vf = VK_NULL_HANDLE; }
    if (!r) r = vka_submit_fence(g_cb, signal, !signal ? g_slot[ks].vf : VK_NULL_HANDLE);
    if (!r && !signal && g_slot[ks].vf) g_slot[ks].vf_on = 1;   // (no fence made: the slot waits for the timeline, as a signalled one)
    g_t_cm_sub += now_ns() - tc1;
    g_slot[(g_slot_i - 1) % NSLOT].fence = fence;
    if (vka_nan_on()) vka_nan_commit(fence);
    vka_scratch_retire(fence);
    if (r) {
        if (r == VK_ERROR_DEVICE_LOST) vka_device_lost("submission");
        static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) VRED("submission %llu: %d (its effects run without the GPU)", (unsigned long long)fence, r);
        VkResult sr = VK_SUCCESS;
        if (signal) {
            VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = V.tl, .value = signal};
            pthread_mutex_lock(&V.qmu);   // signalled by the host: later submissions may wait for it
            sr = vka_hung() ? VK_ERROR_DEVICE_LOST : vk.vkSignalSemaphore(V.dev, &si);
            if (!sr && signal > V.tl_queued) __atomic_store_n(&V.tl_queued, signal, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&V.qmu);
        }
        if (sr) { vka_fence_cpu(signal, ops, n, post_run); g_cb = VK_NULL_HANDLE; return fence; }
    }
    if (signal) vka_fence_post(signal, ops, n, post_run);
    if (atomic_load_explicit(&g_synclog, memory_order_relaxed) > 0)
        VLOG("sync sub: frame %llu fence %llu: %llu draws, %d parts, recorded %.2f-%.2f ms, queued", (unsigned long long)g_frame, (unsigned long long)fence,
             (unsigned long long)(g_stat_draws - g_sub_draws0), g_nx + 1, (g_sub_t0 % 1000000000000ull) / 1e6, ms_now());
    g_cb = VK_NULL_HANDLE;
    return fence;
}
uint64_t mtl_last_fence(void) { return vka_fence_last(); }
// DM_VK_SYNCLOG=n (live): the next n of the game's blocking waits (the fence against the last reserved and the last queued, when the wait began and ended), and each
// submission's recording and queueing times (vka_rp_sub_commit), on one clock (ms): where a frame's wait goes. The count starts again when the value changes
static int synclog(void) { static int last; int v = VKA_ENV("DM_VK_SYNCLOG", 0); if (v != last) { last = v; atomic_store(&g_synclog, v); } return atomic_load(&g_synclog) > 0; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (timeout_ns && vka_done() < fence && synclog()) {
        double t0 = ms_now(); int r = vka_wait_done(fence, timeout_ns);
        if (atomic_fetch_sub(&g_synclog, 1) > 0) VLOG("sync wait: frame %llu fence %llu (last reserved %llu, queued %llu, done %s): %.2f-%.2f ms, %.2f ms", (unsigned long long)g_frame, (unsigned long long)fence,
                                                (unsigned long long)vka_fence_last(), (unsigned long long)vka_fence_queued(), r ? "yes" : "timeout", t0, ms_now(), ms_now() - t0);
        return r;
    }
    return vka_wait_done(fence, timeout_ns);
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return vka_wait_done(fence, timeout_ns); }
void mtl_finish(void) { vka_wait_done(vka_fence_last(), UINT64_MAX); }
void mtl_present_flush(void);
void vka_pso_pause(int on);
void mtl_drain(void) { vka_pso_pause(1); vka_pso_save(1); mtl_present_flush(); mtl_finish(); if (vka_ok()) vka_queue_idle(); }

// ---------- Buffers ----------
static void buf_copy(uint64_t src, uint64_t dst, uint64_t n) {
    vka_tex_gw_copy(src, dst, n);
    while (n) {
        VkBuffer sb, db; VkDeviceSize so, dof;
        uint64_t a = vka_addr_run(src, n, &sb, &so), b = vka_addr_run(dst, n, &db, &dof), k = a < b ? a : b;
        if (!k) { static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 8) VRED("buffer copy %#llx → %#llx (%llu bytes): %s is not in placed guest RAM", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n, a ? "the destination" : "the source"); return; }
        VkBufferCopy c = {so, dof, k}; vk.vkCmdCopyBuffer(g_cb, sb, db, 1, &c);
        src += k; dst += k; n -= k;
    }
}
static void buf_fill(uint64_t dst, uint64_t n, uint32_t value) {
    vka_tex_gw_fill(dst, n, value);
    if ((dst | n) & 3) { VRED_ONCE("buffer fill at %#llx of %llu bytes is not 4-byte aligned", (unsigned long long)dst, (unsigned long long)n); return; }
    while (n) {
        VkBuffer b; VkDeviceSize o; uint64_t k = vka_addr_run(dst, n, &b, &o);
        if (!k) { VRED_ONCE("buffer fill at %#llx is not in placed guest RAM", (unsigned long long)dst); return; }
        vk.vkCmdFillBuffer(g_cb, b, o, k & ~3ull, value); dst += k; n -= k;
    }
}
static void buf_update(const void *data, uint64_t dst, uint64_t n) {   // inline data, in GPU order: through scratch memory, staged in the batch ("Inline uniform updates")
    vka_tex_gw_update(dst, n, data);
    vka_span s; if (!n || !vka_scratch(n, 16, &s)) return;
    memcpy(s.ptr, data, n);
    ub_copy(g_cb, &s, dst, n);
}

// ---------- Clears ----------
static float srgb_enc(float c) { c = c < 0 ? 0 : c > 1 ? 1 : c; return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }
static VkClearColorValue color_value(const vka_fmt *f, const float rgba[4]) {
    VkClearColorValue v;
    for (int i = 0; i < 4; i++) {
        // (saturating: a float outside the integer type's range, or NaN, converted with a cast is undefined in C)
        if (f->kind == 1) v.uint32[i] = !(rgba[i] > 0) ? 0 : rgba[i] >= 4294967295.0f ? UINT32_MAX : (uint32_t)rgba[i];
        else if (f->kind == 2) v.int32[i] = rgba[i] != rgba[i] ? 0 : rgba[i] <= -2147483648.0f ? INT32_MIN : rgba[i] >= 2147483647.0f ? INT32_MAX : (int32_t)rgba[i];
        else v.float32[i] = f->srgb && i < 3 ? srgb_enc(rgba[i]) : rgba[i];
    }
    return v;
}
static void level_size(const vka_img *im, uint32_t lv, uint32_t *w, uint32_t *h) { *w = im->w >> lv ? im->w >> lv : 1; *h = im->h >> lv ? im->h >> lv : 1; }
static void rt_range(const vka_img *im, const view_t *v, int has_v, uint32_t *lv, uint32_t *l0, uint32_t *nl) {
    *lv = has_v && v->set_levels ? v->base_level : 0;
    *l0 = has_v && v->set_layers ? v->min_layer : 0;
    if (!vka_img_lv(im, lv)) { *lv = im->levels; *nl = 0; return; }   // a level the image leaves out (a sparse texture's unmapped levels)
    uint32_t all = vka_img_slices(im, *lv);
    *nl = has_v && v->set_layers && v->nlayers ? v->nlayers : all - (*l0 < all ? *l0 : 0);
    if (*l0 + *nl > all) *nl = *l0 < all ? all - *l0 : 0;
}
// A clear of part of a level: one dynamic-rendering pass over the attachment, vkCmdClearAttachments in the rectangle
static void clear_rect(vka_img *im, VkFormat fmt, VkImageAspectFlags asp, uint32_t lv, uint32_t l0, uint32_t nl, VkRect2D rc, const VkClearValue *val) {
    int color = asp == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!(im->usage & (color ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        VRED_ONCE("partial clear of a texture that cannot be an attachment (serial %llu format %#x)", (unsigned long long)im->serial, im->d.format); return; }
    VkImageView view = vka_view(im, fmt, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, im->aspect, lv, 1, l0, nl, NULL);
    if (!view) return;
    uint32_t w, h; level_size(im, lv, &w, &h);
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = nl, .colorAttachmentCount = color, .pColorAttachments = color ? &a : NULL,
                          .pDepthAttachment = im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? &a : NULL, .pStencilAttachment = im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? &a : NULL};
    vk.vkCmdBeginRendering(g_cb, &ri);
    VkClearAttachment ca = {asp, 0, *val}; VkClearRect cr = {rc, 0, nl};
    vk.vkCmdClearAttachments(g_cb, 1, &ca, 1, &cr);
    vk.vkCmdEndRendering(g_cb);
}
static int scissored(const vka_img *im, uint32_t lv, VkRect2D *rc) {   // 1 = the scissor leaves part of the level out; rc = what is left (may be empty)
    uint32_t w, h; level_size(im, lv, &w, &h);
    *rc = (VkRect2D){{0, 0}, {w, h}};
    if (!S.has_sc) return 0;
    int64_t x0 = S.sc.x > 0 ? S.sc.x : 0, y0 = S.sc.y > 0 ? S.sc.y : 0, x1 = (int64_t)S.sc.x + S.sc.w, y1 = (int64_t)S.sc.y + S.sc.h;
    if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) { rc->extent = (VkExtent2D){0, 0}; return 1; }
    *rc = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return !(x0 == 0 && y0 == 0 && x1 == w && y1 == h);
}
// Clears of the bound targets go into the render pass over them (vka_rp_clear): the title's 63 draws took 59 passes when each clear and rebinding ended the pass. Outside a
// pass otherwise (vka_clear_outside); DM_VK_PASS_CLEARS=0 keeps them all outside
static int clear_in_pass(const vka_pclear *c) {
    int on = VKA_ENV("DM_VK_PASS_CLEARS", 1);
    if (on && vka_rp_clear(&S.rt, S.has_rt, &S.d, c)) { g_n_pclear++; return 1; }
    vka_rp_end();
    return 0;
}
void vka_clear_outside(const vka_pclear *c) {
    vka_img *im = c->im; VkClearValue val = c->out;
    if (c->ci >= 0) {
        int is3d = im->d.target == NVN_TEXTURE_TARGET_3D, some = is3d && (c->l0 || c->nl < vka_img_slices(im, c->lv));   // some of a 3D texture's slices: through a 2D-array view of them
        if (c->whole && c->fmt == im->f.vk && !some) {
            VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, c->lv, 1, is3d ? 0 : c->l0, is3d ? 1 : c->nl};
            vk.vkCmdClearColorImage(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &val.color, 1, &rg);
        } else clear_rect(im, c->fmt, VK_IMAGE_ASPECT_COLOR_BIT, c->lv, c->l0, c->nl, c->rc, &val);
    } else {
        if (c->whole) { VkImageSubresourceRange rg = {c->asp, c->lv, 1, c->l0, c->nl}; vk.vkCmdClearDepthStencilImage(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &val.depthStencil, 1, &rg); }
        else clear_rect(im, im->f.vk, c->asp, c->lv, c->l0, c->nl, c->rc, &val);
    }
    vka_barrier(g_cb);
}
static void clear_color(const p_clear_color *c) {
    vka_rp_prep_clear(&S.rt, S.has_rt);
    if (S.rc_skip) return;
    if (!S.has_rt || c->index < 0 || c->index >= S.rt.n || c->index >= 8 || !S.rt.color[c->index]) { VRED_ONCE("color clear of target %d, which is not bound", c->index); return; }
    if (!c->mask) return;
    const tex_t *t = S.rt.color[c->index]; const view_t *v = &S.rt.cview[c->index]; int hv = S.rt.has_cview[c->index];
    vka_img *im = vka_tex_get(t, g_cb); vka_fmt f;
    if (!im || !vka_fmt_of(hv && v->set_format ? v->format : t->d.format, &f)) return;
    // (an integer target, uint or sint, is cleared too: color_value converts the clear colour to its type, as ClearTexture below does. Refusing every kind but float
    // left Mario Kart World's R32_UINT and R8_UINT targets never cleared on the Vulkan replay, holding older frames' values; only depth and stencil are not colour clears)
    if (f.kind >= 3) { VRED_ONCE("color clear of a depth / stencil texture (format %#x)", t->d.format); return; }
    if ((c->mask & 0xf) != 0xf) { VRED_ONCE("color clear with channel mask %#x is not on Vulkan yet (needs a pipeline): skipped", c->mask); return; }
    vka_tex_own(im, g_cb);   // (zero-shared: memory of its own before the clear, which may also become the next pass's loadOp)
    uint32_t lv, l0, nl; rt_range(im, v, hv, &lv, &l0, &nl);
    if (lv >= im->levels || !nl) return;
    VkRect2D rc; int part = scissored(im, lv, &rc);
    if (!rc.extent.width) return;
    vka_fmt fl = f; fl.srgb = 0;   // in a pass the attachment is the sRGB view: the clear colour stays linear there
    vka_pclear pc = {c->index, im, f.vk, VK_IMAGE_ASPECT_COLOR_BIT, lv, l0, nl, rc, !part, {.color = color_value(&f, c->rgba)}, {.color = color_value(&fl, c->rgba)}};
    vka_img_written(im); g_stat_ops++;
    if (!clear_in_pass(&pc)) vka_clear_outside(&pc);
}
// A stencil clear with a write mask (agl clears single stencil bits it uses as masks): Vulkan's clears write every bit, so it is a draw. A full-viewport triangle (the present's
// rotation vertex shader, no fragment shader) with the stencil test always passing, REPLACE with the reference = the clear value, and the clear's write mask as the dynamic
// write mask; depth untouched. One pipeline per depth-stencil format and sample count
static struct { VkFormat f; uint32_t samples; VkPipeline p; } g_smc[8]; static int g_nsmc; static VkPipelineLayout g_smc_lay; static VkShaderModule g_smc_vs;
static VkPipeline smc_pipeline(const vka_img *im) {
    for (int i = 0; i < g_nsmc; i++) if (g_smc[i].f == im->f.vk && g_smc[i].samples == im->samples) return g_smc[i].p;
    if (g_nsmc == 8) return VK_NULL_HANDLE;
    VkResult r = 0;
    if (!g_smc_lay) {
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 6 * sizeof(float)};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        VkShaderModuleCreateInfo mv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vka_rot_vert, .pCode = vka_rot_vert};
        if ((r = vk.vkCreatePipelineLayout(V.dev, &li, NULL, &g_smc_lay)) || (r = vk.vkCreateShaderModule(V.dev, &mv, NULL, &g_smc_vs))) { VRED("masked stencil clear: layout / shader %d", r); return VK_NULL_HANDLE; }
    }
    VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_smc_vs, .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)im->samples};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0, 0};
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .stencilTestEnable = VK_TRUE, .front = so, .back = so};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 4, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? im->f.vk : VK_FORMAT_UNDEFINED, .stencilAttachmentFormat = im->f.vk};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rci, .stageCount = 1, .pStages = &st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = g_smc_lay};
    VkPipeline p = VK_NULL_HANDLE;
    if ((r = vk.vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, NULL, &p))) { VRED_ONCE("masked stencil clear pipeline (format %d): %d", im->f.vk, r); return VK_NULL_HANDLE; }
    g_smc[g_nsmc++] = (typeof(g_smc[0])){im->f.vk, im->samples, p};
    return p;
}
static void stencil_masked(vka_img *im, uint32_t lv, uint32_t l0, uint32_t nl, VkRect2D rc, uint32_t mask, uint32_t value) {
    VkPipeline p = smc_pipeline(im);
    if (!p || !(im->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) { VRED_ONCE("stencil clear with write mask %#x: no pipeline, stencil left", mask); return; }
    uint32_t w = im->w >> lv ? im->w >> lv : 1, h = im->h >> lv ? im->h >> lv : 1;
    for (uint32_t l = l0; l < l0 + nl; l++) {   // a layer at a time: the vertex shader writes no layer
        VkImageView view = vka_view(im, im->f.vk, VK_IMAGE_VIEW_TYPE_2D, im->aspect, lv, 1, l, 1, NULL);
        if (!view) return;
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                              .pDepthAttachment = im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? &a : NULL, .pStencilAttachment = &a};
        vk.vkCmdBeginRendering(g_cb, &ri);
        float pc[6] = {0};
        VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
        vk.vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vk.vkCmdPushConstants(g_cb, g_smc_lay, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
        vk.vkCmdSetViewport(g_cb, 0, 1, &v); vk.vkCmdSetScissor(g_cb, 0, 1, &rc);
        vk.vkCmdSetStencilWriteMask(g_cb, VK_STENCIL_FACE_FRONT_AND_BACK, mask); vk.vkCmdSetStencilReference(g_cb, VK_STENCIL_FACE_FRONT_AND_BACK, value);
        vk.vkCmdDraw(g_cb, 3, 1, 0, 0);
        vk.vkCmdEndRendering(g_cb);
    }
    vka_dyn_invalidate();   // its pipeline's static state replaced the draws' dynamic state
}
static void clear_ds(const p_clear_ds *c) {
    vka_rp_prep_clear(&S.rt, S.has_rt);
    if (S.rc_skip || !S.has_rt || !S.rt.depth) return;
    vka_img *im = vka_tex_get(S.rt.depth, g_cb);
    if (!im || im->f.kind < 3) return;
    vka_tex_own(im, g_cb);
    VkImageAspectFlags asp = 0; unsigned sm = (unsigned)c->stencil_mask & 0xff;
    if (c->depth_mask && (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    int masked = sm && sm != 0xff && (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);   // drawn (stencil_masked)
    if (sm == 0xff && (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp && !masked) return;
    uint32_t lv, l0, nl; rt_range(im, &S.rt.dview, S.rt.has_dview, &lv, &l0, &nl);
    if (lv >= im->levels || !nl) return;
    VkRect2D rc; int part = scissored(im, lv, &rc);
    if (!rc.extent.width) return;
    VkClearValue val = {.depthStencil = {c->depth, (uint32_t)c->stencil & 0xff}};
    vka_pclear pc = {-1, im, im->f.vk, asp, lv, l0, nl, rc, !part, val, val};
    if (!masked && asp && clear_in_pass(&pc)) { vka_img_written(im); g_stat_ops++; return; }
    vka_rp_end();
    if (masked) { stencil_masked(im, lv, l0, nl, rc, sm, (uint32_t)c->stencil & 0xff); vka_img_written(im); g_stat_ops++; vka_barrier(g_cb); if (!asp) return; }
    vka_clear_outside(&pc);
    vka_img_written(im); g_stat_ops++;
}
static void clear_tex(const p_clear_tex *c) {
    if (S.rc_skip || !c->tex) return;
    vka_img *im = vka_tex_get(c->tex, g_cb); vka_fmt f;
    if (!im || !vka_fmt_of(c->has_view && c->view.set_format ? c->view.format : c->tex->d.format, &f)) return;
    if (f.kind >= 3) { VRED_ONCE("ClearTexture of a depth / stencil texture (format %#x)", c->tex->d.format); return; }
    if ((c->mask & 0xf) != 0xf) { if (c->mask & 0xf) VRED_ONCE("ClearTexture with channel mask %#x is not on Vulkan yet: skipped", c->mask); return; }
    vka_tex_own(im, g_cb);
    uint32_t lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = (c->has_view && c->view.set_layers ? c->view.min_layer : 0) + (uint32_t)c->r.zoffset, nl = c->r.depth > 0 ? (uint32_t)c->r.depth : 1;
    if (!vka_img_lv(im, &lv)) return;
    uint32_t all = vka_img_slices(im, lv); int is3d = im->d.target == NVN_TEXTURE_TARGET_3D;
    if (l0 >= all) return; if (l0 + nl > all) nl = all - l0;
    uint32_t w, h; level_size(im, lv, &w, &h);
    VkClearValue val;
    if (c->is_uint) for (int i = 0; i < 4; i++) val.color.uint32[i] = c->c.u[i]; else val.color = color_value(&f, c->c.f);
    if (c->r.xoffset == 0 && c->r.yoffset == 0 && (uint32_t)c->r.width >= w && (uint32_t)c->r.height >= h && f.vk == im->f.vk && (!is3d || (!l0 && nl == all))) {
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 1, is3d ? 0 : l0, is3d ? 1 : nl}; vk.vkCmdClearColorImage(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &val.color, 1, &rg);
    } else clear_rect(im, f.vk, VK_IMAGE_ASPECT_COLOR_BIT, lv, l0, nl, (VkRect2D){{c->r.xoffset, c->r.yoffset}, {(uint32_t)c->r.width, (uint32_t)c->r.height}}, &val);
    vka_img_written(im); g_stat_ops++;
    vka_barrier(g_cb);
}

// ---------- Copies ----------
static VkImageAspectFlags copy_aspect(const vka_img *im, const char *what) {
    if (im->f.kind == 0 || im->f.kind == 1 || im->f.kind == 2) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (im->f.vk == VK_FORMAT_D16_UNORM || im->f.vk == VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (im->f.vk == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    VRED_ONCE("%s of a packed depth-stencil texture (format %#x) is not on Vulkan yet: NVN's buffer layout interleaves what Vulkan copies by aspect", what, im->d.format);
    return 0;
}
// Buffer ↔ texture: c->src is the buffer's guest address. Rows are SetCopyRowStride bytes apart (tightly packed without it), layers / slices SetCopyImageStride
static void copy_bt(const p_copy_b2t *c, int to_tex) {
    if (!c->tex) return;
    vka_img *im = vka_tex_get(c->tex, g_cb);
    if (!im) return;
    if (to_tex) vka_tex_own(im, g_cb);
    VkImageAspectFlags asp = copy_aspect(im, to_tex ? "buffer → texture copy" : "texture → buffer copy"); if (!asp) return;
    if (im->samples > 1) { VRED_ONCE("buffer copy of a multisampled texture"); return; }
    const vka_fmt *f = &im->f;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (!vka_img_lv(im, &lv) || c->r.width <= 0 || c->r.height <= 0) return;
    size_t row = S.copy_stride ? (size_t)S.copy_stride : (size_t)((c->r.width + f->bw - 1) / f->bw) * f->bpb, rows = (size_t)(c->r.height + f->bh - 1) / f->bh;
    size_t img = S.copy_img_stride ? (size_t)S.copy_img_stride : row * rows;
    int is3d = im->depth > 1 || c->tex->d.target == NVN_TEXTURE_TARGET_3D, nz = c->r.depth > 0 ? c->r.depth : 1;
    if (row % f->bpb) { VRED_ONCE("buffer copy row stride %zu is not a whole number of %u-byte blocks", row, f->bpb); return; }
    uint64_t span = img * (size_t)(nz - 1) + row * (rows - 1) + (size_t)((c->r.width + f->bw - 1) / f->bw) * f->bpb;
    if (f->dec) {   // BC decoded on the CPU (vka_bcdec.h): the blocks are read from the buffer's guest memory, a slice at a time; a copy back into a buffer has no blocks to give
        if (!to_tex) { VRED_ONCE("texture → buffer copy of a BC texture decoded on the CPU (format %#x): not done", im->d.format); return; }
        uint32_t cbx = (uint32_t)((c->r.width + f->bw - 1) / f->bw);
        for (int z = 0; z < nz; z++)
            vka_tex_bc_copy(g_cb, im, (const uint8_t *)(uintptr_t)c->src + (size_t)z * img, row, cbx, (uint32_t)rows, 1, lv, is3d ? 0 : l0 + (uint32_t)c->r.zoffset + (uint32_t)z,
                            c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset + z : 0, (uint32_t)c->r.width, (uint32_t)c->r.height);
        vka_img_written(im); g_stat_ops++; vka_barrier(g_cb); return;
    }
    VkBuffer b; VkDeviceSize o;
    if (!vka_addr(c->src, span, &b, &o, NULL)) { static _Atomic int nr; if (atomic_fetch_add(&nr, 1) < 8) VRED("%s at %#llx (%llu bytes) is not in placed guest RAM", to_tex ? "buffer → texture copy source" : "texture → buffer copy destination", (unsigned long long)c->src, (unsigned long long)span); return; }
    vka_span s = {0}; VkDeviceSize al = asp == VK_IMAGE_ASPECT_COLOR_BIT ? (f->bpb % 4 == 0 ? f->bpb : f->bpb % 2 == 0 ? 2 * f->bpb : 4 * f->bpb) : 4;   // a whole block and a multiple of 4
    int bounce = o % al != 0 || (img % row) != 0;   // Vulkan wants block-aligned offsets and layers a whole number of rows apart
    if (bounce) {
        size_t wb = (size_t)((c->r.width + f->bw - 1) / f->bw) * f->bpb;
        if (!vka_scratch(wb * rows * (size_t)nz, al * 16, &s)) return;
        if (to_tex) for (int z = 0; z < nz; z++) for (size_t y = 0; y < rows; y++) { VkBufferCopy bc = {o + z * img + y * row, s.off + (z * rows + y) * wb, wb}; vk.vkCmdCopyBuffer(g_cb, b, s.buf, 1, &bc); }
        if (to_tex) vka_barrier(g_cb);
        row = wb; img = wb * rows;
    }
    VkBufferImageCopy rg[64]; int n = 0;
    for (int z = 0; z < (is3d ? 1 : nz) && n < 64; z++)
        rg[n++] = (VkBufferImageCopy){(bounce ? s.off : o) + (is3d ? 0 : z * img), (uint32_t)(row / f->bpb * f->bw), (uint32_t)(img / row * f->bh),
            {asp, lv, is3d ? 0 : l0 + (uint32_t)c->r.zoffset + (uint32_t)z, 1}, {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0}, {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    if (to_tex) { vk.vkCmdCopyBufferToImage(g_cb, bounce ? s.buf : b, im->img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)n, rg); vka_img_written(im); }
    else {
        vka_tex_gw_opaque(c->src, span);
        vk.vkCmdCopyImageToBuffer(g_cb, im->img, VK_IMAGE_LAYOUT_GENERAL, bounce ? s.buf : b, (uint32_t)n, rg);
        if (bounce) {
            vka_barrier(g_cb);
            size_t wb = row, srow = S.copy_stride ? (size_t)S.copy_stride : wb, simg = S.copy_img_stride ? (size_t)S.copy_img_stride : srow * rows;
            for (int z = 0; z < nz; z++) for (size_t y = 0; y < rows; y++) { VkBufferCopy bc = {s.off + (z * rows + y) * wb, o + z * simg + y * srow, wb}; vk.vkCmdCopyBuffer(g_cb, s.buf, b, 1, &bc); }
        }
    }
    g_stat_ops++;
    vka_barrier(g_cb);
}
static void copy_t2t(const p_copy_t2t *c) {
    if (!c->src || !c->dst) return;
    vka_img *s = vka_tex_get(c->src, g_cb), *d = vka_tex_get(c->dst, g_cb);
    if (!s || !d) return;
    vka_tex_own(d, g_cb);
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (!vka_img_lv(s, &sl) || !vka_img_lv(d, &dl)) return;
    VkImageAspectFlags sa = s->aspect & (s->f.kind >= 3 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), da = d->aspect;
    if ((sa == VK_IMAGE_ASPECT_COLOR_BIT) != (da == VK_IMAGE_ASPECT_COLOR_BIT)) { VRED_ONCE("texture copy between a color and a depth texture (formats %#x → %#x)", c->src->d.format, c->dst->d.format); return; }
    sa &= da;
    int s3 = s->depth > 1, d3 = d->depth > 1, nz = c->sr.depth > 0 ? c->sr.depth : 1;
    VkImageSubresourceLayers ss = {sa, sl, s3 ? 0 : sz0 + (uint32_t)c->sr.zoffset, s3 ? 1 : (uint32_t)nz}, ds = {sa, dl, d3 ? 0 : dz0 + (uint32_t)c->dr.zoffset, d3 ? 1 : (uint32_t)nz};
    if (ss.baseArrayLayer + ss.layerCount > s->layers || ds.baseArrayLayer + ds.layerCount > d->layers) { VRED_ONCE("texture copy layers out of range"); return; }
    if (c->flags & DM_COPY_RESOLVE && s->samples > 1 && d->samples == 1) {
        VkImageResolve r = {ss, {c->sr.xoffset, c->sr.yoffset, 0}, ds, {c->dr.xoffset, c->dr.yoffset, 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, 1}};
        if (sa == VK_IMAGE_ASPECT_COLOR_BIT) vk.vkCmdResolveImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        else VRED_ONCE("depth resolve is not on Vulkan yet");
    } else if (c->sr.width == c->dr.width && c->sr.height == c->dr.height && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT))
               && s->samples == d->samples && s->f.bpb == d->f.bpb && s->f.bw == d->f.bw && s->f.bh == d->f.bh && s->f.tb == d->f.tb) {   // (tb: BC decoded on the CPU, both sides into texels of one size)
        VkImageCopy r = {ss, {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0}, ds, {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0}, {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)nz : 1}};
        vk.vkCmdCopyImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {   // a scaled, mirrored or format-changing copy: a blit (filtered with LINEAR_FILTER)
        if (s->f.kind != d->f.kind || (s->f.kind && s->f.kind < 3) || s->f.bw > 1 || d->f.bw > 1 || s->samples > 1 || d->samples > 1) {
            VRED_ONCE("texture copy %#x → %#x needs a draw (integer, compressed, multisampled or depth with scaling)", c->src->d.format, c->dst->d.format); return; }
        int x0 = c->sr.xoffset, x1 = x0 + c->sr.width, y0 = c->sr.yoffset, y1 = y0 + c->sr.height, t;
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { t = x0; x0 = x1; x1 = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { t = y0; y0 = y1; y1 = t; }
        VkImageBlit r = {ss, {{x0, y0, 0}, {x1, y1, 1}}, ds, {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, 1}}};
        VkFormatProperties fp; vk.vkGetPhysicalDeviceFormatProperties(V.pd, s->f.vk, &fp);
        VkFilter fl = (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && sa == VK_IMAGE_ASPECT_COLOR_BIT && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        vk.vkCmdBlitImage(g_cb, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r, fl);
    }
    vka_img_written(d); g_stat_ops++;
    vka_barrier(g_cb);
}

static void not_yet(uint32_t op) {
    g_stat_skipped++;
    VRED_ONCE("DrawTexture (op %u) is not on Vulkan yet: skipped from now on", op);
}
// Indirect mesh draws look-ahead: when a render pass is about to open and no scan was made since the last write that could change an indirect mesh draw record (a dispatch,
// buffer copy or fill, texture-to-buffer copy, inline update), mdx_scan looks ahead in replay order (this list, then the lists that called it) up to that next write, and
// converts every indirect mesh record it meets into {count, 1, 1} before the pass begins (vka_mdx_batch).
static uint64_t g_mdx_ep = 1, g_mdx_scan_ep, g_mdx_sub = ~0ull;
static const uint8_t *g_xret[17]; static int g_nxret;
static void mdx_write(void) { if (g_mdx_scan_ep == g_mdx_ep) g_mdx_ep++; vka_mdx_inval(); }
static void mdx_sub(void) { if (g_mdx_sub != g_n_sub) { g_mdx_sub = g_n_sub; g_mdx_ep++; vka_mdx_inval(); } }
static void mdx_scan(const uint8_t *p) {
    enum { QMAX = 256 }; uint64_t q[QMAX]; int nq = 0;
    const uint8_t *ret[17]; int nret = g_nxret; memcpy(ret, g_xret, sizeof(ret[0]) * (size_t)nret);
    for (int guard = 0; guard < 200000 && nq < QMAX; guard++) {
        op_hdr h; memcpy(&h, p, sizeof h); const void *a = p + 8;
        if (h.op == OP_END) { if (!nret) break; p = ret[--nret]; continue; }
        if (h.op == OP_JUMP) { memcpy(&p, a, 8); continue; }
        if (h.op == OP_CALL) { if (nret == 17) break; ret[nret++] = p + h.size; memcpy(&p, a, 8); continue; }
        if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B || h.op == OP_UPDATE_UB) break;
        if (h.op == OP_DRAW) { p_draw t; const p_draw *d = draw_rec(a, h.size, &t);
            if (d->prim == NVN_PRIM_MESH && d->ind && !vka_mdx_has(d->ind)) { int dup = 0; for (int k = 0; k < nq && !dup; k++) dup = q[k] == d->ind; if (!dup) q[nq++] = d->ind; } }
        if (h.size < 8) break;
        p += h.size;
    }
    g_mdx_scan_ep = g_mdx_ep;
    if (nq) vka_mdx_batch(q, nq);
}
static void mdx_before_draw(const uint8_t *p, const p_draw *d) {
    if (!V.mesh || !VKA_ENV("DM_VK_MESH_IND", 1)) return;
    mdx_sub();
    int ind_mesh = d && d->prim == NVN_PRIM_MESH && d->ind;
    if (!vka_rp_is_open()) { if (g_mdx_scan_ep != g_mdx_ep || (ind_mesh && !vka_mdx_has(d->ind))) mdx_scan(p); return; }
    if (ind_mesh && !vka_mdx_has(d->ind)) { vka_rp_end(); mdx_scan(p); }
}
static void seg_write(const void *a) { struct { VkCommandBuffer cb; VkPipelineStageFlagBits st; uint32_t q; } w; memcpy(&w, a, sizeof w); qf.write(w.cb, w.st, g_sqp, w.q); }
// NVN queue waits (vka_qsched.h): the replay worker answers whether a wait holds (vka_wait_pass); one that does not ends the replay of this submission here, and its continuation
// (the record after the wait and the return points of the lists that called it) is written to g_pk for vka_rp_resume. Replays outside the worker pass every wait
int vka_wait_pass(uint32_t op, const void *a, const uint8_t *rec);   // vka_worker.c
void vka_event_replayed(volatile uint32_t *w, uint32_t mode, uint32_t v);
static qs_cont *g_pk;
static int exec(const uint8_t *p, int depth) {   // 1: parked at a wait (g_pk holds where it goes on)
    const int ub_batch_on = VKA_ENV("DM_VK_UB_BATCH", 1) != 0, ub_defer_on = VKA_ENV("DM_VK_UB_DEFER", 0) != 0;   // ("Inline uniform updates"; live, read per list)
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const void *a = p + 8;
        int bar_kept = 0;   // a game barrier taken inside the open pass (vka_rp_game_barrier): no pass end, no full barrier
        int bar_covered = 0;   // a game barrier that ended the open pass with a full barrier (vka_rp_end_covers_barrier, DM_VK_BAR_DEDUP): no second one
        int ub_keep = 0;    // an inline update kept in the open pass until its end ("Inline uniform updates")
        // The staged inline updates go out before anything but another update or a state record (a part may end at the draw below: they stay in the command buffer
        // they were staged for). With updates kept in the pass, a signalled event ends it: another queue may consume what this one wrote once the event is replayed
        // (vka_event_replayed). The other records the first switch below keeps the pass open across stay in this command buffer, and a parked wait commits, which ends it.
        // The list's own flow (BEGIN, JUMP, CALL, and END back into a calling list) records nothing either, so updates in lists one after another share a copy; only the
        // outermost list's END flushes (its caller may record the submission's end)
        if (g_inline_ub.n && h.op != OP_UPDATE_UB && !ub_state_only(h.op) && !ub_list_flow(h.op, depth)) ub_flush(g_cb);
        if (g_ub_np && h.op == OP_SIGNAL_EVENT) { g_ub_bound_ends++; vka_rp_end(); }
        switch (h.op) {   // GPU work other than a dispatch: after the barrier the dispatches before it left pending (vka_draw.c "Dispatch order"), and before a part ends at a draw
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_TEX: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_CLEAR_TEX: case OP_CLEAR_BUF: case OP_COPY_B2B: case OP_COPY_B2T:
        case OP_COPY_T2B: case OP_COPY_T2T: case OP_UPDATE_UB: case OP_REPORT_COUNTER: vka_disp_settle(g_cb); break;
        default: break;   // (OP_BARRIER records a full barrier of its own, which settles it; the state ops record nothing)
        }
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) chunk_check();   // before a draw that opens a new pass
        switch (h.op) {   // a render pass a draw opened stays open across draws and state; anything else closes it first
        case OP_END: case OP_BEGIN: case OP_JUMP: case OP_CALL: case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_FENCE_SYNC: case OP_RENDER_COND: case OP_RESET_COUNTER: case OP_WAIT_EVENT: case OP_WAIT_SYNC:
        case OP_SCISSOR: case OP_COPY_STRIDE: case OP_DRAW: case OP_DRAW_IDX: break;
        case OP_SET_RT: if (g_cb && !vka_pass_keep()) vka_rp_end(); break;
        case OP_BARRIER: if (g_cb) { int32_t bits; memcpy(&bits, a, 4); bar_kept = vka_rp_game_barrier((uint32_t)bits); if (!bar_kept) bar_covered = vka_rp_end_covers_barrier(); } break;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: break;   // the pass is ended by the next draw over other targets, or the clear (vka_rp_clear)
        case OP_UPDATE_UB:   // kept in the open pass when nothing in it can see the bytes before its end (vka_rp_ub_refuse), else the pass ends first, as for any other op
            if (g_cb) {
                uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                if (ub_defer_on && vka_rp_is_open()) {
                    int why = n ? vka_rp_ub_refuse(d, n) : VKA_UBR_OK;   // (an empty update writes nothing: the pass stays)
                    if (!why && n && g_ub_np >= UB_DEFER_MAX) why = UBR_FULL;
                    if (!why && n > UB_DEFER_BYTES) why = UBR_SIZE;
                    if (why) g_ub_refused[why]++; else ub_keep = 1;
                }
                if (!ub_keep) vka_rp_end();
            }
            break;
        default: if (g_cb && !vka_state_op(&S.d, h.op, a)) vka_rp_end(); else if (!g_cb) vka_state_op(&S.d, h.op, a); break;
        }
        switch (h.op) {
        case OP_END: return 0;
        case OP_BEGIN: break;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
            if (depth < 16) { g_xret[g_nxret++] = p + h.size; int pk = exec((const uint8_t *)(uintptr_t)t, depth + 1); g_nxret--; if (pk) return 1; } else VRED_ONCE("command lists nested deeper than 16");
            break; }
        case OP_SIGNAL_EVENT: { uint64_t val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4); post((pop){1, m, v, 0, val});
            vka_event_replayed((volatile uint32_t *)(uintptr_t)val, m, v); break; }
        case OP_WAIT_EVENT: case OP_WAIT_SYNC:
            if (g_pk && !vka_wait_pass(h.op, a, p)) {
                if (g_nxret > QS_NRET) { VRED_ONCE("a wait inside command lists nested deeper than %d: passed", QS_NRET); break; }
                g_pk->p = p + h.size; g_pk->nret = g_nxret; memcpy(g_pk->ret, g_xret, sizeof(g_xret[0]) * (size_t)g_nxret);
                return 1;
            }
            break;
        case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
            if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_report(ad, (uint32_t)t)) { if (DM_GAME_RENDER_COND) rc_note(ad, 3); }   // counted (Occlusion queries)
            else {
                uint32_t q = 0;
                if (g_sqp && g_cb && g_sq_n < SEGQ && (t == NVN_COUNTER_TYPE_TIMESTAMP || t == NVN_COUNTER_TYPE_TIMESTAMP_TOP)) {
                    q = (uint32_t)(g_sq_base + g_sq_n++) + 1;
                    struct { VkCommandBuffer cb; VkPipelineStageFlagBits st; uint32_t q; } w = {g_cb, t == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, q - 1};
                    if (!vka_enc_call(seg_write, &w, sizeof w)) seg_write(&w);
                }
                if (q) g_slot_nq[g_sq_base / SEGQ] = g_sq_n;   // (sh_save keeps this many of the slot's queries)
                post((pop){2, 0, q, (uint32_t)t, ad, q ? atomic_load(&g_slot_use[g_sq_base / SEGQ]) : 0});   // q0: the slot's use (sh_get)
                if (DM_GAME_RENDER_COND) rc_note(ad, t == NVN_COUNTER_TYPE_TIMESTAMP || t == NVN_COUNTER_TYPE_TIMESTAMP_TOP ? 1 : 2);   // (OP_RENDER_COND is recorded only with render_cond)
            }
            if (atomic_load_explicit(&g_synclog, memory_order_relaxed) > 0) VLOG("sync report: frame %llu type %llu at draw %llu of the submission, part %d, recording at %.2f ms", (unsigned long long)g_frame,
                                                                           (unsigned long long)t, (unsigned long long)(g_stat_draws - g_sub_draws0), g_nx + 1, ms_now());
            break; }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_cur_fence ? g_cur_fence : vka_fence_last() + 1; s->fenced = 1; break; }   // the fence this submission commits with
        case OP_RENDER_COND: S.rc_skip = rc_eval(a); break;
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) oq_reset(); break; }
        case OP_CLEAR_BUF: mdx_write(); if (g_cb) { const p_clear_buf *cb = a; buf_fill(cb->addr, cb->size, cb->value); vka_barrier(g_cb); } break;
        case OP_COPY_B2B: mdx_write(); if (g_cb) { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8); buf_copy(s_, d, n); vka_barrier(g_cb); } break;
        case OP_UPDATE_UB: mdx_write(); g_n_ub++;
            if (g_cb) {
                uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                if (ub_keep && (!n || ub_defer_try(d, n, (const uint8_t *)a + 16))) break;
                if (vka_rp_is_open()) vka_rp_end();   // (not kept after all, no scratch: the kept ones are copied first, so the order holds)
                buf_update((const uint8_t *)a + 16, d, n);
                // (DM_VK_UB_BATCH=0: its copies and their barrier at once; the barrier also when nothing was staged, an empty update or one outside placed RAM, as each
                // update recorded before the batch: with DM_VK_UB_SCOPED=0 too, the old full barrier after every update)
                if (!ub_batch_on) { if (g_inline_ub.n) ub_flush(g_cb); else vka_transfer_write_barrier(g_cb); }
            }
            break;
        case OP_SET_RT: S.rt = *(const p_set_rt *)a; S.has_rt = 1; break;
        case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
        case OP_COPY_STRIDE: { const p_copy_stride *cs = a; if (cs->image) S.copy_img_stride = cs->stride; else S.copy_stride = cs->stride; break; }
        case OP_CLEAR_COLOR: if (g_cb) clear_color(a); break;
        case OP_CLEAR_DS: if (g_cb) clear_ds(a); break;
        case OP_CLEAR_TEX: if (g_cb) clear_tex(a); break;
        case OP_COPY_B2T: if (g_cb) copy_bt(a, 1); break;
        case OP_COPY_T2B: mdx_write(); if (g_cb) copy_bt(a, 0); break;
        case OP_COPY_T2T: if (g_cb) copy_t2t(a); break;
        case OP_BARRIER: if (g_cb && !bar_kept && !bar_covered) { int32_t bits; memcpy(&bits, a, 4); if (!vka_gbar_game(g_cb, (uint32_t)bits)) vka_barrier(g_cb); } break;   // (vka_gbar.c: between dispatches, a compute-to-compute barrier at the next one)
        case OP_DRAW: if (!S.rc_skip && g_cb) { p_draw t; const p_draw *d = draw_rec(a, h.size, &t); g_stat_draws++; mdx_before_draw(p, d); uint64_t sk = oq_sk0(); vka_draw(&S.d, &S.rt, S.has_rt, &S.sc, S.has_sc, d, NULL); oq_sk1(sk); }
                      else if (!S.rc_skip && g_oq_count) { g_oq_lost++; g_oqs_skip++; } break;   // (dropped with no command buffer: counts 1, Occlusion queries)
        case OP_DRAW_IDX: if (!S.rc_skip && g_cb) { p_draw_idx t; g_stat_draws++; mdx_before_draw(p, NULL); uint64_t sk = oq_sk0(); vka_draw(&S.d, &S.rt, S.has_rt, &S.sc, S.has_sc, NULL, draw_idx_rec(a, h.size, &t)); oq_sk1(sk); }
                          else if (!S.rc_skip && g_oq_count) { g_oq_lost++; g_oqs_skip++; } break;
        case OP_DISPATCH: mdx_write(); if (g_cb) vka_dispatch(&S.d, a); break;
        case OP_DRAW_TEX: not_yet(h.op); if (g_oq_count && !S.rc_skip) { g_oq_lost++; g_oqs_skip++; } break;
        default: break;   // state: taken by vka_state_op above
        }
        if (h.size < 8) { ub_flush(g_cb); VRED("command record of size %u at %p (op %u): the list is cut here", h.size, (const void *)p, h.op); return 0; }
        p += h.size;
    }
}
void vka_rp_exec(const uint8_t *handle, uint64_t fence) { if (fence) g_cur_fence = fence; if (handle) { g_pk = NULL; g_nxret = 0; exec(handle, 0); } }
// The worker's replay of one handle of a submission: 1 when it stopped at a wait that does not hold yet (out: where it goes on)
int vka_rp_exec_pk(const uint8_t *handle, uint64_t fence, qs_cont *out) {
    if (fence) g_cur_fence = fence;
    if (!handle) return 0;
    g_pk = out; g_nxret = 0; int r = exec(handle, 0); g_pk = NULL; g_nxret = 0;
    return r;
}
// Goes on where a park stopped (c): the rest of the innermost list, then each caller's rest, outermost last
int vka_rp_resume(const qs_cont *c, uint64_t fence, qs_cont *out) {
    if (fence) g_cur_fence = fence;
    qs_cont k = *c; int r = 0;
    g_pk = out; g_nxret = k.nret; memcpy(g_xret, k.ret, sizeof(g_xret[0]) * (size_t)k.nret);
    if (exec(k.p, k.nret)) r = 1;
    for (int i = k.nret - 1; !r && i >= 0; i--) { g_nxret = i; if (exec(k.ret[i], i)) r = 1; }
    g_pk = NULL; g_nxret = 0;
    return r;
}
#endif
