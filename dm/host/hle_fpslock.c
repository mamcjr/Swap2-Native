// Engine frame limits kept in game memory (FPSLocker's evaluate_write entries): some engines cap their own frame rate with a value the host can raise. The target is the profile's
// fps_target (DM_FPS_TARGET overrides; 0 = leave the game's own limit), the same key that drives Tears of the Kingdom's frame step (loader.c install_fps_state). Called on every present
// (hle_nvn.c); the game sets the values after boot and may set them again, so each is written whenever it differs. Fixed addresses: each row runs only on the main build it was found in (AGENTS.md).
// Unreal Engine needs no row: the rewriter finds its console variables by their registered names in any build (dm/tools/ue_cvar.py, main's fixups UE_CVAR:<name>), and the host writes
// the frame limit, the profile's ue_fps_cvar (t.MaxFPS, or a game's own variable its settings apply to t.MaxFPS: SHIN MEGAMI TENSEI V: Vengeance's Project.FrameRateLimit), and the
// present interval, rhi.SyncInterval (nvn.SyncInterval in UE 4.18's Switch RHI: DRAGON QUEST XI S). Each is a TAutoConsoleVariable whose +0x10 points to its value, the game thread's
// copy then the render thread's. The games' configs set the limit to 30 and the interval to 2 (their notes name the .ini files)
#include "hle.h"
#include <stdatomic.h>

// What a field holds: the frame rate as a float, double or int32, vsyncs per frame (60 / frame rate) as an int32, or the frame time in milliseconds as a float. Copies: consecutive copies of the
// value (Unreal keeps one for the game thread, one for the render thread). A row with no pointer (ptr_off 0) has its field at main + field: a constant in main's read-only data, which the loader
// leaves writable (the image's data block on iOS too, loader.c map_image).
// At 60 a frame time gets MS_MARGIN less than the vsync: such a limiter counts from the frame's start, and the work after it (the GPU fence wait, the next frame's start) adds to the
// period, so frames at exactly 16.667 ms came 0.11 ms late each on the Mac (59.6 fps, every 150th frame shown two vsyncs). Slightly shorter, the host's present pacing sets the pace
// (the acquire waits): Luigi's Mansion 3's display interval p99 18.69 → 16.67 ms, 60.0 fps. Not at 30: with present_interval display the presents follow every vsync and hold no
// frame for two, so a shorter time showed some frames for one (30.4 fps); 30 writes the game's own 33.333 ms
// Also: a frame time in ticks as a uint32 (scale / frame rate: 19,200,000 for the Switch's 19.2 MHz system tick), a frame step in seconds as a float (1 / frame rate), and a mode
// switch an engine reads before it takes those fields at all (written 1 at any target)
// And a floor: the least vsyncs per frame (60 / frame rate) a dynamic present interval controller may pick (sead's, Donkey Kong Bananza). Unlike the others it does not fix the
// rate: the game still raises its interval when frames run long and steps its logic by the interval it picked, so presentation keeps following the game's interval
// (dm_fps_lock_engine leaves floors out). Target 60 writes 1, the controller's own floor. The int32 after the floor is the controller's own maximum, which the game sets (3 in
// Donkey Kong Bananza): a floor above it breaks the controller's min <= max, and DKB's then stayed at the floor (interval 4 at a 15 cap) and never came back down when the host
// put its floor back, until the game restarted. So a floor row never writes above that maximum, and a rate needing more vsyncs than that is not offered (row_holds)
// The vsync kinds hold only rates that divide 60 (60, 30, 20, 15); the rate kinds any target (rate_ok)
enum { FPS_F32, VSYNCS_I32, MS_F32, FPS_I32, TICKS_U32, SECS_F32, MODE_U32, FPS_F64, MIN_VSYNCS_I32 };
#define MS_MARGIN 0.5f
static float ms_want(int fps) { return 1000.0f / (float)fps - (fps == 60 ? MS_MARGIN : 0); }
// deref: after [main + ptr_off], nderef more pointers, each at the object reached so far + deref[i] (an object behind a global, then members)
typedef struct { const char *build, *what; uint64_t ptr_off; uint32_t field; uint8_t kind, copies; uint32_t scale; uint8_t nderef; uint16_t deref[3]; } lock_t;
static int vsync_kind(int kind) { return kind == VSYNCS_I32 || kind == MIN_VSYNCS_I32; }
static int rate_ok(int kind, int target) { return !vsync_kind(kind) || 60 % target == 0; }
static const lock_t k_locks[] = {
    // Donkey Kong Bananza v458752: sead's dynamic present interval controller (main+0x204a18, called each frame from the framework's main+0x203674). It picks the interval from
    // the last frame's start to the later of its submission and its GPU end, clamps it to [+0, +4] (its settings, 1 and 3, made in nnMain at main+0x5bf180 and copied in by
    // main+0x5bfb20), and hands 60 / interval to the framework's frame rate (main+0x204c60, framework+0xe0) and the interval to the window (main+0x204c00, framework+0x88): the
    // game's logic steps by that interval, as BotW's VFR does (docs/reference/frame-rate.md). A floor of 2 makes the game itself run at 30 and stay at the right speed.
    // The controller is at [framework+0x218] (main+0x5bfb00); the framework at [app], the app object at main+0x26cc208 (nnMain main+0x5bdff4, its GOT slot main+0x2455ca0)
    { "e25c90c1c136638885659fd55bf8a03b", "sead frame-rate controller minimum interval", 0x26cc208, 0x0, MIN_VSYNCS_I32, 1, 0, 2, {0x0, 0x218} },
    // Mario Kart World 1.8.0 has no row: its sead GameFrameworkNx has no controller object like Bananza's. Its frame (main+0x69cbb0) reads a mode at framework+0x210 (the
    // framework at the +8 of the app object, a static at main+0x379ae80): 0 picks 1 or 2 vsyncs from the frame time's drift, 1 or 2 fixes the interval. A fixed 2 at a 30 cap
    // held 30.9 fps but ran the race clock at 0.84 of real time, worse than the game's own choice at the same frame rate, while presents paced to 30 with the mode left at 0
    // ran as the game's own did (0.87-0.95 at 26-31 fps, below 30: MKW never steps more than 2 vsyncs a frame). So MKW takes the paced cap (dm_fps_lock_paced), with the
    // front end's note (docs/reference/frame-rate.md, "A sead framework's fixed interval")
    // Pokemon Brilliant Diamond 1.3.0: Unity's Application.targetFrameRate. Its icall registration (main+0xe34e60) resolves to main+0xdc8e70 -> +0x5fa640,
    // which stores w0 at main+0x4afc868; the getter at +0x5fa630 reads the same int32. The game sets 30 when entering the field; display pacing alone does not remove that cap.
    { "94ceae325c205c4b9d6f7235552f28fd", "Unity targetFrameRate", 0, 0x4afc868, FPS_I32, 1 },
    // Monster Hunter Rise 16.0.2: RE Engine's frame limit, a float at [main+0x1295b3f0]+0x434 (the pointer is a global in main's bss). The frame loop (main+0x6b106c8) sleeps until
    // [+0x430] / [+0x434] sixtieths of a second have passed ([+0x430] = 60) and steps the game by the time that really passed (+0x41c), so a higher limit runs the game faster in frames
    // at the same speed. The game sets 30 after boot; it also asks for present interval 2, which the profile's present_interval display ignores.
    // From masagrator's FPSLocker config for this build (FPSLocker-Warehouse, patches/0100B04011742000/C9A3DD7702075ECD.yaml)
    { "c9a3dd7702075ecd90101f4555389582", "RE Engine frame limit", 0x1295b3f0, 0x434, FPS_F32, 1 },
    // Luigi's Mansion 3 1.4.0: Next Level Games' frame limiter. At the end of each frame the main thread calls the frame callbacks (main+0xb7c308) with the milliseconds since the frame
    // started; the limiter (main+0x7a3e70) runs jobs or sleeps until 33.333 ms have passed, the float at main+0x13c94dc (read only there, at main+0x7a3ea8 and main+0x7a3f18), and the game
    // steps by the time that really passed. The game asks for present interval 2 too, which the profile's present_interval display ignores; both are needed
    { "79e5950ffa85acf63e28c9aec051eac2", "NLG frame limiter target", 0, 0x13c94dc, MS_F32, 1 },
    // Need for Speed: Hot Pursuit Remastered 1.0.3: Criterion's frame timing. A uint32 in main's bss (main+0xf0043c) selects the engine's 60 fps mode, in which it takes its frame time from
    // the timer object at [main+0xed96a8]: +0x30 the frame in system ticks (19.2 MHz, 640,000 at 30 fps) and +0x28 a tenth of it; the game's own step, at [main+0xeffc60] +0x30 (2D) and +0x50
    // (3D), is a float of seconds per frame. Its limiter, a spin on the system tick at main+0x483588..+0x4835a4, is cut by the rewriter (patches.py). From masagrator's FPSLocker config for this
    // build (FPSLocker-Warehouse, patches/010029B0118E8000/799D1061182C1302.yaml; the frame-time instruction found by theboy181)
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 60 fps mode", 0, 0xf0043c, MODE_U32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks", 0xed96a8, 0x30, TICKS_U32, 1, 19200000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion frame ticks / 10", 0xed96a8, 0x28, TICKS_U32, 1, 1920000 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 2D game step", 0xeffc60, 0x30, SECS_F32, 1, 0 },
    { "799d1061182c1302e08e706b84ce76e5", "Criterion 3D game step", 0xeffc60, 0x50, SECS_F32, 1, 0 },
    // EA SPORTS FC 27 1.8d.310c: the game's frame regulator, an object at [main+0xda20e38] (its constructor, main+0x99e80c8, sets 60.0). Its tick (main+0x99ea580) waits until
    // 1 / [+0x8] seconds have passed since the last frame; [+0x10] is the rate it reports. main+0x5d501c0 sets both to 30.0 whenever GameRenderSettings.Enable is 1 (fmov v1.2d, #30.0;
    // stur q1, [x9, #8]), a constant of the Switch build, so it is written here. The simulation's own rate is Frostbite's SimulationTimeSettings, set below by name: on a build without
    // this row the game keeps 30 fps at the right speed (the simulation at 60 and the regulator at 30: match clock as at 30). The game asks for present interval 2
    // (BaseDisplaySettings.PresentInterval), which the profile's present_interval display ignores
    { "a48c0de3bc353efa0e4d359bde05ef24", "FIFA frame regulator", 0xda20e38, 0x8, FPS_F64, 2, 0 },
};
#define NLOCKS ((int)(sizeof k_locks / sizeof k_locks[0]))

static int rd(uint64_t a, void *out, size_t n) { return a >= 0x1000 && !plat_mem_read(a, out, n); }

// NieR 1.0.3 exposes its preset minimum but ships fixed mode-specific fallbacks.
// Keep the native current-scale and UV controller intact; only select its
// verified lower bound. This is independent of the FPS choice.
// The 1.0.3 builds the rewrite cuts (patches.py): the scale clamp and the publish branch that are checked, the two fallback floors the cuts
// replace, and the preset minimum. The Asian release (0100B8E016F76000) shifts the function by 0x13c0; its preset minimum is the target of the
// relocation of the slot the function loads it through (main+0x12c8500).
// display is the game's display object: its first word points to the NVN device (the present interval at +0x9c160), and +0x504 is the frame-interval
// mode (0: 60 fps, 1: 30 fps) the setter main+0x9c0590 (Asian release; the global's 0x13c0 before) writes; in the Asian release it is the target of the
// relocation of the slot that setter reads (main+0x12c2d18).
static const struct { const char *build; uint32_t clamp, publish, floor0, floor1, minimum, display; } k_nier_dr[] = {
    {"3c10ec35c5ef4c3686b4053afb0e58e7", 0x9bf568, 0x9bf578, 0x9bf528, 0x9bf534, 0x128e068, 0x192be00},
    {"98397a10419894652f372d4dc1b6fc2c", 0x9c0928, 0x9c0938, 0x9c08e8, 0x9c08f4, 0x128f130, 0x192d600},
};
static void dynamic_resolution_tick(void) {
    static int state = -1, dynamic, row;
    static uint8_t *base;
    if (state < 0) {
        state = 0;
        for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
            for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; row = k; }
        if (!base) return;
        const char *e=getenv("DM_DYNAMIC_RESOLUTION");
        if (e && strcmp(e,"0") && strcmp(e,"1")) LOG("[RED] DM_DYNAMIC_RESOLUTION=%s: must be 0 or 1; keeping full resolution",e);
        dynamic=e && !strcmp(e,"1");
        if (dynamic) {
            uint32_t clamp = 0, publish = 0;
            uint64_t floors[2] = {0};
            if (!rd((uint64_t)(base + k_nier_dr[row].clamp), &clamp, 4) || !rd((uint64_t)(base + k_nier_dr[row].publish), &publish, 4) ||
                !rd((uint64_t)(base + k_nier_dr[row].floor0), &floors[0], 8) || !rd((uint64_t)(base + k_nier_dr[row].floor1), &floors[1], 8) ||
                clamp != 0x1e254c80u || publish != 0x54000489u ||
                floors[0] != 0x72a7f00c5280000cull || floors[1] != 0x72a7f00c5280000cull) {
                LOG("[RED] dynamic resolution needs a verified native scale/UV rewrite; keeping full resolution");
                dynamic = 0;
            }
        }
        state=1;
        LOG("main: NieR dynamic resolution %s (DM_DYNAMIC_RESOLUTION)",dynamic?"on; native controller, 70–100% scale":"off; full resolution");
    }
    if (!state) return;
    uint32_t cur[2];
    uint64_t a = (uint64_t)(base + k_nier_dr[row].minimum);
    if (!rd(a, cur, sizeof cur)) return;
    uint32_t minimum = dynamic ? 0x3f333333u : 0x3f800000u;
    if (cur[0] != minimum) {
        uint32_t *p = (uint32_t *)(uintptr_t)a;
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, minimum, __ATOMIC_RELAXED);
    }
    if (!dynamic && cur[1] != 0x3f800000u) {
        uint32_t *p = (uint32_t *)(uintptr_t)(a + 4);
        dm_guest_write_prepare(p, 4);
        __atomic_store_n(p, 0x3f800000u, __ATOMIC_RELAXED);
    }
}

// Frostbite's simulation rate, found by name (no fixed addresses, so it holds across a game's updates). Frostbite reflects its settings classes: a class record (its name, then at
// +0x40 its field list) and per field a record of 0x20 bytes (name, hash, offset at +0x10, type). SimulationTimeSettings' ForceSimRate (int32) and MaxSimFps (float) cap the
// simulation's ticks a second: 30 in EA SPORTS FC 27, whose frames then cannot go faster than 30 whatever paces them. A static constructor registers the class: it stores the record's
// address into its type info (adrp / add of the record, then str into the type info just made by adrp / add), and the code that reads the settings asks the settings registry for the
// instance with that type info in x1 and a cache word in x2 (adrp / add each, then bl), which keeps the instance's address. The instance holds its type info at +8, which checks the cache.
// Both fields are written each frame they differ (the game may load its settings again), like the frame limits above
static struct { int n; uint8_t *base; uint64_t ti, cache[8]; int ncache; uint32_t off_rate, off_max; } fb;
static int fb_name_at(uint8_t *base, uint64_t size, uint64_t p, const char *want) {   // p, a relocated pointer, points into the module at the string want
    uint64_t b = (uint64_t)(uintptr_t)base; size_t n = strlen(want);
    return p >= b && p - b + n + 1 <= size && !memcmp((const void *)(uintptr_t)p, want, n + 1);
}
static int64_t fb_pair(const uint32_t *t, uint64_t i, int reg) {   // the target of adrp / add into reg at words i, i + 1, or -1 (module offsets: the text sits at offset 0)
    uint32_t w = t[i], x = t[i + 1];
    if ((w & 0x9F000000u) != 0x90000000u || (int)(w & 31) != reg || (x & 0xFFC00000u) != 0x91000000u || (x & 31) != (uint32_t)reg || ((x >> 5) & 31) != (uint32_t)reg) return -1;
    int64_t imm = (int64_t)(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
    return (int64_t)((i * 4) & ~0xfffull) + imm * 4096 + ((x >> 10) & 0xfff);
}
static void fb_find(void) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !m->text_end || !m->ro_off || m->ro_off >= m->size) return;
    static const char cls[] = "SimulationTimeSettings";
    uint8_t *base = m->base, *ro = base + m->ro_off; uint64_t size = m->back_off ? m->back_off : m->size, b = (uint64_t)(uintptr_t)base;
    const uint8_t *s = memmem(ro, (size_t)(size - m->ro_off), "\0SimulationTimeSettings\0", sizeof cls + 1); if (!s) return;   // not Frostbite
    uint64_t name = (uint64_t)(uintptr_t)(s + 1), rec = 0;
    for (uint64_t o = (m->ro_off + 7) & ~7ull; o + 0x48 <= size && !rec; o += 8) {
        if (*(uint64_t *)(base + o) != name) continue;
        uint64_t fl = *(uint64_t *)(base + o + 0x40); if (fl < b || fl - b + 0x20 > size) continue;
        for (uint64_t f = fl - b; f + 0x20 <= size; f += 0x20) {   // the field list ends where a name stops being one
            uint64_t fn = *(uint64_t *)(base + f); if (fn < b || fn - b >= size) break;
            uint32_t off = *(uint32_t *)(base + f + 0x10);
            if (fb_name_at(base, size, fn, "ForceSimRate")) fb.off_rate = off;
            else if (fb_name_at(base, size, fn, "MaxSimFps")) fb.off_max = off;
        }
        if (fb.off_rate && fb.off_max) rec = o;
    }
    if (!rec) { LOG("[RED] main: Frostbite's %s found, but not its ForceSimRate and MaxSimFps fields; leaving the simulation rate", cls); return; }
    const uint32_t *t = (const uint32_t *)base; uint64_t nw = m->text_end / 4;
    for (uint64_t i = 0; i + 3 < nw && !fb.ti; i++) {   // the registration: adrp / add xR = record, str xR, [xT] with xT = the type info from an adrp / add just before
        int r = (int)(t[i] & 31); if (fb_pair(t, i, r) != (int64_t)rec) continue;
        uint32_t st = t[i + 2]; if ((st & 0xFFFFFC00u) != 0xF9000000u || (int)(st & 31) != r) continue;   // str xR, [xT] (no offset)
        int xt = (int)((st >> 5) & 31);
        for (uint64_t k = i >= 6 ? i - 6 : 0; k < i; k++) { int64_t ti = fb_pair(t, k, xt); if (ti > 0) fb.ti = (uint64_t)ti; }
    }
    if (!fb.ti) { LOG("[RED] main: Frostbite's %s: its type info was not found; leaving the simulation rate", cls); return; }
    for (uint64_t i = 8; i + 12 < nw && fb.ncache < 8; i++) {   // the readers: adrp / add x1 = type info, adrp / add x2 = cache word, near each other before a bl
        if (fb_pair(t, i, 1) != (int64_t)fb.ti) continue;
        for (uint64_t k = i - 8; k < i + 10; k++) {
            int64_t c = fb_pair(t, k, 2); if (c <= 0 || (uint64_t)c + 8 > m->size) continue;
            int known = 0; for (int j = 0; j < fb.ncache; j++) known |= fb.cache[j] == (uint64_t)c;
            if (!known) fb.cache[fb.ncache++] = (uint64_t)c;
            break;
        }
    }
    if (!fb.ncache) { LOG("[RED] main: Frostbite's %s: no settings lookup found; leaving the simulation rate", cls); return; }
    fb.base = base; fb.n = 1;
    LOG("main: Frostbite %s at main+%#llx (ForceSimRate +%#x, MaxSimFps +%#x), type info main+%#llx, %d instance caches", cls, (unsigned long long)rec, fb.off_rate, fb.off_max,
        (unsigned long long)fb.ti, fb.ncache);
}
static void fb_tick(int target, const char *src) {
    static int nlog; uint64_t ti = (uint64_t)(uintptr_t)(fb.base + fb.ti);
    for (int j = 0; j < fb.ncache; j++) {
        uint64_t inst = 0, tinfo = 0; int32_t rate; float max;
        if (!rd((uint64_t)(uintptr_t)(fb.base + fb.cache[j]), &inst, sizeof inst) || !inst || !rd(inst + 8, &tinfo, sizeof tinfo) || tinfo != ti) continue;   // not made yet, or another word
        if (!rd(inst + fb.off_rate, &rate, sizeof rate) || !rd(inst + fb.off_max, &max, sizeof max)) continue;
        if (rate != target) { int32_t *p = (int32_t *)(uintptr_t)(inst + fb.off_rate); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, target, __ATOMIC_RELAXED); }
        if (max != (float)target) { float w = (float)target, *p = (float *)(uintptr_t)(inst + fb.off_max); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint32_t *)p, *(uint32_t *)&w, __ATOMIC_RELAXED); }
        if ((rate != target || max != (float)target) && nlog < 8) { nlog++; LOG("main: Frostbite simulation rate ForceSimRate %d, MaxSimFps %g → %d (%s)", rate, max, target, src); }
        return;
    }
}

// Shin'en's frame skip, found by name (no fixed addresses). The engine steps its game once a loop with a fixed 1/60 s (GfxDeltaTime = slow motion / the display's frame rate, a 60.0
// that only the display's constructor writes), and its scripts count ticks, so a frame limit alone runs it slow: at 30 fps, half speed. Its own way to draw less often is the
// timer's frame-skip type, which the scripts set through CGameEngine::SetFrameSkip(shn::eFrameskipType): each loop the timer sets bit 1 of its flags ("draw this frame"), and
// the main loop skips the scene's render and the display's present and next acquire on a loop without it, while the game still steps. Type none draws every loop; odd every
// second one (its 30 fps form); gpu, cpuGpu and cpuGpuDelta keep a debt in frames (the loop's measured time over the frame period, less one a loop, clamped to 0..4) and skip
// while it is above one, so the game steps 60 times a second at any frame rate down to 15. Fast Fusion v262144: timer update main+0xfaad0 (the decision at +0xfabdc), main loop
// main+0xf0600 (bit 1 at +0xf0740 / +0xf07e4), the timer at [main+0x718308] (set by its constructor, main+0xfa8fc), the type at +0x64 (SetFrameSkip's native main+0xeff68).
// The finder reads the scripts' native binding table: name, wrapper pairs in main's data, the wrapper ending in a branch to the native. SetFrameSkip's native is
// mov / movk w8 (the timer's offset in the engine), ldr x8, [x0, x8], str w1, [x8, #type], ret; isFrameSkipped's is adrp / add / ldr of the timer's global, then ldrb of its flags.
// The type's values come from the enum's name table (name, value pairs). The host writes, every present, odd at a cap of 30 (60 steps, 30 frames drawn) and cpuGpu below it,
// and presentation follows the cap (dm_fps_lock_engine): the drawn frames wait their 2 or 3 vsyncs, the skipped ones do not present at all, so 60 steps a second. (Upstream's
// frame limit, DM_FPS_LIMIT, counts there too; this fork has no host frame limiter, so the cap alone decides.) The game's own type comes back at 60 and with no cap; one
// the game sets meanwhile is the one given back. 15 is not offered: at four vsyncs a frame the debt reaches the clamp of 4
static struct { int n, nlog; uint8_t *base; uint64_t global; uint32_t field; int32_t odd, debt, game, mine; uint8_t ours; } sk;
static uint64_t sk_entry(uint8_t *base, uint64_t lo, uint64_t hi, const char *name) {   // the word after the data word that points to name (a native's wrapper, an enum's value), or 0
    size_t n = strlen(name); uint64_t b = (uint64_t)(uintptr_t)base;
    for (const uint8_t *s = base + lo; (s = memmem(s, (size_t)(base + hi - s), name, n + 1)) != NULL; s++) {
        if (s[-1]) continue;   // the whole name, not the end of a longer one
        uint64_t want = (uint64_t)(uintptr_t)s;
        for (uint64_t o = (lo + 7) & ~7ull; o + 16 <= hi; o += 8) if (*(uint64_t *)(base + o) == want) return *(uint64_t *)(base + o + 8);
    }
    (void)b; return 0;
}
static uint64_t sk_native(uint8_t *base, uint64_t text_end, uint64_t wrapper) {   // the module offset of the native a wrapper branches to, or 0
    uint64_t w = wrapper - (uint64_t)(uintptr_t)base;
    if (!base || wrapper < (uint64_t)(uintptr_t)base || w >= text_end || (w & 3)) return 0;   // no module image (a NULL base made base + w a read of address w)
    for (int k = 0; k < 16 && w + 4 * (uint64_t)k + 4 <= text_end; k++) {
        uint32_t x = *(uint32_t *)(base + w + 4 * (uint64_t)k);
        if ((x & 0x7C000000u) != 0x14000000u) continue;   // b / bl
        int64_t imm = x & 0x3FFFFFF; if (imm & (1 << 25)) imm -= 1 << 26;
        int64_t t = (int64_t)(w + 4 * (uint64_t)k) + imm * 4;
        return t > 0 && (uint64_t)t + 20 <= text_end ? (uint64_t)t : 0;
    }
    return 0;
}
static void sk_find(void) {
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m || !m->text_end || !m->ro_off || m->ro_off >= m->size) return;
    uint8_t *base = m->base; uint64_t hi = m->back_off ? m->back_off : m->size, lo = m->ro_off;
    if (!memmem(base + lo, (size_t)(hi - lo), "shn::eFrameskipType::odd", 25)) return;   // not Shin'en's engine
    uint64_t set = sk_native(base, m->text_end, sk_entry(base, lo, hi, "CGameEngine::SetFrameSkip(shn::eFrameskipType)"));
    uint64_t get = sk_native(base, m->text_end, sk_entry(base, lo, hi, "CGameEngine::isFrameSkipped()"));
    int64_t odd = (int64_t)sk_entry(base, lo, hi, "shn::eFrameskipType::odd"), debt = (int64_t)sk_entry(base, lo, hi, "shn::eFrameskipType::cpuGpu");
    const uint32_t *s = set ? (const uint32_t *)(base + set) : NULL, *g = get ? (const uint32_t *)(base + get) : NULL;
    // SetFrameSkip: mov wE, #lo; movk wE, #hi, lsl #16; ldr xT, [x0, xE]; str w1, [xT, #type]; ret
    int set_ok = s && (s[0] & 0xFFE00000u) == 0x52800000u && (s[1] & 0xFFE00000u) == 0x72A00000u && (s[0] & 31) == (s[1] & 31)
                 && (s[2] & 0xFFE0FC00u) == 0xF8606800u && ((s[2] >> 16) & 31) == (s[0] & 31) && ((s[2] >> 5) & 31) == 0
                 && (s[3] & 0xFFC0001Fu) == 0xB9000001u && ((s[3] >> 5) & 31) == (s[2] & 31) && s[4] == 0xD65F03C0u;
    // isFrameSkipped: adrp xG; add xG, xG, #lo; ldr xG, [xG, #o]; ldrb wF, [xG, #flags]
    int get_ok = g && (g[0] & 0x9F000000u) == 0x90000000u && (g[1] & 0xFFC00000u) == 0x91000000u && (g[2] & 0xFFC00000u) == 0xF9400000u && (g[3] & 0xFFC00000u) == 0x39400000u
                 && ((g[1] >> 5) & 31) == (g[0] & 31) && ((g[2] >> 5) & 31) == (g[1] & 31) && ((g[3] >> 5) & 31) == (g[2] & 31);
    if (!set_ok || !get_ok || odd < 1 || odd > 16 || debt < 1 || debt > 16 || odd == debt) {
        LOG("[RED] main: Shin'en's frame skip names found, but not its setter, the timer or the type values (setter %s, timer %s, odd %lld, cpuGpu %lld); a frame rate cap is not held",
            set_ok ? "ok" : "?", get_ok ? "ok" : "?", (long long)odd, (long long)debt);
        return;
    }
    int64_t imm = (int64_t)(((g[0] >> 29) & 3) | (((g[0] >> 5) & 0x7FFFF) << 2)); if (imm & (1 << 20)) imm -= 1 << 21;
    uint64_t glob = (uint64_t)((int64_t)(get & ~0xfffull) + imm * 4096) + ((g[1] >> 10) & 0xfff) + ((g[2] >> 10) & 0xfff) * 8;
    if (glob + 8 > hi) return;
    sk.base = base; sk.global = glob; sk.field = ((s[3] >> 10) & 0xfff) * 4; sk.odd = (int32_t)odd; sk.debt = (int32_t)debt; sk.n = 1;
    LOG("main: Shin'en frame skip: the timer at [main+%#llx], its type at +%#x (SetFrameSkip main+%#llx; odd %d, cpuGpu %d)", (unsigned long long)glob, sk.field, (unsigned long long)set, sk.odd, sk.debt);
}
static void sk_tick(int target, const char *src) {
    uint64_t t = 0; int32_t cur;
    if (!rd((uint64_t)(uintptr_t)(sk.base + sk.global), &t, sizeof t) || !t || !rd(t + sk.field, &cur, sizeof cur) || cur < 0 || cur > 16) return;   // not made yet
    int c = target > 0 && target < 60 ? target : 0;
    int32_t *p = (int32_t *)(uintptr_t)(t + sk.field), want = !c ? -1 : c == 30 ? sk.odd : sk.debt;
    if (sk.ours && cur != sk.mine) sk.ours = 0;   // the game set its own since: that is the one to give back
    if (!sk.ours) sk.game = cur;
    if (want >= 0 && cur != want) {
        dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED); sk.ours = 1; sk.mine = want;
        if (sk.nlog < 16) { sk.nlog++; LOG("main: Shin'en frame skip type %d → %d at %d fps (%s: 60 steps a second, %s; %s)", cur, want, c, want == sk.odd ? "odd" : "cpuGpu",
                                           want == sk.odd ? "every second frame drawn" : "frames drawn as time allows", src); }
    } else if (want < 0 && sk.ours) {
        dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, sk.game, __ATOMIC_RELAXED); sk.ours = 0;
        if (sk.nlog < 16) { sk.nlog++; LOG("main: Shin'en frame skip back to the game's own type %d (%s)", sk.game, src); }
    }
}

// The Unreal rows of this main: the profile's frame-limit variable and the present intervals, at the objects the rewriter found (0 rows: not an Unreal main). *missing: an Unreal main
// (the rewriter found some of its variables) without the profile's frame-limit variable
static int ue_rows(const module_t *m, lock_t *out, int *missing) {
    static char what[3][48]; int n = 0, have = 0;
    for (int k = 0; k < m->n_ue_cvar && n < 3; k++) {
        const char *name = m->ue_cvar[k].name; int fps = !strcmp(name, DM_GAME_UE_FPS_CVAR);
        if (!fps && strcmp(name, "rhi.SyncInterval") && strcmp(name, "nvn.SyncInterval")) continue;
        have |= fps; snprintf(what[n], sizeof what[n], "Unreal %s", name);
        out[n] = (lock_t){ "", what[n], m->ue_cvar[k].off + 0x10, 0, fps ? FPS_F32 : VSYNCS_I32, 2, 0 }; n++;
    }
    *missing = m->n_ue_cvar > 0 && !have;
    return n;
}

// The running main's engine limiters, found once: by the first present (dm_fps_lock_tick) or by the graphics layer's present cap, which may ask first (dm_fps_lock_engine, from the acquire)
static lock_t ue[3]; static const lock_t *rows[NLOCKS + 3]; static int nrow, nier, ue_missing; static uint8_t *base; static uint32_t display;
static pthread_once_t g_found = PTHREAD_ONCE_INIT;
static void lock_find(void) {
    const char *e = getenv("DM_FPS_ROWS"); int no_rows = e && !strcmp(e, "0");   // diagnostic: the build's k_locks rows left out (a row against the paced cap, A/B)
    if (no_rows) LOG("main: DM_FPS_ROWS=0: this build's frame limiter rows are left out");
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        for (int k = 0; k < (int)(sizeof k_nier_dr / sizeof *k_nier_dr); k++) if (!strcmp(g_mod[i].build_id, k_nier_dr[k].build)) { base = g_mod[i].base; nier = 1; display = k_nier_dr[k].display; }
        for (int k = 0; k < NLOCKS && !no_rows; k++) if (!strcmp(g_mod[i].build_id, k_locks[k].build)) { rows[nrow++] = &k_locks[k]; base = g_mod[i].base; }
        int nue = ue_rows(&g_mod[i], ue, &ue_missing);
        for (int k = 0; k < nue; k++) { rows[nrow++] = &ue[k]; base = g_mod[i].base; }
    }
    fb_find();
    sk_find();
}
// Whether the host writes fps_target into an engine limiter of this main (a row for its build, NieR's frame interval, Unreal's variables, Frostbite's simulation rate or Shin'en's
// frame skip, found by name).
// Without one, and without a frame step state page, nothing makes the game follow the cap, so the graphics layer keeps the game's own present interval (hle_nvn.c fps_present_cap):
// presenting at a cap the game does not step by runs a fixed-step game fast or slow (Tears of the Kingdom 1.4.2 at 2x with DM_FPS_TARGET=60)
static int floor_only(void) { for (int r = 0; r < nrow; r++) if (rows[r]->kind != MIN_VSYNCS_I32) return 0; return nrow > 0 && !nier && !fb.n && !sk.n; }
int dm_fps_lock_engine(void) { pthread_once(&g_found, lock_find); return (nrow || nier || fb.n || sk.n) && !floor_only(); }
int dm_fps_lock_floor(void) { pthread_once(&g_found, lock_find); for (int r = 0; r < nrow; r++) if (rows[r]->kind == MIN_VSYNCS_I32) return 1; return 0; }   // the game picks its interval at or above a floor the host writes
__attribute__((weak)) uint8_t *dm_fps_state;   // loader.c: the frame step state page (NULL without a frame step); the unit tests have no loader
// Whether the host holds the cap by pacing presents alone (hle_nvn.c fps_present_cap): a main with no frame step and no limiter the host writes. Every game then still has a cap
// (the user asked for one in all of them): each frame is shown for at least the cap's vsyncs, never fewer than the game's own present interval, so the cap never runs a game
// faster than it asks. The game's logic is not told: a game that steps by the time that passed keeps real time, one that steps a fixed amount per frame runs slower than real
// time while it could go faster than the cap (the Android launcher says so). DM_FPS_PACE=0 turns it off: no cap for such a game, as before
int dm_fps_lock_paced(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_FPS_PACE"); on = !(e && !strcmp(e, "0")); }
    pthread_once(&g_found, lock_find);
    return on && !dm_fps_state && !nrow && !nier && !fb.n && !sk.n;
}
// The cap: DM_FPS_TARGET or the profile's fps_target at start, then whatever the front end sets while the game runs (dm_fps_target_set: the Android launcher's in-game panel,
// DM_CTL "fps <n>"). 0 = the game's own; else 15, 20, 30, 40, 45 or 60, each applied by the limiters that can hold it (rate_ok)
static int target_ok(int t) { return t == 0 || t == 15 || t == 20 || t == 30 || t == 40 || t == 45 || t == 60; }
static _Atomic int g_target = -1, g_target_gen; static const char *g_target_src;
int dm_fps_target(void) {
    int t = atomic_load(&g_target);
    if (t >= 0) return t;
    const char *e = getenv("DM_FPS_TARGET"); t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (!target_ok(t)) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 15, 20, 30, 40, 45 or 60; leaving the game's frame limit", e); t = 0; }
    int was = -1;
    if (atomic_compare_exchange_strong(&g_target, &was, t)) g_target_src = e ? "DM_FPS_TARGET" : "game profile fps_target";
    return atomic_load(&g_target);
}
int dm_fps_target_gen(void) { return atomic_load(&g_target_gen); }
// The active frame rate cap (0 = uncapped): the cap, where a limiter holds it (the frame step, an engine limiter or a floor). The GPU floor and the ADPF target aim at its
// period instead of a shorter one (vka_dev.c vka_floor_period_ns, vka_perf.c; Banana Native v0.3.51 80457bb8, whose frame limiter this tree does not have)
int dm_fps_cap_get(void) { return dm_fps_state || dm_fps_lock_engine() || dm_fps_lock_floor() ? dm_fps_target() : 0; }
void dm_fps_target_set(int t, const char *src) {
    if (!target_ok(t)) { LOG("[RED] frame rate cap %d (%s): must be 0, 15, 20, 30, 40, 45 or 60; unchanged", t, src); return; }
    dm_fps_target(); g_target_src = src; atomic_store(&g_target, t); atomic_fetch_add(&g_target_gen, 1);
    LOG("frame rate cap %d%s (%s)", t, t ? " fps" : " (the game's own)", src);
    // A frame step (loader.c dm_fps_state_apply) reads its cap from the state page at every frame: 0 (the game's own step), 30 and 60 go there too; its stubs know no other rate
    if (dm_fps_state) {
        if (t == 0 || t == 30 || t == 60) { float f = (float)t; __atomic_store_n((uint32_t *)dm_fps_state, *(uint32_t *)&f, __ATOMIC_RELAXED); }
        else LOG("[RED] frame rate cap %d: this game's frame step holds 30 or 60 only; its cap stays until the next start", t);
    }
}
// The object a row's field is in, or 0 while the game has not made it yet
static uint64_t row_obj(const lock_t *L) {
    uint64_t obj = (uint64_t)(uintptr_t)base;
    if (!L->ptr_off) return obj;
    if (!rd((uint64_t)(uintptr_t)(base + L->ptr_off), &obj, sizeof obj) || !obj) return 0;
    for (int i = 0; i < L->nderef; i++) if (!rd(obj + L->deref[i], &obj, sizeof obj) || !obj) return 0;
    return obj;
}
// A floor row's controller maximum (the int32 after the floor, set by the game), or 0 while the game has not made the controller or set it
static int floor_max(const lock_t *L) {
    uint64_t obj = row_obj(L); int32_t mx;
    if (!obj || !rd(obj + L->field + 4, &mx, sizeof mx) || mx < 1 || mx > 60) return 0;
    return mx;
}
// Whether a row can hold cap t: its kind's rates (rate_ok), and for a floor no more vsyncs than the controller's own maximum (while that is unknown, the write waits for it)
static int row_holds(const lock_t *L, int t) {
    if (!rate_ok(L->kind, t)) return 0;
    int mx = L->kind == MIN_VSYNCS_I32 ? floor_max(L) : 0;
    return !mx || 60 / t <= mx;
}
static int holds(int t) {
    int ok = (nier && 60 % t == 0) || fb.n || (sk.n && (t == 20 || t == 30 || t == 60));   // Shin'en's frame skip: whole vsyncs the debt holds
    for (int r = 0; r < nrow && !ok; r++) ok = row_holds(rows[r], t);
    return ok;
}
// Which caps this main's limiters hold, as a bit per rate (bit 0: 15, 1: 20, 2: 30, 3: 40, 4: 45, 5: 60); 0 = none, the game keeps its own frame rate.
// A frame step (the rewriter's FRAME_STEP stub, its state page dm_fps_state) holds 30 and 60 (dm_fps_target_set) without any limiter row: Ocarina of Time has only the step,
// and with 0 here the Android panel and Game settings said "this game picks its own frame rate" and offered no cap, so the game ran at its own step, 2 vsyncs of game time
// a frame whatever the frame took: at 21 fps about 0.7 of real time (POCO F7 Ultra, the signed 0.1.8.8-beta, 2026-11-15)
// Paced by presents alone (dm_fps_lock_paced): the whole-vsync rates 15, 20, 30 and 60, and bit 6 (DM_FPS_RATES_PACED) so a front end can say the game is not told
static const int k_rates[] = {15, 20, 30, 40, 45, 60};
#define DM_FPS_RATES_PACED (1 << 6)
int dm_fps_lock_rates(void) {
    pthread_once(&g_found, lock_find);
    int nxhost_fps_decoupled(void);
    if (dm_fps_lock_paced()) return 1 << 0 | 1 << 1 | 1 << 2 | 1 << 5 | (nxhost_fps_decoupled() ? 0 : DM_FPS_RATES_PACED);   // decoupled: the game is told the paced rate (sead_rate)
    int m = dm_fps_state ? 1 << 2 | 1 << 5 : 0;
    for (int i = 0; i < 6; i++) if (holds(k_rates[i])) m |= 1 << i;   // a floor's maximum is read each time: the panel asks again while the game runs
    return m;
}

// Decoupling the game's speed from its present interval (fps_decouple sead; DM_FPS_DECOUPLE, default 1; Android only), from upstream Banana Native (eden/v0.3.42). sead's
// dynamic present interval controller hands the framework a frame rate of 60 / interval, and the game steps its world by that rate: a frame presented late at interval 1
// (30 fps on screen) still advanced the world 1/60 s, so the game ran at half speed (Splatoon Raiders on the POCO F7 Ultra, 2026-11-17). The rewriter puts observation gate
// OBS_SEAD_RATE on the controller's sdiv of 60 by the interval in the main of a game whose profile opts in (dm/tools/nso_code.py sead_find: found by its code, no build ID; a
// profile's OBS row names it for a build), with the dividend N, the divisor M (the interval) and the controller's register C in the gate number's upper bits. The callback
// writes N = rate * M, so the sdiv yields the measured rate, and the controller stores it and hands it to the framework as it would its own.
// The rate is a whole number of frames a second (the field is an int32), the game's time step 1 / rate: the time between this frame's start and the last one's (the presents
// between two calls, so a frame the controller skips does not count twice), with the rounding error carried to the next frame (at 20.4 fps the rates alternate 20 and 21 so the
// steps add up to the time that passed). Clamped to 10..60: below 10 fps the game slows down, as on the Switch, and the debt is dropped after a long frame (a load), so the game
// does not run ahead to catch up. Every 10 s the log line "sead rate" gives the game time handed out against the time that passed (the check of full speed).
// The paced cap (dm_fps_lock_paced) composes with it: the presents are held to the cap, the gate measures that rate, and the game keeps its speed at the cap.
// Upstream's interval pin (DM_FPS_PIN) and controller tuning (DM_FPS_PATIENCE / DM_FPS_RETRY) are not ported: they serve its frame limiter and a floor row, which this fork has not
#define SEAD_RATE_MIN 10
#define SEAD_RATE_MAX 60
static _Atomic uint32_t g_presents;   // presents so far (dm_fps_lock_tick)
// The rate for a frame of dt seconds with the carried rounding error *debt (seconds of real time not yet stepped), and the debt after it
static int32_t sead_rate_pick(double dt, double *debt) {
    double want = dt + *debt;
    int32_t r = want > 0 ? (int32_t)(1.0 / want + 0.5) : SEAD_RATE_MAX;
    if (r < SEAD_RATE_MIN) r = SEAD_RATE_MIN;
    if (r > SEAD_RATE_MAX) r = SEAD_RATE_MAX;
    *debt = want - 1.0 / r;
    if (*debt > 1.0 / SEAD_RATE_MAX) *debt = 0;   // a frame longer than the floor's: the game slows down, and does not catch up afterwards
    if (*debt < -1.0 / SEAD_RATE_MAX) *debt = -1.0 / SEAD_RATE_MAX;
    return r;
}
static struct { uint64_t ctl; double debt, game_s, wall_s; uint32_t presents, frames; struct timespec last, last_log; int32_t rate; } g_sr;
// On or off: the Android launcher's "Full game speed at low frame rates" (Game settings at start, the in-game panel live: nxhost_fps_decouple_set), else DM_FPS_DECOUPLE at the
// first frame (default on; the launcher passes DM_FPS_DECOUPLE=0 for a start with the setting off). Off, the gate leaves the controller's 60 / interval as it is (the game's own
// step); on again, it starts from a fresh measurement. Read at every frame, so a change applies at the next one
static _Atomic int g_decouple = -1;
static const char *g_decouple_src;
__attribute__((visibility("default"))) void nxhost_fps_decouple_set(int on) {
    atomic_store(&g_decouple, on != 0); g_decouple_src = "the launcher's in-game panel";
    LOG("main: frame rate decoupling %s (%s)", on ? "on" : "off", g_decouple_src);
}
static int sead_rate_on(void) {
    int v = atomic_load(&g_decouple);
    if (v >= 0) return v;
    const char *e = getenv("DM_FPS_DECOUPLE"); int was = -1;
    v = !e || atoi(e) != 0;
    if (atomic_compare_exchange_strong(&g_decouple, &was, v)) { g_decouple_src = e ? "DM_FPS_DECOUPLE" : "default"; if (!v) LOG("main: frame rate decoupling off (DM_FPS_DECOUPLE=0)"); }
    return atomic_load(&g_decouple);
}
int dm_fps_decouple_on(void) { return sead_rate_on(); }
// This frame's measured rate (sead_rate_pick) for the controller at ctl, or 0 for the first frame of a measurement (the game's own rate then). Both gates, the sdiv's and the
// locked path's, run it: once a frame, whichever path the controller takes
static int32_t sead_measure(uint64_t ctl, struct timespec now, double *dt_out, uint32_t *frames_out) {
    g_sr.ctl = ctl;
    uint32_t pr = atomic_load(&g_presents), frames = pr - g_sr.presents; g_sr.presents = pr;
    struct timespec last = g_sr.last; g_sr.last = now;
    if (!last.tv_sec && !last.tv_nsec) { g_sr.last_log = now; return 0; }
    double dt = (double)(now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9;
    if (frames > 1) dt /= frames;
    *dt_out = dt; *frames_out = frames;
    return sead_rate_pick(dt, &g_sr.debt);
}
static void sead_account(int32_t rate, double dt, uint32_t frames, struct timespec now) {   // the 10 s check of full speed: game time = wall time, whatever the frame rate
    g_sr.rate = rate; g_sr.frames++; g_sr.game_s += 1.0 / rate; g_sr.wall_s += dt * (frames > 1 ? frames : 1);
    double since = (double)(now.tv_sec - g_sr.last_log.tv_sec) + (now.tv_nsec - g_sr.last_log.tv_nsec) / 1e9;
    if (since >= 10) {
        LOG("main: sead rate: %.2f s of game time in %.2f s (%.1f%%), %u frames, %.1f fps, rate now %d", g_sr.game_s, g_sr.wall_s, g_sr.wall_s > 0 ? 100 * g_sr.game_s / g_sr.wall_s : 0,
            g_sr.frames, g_sr.wall_s > 0 ? g_sr.frames / g_sr.wall_s : 0, rate);
        g_sr.game_s = g_sr.wall_s = 0; g_sr.frames = 0; g_sr.last_log = now;
    }
}
// The gate's work at time now (the unit tests call it with their own clock): regs x0..x29 as the gate saved them, param N | M << 5 | C << 10
static void sead_rate_at(uint64_t *r, int param, struct timespec now) {
    int n = param & 31, m = (param >> 5) & 31, c = (param >> 10) & 31; static int nlog;
    if (n > 28 || m > 28 || c > 28) return;
    int32_t interval = (int32_t)r[m]; double dt = 0; uint32_t frames = 0;
    int32_t rate = sead_measure(r[c], now, &dt, &frames);
    if (!rate) return;   // the first frame: its own 60 / interval
    if (interval < 1 || interval > 8) return;   // not an interval: the game's own division
    r[n] = (uint32_t)(rate * interval);   // sdiv wD, wN, wM: rate * interval / interval
    if (!nlog++) LOG("main: frame rate decoupled from the present interval: sead's controller (%#llx) hands the game the measured rate (now %d fps, interval %d) instead of 60 / interval (DM_FPS_DECOUPLE)",
                     (unsigned long long)g_sr.ctl, rate, interval);
    sead_account(rate, dt, frames, now);
}
// OBS_SEAD_RATE: on the game's main thread, once a frame, before the controller's sdiv
static void sead_locked_off(void);
static void sead_rate_gate(uint64_t *r, int param, struct timespec now) {   // the gate's work, the platform aside (the unit tests call it)
    if (!sead_rate_on()) { g_sr.last = (struct timespec){0}; g_sr.debt = 0; sead_locked_off(); return; }   // off: the game's own 60 / interval
    sead_rate_at(r, param, now);
}
static void sead_rate(uint64_t *r, int param) {
    if (!PLAT_ANDROID) return;   // Android only: elsewhere the game keeps its own division
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    sead_rate_gate(r, param, now);
}
// OBS_SEAD_LOCKED: the same controller locked by the game at a fixed interval of its own (nso_code.py sead_locked; Splatoon Raiders locks it at interval 1, 60, through
// main+0x11c1c40 → main+0x3df0f60, and played at half speed at 30 fps): the gate is on the locked path's load of the stored rate, ldr wT, [xC, #r], param C | r / 4 << 5. It
// writes the measured rate into [xC + r] before the load, so the controller hands the framework that rate as it would its own, and the game's step follows it as on the
// sdiv's path. The game's own stored rate (60 / its interval, which its setter writes) is kept and put back when decoupling goes off; a new lock by the game (another
// value there) is taken as the game's own again
static struct { uint64_t addr; int32_t own, mine; } g_lk;
static void sead_locked_off(void) {
    if (!g_lk.addr) return;
    int32_t cur; if (rd(g_lk.addr, &cur, sizeof cur) && cur == g_lk.mine) { int32_t *p = (int32_t *)(uintptr_t)g_lk.addr; dm_guest_write_prepare(p, 4); __atomic_store_n(p, g_lk.own, __ATOMIC_RELAXED); }
    LOG("main: sead's locked controller back to the game's own rate %d", g_lk.own); g_lk.addr = 0;
}
static void sead_locked_at(uint64_t *r, int param, struct timespec now) {
    int c = param & 31, off = ((param >> 5) & 0xfff) * 4; static int nlog;
    if (c > 28 || !r[c]) return;
    if (!sead_rate_on()) { g_sr.last = (struct timespec){0}; g_sr.debt = 0; sead_locked_off(); return; }
    uint64_t a = r[c] + (uint64_t)off; int32_t cur;
    if (!rd(a, &cur, sizeof cur)) return;
    if (g_lk.addr != a || cur != g_lk.mine) { g_lk.addr = a; g_lk.own = cur; }   // the game's own rate (first frame, or the game locked it again)
    if (g_lk.own < 1 || g_lk.own > 60) { g_lk.addr = 0; return; }   // not a rate: left as it is
    double dt = 0; uint32_t frames = 0;
    int32_t rate = sead_measure(r[c], now, &dt, &frames);
    if (!rate) return;
    int32_t *p = (int32_t *)(uintptr_t)a; dm_guest_write_prepare(p, 4); __atomic_store_n(p, rate, __ATOMIC_RELAXED); g_lk.mine = rate;
    if (!nlog++) LOG("main: frame rate decoupled: sead's controller (%#llx) is locked by the game at rate %d; it hands the game the measured rate (now %d fps) instead (DM_FPS_DECOUPLE)",
                     (unsigned long long)r[c], g_lk.own, rate);
    sead_account(rate, dt, frames, now);
}
static void sead_locked(uint64_t *r, int param) {
    if (!PLAT_ANDROID) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    sead_locked_at(r, param, now);
}
__attribute__((constructor)) static void sead_rate_attach(void) { gate_obs_set(OBS_SEAD_RATE, sead_rate); gate_obs_set(OBS_SEAD_LOCKED, sead_locked); }
// Whether the game's speed follows its measured frame rate (the rewriter gated sead's controller in this main, decoupling on): the paced cap then tells the game (no bit 6 in
// dm_fps_lock_rates, so the in-game panel does not say it may slow down). Live: the in-game panel may turn it off
int nxhost_fps_decoupled(void) { return PLAT_ANDROID && gate_obs_present(OBS_SEAD_RATE) && sead_rate_on(); }

void dm_fps_lock_tick(void) {
    atomic_fetch_add_explicit(&g_presents, 1, memory_order_relaxed);   // sead_rate's frame count
    dynamic_resolution_tick();   // separate from the FPS cap
    static int state = -2, nlog[NLOCKS + 3], target, gen = -1, start_cap; static const char *src; static uint8_t over[NLOCKS + 3];
    static uint32_t orig[NLOCKS + 3][2]; static uint8_t have_orig[NLOCKS + 3][2];   // what a row held before the host first wrote it: put back when the cap goes to 0 while the game runs
    if (state == -1) return;
    if (state == -2) {
        pthread_once(&g_found, lock_find);
        if (!nrow && !nier && !fb.n && !sk.n && !ue_missing) { state = -1; return; }
        int t = dm_fps_target();
        if (t && ue_missing) LOG("[RED] main: Unreal %s not found by the rewriter (ue_cvar.py NAMES; game profile ue_fps_cvar): the game's own frame limit stays", DM_GAME_UE_FPS_CVAR);
        if (!nrow && !nier && !fb.n && !sk.n) { state = -1; return; }
        state = 0;
    }
    int g = dm_fps_target_gen();
    if (g != gen) {   // the start, or a cap set while the game runs
        int first = gen < 0, t = dm_fps_target(); gen = g; src = g_target_src;
        if (!t) {
            if (first) { for (int r = 0; r < nrow; r++) LOG("main: %s left as the game sets it (%s)", rows[r]->what, src); if (nier) LOG("main: NieR frame interval left as the game sets it (%s)", src);
                         if (fb.n) LOG("main: Frostbite simulation rate left as the game sets it (%s)", src); }
            else for (int r = 0; r < nrow; r++) {   // back to the game's own: the values the game had before the host wrote them (NieR and Frostbite keep the last cap until the game sets them again)
                uint64_t obj = row_obj(rows[r]); if (!obj) continue;
                for (int c = 0; c < rows[r]->copies && c < 2; c++) if (have_orig[r][c] && rows[r]->kind != FPS_F64) {
                    uint32_t *p = (uint32_t *)(uintptr_t)(obj + rows[r]->field + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, orig[r][c], __ATOMIC_RELAXED);
                    int k = rows[r]->kind; float fv; memcpy(&fv, &orig[r][c], sizeof fv);
                    if (k == FPS_F32 || k == MS_F32 || k == SECS_F32) LOG("main: %s back to the game's %g (%s)", rows[r]->what, fv, src);
                    else LOG("main: %s back to the game's %u (%s)", rows[r]->what, orig[r][c], src);
                }
            }
        }
        else for (int r = 0; r < nrow; r++) if (!rate_ok(rows[r]->kind, t)) LOG("main: %s cannot hold %d fps (whole vsyncs only): left as the game sets it (%s)", rows[r]->what, t, src);
        target = t; start_cap = first && t;
        if (!first) for (int r = 0; r < NLOCKS + 3; r++) nlog[r] = over[r] = 0;
    }
    if (sk.n) sk_tick(target, src);   // every frame: with no cap it gives the game its own type back
    if (!target) return;
    // The cap at start (DM_FPS_TARGET: the Android panel's saved cap, env.txt; or the profile) may be one this game cannot hold: 15 on Donkey Kong Bananza, whose controller goes to
    // 3 vsyncs at most. Once every floor's maximum is known, such a cap becomes 0, the game's own, so the front end and presentation show what the game really does
    if (start_cap) {
        int known = 1; for (int r = 0; r < nrow; r++) if (rows[r]->kind == MIN_VSYNCS_I32 && !floor_max(rows[r])) known = 0;
        if (known) {
            start_cap = 0;
            if (!holds(target)) {
                LOG("main: frame rate cap %d fps (%s) is not one this game can hold: the game's own frame rate instead", target, src);
                dm_fps_target_set(0, "a start cap this game cannot hold"); return;
            }
        }
    }
    if (nier && 60 % target == 0) {
        // NieR:Automata US/EU 1.0.3 (3c10ec35c5ef4c3686b4053afb0e58e7):
        // - [base + 0x192be00] points to the NVN graphics device object whose +0x9c160 holds the present interval (1 = 60 FPS, 2 = 30 FPS),
        //   and base + 0x192c304 (+0x504) holds the requested interval mode (0 = 60 FPS, 1 = 30 FPS).
        // Resolution policy is applied independently above, so 30/60 FPS does
        // not implicitly enable or disable the native resolution controller.
        uint64_t p = 0; static int nlog_nier;
        uint32_t want = (uint32_t)(60 / target), want_mode = want - 1, cur_mode = 0;
        if (rd((uint64_t)(uintptr_t)(base + display + 0x504), &cur_mode, sizeof cur_mode) && cur_mode != want_mode && (cur_mode == 0 || cur_mode == 1)) {
            uint32_t *dst = (uint32_t *)(uintptr_t)(base + display + 0x504);
            dm_guest_write_prepare(dst, sizeof *dst);
            __atomic_store_n(dst, want_mode, __ATOMIC_RELAXED);
        }
        if (rd((uint64_t)(uintptr_t)(base + display), &p, sizeof p) && p) {
            uint64_t a = p + 0x9c160; uint32_t cur = 0;
            if (rd(a, &cur, sizeof cur) && cur != want && (cur == 1 || cur == 2)) {
                uint32_t *dst = (uint32_t *)(uintptr_t)a;
                dm_guest_write_prepare(dst, sizeof *dst); __atomic_store_n(dst, want, __ATOMIC_RELAXED);
                if (nlog_nier++ < 8) LOG("main: NieR frame interval %u → %u (%d fps; %s)", cur, want, target, src);
            }
        }
    }
    if (fb.n) fb_tick(target, src);
    for (int r = 0; r < nrow; r++) {
        const lock_t *L = rows[r]; if (!rate_ok(L->kind, target)) continue;
        uint64_t obj = row_obj(L); if (!obj) continue;   // not created yet
        if (L->kind == FPS_F64) {
            for (int c = 0; c < L->copies; c++) {
                uint64_t a = obj + L->field + 8 * (uint64_t)c; double cur, want = target;
                if (!rd(a, &cur, sizeof cur)) break;
                if (cur == want || !(cur >= 10 && cur <= 120)) continue;   // already set, or not set yet
                double *p = (double *)(uintptr_t)a;
                dm_guest_write_prepare(p, sizeof *p); __atomic_store_n((uint64_t *)p, *(uint64_t *)&want, __ATOMIC_RELAXED);
                if (nlog[r] < 8) { nlog[r]++; LOG("main: %s%s %g → %d fps (%s)", L->what, L->copies > 1 ? (c ? " (reported rate)" : " (target)") : "", cur, target, src); }
            }
            continue;
        }
        for (int c = 0; c < L->copies; c++) {
            uint64_t a = obj + L->field + 4 * (uint64_t)c; uint32_t cur, want; char was[16];
            if (!rd(a, &cur, sizeof cur)) break;
            if (L->kind == FPS_F32 || L->kind == MS_F32) {
                int ms = L->kind == MS_F32; float f, w = ms ? ms_want(target) : (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(ms ? f >= 8 && f <= 100 : f >= 10 && f <= 120)) continue;   // already set, or not set yet (RE Engine: 6000 at boot; Unreal: 0 = no limit until the config sets it)
                snprintf(was, sizeof was, ms ? "%.3f ms" : "%g", f);
            } else if (L->kind == SECS_F32) {
                float f, w = 1.0f / (float)target; memcpy(&f, &cur, sizeof f); memcpy(&want, &w, sizeof want);
                if (cur == want || !(f >= 1.0f / 120 && f <= 0.1f)) continue;   // not set yet (0 before the game's first frame)
                snprintf(was, sizeof was, "%.5f s", f);
            } else if (L->kind == TICKS_U32) {
                want = L->scale / (uint32_t)target;
                if (cur == want || cur < L->scale / 120 || cur > L->scale / 10) continue;
                snprintf(was, sizeof was, "%u", cur);
            } else if (L->kind == MODE_U32) {
                want = 1;
                if (cur) continue;   // already 1, or not this switch: 0 is the engine's default
                snprintf(was, sizeof was, "%u", cur);
            } else {
                int fps = L->kind == FPS_I32;
                want = fps ? (uint32_t)target : 60 / (uint32_t)target;
                if (cur == want || cur < (fps ? 10u : 1u) || cur > (fps ? 120u : 4u)) continue;
                if (L->kind == MIN_VSYNCS_I32) {   // never above the controller's own maximum (row_holds)
                    int mx = floor_max(L); if (!mx) continue;   // not set yet
                    if (want > (uint32_t)mx) { if (!over[r]) { over[r] = 1; LOG("main: %s left at %u: %d fps needs %u vsyncs, above the controller's maximum %d (%s)", L->what, cur, target, want, mx, src); } continue; }
                }
                snprintf(was, sizeof was, "%u", cur);
            }
            if (c < 2 && !have_orig[r][c]) { orig[r][c] = cur; have_orig[r][c] = 1; }
            uint32_t *p = (uint32_t *)(uintptr_t)a;
            dm_guest_write_prepare(p, sizeof *p); __atomic_store_n(p, want, __ATOMIC_RELAXED);
            if (nlog[r] < 8) { nlog[r]++; char now[24];
                if (L->kind == MS_F32) snprintf(now, sizeof now, "%.3f ms (%d fps)", ms_want(target), target);
                else if (L->kind == SECS_F32) snprintf(now, sizeof now, "%.5f s (%d fps)", 1.0 / target, target);
                else if (L->kind == TICKS_U32) snprintf(now, sizeof now, "%u (%d fps)", want, target);
                else if (L->kind == MODE_U32) snprintf(now, sizeof now, "1 (%d fps)", target);
                else snprintf(now, sizeof now, "%u%s", L->kind == FPS_F32 ? (unsigned)target : want, L->kind == FPS_F32 || L->kind == FPS_I32 ? " fps" : " vsyncs");
                LOG("main: %s%s %s → %s (%s)", L->what, L->copies > 1 ? (c ? " (render thread)" : " (game thread)") : "", was, now, src); }
        }
    }
}

// The Mac's DM_CTL "cvarf <main offset> <value>" (hle_hid.c), for trying an Unreal console variable while the game runs: the TAutoConsoleVariable at main+offset holds at +0x10
// the pointer to its value, the game thread's copy then the render thread's; both are set to the float
void dm_ctl_cvar_f32(uint64_t off, float v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvarf main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { float *p = (float *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvarf main+%#llx = %g (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
// "peek <guest address> <count>": count floats (and their bits) from guest memory, read safely, to the log
void dm_ctl_peek(uint64_t a, int n) {
    for (int i = 0; i < n && i < 64; i += 4) {
        uint32_t w[4] = {0}; int k = n - i < 4 ? n - i : 4;
        if (!rd(a + 4 * (uint64_t)i, w, 4 * (size_t)k)) { LOG("DM_CTL: peek %#llx: not readable", (unsigned long long)(a + 4 * (uint64_t)i)); return; }
        float f[4]; memcpy(f, w, sizeof f);
        LOG("DM_CTL: peek %#llx: %g %g %g %g (%08x %08x %08x %08x)", (unsigned long long)(a + 4 * (uint64_t)i), f[0], f[1], f[2], f[3], w[0], w[1], w[2], w[3]);
    }
}
// "memdump <guest address> <bytes> <path>": guest memory to a file, read safely (stops at the first unreadable page)
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) { LOG("[RED] DM_CTL: memdump: cannot open %s", path); return; }
    static uint8_t buf[1 << 16]; uint64_t done = 0;
    while (done < n) { size_t k = n - done < sizeof buf ? (size_t)(n - done) : sizeof buf; if (!rd(a + done, buf, k)) break; fwrite(buf, 1, k, f); done += k; }
    fclose(f); LOG("DM_CTL: memdump %#llx: %llu of %llu bytes → %s", (unsigned long long)a, (unsigned long long)done, (unsigned long long)n, path);
}
// "cvari <main offset> <value>": the same for an int console variable
void dm_ctl_cvar_i32(uint64_t off, int32_t v) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        uint64_t obj; if (!rd((uint64_t)(uintptr_t)(g_mod[i].base + off + 0x10), &obj, sizeof obj) || !obj) { LOG("DM_CTL: cvari main+%#llx: no value pointer yet", (unsigned long long)off); return; }
        for (int c = 0; c < 2; c++) { int32_t *p = (int32_t *)(uintptr_t)(obj + 4 * (uint64_t)c); dm_guest_write_prepare(p, sizeof *p); *p = v; }
        LOG("DM_CTL: cvari main+%#llx = %d (both copies at %#llx)", (unsigned long long)off, v, (unsigned long long)obj);
    }
}
