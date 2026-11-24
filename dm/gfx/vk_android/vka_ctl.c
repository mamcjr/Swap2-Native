// Live control of the replay while the game runs (android/TESTING.md "Live control"): the host's DM_CTL reader (hle_hid.c ctl_main, a thread polling the file every 100 ms;
// the launcher points DM_CTL at <app files>/ctl.txt) hands the Android-only lines to dm_gfx_ctl / dm_gfx_ctl_env here:
//   "env KEY=VALUE"   queued, applied at the next frame boundary: the first submission after the next present (vka_ctl_sub, under the host's submit lock, so no submission is
//                     being recorded). It sets the value for VKA_ENV readers (an override table, read before the process environment) and in the environment (setenv, for host
//                     switches read at use), then bumps vka_env_gen: every VKA_ENV call site reads its switch again on its next call. "env KEY=" clears an override
//                     (the switch reads as unset). Switches that need a restart are named in the log line (g_restart)
//   "capture <n>"     the next n frames, one log line each: draws, skipped draws, render passes, full barriers, clears / copies, submissions, recording ms, GPU ms
//   "screenshot [p]"  the next presented game frame (the window texture, before the present's scaling) as a PNG, by default ctl-shot-<frame>.png beside the control file
//   "stats"           one log line now: the last 60 frames' averages, the frame rate, placed memory, pipelines, the switches set live
// Every answer is a log line starting "[vk] ctl:" (android/tools/nxctl.sh waits for them)
#if defined(__ANDROID__)
#include "vka.h"
#include "vk_android.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

_Atomic uint32_t vka_env_gen = 1;   // 1: call sites' zeroed caches read their switch on the first call

// ---------- Switches ----------
enum { NOVR = 64 };
static struct { char key[64]; char val[256]; int set; } g_ovr[NOVR];   // the switches set live; set 0 = cleared (reads as unset)
static int g_novr;
static pthread_mutex_t g_emu = PTHREAD_MUTEX_INITIALIZER;
static struct { char key[64]; char val[256]; } g_pend[NOVR];   // queued by the control thread, applied at the frame boundary
static int g_npend;
static _Atomic int g_pending; static _Atomic uint64_t g_pend_frame;
uint64_t vka_frame(void);

static const char *ovr_find(const char *key, int *found) {   // under g_emu
    for (int i = 0; i < g_novr; i++) if (!strcmp(g_ovr[i].key, key)) { *found = 1; return g_ovr[i].set ? g_ovr[i].val : NULL; }
    *found = 0; return NULL;
}
static int str_get(const char *key, char *out, size_t n) {   // 1 = set (out holds the value)
    pthread_mutex_lock(&g_emu);
    int found; const char *v = ovr_find(key, &found);
    if (!found) v = getenv(key);
    int r = v && *v; if (r) snprintf(out, n, "%s", v);
    pthread_mutex_unlock(&g_emu);
    return r;
}
int vka_env_str(const char *key, char *out, size_t n) { return str_get(key, out, n); }
double vka_env_slow(vka_envc *c, const char *key, double def) {
    uint32_t g = atomic_load(&vka_env_gen);   // before the value: a change applied meanwhile makes the next call read again
    char b[256]; double v = str_get(key, b, sizeof b) ? strtod(b, NULL) : def;
    // Refresh the cache only to a newer generation, one thread at a time (gen UINT32_MAX while v is written: readers take this slow path meanwhile); a thread whose g is
    // older than the cache's, or that loses the race, returns its value uncached
    uint32_t cur = atomic_load_explicit(&c->gen, memory_order_relaxed);
    if (cur != UINT32_MAX && (int32_t)(g - cur) > 0 && atomic_compare_exchange_strong_explicit(&c->gen, &cur, UINT32_MAX, memory_order_acquire, memory_order_relaxed)) {
        atomic_store_explicit(&c->v, v, memory_order_relaxed); atomic_store_explicit(&c->gen, g, memory_order_release);
    }
    return v;
}

// Switches read once (at start, or when a device object is made): a live change is logged as taking effect at the next start
static const char *const g_restart[] = {"DM_VK_VALIDATE", "DM_VK_HOSTMEM", "DM_VK_OBJCOUNT", "DM_VK_DRIVER", "DM_VK_HOOKDIR", "DM_VK_PLACED", "DM_VK_PREFAULT", "DM_VK_PCACHE", "DM_VK_GUEST_FPCR", "DM_VK_SHLIB", "DM_VK_SHLIB_CHECK", "DM_VK_GS_CLAIM", "DM_VK_GS_FILL", "DM_VK_SUBGROUP_PIN", "DM_VK_GPL", "DM_VK_MESH", "DM_VK_SUBGROUP_GS", "DM_VK_SHUFFLE_LDS", "DM_VK_ADPF", "DM_VK_ADPF_PIPELINE", "DM_VK_GPU_FLOOR_KGSL", "DM_VK_NANSCAN",
                                        "DM_VK_NANSCAN_STEP", "DM_VK_NANSCAN_DRAWS", "DM_VK_NANSCAN_SERIAL", "DM_VK_NANSCAN_FS", "DM_VK_WORKER",
                                        "DM_VK_WORKER_FRAMES", "DM_VK_TEST_HANG", "DM_LSFG_DLL", "DM_FPS", "NXHOST_VK_SELFTEST", "DM_CTL", "DM_VK_PREWARM", "DM_VK_PREWARM_MAX", "DM_VK_PREWARM_FILE", "DM_VK_LC_GLOBAL",
                                        "DM_VK_PSO_ASYNC", "DM_NVN_GPU_TS", "DM_VK_POOL_FB", "DM_VK_POOL_FB_EARLY", "DM_VK_EAGER_TEX", NULL};
// Switches the replay reads again after a change (VKA_ENV, or at use); a table, so the log line can say which kind a key is
static const char *const g_live[] = {"DM_VK_DRAW", "DM_VK_STATS", "DM_VK_HANG_S", "DM_VK_HANG_EXIT", "DM_VK_FRAME_RATE", "DM_VK_FRAME_RATE_HOLD", "DM_VK_GENLOCK", "DM_VK_PCACHE_SAVE_S", "DM_VK_GPL_GIVEUP", "DM_VK_GPL_PROBE", "DM_VK_PREWARM_KEEP", "DM_VK_PRETRANSFORM", "DM_VK_FSR", "DM_VK_FSR_SHARP", "DM_VK_TEX_TRACK",
                                     "DM_VK_CHUNK", "DM_VK_PASS_CLEARS", "DM_VK_PASS_KEEP", "DM_VK_DYN_FILTER", "DM_VK_PROF", "DM_VK_ROWS", "DM_VK_SYNCLOG", "DM_VK_UBWC", "DM_VK_IMG_UPGRADE", "DM_VK_DONE_LAST", "DM_VK_AFF_REPLAY", "DM_VK_AFF_ALL",
                                     "DM_VK_NANSCAN_DUMP", "DM_VK_VB_FILTER", "DM_VK_ENC_BATCH", "DM_VK_BAR_KEEP", "DM_VK_DRV_MEMO", "DM_VK_PUSH_DELTA", "DM_VK_UPLOAD_MT", "DM_VK_UPLOAD_MT_MIN",
                                     "DM_VK_NDSR", "DM_VK_NDSR_INPUT", "DM_VK_NDSR_MAP", "DM_VK_NDSR_LOG", "DM_VK_PRESENT_NEAREST", "DM_VK_STRETCH",
                                     "DM_LSFG", "DM_LSFG_MULT", "DM_LSFG_TARGET", "DM_LSFG_FLOW", "DM_VK_POOL_FB_BLIND_EAGER", "DM_VK_POOL_FB_DEAD", "DM_VK_PRUNE_RETIRED", "DM_VK_PRUNE_DRY", "DM_VK_DISPATCH_ORDER", "DM_VK_BAR_NARROW",
                                     "DM_VK_RP_SCOPE", "DM_VK_RP_SCOPE_NARROW", "DM_VK_BAR_DEDUP", "DM_VK_MESH_GUEST_BAR", "DM_VK_POOL_SLOT_DEDUP", NULL};
static int in(const char *const *t, const char *k) { for (; *t; t++) if (!strcmp(*t, k)) return 1; return 0; }

void dm_gfx_ctl_env(const char *key, const char *val) {   // the control thread
    if (!*key || strlen(key) >= sizeof g_pend[0].key || strlen(val) >= sizeof g_pend[0].val) { VLOG("ctl: env %s: key or value too long", key); return; }
    pthread_mutex_lock(&g_emu);
    int i = 0; while (i < g_npend && strcmp(g_pend[i].key, key)) i++;
    if (i == g_npend && g_npend < NOVR) g_npend++;
    if (i < NOVR) { snprintf(g_pend[i].key, sizeof g_pend[i].key, "%s", key); snprintf(g_pend[i].val, sizeof g_pend[i].val, "%s", val); }
    pthread_mutex_unlock(&g_emu);
    atomic_store(&g_pend_frame, vka_frame()); atomic_store(&g_pending, 1);
    VLOG("ctl: env %s=%s queued for the next frame (frame %llu now)", key, val, (unsigned long long)vka_frame());
}
static int lsfg_int(const char *k, int def) { char b[64]; return str_get(k, b, sizeof b) ? atoi(b) : def; }
static void apply(void) {
    int lsfg = 0, pre = 0; char done[1024]; size_t dn = 0; done[0] = 0;
    pthread_mutex_lock(&g_emu);
    static typeof(g_pend[0]) p[NOVR];   // apply runs under the submit lock: one caller at a time
    int n = g_npend; memcpy(p, g_pend, (size_t)n * sizeof p[0]); g_npend = 0;
    for (int i = 0; i < n; i++) {
        int j = 0; while (j < g_novr && strcmp(g_ovr[j].key, p[i].key)) j++;
        if (j == g_novr) { if (g_novr == NOVR) { VRED("ctl: more than %d live switches: %s dropped", NOVR, p[i].key); continue; } g_novr++; }
        snprintf(g_ovr[j].key, sizeof g_ovr[j].key, "%s", p[i].key); snprintf(g_ovr[j].val, sizeof g_ovr[j].val, "%s", p[i].val); g_ovr[j].set = p[i].val[0] != 0;
        if (p[i].val[0]) setenv(p[i].key, p[i].val, 1); else unsetenv(p[i].key);
        lsfg |= !strncmp(p[i].key, "DM_LSFG", 7) && strcmp(p[i].key, "DM_LSFG_DLL"); pre |= !strcmp(p[i].key, "DM_VK_PRETRANSFORM");
        const char *kind = in(g_restart, p[i].key) ? "restart only: takes effect at the next start" : in(g_live, p[i].key) ? "live"
                         : !strncmp(p[i].key, "DM_VK_", 6) ? "not in the live table: live where its reader uses VKA_ENV" : "set in the environment (live where the host reads it at use)";
        VLOG("ctl: env %s=%s applied at frame %llu (%s)", p[i].key, p[i].val, (unsigned long long)vka_frame(), kind);
        dn += (size_t)snprintf(done + dn, dn < sizeof done ? sizeof done - dn : 0, "%s%s", dn ? " " : "", p[i].key); if (dn >= sizeof done) dn = sizeof done - 1;
    }
    pthread_mutex_unlock(&g_emu);
    atomic_fetch_add(&vka_env_gen, 1);
    (void)pre;   // the present reads DM_VK_PRETRANSFORM through VKA_ENV and makes a new swapchain when it changed (vka_present.c)
    if (lsfg) { nxhost_framegen_set(lsfg_int("DM_LSFG", 0) != 0, lsfg_int("DM_LSFG_MULT", 2), lsfg_int("DM_LSFG_TARGET", 0), lsfg_int("DM_LSFG_FLOW", 75));
                nxhost_framegen_quality(lsfg_int("DM_LSFG_QUALITY", 0) != 0); }
}
void vka_ctl_sub(void) {   // mtl_sub_begin: the submit lock is held, no submission is being recorded
    if (!atomic_load_explicit(&g_pending, memory_order_relaxed) || vka_frame() == atomic_load(&g_pend_frame)) return;
    atomic_store(&g_pending, 0);
    apply();
}

// ---------- Frame statistics: capture and stats ----------
extern uint64_t vka_n_rp, vka_n_bar;   // vka_draw.c, vka_tex.c
void vka_exec_counts(uint64_t *ops, uint64_t *subs, uint64_t *rec_ns);   // vka_exec.c
double mtl_gpu_ms(uint64_t f);
enum { NREC = 128 };
typedef struct { uint64_t frame, t, draws, skipped, disp, rp, bar, ops, subs, rec_ns; } frec;   // counters at the start of frame
static frec g_rec[NREC];
static _Atomic long g_capture; static uint64_t g_cap_from, g_cap_to; static _Atomic int g_stats;
static void snap(frec *r, uint64_t f) {
    r->frame = f; r->t = now_ns(); uint64_t pipes; vka_draw_stats(&r->draws, &r->skipped, &r->disp, &pipes);
    r->rp = vka_n_rp; r->bar = vka_n_bar; vka_exec_counts(&r->ops, &r->subs, &r->rec_ns);
}
static void cap_line(uint64_t f) {   // frame f, once its GPU time is in (8 frames on)
    const frec *a = &g_rec[f % NREC], *b = &g_rec[(f + 1) % NREC];
    if (a->frame != f || b->frame != f + 1) return;
    VLOG("ctl: capture frame %llu: %.2f ms, %llu draws (%llu skipped), %llu dispatches, %llu render passes, %llu full barriers, %llu clears / copies, %llu submissions; recording %.2f ms, GPU %.2f ms",
         (unsigned long long)f, (b->t - a->t) / 1e6, (unsigned long long)(b->draws - a->draws), (unsigned long long)(b->skipped - a->skipped), (unsigned long long)(b->disp - a->disp),
         (unsigned long long)(b->rp - a->rp), (unsigned long long)(b->bar - a->bar), (unsigned long long)(b->ops - a->ops), (unsigned long long)(b->subs - a->subs),
         (b->rec_ns - a->rec_ns) / 1e6, mtl_gpu_ms(f));
}
static void stats_line(uint64_t f) {
    int n = 60; while (n > 1 && g_rec[(f - (uint64_t)n) % NREC].frame != f - (uint64_t)n) n--;
    const frec *a = &g_rec[(f - (uint64_t)n) % NREC], *b = &g_rec[f % NREC];
    double k = n, gpu = 0; int ng = 0;
    for (uint64_t i = f - (uint64_t)n; i < f; i++) { double g = mtl_gpu_ms(i); if (g >= 0) { gpu += g; ng++; } }
    float fps[5] = {0}; nxhost_fps(fps, 5); uint64_t peak, placed = vka_mem_placed_bytes(&peak), img_gw; int img_n; uint64_t img = vka_tex_mem(&img_n, &img_gw);
    char sw[600] = ""; size_t sn = 0;
    pthread_mutex_lock(&g_emu);
    for (int i = 0; i < g_novr && sn < sizeof sw; i++) sn += (size_t)snprintf(sw + sn, sizeof sw - sn, " %s=%s", g_ovr[i].key, g_ovr[i].set ? g_ovr[i].val : "(unset)");
    pthread_mutex_unlock(&g_emu);
    VLOG("ctl: stats at frame %llu, last %d frames: %.1f fps (%.1f presented), %.2f ms a frame; per frame %.0f draws (%.0f skipped), %.0f dispatches, %.0f render passes, %.0f full barriers, "
         "%.0f clears / copies, %.1f submissions, recording %.2f ms, GPU %.2f ms (%d frames); placed memory %.0f MB (peak %.0f); images %.0f MB (%d; GPU-written %.0f MB, from storage %.0f MB); switches set live:%s",
         (unsigned long long)f, n, fps[0], fps[1], (b->t - a->t) / 1e6 / k, (b->draws - a->draws) / k, (b->skipped - a->skipped) / k, (b->disp - a->disp) / k, (b->rp - a->rp) / k,
         (b->bar - a->bar) / k, (b->ops - a->ops) / k, (b->subs - a->subs) / k, (b->rec_ns - a->rec_ns) / 1e6 / k, ng ? gpu / ng : -1.0, ng, placed / 1e6, peak / 1e6, img / 1e6, img_n, img_gw / 1e6, (img - img_gw) / 1e6, sn ? sw : " none");
    { char ab[400]; vka_tex_alias_stats(ab, sizeof ab); VLOG("ctl: %s", ab); }
    { char im[600]; vka_imem_stats(im, sizeof im); VLOG("ctl: %s", im); }
    { char bs[1000]; vka_tex_big_stats(bs, sizeof bs); if (bs[0]) VLOG("ctl: %s", bs); }
}
static uint64_t census_value(const char *text, const char *field) {
    size_t n = strlen(field);
    for (const char *p = text; *p;) {
        if (!strncmp(p, field, n)) return strtoull(p + n, NULL, 10);
        const char *end = strchr(p, '\n'); if (!end) break; p = end + 1;
    }
    return 0;
}
static void census_read(const char *path, char *out, size_t size) {
    out[0] = 0; FILE *f = fopen(path, "re"); if (!f) return;
    size_t n = fread(out, 1, size - 1, f); out[n] = 0; fclose(f);
}
// What the census says of the system and of the kind of memory the process holds, since 2026-11-14: a game killed from outside (signal 9) in the first frame of a
// scene on a Redmagic 11 Air, five runs, with "available 5337 MiB" in its last census and "swap PSS 0 MiB" in every one (Ocarina of Time's note). Whether that phone
// has swap at all, how full it is, how much of the resident set is anonymous (cannot go anywhere without swap) or file pages (can), and whether huge pages inflate a
// sparsely touched heap were not in the log. The system's side once (census_system) and a second line with each census; and a census as soon as the resident set
// moved 256 MiB (statm, read each second: cheap, unlike smaps_rollup), because the ten-second one never caught a scene's first seconds
static void census_line(const char *path, char *out, size_t size) {   // a one-line file, or "not readable (<why>)"
    FILE *f = fopen(path, "re"); if (!f) { snprintf(out, size, "not readable (%s)", strerror(errno)); return; }
    size_t n = fread(out, 1, size - 1, f); out[n] = 0; fclose(f);
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    for (char *p = out; *p; p++) if (*p == '\n') *p = ';';
}
static void census_system(void) {
    char thp[160], cg[400], adj[32], swp[32]; census_line("/sys/kernel/mm/transparent_hugepage/enabled", thp, sizeof thp); census_line("/proc/self/cgroup", cg, sizeof cg);
    census_line("/proc/self/oom_score_adj", adj, sizeof adj); census_line("/proc/sys/vm/swappiness", swp, sizeof swp);
    VLOG("memory census system: transparent huge pages %s; swappiness %s; this process: oom_score_adj %s, cgroup %s", thp, swp, adj, cg);
    // The memory limits of the process's cgroup, when it has a memory controller and the app may read them (a kill by the kernel for a cgroup's limit reads as
    // signal 9 with memory to spare in the system): the unified hierarchy's "0::<path>" under /sys/fs/cgroup, or a v1 "N:memory:<path>" under /dev/memcg
    char path[300] = "", *p; int v2 = 0;
    for (p = cg; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        char *c1 = strchr(p, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL, *end = strchr(p, ';'); if (!c2) break;
        int mem = c2 - c1 == 7 && !strncmp(c1 + 1, "memory", 6), uni = c2 == c1 + 1;
        if (mem || (uni && !path[0])) { snprintf(path, sizeof path, "%s%.*s", mem ? "/dev/memcg" : "/sys/fs/cgroup", (int)(end ? end - c2 - 1 : (long)strlen(c2 + 1)), c2 + 1); v2 = !mem; if (mem) break; }
    }
    if (path[0]) {
        static const char *const f2[] = {"memory.max", "memory.high", "memory.current", "memory.swap.max", "memory.events"}, *const f1[] = {"memory.limit_in_bytes", "memory.usage_in_bytes", "memory.memsw.limit_in_bytes", "memory.swappiness", "memory.oom_control"};
        char line[900]; int n = snprintf(line, sizeof line, "memory census cgroup %s:", path);
        for (int i = 0; i < 5 && n < (int)sizeof line - 200; i++) { char file[400], val[160]; snprintf(file, sizeof file, "%s/%s", path, v2 ? f2[i] : f1[i]); census_line(file, val, sizeof val);
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %s;", v2 ? f2[i] : f1[i], val); }
        VLOG("%s", line);
    }
}
static uint64_t census_rss_kb(void) {   // /proc/self/statm: size resident shared ... in pages
    char b[128]; census_read("/proc/self/statm", b, sizeof b); unsigned long long sz = 0, rss = 0;
    return sscanf(b, "%llu %llu", &sz, &rss) == 2 ? rss * (uint64_t)sysconf(_SC_PAGESIZE) / 1024 : 0;
}
static void *memory_census_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-memstats"); setpriority(PRIO_PROCESS, 0, 10);
    uint64_t rss_logged = 0; int since = 0, first = 1;
    for (;;) {
        int seconds = VKA_ENV("DM_VK_MEM_CENSUS_S", 10);
        struct timespec delay = {1, 0}; nanosleep(&delay, NULL);
        if (seconds <= 0 || vka_hung()) continue;
        uint64_t rss_now = census_rss_kb(); int moved = rss_logged && (rss_now > rss_logged ? rss_now - rss_logged : rss_logged - rss_now) >= 256 * 1024;
        if (++since < seconds && !moved) continue;
        since = 0; rss_logged = rss_now;
        if (first) { first = 0; census_system(); }
        char mem[2048], roll[4096]; census_read("/proc/meminfo", mem, sizeof mem);
        // (the rollup walks every mapping and its page tables under the process's mmap lock, which the game's threads need for mprotect and page faults: timed, since a slow
        // read here would be a hitch every DM_VK_MEM_CENSUS_S seconds)
        struct timespec r0, r1; clock_gettime(CLOCK_MONOTONIC, &r0); census_read("/proc/self/smaps_rollup", roll, sizeof roll); clock_gettime(CLOCK_MONOTONIC, &r1);
        double roll_ms = (r1.tv_sec - r0.tv_sec) * 1e3 + (r1.tv_nsec - r0.tv_nsec) / 1e6;
        struct timespec at; clock_gettime(CLOCK_MONOTONIC, &at);
        int images = 0, active = 0, queued = 0; uint64_t gpu = 0;
        uint64_t bytes = vka_tex_mem(&images, &gpu), pending = vka_tex_retired_bytes();
        uint32_t blocks = 0; uint64_t staging = vka_scratch_bytes(&blocks);
        vka_pso_work_stats(&active, &queued);
        struct rusage usage = {0}; getrusage(RUSAGE_SELF, &usage);
        VLOG("memory census t=%lld: RSS %.0f MiB, resident PSS %.0f MiB, swap PSS %.0f MiB (rollup %s in %.1f ms); available %.0f MiB; renderer accounting images %.0f MiB (%d, GPU-written %.0f, pending %.0f), pools %.0f, staging %.0f (%u blocks); compiler %d active/%d queued; major faults %ld",
             (long long)at.tv_sec, census_value(roll, "Rss:") / 1024.0, census_value(roll, "Pss:") / 1024.0, census_value(roll, "SwapPss:") / 1024.0, roll[0] ? "read" : "unavailable", roll_ms,
             census_value(mem, "MemAvailable:") / 1024.0, bytes / 1048576.0, images, gpu / 1048576.0, pending / 1048576.0, vka_mem_placed_nolock() / 1048576.0, staging / 1048576.0, blocks, active, queued, usage.ru_majflt);
        VLOG("memory census kinds%s: this process anonymous %.0f MiB, file %.0f MiB, shared memory %.0f MiB (PSS), anonymous huge pages %.0f MiB, swapped %.0f MiB; the system: total %.0f MiB, free %.0f MiB, file cache %.0f MiB, swap %.0f of %.0f MiB free",
             moved ? " (the resident set moved 256 MiB)" : "", census_value(roll, "Pss_Anon:") / 1024.0, census_value(roll, "Pss_File:") / 1024.0, census_value(roll, "Pss_Shmem:") / 1024.0,
             census_value(roll, "AnonHugePages:") / 1024.0, census_value(roll, "Swap:") / 1024.0, census_value(mem, "MemTotal:") / 1024.0, census_value(mem, "MemFree:") / 1024.0,
             census_value(mem, "Cached:") / 1024.0, census_value(mem, "SwapFree:") / 1024.0, census_value(mem, "SwapTotal:") / 1024.0);
        char detail[700]; vka_imem_stats(detail, sizeof detail); VLOG("memory census allocator: %s", detail);
    }
    return NULL;
}
static void memory_census_start(void) {
    pthread_t thread;
    if (!pthread_create(&thread, NULL, memory_census_main, NULL)) pthread_detach(thread);
}
void vka_ctl_frame(uint64_t f) {   // mtl_set_frame: frame f begins
    static pthread_once_t census_once = PTHREAD_ONCE_INIT; pthread_once(&census_once, memory_census_start);
    snap(&g_rec[f % NREC], f);
    long c = atomic_load_explicit(&g_capture, memory_order_relaxed);
    if (c > 0) { if (!g_cap_from) { g_cap_from = f; g_cap_to = f + (uint64_t)c; VLOG("ctl: capture of frames %llu-%llu begins", (unsigned long long)f, (unsigned long long)(g_cap_to - 1)); } atomic_store(&g_capture, 0); }
    if (g_cap_from && f >= 9 && f - 9 >= g_cap_from) {   // GPU time is in 8 frames late
        cap_line(f - 9);
        if (f - 9 + 1 >= g_cap_to) { VLOG("ctl: capture done"); g_cap_from = g_cap_to = 0; }
    }
    if (atomic_exchange_explicit(&g_stats, 0, memory_order_relaxed)) stats_line(f);
    { void vka_mem_lazy_diag(void); vka_mem_lazy_diag(); }
    { void vka_mem_cohere(void); vka_mem_cohere(); }
    { void vka_img_stat_diag(void); vka_img_stat_diag(); }
    { void vka_texstat_flush(void); vka_texstat_flush(); }
}

// ---------- Screenshot ----------
// The present thread copies the window texture's crop into a host-visible buffer in the present's command buffer (vka_ctl_shot_record), waits for that submission and hands the
// texels to a thread that writes the PNG (zlib, the NDK's)
static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;
static char g_shot_path[1024]; static _Atomic int g_shot_want;
static VkBuffer g_sbuf; static VkDeviceMemory g_smem; static void *g_smap; static VkDeviceSize g_ssize;
static struct { int on, w, h, fmt, flip; uint64_t frame; } g_rec_shot;
static char g_ctl_dir[1024];
static void ctl_dir(void) {
    if (g_ctl_dir[0]) return;
    const char *c = getenv("DM_CTL"); snprintf(g_ctl_dir, sizeof g_ctl_dir, "%s", c && *c ? c : getenv("HOME") ? getenv("HOME") : "/data/local/tmp/x");
    char *s = strrchr(g_ctl_dir, '/'); if (s) *s = 0;
}
void dm_gfx_shot_at(long k, const char *path) {   // the Mac's "shot <path>" too (k: frames ahead; the next frame here)
    (void)k; pthread_mutex_lock(&g_smu); snprintf(g_shot_path, sizeof g_shot_path, "%s", path ? path : ""); pthread_mutex_unlock(&g_smu); atomic_store(&g_shot_want, 1);
}
static int shot_buf(VkDeviceSize n) {
    if (g_ssize >= n) return 1;
    if (g_sbuf) { vk.vkDestroyBuffer(V.dev, g_sbuf, NULL); vk.vkFreeMemory(V.dev, g_smem, NULL); g_sbuf = VK_NULL_HANDLE; g_smem = VK_NULL_HANDLE; g_ssize = 0; }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(V.dev, &bi, NULL, &g_sbuf)) return 0;
    VkMemoryRequirements mr; vk.vkGetBufferMemoryRequirements(V.dev, g_sbuf, &mr);
    uint32_t t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (t == UINT32_MAX) t = vka_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = t};
    if (t == UINT32_MAX || vk.vkAllocateMemory(V.dev, &ai, NULL, &g_smem) || vk.vkBindBufferMemory(V.dev, g_sbuf, g_smem, 0) || vk.vkMapMemory(V.dev, g_smem, 0, VK_WHOLE_SIZE, 0, &g_smap)) {
        if (g_smem) vk.vkFreeMemory(V.dev, g_smem, NULL);
        vk.vkDestroyBuffer(V.dev, g_sbuf, NULL); g_sbuf = VK_NULL_HANDLE; g_smem = VK_NULL_HANDLE; return 0;
    }
    g_ssize = n; return 1;
}
static int shot_bpp(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 0;
    }
}
void vka_ctl_shot_record(VkCommandBuffer cb, vka_img *im, int cx, int cy, int cw, int ch) {   // present thread, before vkEndCommandBuffer
    g_rec_shot.on = 0;
    if (!atomic_load_explicit(&g_shot_want, memory_order_relaxed) || !im || !im->inited) return;
    atomic_store(&g_shot_want, 0);
    int bpp = shot_bpp(im->f.vk);
    if (!bpp || im->samples != 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { VLOG("ctl: screenshot: the window texture's format %d is not one the writer takes (RGBA8, BGRA8, RGB10A2, RGBA16F)", im->f.vk); return; }
    if (cx < 0 || cy < 0 || cw <= 0 || ch <= 0 || (uint32_t)(cx + cw) > im->w || (uint32_t)(cy + ch) > im->h) { cx = cy = 0; cw = (int)im->w; ch = (int)im->h; }
    if (!shot_buf((VkDeviceSize)cw * ch * bpp)) { VLOG("ctl: screenshot: no host-visible buffer of %d x %d", cw, ch); return; }
    vka_barrier(cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {cx, cy, 0}, {(uint32_t)cw, (uint32_t)ch, 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, g_sbuf, 1, &r);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    g_rec_shot.on = 1; g_rec_shot.w = cw; g_rec_shot.h = ch; g_rec_shot.fmt = im->f.vk; g_rec_shot.flip = vka_origin_ll(); g_rec_shot.frame = vka_frame();
}
typedef struct { uint8_t *rgb; int w, h; char path[1100]; uint64_t frame; } shot_job;
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; put32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4); if (n) c = crc32(c, d, n); uint8_t cc[4]; put32(cc, (uint32_t)c); fwrite(cc, 1, 4, f);
}
static void *shot_main(void *arg) {
    shot_job *j = arg; pthread_setname_np(pthread_self(), "vk-shot");
    size_t row = (size_t)j->w * 3 + 1, raw = row * (size_t)j->h; uLongf zn = compressBound((uLong)raw);
    uint8_t *flt = malloc(raw), *z = malloc(zn); FILE *f = NULL;
    if (flt && z) {
        for (int y = 0; y < j->h; y++) { flt[row * y] = 0; memcpy(flt + row * y + 1, j->rgb + (size_t)y * j->w * 3, (size_t)j->w * 3); }
        if (compress2(z, &zn, flt, (uLong)raw, 3) == Z_OK && (f = fopen(j->path, "wb"))) {
            static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; fwrite(sig, 1, 8, f);
            uint8_t ih[13]; put32(ih, (uint32_t)j->w); put32(ih + 4, (uint32_t)j->h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
            chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)zn); chunk(f, "IEND", NULL, 0);
            fclose(f); chmod(j->path, 0644);
            VLOG("ctl: screenshot of frame %llu, %dx%d → %s", (unsigned long long)j->frame, j->w, j->h, j->path);
        } else VLOG("ctl: screenshot: cannot write %s", j->path);
    }
    free(flt); free(z); free(j->rgb); free(j); return NULL;
}
static uint8_t half8(uint16_t h) {   // an RGBA16F channel, clamped to [0, 1] and sRGB-encoded roughly (gamma 1/2.2)
    uint32_t e = h >> 10 & 31, m = h & 1023; if (h & 0x8000) return 0;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 1.0f : (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f;
    if (!(v > 0)) return 0; if (v >= 1) return 255;
    return (uint8_t)(powf(v, 1 / 2.2f) * 255 + 0.5f);
}
void vka_ctl_shot_done(VkFence fence) {   // present thread, after the present's submission
    if (!g_rec_shot.on) return;
    g_rec_shot.on = 0;
    if (vk.vkWaitForFences(V.dev, 1, &fence, VK_TRUE, 2000000000ull)) { VLOG("ctl: screenshot: the present's copy did not finish in 2 s"); return; }
    int w = g_rec_shot.w, h = g_rec_shot.h, fmt = g_rec_shot.fmt;
    shot_job *j = calloc(1, sizeof *j); uint8_t *rgb = j ? malloc((size_t)w * h * 3) : NULL;
    if (!rgb) { free(j); return; }
    for (int y = 0; y < h; y++) {
        int sy = g_rec_shot.flip ? h - 1 - y : y; uint8_t *d = rgb + (size_t)y * w * 3;
        if (fmt == VK_FORMAT_R16G16B16A16_SFLOAT) { const uint16_t *s = (const uint16_t *)g_smap + (size_t)sy * w * 4; for (int x = 0; x < w; x++) { d[3 * x] = half8(s[4 * x]); d[3 * x + 1] = half8(s[4 * x + 1]); d[3 * x + 2] = half8(s[4 * x + 2]); } }
        else {
            const uint8_t *s = (const uint8_t *)g_smap + (size_t)sy * w * 4;
            for (int x = 0; x < w; x++, s += 4) {
                if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { uint32_t p; memcpy(&p, s, 4); d[3 * x] = (uint8_t)((p & 1023) >> 2); d[3 * x + 1] = (uint8_t)((p >> 10 & 1023) >> 2); d[3 * x + 2] = (uint8_t)((p >> 20 & 1023) >> 2); }
                else if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) { d[3 * x] = s[2]; d[3 * x + 1] = s[1]; d[3 * x + 2] = s[0]; }
                else { d[3 * x] = s[0]; d[3 * x + 1] = s[1]; d[3 * x + 2] = s[2]; }
            }
        }
    }
    j->rgb = rgb; j->w = w; j->h = h; j->frame = g_rec_shot.frame;
    pthread_mutex_lock(&g_smu);
    if (g_shot_path[0]) snprintf(j->path, sizeof j->path, "%s", g_shot_path);
    else { ctl_dir(); snprintf(j->path, sizeof j->path, "%s/ctl-shot-%llu.png", g_ctl_dir, (unsigned long long)j->frame); }
    pthread_mutex_unlock(&g_smu);
    pthread_t th; if (pthread_create(&th, NULL, shot_main, j)) { free(rgb); free(j); return; }
    pthread_detach(th);
}

// ---------- Commands ----------
int dm_gfx_ctl(const char *line) {   // the control thread (hle_hid.c ctl_main): 1 = a replay command, taken
    if (!strncmp(line, "framecapture ", 13)) {
        int vka_capture_arm(const char *path);
        VLOG("ctl: framecapture %s", vka_capture_arm(line + 13) ? "queued for the next complete frame" : "refused (absolute path required, or request pending)");
        return 1;
    }
    if (!strncmp(line, "capture", 7) && (!line[7] || line[7] == ' ')) {
        long n = line[7] ? atol(line + 8) : 60; if (n <= 0) n = 60; if (n > NREC - 16) n = NREC - 16;
        if (g_cap_from) { VLOG("ctl: capture: one is running (frames %llu-%llu)", (unsigned long long)g_cap_from, (unsigned long long)(g_cap_to - 1)); return 1; }
        atomic_store(&g_capture, n); VLOG("ctl: capture of the next %ld frames queued", n); return 1;
    }
    if (!strncmp(line, "screenshot", 10) && (!line[10] || line[10] == ' ')) {
        const char *p = line[10] ? line + 11 : ""; while (*p == ' ') p++;
        dm_gfx_shot_at(1, p); VLOG("ctl: screenshot of the next frame queued%s%s", *p ? " → " : "", p); return 1;
    }
    if (!strcmp(line, "stats")) { atomic_store(&g_stats, 1); return 1; }
    return 0;
}
#endif
