package dev.nxnative.android;

import android.app.ActivityManager;
import android.app.AlertDialog;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;

/**
 * The automatic diagnosis (a game's settings): for a game that closes on a phone nobody here holds, whose owner cannot be asked to type switches and send a log
 * for each. The launcher starts the game several times, each run with one set of host switches, on a copy of the save; the game's screen presses A to get past
 * the title, and ends the run when it lived its time (RUN_SECS for the first; for the others AFTER seconds past where the first died). Back in the launcher each run's end is read (the marker of a run that lived, else Android's exit info) with
 * the lines of its log that matter, and the next run is picked from what the earlier ones did. One text report, shared as a log (LogProvider serves *.log).
 *
 * Written for Ocarina of Time on a Redmagic 11 Air (2026-11-14): killed by signal 9 as the scene starts since 0.1.7.1-beta, playable up to 0.1.5-beta; the
 * switches below put back, together and then one at a time, what the renderer changed since (the game's note). They are the host's general switches, so the
 * same runs serve any game that started closing after an update.
 *
 * Two processes: the launcher's (begin, resume: the plan and the report, state.properties) and the game's (takeRun, started: run.properties names the one run
 * to make). The game's screen reads its run from that file, not from its intent: a screen Android restores after the process was killed comes back with the
 * intent of the run before
 */
final class AutoDiag {
    private AutoDiag() {}

    static final int PAD = -7701;   // the pad that presses A (a device id no real device has)
    // The first run may take RUN_SECS: on the phone this was written for the game shows a 154 s movie before its first scene and died about 180 s in, and a
    // first version that ended every run at 150 s never got there (its report of 2026-11-14: "alive", still in the movie). The runs after it need only outlive the
    // first one's death by AFTER seconds. A is pressed until a run ends: what comes after a movie (a name, a dialogue) needs it too
    private static final int RUN_SECS = 420, PRESS_FROM = 30, PRESS_EVERY = 3, AFTER = 120;
    /** What the renderer does differently since 0.1.5-beta, each with the switch that puts the earlier behaviour back (docs/env_switches.tsv) */
    private static final String[] OLD = {"DM_VK_MESH=1", "DM_VK_RP_SCOPE=0", "DM_VK_UB_BATCH=0", "DM_VK_OQ_BATCH=0", "DM_VK_POOL_SLOT_DEDUP=0", "DM_NVN_GPU_TS=submission", "DM_VK_UNPLACE_MREMAP=1"};
    // (No "no mesh" run: DM_VK_MESH=0 only leaves the native extension off, the compute fallback still draws them; the first report from the field showed it)
    private static final String[][] OTHER = {{"sync-pipelines", "DM_VK_PSO_ASYNC=0"}, {"no-queries", "DM_VK_OCCLUSION=0"}, {"no-file-pools", "DM_MEM_FILEBACK=0"}};
    private static final int MAX_DRIVERS = 3, MAX_RUNS = 7 + MAX_DRIVERS + OLD.length;

    private static File dir(Context c) { File d = new File(c.getFilesDir(), "diag"); d.mkdirs(); return d; }
    private static File stateFile(Context c) { return new File(dir(c), "state.properties"); }
    private static File runFile(Context c) { return new File(dir(c), "run.properties"); }
    private static File aliveFile(Context c) { return new File(dir(c), "alive"); }
    /** The save the runs use: a copy of the player's, made by the first run (GameActivity.saveDir) */
    static File saveCopy(Context c, String id) { return new File(c.getExternalFilesDir(null), "diag-save/" + id); }

    private static Properties load(File f) {
        Properties p = new Properties();
        if (f.isFile()) try (InputStreamReader r = new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8)) { p.load(r); } catch (IOException ignored) { /* as none */ }
        return p;
    }
    private static void store(File f, Properties p) {
        File t = new File(f.getPath() + ".tmp");
        try (OutputStreamWriter w = new OutputStreamWriter(new FileOutputStream(t), StandardCharsets.UTF_8)) { p.store(w, null); }
        catch (IOException e) { return; }
        try { Files.move(t.toPath(), f.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE); } catch (IOException ignored) { /* the state before stays */ }
    }

    // ---- The plan ----
    private static String[] envOf(Context c, String key) {
        if (key.equals("baseline")) return new String[0];
        if (key.equals("system-driver")) return new String[] {"DM_VK_DRIVER=system"};
        if (key.startsWith("driver:")) return new String[] {"DM_VK_DRIVER=" + new File(GpuDriver.root(c), key.substring(7)).getPath()};
        if (key.equals("no-draws")) return new String[] {"DM_VK_DRAW=0"};
        if (key.equals("no-huge-pages")) return new String[] {"DM_THP=0"};
        if (key.equals("alias-fd")) return new String[] {"DM_VK_ALIAS=fd"};
        if (key.equals("placed-maps")) return new String[] {"DM_VK_ALIAS=placed"};
        if (key.equals("alias-ahb")) return new String[] {"DM_VK_ALIAS=ahb"};
        if (key.equals("as-0.1.5")) return OLD;
        if (key.startsWith("only:")) return new String[] {key.substring(5)};
        for (String[] o : OTHER) if (o[0].equals(key)) return new String[] {o[1]};
        return new String[0];
    }
    private static String outcome(Properties st, String key) {
        for (int i = 0, n = count(st); i < n; i++) if (key.equals(st.getProperty("r" + i + ".key"))) return st.getProperty("r" + i + ".out", "");
        return null;
    }
    private static int count(Properties st) { try { return Integer.parseInt(st.getProperty("n", "0")); } catch (RuntimeException e) { return 0; } }
    /** The next run, or null when the runs made say all they can: the baseline; then the GPU drivers the phone has other than the game's (the system's, then up
     *  to MAX_DRIVERS installed ones) and a run that draws nothing, because the second report from the field (2026-11-14: six runs dead whatever the switch,
     *  each a fraction of a second after the scene's first submission was queued) points at the GPU's side; then every change since 0.1.5-beta put back at once;
     *  when the game lives with that, each of them alone (which one it is); when it does not, the other things the first frame of a scene uses */
    private static String nextKey(Context c, Properties st) {
        String base = outcome(st, "baseline");
        if (base == null) return "baseline";
        if (base.equals("alive")) return null;
        // First what the fourth report (2026-11-14) points at: dead with nothing drawn, dead without huge pages, dead on three drivers with placed maps, alive
        // only on the system's driver, which has none and runs on the exported alias. So the game's own driver without its placed maps (the exported alias,
        // then the hardware buffer one), then the system's driver, then nothing drawn, no huge pages and the other installed drivers
        // (the way of guest memory the game is not set to, in its Graphics settings: the first run was the one it is set to)
        String other = GameGraphics.memory(c, st.getProperty("title")).equals("placed") ? "alias-fd" : "placed-maps";
        if (outcome(st, other) == null) return other;
        if (outcome(st, "alias-ahb") == null) return "alias-ahb";
        GpuDriver.Info sel = GpuDriver.selected(c, st.getProperty("title"));
        if (sel != null && outcome(st, "system-driver") == null) return "system-driver";
        if (outcome(st, "no-draws") == null) return "no-draws";
        if (outcome(st, "no-huge-pages") == null) return "no-huge-pages";
        int k = 0;
        for (GpuDriver.Info i : GpuDriver.installed(c)) {
            if (sel != null && i.dir.equals(sel.dir)) continue;
            if (k++ >= MAX_DRIVERS) break;
            if (outcome(st, "driver:" + i.dir.getName()) == null) return "driver:" + i.dir.getName();
        }
        String old = outcome(st, "as-0.1.5");
        if (old == null) return "as-0.1.5";
        if (old.equals("alive")) { for (String e : OLD) if (outcome(st, "only:" + e) == null) return "only:" + e; return null; }
        for (String[] o : OTHER) if (outcome(st, o[0]) == null) return o[0];
        return null;
    }

    // ---- The game's process (GameActivity) ----
    static final class Run {
        final String key; final String[] env; final int ordinal, secs;
        Run(String key, String[] env, int ordinal, int secs) { this.key = key; this.env = env; this.ordinal = ordinal; this.secs = secs; }
        String label() { return "run " + ordinal + " (" + key + ")"; }
    }
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static WeakReference<GameActivity> screen = new WeakReference<>(null);

    /** GameActivity.onCreate, without a host in this process yet: the run waiting to be made (marked started), or null (none, or the one before, whose
     *  process was killed: that screen has nothing to show) */
    static Run takeRun(Context c) {
        Properties r = load(runFile(c));
        if (!"pending".equals(r.getProperty("state"))) return null;
        r.setProperty("state", "started");
        store(runFile(c), r);
        String key = r.getProperty("key", "baseline");
        int ord = 1, secs = RUN_SECS;
        try { ord = Integer.parseInt(r.getProperty("ordinal", "1")); secs = Integer.parseInt(r.getProperty("secs", "" + RUN_SECS)); } catch (RuntimeException ignored) { /* the defaults */ }
        String env = r.getProperty("env", "").trim();   // the launcher's list (a driver's path needs its context), one switch a word
        return new Run(key, env.isEmpty() ? new String[0] : env.split(" "), ord, secs);
    }

    static void screen(GameActivity a) { screen = new WeakReference<>(a); }

    /** The host started this run: A is pressed every PRESS_EVERY s from PRESS_FROM on (the title, a save's slot, what follows a movie), and a run that lived
     *  run.secs is marked and ended. On this process's main handler, not the screen's: a screen created again (a configuration change) must not lose them */
    static void started(Context app, Run run) {
        Context c = app.getApplicationContext();
        Native.note("[nxlaunch] automatic diagnosis: " + run.label() + (run.env.length == 0 ? ", no switch" : ", " + String.join(" ", run.env))
                + "; A pressed every " + PRESS_EVERY + " s from " + PRESS_FROM + " s on, ended at " + run.secs + " s");
        for (int t = PRESS_FROM; t <= run.secs - 5; t += PRESS_EVERY) {
            final boolean first = t == PRESS_FROM;
            UI.postDelayed(() -> {
                if (first) Native.deviceAdded(PAD, "Automatic diagnosis", true);   // face buttons by label: BUTTON_A is the game's A
                Native.key(PAD, KeyEvent.KEYCODE_BUTTON_A, true);
                UI.postDelayed(() -> Native.key(PAD, KeyEvent.KEYCODE_BUTTON_A, false), 150);
            }, t * 1000L);
        }
        UI.postDelayed(() -> {
            try { Files.write(aliveFile(c).toPath(), run.key.getBytes(StandardCharsets.UTF_8)); } catch (IOException ignored) { /* told as an end without a record */ }
            Native.note("[nxlaunch] automatic diagnosis: " + run.label() + " lived " + run.secs + " s: ended here");
            GameActivity a = screen.get();
            if (a != null && !a.isFinishing() && !a.isDestroyed()) a.diagEnd();
            else { ExitWatch.stopped(); android.os.Process.killProcess(android.os.Process.myPid()); }
        }, run.secs * 1000L);
    }

    // ---- The launcher ----
    static boolean active(Context c) { return stateFile(c).isFile(); }

    private static AlertDialog dialog;
    private static Runnable poll;
    private static int waits;

    /** The game's settings row: what it does, then the first run */
    static void begin(MainActivity a, Game g) {
        try {
            String bad = PackCheck.playBlocking(a, Pack.read(new File(g.packDir)), new File(g.gameDir));
            if (bad != null) throw new IOException(bad);
        } catch (IOException e) {
            new AlertDialog.Builder(a).setTitle(R.string.s2_diag).setMessage(a.getString(R.string.s2_diag_not_ready)).setPositiveButton(android.R.string.ok, null).show();
            return;
        }
        new AlertDialog.Builder(a).setTitle(R.string.s2_diag).setMessage(a.getString(R.string.s2_diag_confirm, MAX_RUNS, MAX_RUNS * 150 / 60))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.s2_diag_start, (d, w) -> beginNow(a, g, RUN_SECS)).show();
    }

    /** The first run, without the question (the row's Start; the test app's intent, MainActivity: --ez diag true [--ei diag_secs <n>] with --es game) */
    static void beginNow(MainActivity a, Game g, int secs) {
        cancel(a);
        SaveStore.deleteTree(saveCopy(a, g.titleId));   // each diagnosis from the save as it is now (the whole tree: Pack.removeDir leaves a folder with folders in it)
        Properties st = new Properties();
        st.setProperty("title", g.titleId); st.setProperty("name", g.name); st.setProperty("game", g.gameDir); st.setProperty("pack", g.packDir);
        st.setProperty("begun", "" + System.currentTimeMillis()); st.setProperty("n", "0"); st.setProperty("cur", ""); st.setProperty("secs", "" + secs);
        store(stateFile(a), st);
        log("begun for " + g.titleId + ", runs of " + secs + " s" + (stateFile(a).isFile() ? "" : "; [RED] the state file was not written"));
        resume(a);
    }
    private static void log(String s) { android.util.Log.i("Swap2Native", "automatic diagnosis: " + s); }

    private static void cancel(Context c) {
        stateFile(c).delete(); runFile(c).delete(); aliveFile(c).delete();
        File[] logs = dir(c).listFiles((d, n) -> n.startsWith("log-"));
        if (logs != null) for (File f : logs) f.delete();
    }

    static void pause() {
        if (poll != null) UI.removeCallbacks(poll);
        poll = null;
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
        dialog = null;
    }

    /** MainActivity.onResume: with a diagnosis under way the launcher is its driver (true: the usual notice of a game that ended stays out) */
    static boolean resume(MainActivity a) {
        if (!active(a)) return false;
        pause();
        waits = 0;
        Properties st0 = load(stateFile(a));
        dialog = new AlertDialog.Builder(a).setTitle(R.string.s2_diag).setMessage(a.getString(R.string.s2_diag_working, count(st0) + 1, MAX_RUNS))
                .setCancelable(false).setNegativeButton(android.R.string.cancel, (d, w) -> { pause(); cancel(a); }).show();
        poll = new Runnable() {
            @Override public void run() {
                if (a.isFinishing() || a.isDestroyed() || !active(a)) return;
                if (step(a)) UI.postDelayed(this, 1000);
            }
        };
        UI.postDelayed(poll, 2500);   // the memory of the run before goes back to the system first
        return true;
    }

    private static boolean gameAlive(Context c) {
        ActivityManager am = c.getSystemService(ActivityManager.class);
        List<ActivityManager.RunningAppProcessInfo> l = am == null ? null : am.getRunningAppProcesses();
        if (l != null) for (ActivityManager.RunningAppProcessInfo p : l) if (p.processName != null && p.processName.endsWith(":game")) return true;
        return false;
    }

    /** One look; true: look again in a second */
    private static boolean step(MainActivity a) {
        if (gameAlive(a)) return true;   // a run's game still up (the player came back here), or a restored screen about to leave
        Properties st = load(stateFile(a));
        String cur = st.getProperty("cur", "");
        if (!cur.isEmpty()) {   // the run that was under way: how it ended
            Map<String, String> rec = ExitWatch.read(a);
            boolean lived = false;
            try { lived = aliveFile(a).isFile() && cur.equals(new String(Files.readAllBytes(aliveFile(a).toPath()), StandardCharsets.UTF_8)); } catch (IOException ignored) { /* not lived */ }
            long start = num(st.getProperty("curStart"));
            int pid = (int) num(rec.get("pid"));
            boolean ours = num(rec.get("start")) >= start - 1000;   // the record is this run's (a run whose screen never came up left the one before)
            ApplicationExitInfo e = ours ? ExitWatch.exitInfo(a, pid, start - 1000) : null;
            if (!lived && e == null && waits++ < 6) return true;   // Android writes its exit info as the process dies
            waits = 0;
            int i = count(st);
            st.setProperty("r" + i + ".key", cur);
            st.setProperty("r" + i + ".env", String.join(" ", envOf(a, cur)));
            st.setProperty("r" + i + ".out", lived ? "alive" : "died");
            st.setProperty("r" + i + ".secs", lived ? st.getProperty("curSecs", "" + RUN_SECS) : e != null && ours ? "" + Math.max(0, (e.getTimestamp() - num(rec.get("start"))) / 1000) : "?");
            st.setProperty("r" + i + ".exit", lived ? "ended by the diagnosis" : e == null ? "no exit record from Android (state " + rec.get("state") + ")"
                    : "reason " + e.getReason() + ", status " + e.getStatus() + ", importance " + e.getImportance() + (e.getDescription() != null ? ", " + e.getDescription() : ""));
            File log = ours ? ExitWatch.logOf(a, rec) : null;
            st.setProperty("r" + i + ".log", log == null ? "" : log.getName());
            st.setProperty("r" + i + ".depth", depth(log));
            if (log != null) try { Files.write(new File(dir(a), "log-" + i + ".txt").toPath(), excerpt(log).getBytes(StandardCharsets.UTF_8)); } catch (IOException ignored) { /* the report says none */ }
            st.setProperty("n", "" + (i + 1)); st.setProperty("cur", "");
            store(stateFile(a), st);
            log("run " + (i + 1) + " " + cur + ": " + st.getProperty("r" + i + ".out") + " at " + st.getProperty("r" + i + ".secs") + " s; " + st.getProperty("r" + i + ".exit"));
            aliveFile(a).delete();
            ExitWatch.shown(a);   // the launcher's own notice of this end stays out
            if (dialog != null && dialog.isShowing()) dialog.setMessage(a.getString(R.string.s2_diag_working, i + 2, MAX_RUNS));
            return true;   // the next look starts the next run
        }
        String key = nextKey(a, st);
        if (key == null) { finish(a, st); return false; }
        // The first run's time is the whole allowance; a later run lived when it is still up AFTER seconds past where the first one died
        long max = num(st.getProperty("secs", "" + RUN_SECS)), died = num(st.getProperty("r0.secs"));
        long secs = key.equals("baseline") || died < 0 ? max : Math.min(max, died + AFTER);
        Properties r = new Properties();
        r.setProperty("key", key); r.setProperty("ordinal", "" + (count(st) + 1)); r.setProperty("secs", "" + secs); r.setProperty("state", "pending");
        r.setProperty("env", String.join(" ", envOf(a, key)));
        store(runFile(a), r);
        st.setProperty("cur", key); st.setProperty("curStart", "" + System.currentTimeMillis()); st.setProperty("curSecs", "" + secs);
        store(stateFile(a), st);
        aliveFile(a).delete();
        log("starting run " + (count(st) + 1) + " " + key);
        a.startActivity(new Intent(a, GameActivity.class).putExtra("title_id", st.getProperty("title")).putExtra("name", st.getProperty("name"))
                .putExtra("game", st.getProperty("game")).putExtra("pack", st.getProperty("pack")).putExtra("diag", true));
        return false;   // this screen pauses; the next resume goes on
    }

    private static long num(String s) { try { return Long.parseLong(s); } catch (RuntimeException e) { return -1; } }

    /** How far into the game a run got, as the host's own line says it: the last "[mem] alias region mapped up to +N GB" of its log ("" without one). A run
     *  that lived at the title has a lower mark than the one that died in the scene (the fourth report: a run "alive" that never left the title) */
    private static String depth(File log) {
        if (log == null) return "";
        String t = ExitWatch.tail(log, 4 << 20), mark = "alias region mapped up to +";
        int i = t.lastIndexOf(mark);
        if (i < 0) return "";
        int j = t.indexOf(" GB", i);
        return j < 0 || j - i > mark.length() + 8 ? "" : t.substring(i + mark.length(), j);
    }
    /** A run's performance in one line, from the host's own lines in its picked log: the last frame rate, the last replay statistics (draws, passes, recording) and the
     *  last frame-interval line ("" when the run never drew). The report's header then compares the runs' speed as well as how far they got */
    private static String perf(File x) {
        if (!x.isFile()) return "";
        String fps = "", rep = "", itv = "";
        try {
            for (String l : new String(Files.readAllBytes(x.toPath()), StandardCharsets.UTF_8).split("\n")) {
                int i;
                if ((i = l.indexOf("] fps: ")) >= 0) fps = l.substring(i + 7);
                else if ((i = l.indexOf("replay up to frame ")) >= 0) rep = l.substring(i);
                else if ((i = l.indexOf("frame interval up to frame ")) >= 0) itv = l.substring(i);
            }
        } catch (IOException e) { return ""; }
        if (fps.isEmpty() && rep.isEmpty()) return "";
        return "performance: " + (fps.length() > 120 ? fps.substring(0, 120) : fps) + (rep.isEmpty() ? "" : " | " + (rep.length() > 330 ? rep.substring(0, 330) : rep))
                + (itv.isEmpty() ? "" : " | " + itv);
    }
    private static double gb(String s) { try { return Double.parseDouble(s); } catch (RuntimeException e) { return -1; } }

    /** A run's log for the report: its lines on memory, the device, the first scene's work and what went wrong (the first 80 and the last 400 of them), then its
     *  last 80 lines whole */
    private static String excerpt(File log) throws IOException {
        long n = log.length(); int cap = 16 << 20;
        byte[] b;
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(log, "r")) { r.seek(Math.max(0, n - cap)); b = new byte[(int) Math.min(n, cap)]; r.readFully(b); }
        String[] lines = new String(b, StandardCharsets.UTF_8).split("\n");
        String[] want = {"memory census", "[mem]", "[RED]", "task/mesh fallback", "] fps:", "CRASH", "automatic diagnosis", "[vk] device ", "[vk] driver:", "guest memory:", "previous run",
                "input: device", "[nxlaunch] build", "[nxlaunch] device", "image cache limit", "memory maps outside", "file-backed pools at", "DM_THP", "sync sub", "sync done",
                // performance: the replay's own lines every 300 frames, the frame intervals, the long frames and the swapchain (the plan of 2026-11-15, M4)
                "replay up to frame", "frame interval up to frame", "[longframe]", "pass ends", "command encoder per frame", "dispatches per frame", "swapchain", "GPU clock floor"};
        List<String> hit = new ArrayList<>();
        for (String l : lines) for (String w : want) if (l.contains(w)) { hit.add(l.length() > 600 ? l.substring(0, 600) + " ..." : l); break; }
        StringBuilder o = new StringBuilder();
        o.append(lines.length).append(" lines, ").append(n).append(" bytes; ").append(hit.size()).append(" picked\n");
        for (int i = 0; i < hit.size(); i++) {
            if (hit.size() > 480 && i == 80) { o.append("  ... ").append(hit.size() - 480).append(" picked lines left out ...\n"); i = hit.size() - 400; }
            o.append(hit.get(i)).append('\n');
        }
        o.append("-- the last lines --\n");
        for (int i = Math.max(0, lines.length - 80); i < lines.length; i++) o.append(lines[i].length() > 600 ? lines[i].substring(0, 600) + " ..." : lines[i]).append('\n');
        return o.toString();
    }

    /** The report (logs/diag-<title ID>-<time>.log) and what it found, with Share */
    private static void finish(MainActivity a, Properties st) {
        pause();
        String id = st.getProperty("title", "game");
        StringBuilder o = new StringBuilder(), sum = new StringBuilder();
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ActivityManager am = a.getSystemService(ActivityManager.class);
        if (am != null) am.getMemoryInfo(mi);
        o.append("[nxlaunch] automatic diagnosis of ").append(st.getProperty("name")).append(" (").append(id).append("), ").append(Prefs.buildTag(a)).append('\n');
        o.append("device ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(", SoC ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL)
                .append(", Android ").append(Build.VERSION.RELEASE).append(" (SDK ").append(Build.VERSION.SDK_INT).append("), RAM ").append(mi.totalMem >> 20).append(" MiB (")
                .append(mi.availMem >> 20).append(" MiB available now)\n");
        // The system's side of the GPU: the same chip and the same driver from the app's files run the game on one phone and not on another, so what the
        // phone itself brings (its build, its kernel, the chip's revision as the kernel's driver names it) belongs in the report
        String gpu;
        try { gpu = new String(Files.readAllBytes(new File("/sys/class/kgsl/kgsl-3d0/gpu_model").toPath()), StandardCharsets.UTF_8).trim(); } catch (IOException | RuntimeException e) { gpu = "not readable"; }
        GpuDriver.Info sel = GpuDriver.selected(a, id);
        StringBuilder inst = new StringBuilder();
        for (GpuDriver.Info i : GpuDriver.installed(a)) inst.append(inst.length() == 0 ? "" : ", ").append(i.dir.getName());
        o.append("system build ").append(Build.DISPLAY).append(", kernel ").append(System.getProperty("os.version")).append(", GPU model ").append(gpu)
                .append("; the game's GPU driver: ").append(sel == null ? "system" : sel.label()).append("; installed: ").append(inst.length() == 0 ? "none" : inst).append('\n');
        o.append("each run: the game started on a copy of the save, A pressed every ").append(PRESS_EVERY).append(" s from ").append(PRESS_FROM)
                .append(" s on; the first run ended by the diagnosis at ").append(st.getProperty("secs", "" + RUN_SECS)).append(" s when still alive, the others ").append(AFTER)
                .append(" s past where the first died\n\n");
        int n = count(st);
        for (int i = 0; i < n; i++) {
            String key = st.getProperty("r" + i + ".key", "?"), out = st.getProperty("r" + i + ".out", "?"), secs = st.getProperty("r" + i + ".secs", "?");
            String depth = st.getProperty("r" + i + ".depth", "");
            boolean shallow = out.equals("alive") && i > 0 && gb(depth) >= 0 && gb(depth) < gb(st.getProperty("r0.depth", ""));   // it lived, but short of where the first run died
            String line = "run " + (i + 1) + " " + key + " [" + st.getProperty("r" + i + ".env", "") + "]: " + (out.equals("alive") ? "ALIVE at " + secs + " s" : "DIED after " + secs + " s")
                    + (depth.isEmpty() ? "" : ", memory mapped up to +" + depth + " GB") + (shallow ? " (NOT AS FAR as run 1's +" + st.getProperty("r0.depth") + " GB: it may never have left the title)" : "")
                    + "; " + st.getProperty("r" + i + ".exit", "") + "; log " + st.getProperty("r" + i + ".log", "none");
            o.append(line).append('\n');
            String perf = perf(new File(dir(a), "log-" + i + ".txt"));
            if (!perf.isEmpty()) o.append("    ").append(perf).append('\n');
            sum.append(i + 1).append(". ").append(key).append(": ").append(a.getString(shallow ? R.string.s2_diag_shallow : out.equals("alive") ? R.string.s2_diag_alive : R.string.s2_diag_died, secs)).append('\n');
        }
        o.append('\n');
        for (int i = 0; i < n; i++) {
            o.append("======== run ").append(i + 1).append(' ').append(st.getProperty("r" + i + ".key", "?")).append(" ========\n");
            File x = new File(dir(a), "log-" + i + ".txt");
            try { o.append(x.isFile() ? new String(Files.readAllBytes(x.toPath()), StandardCharsets.UTF_8) : "no log\n"); } catch (IOException e) { o.append("log not read: ").append(e.getMessage()).append('\n'); }
            o.append('\n');
        }
        File report = new File(LogProvider.dir(a), "diag-" + id + "-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log");
        boolean ok = true;
        try { Files.write(report.toPath(), o.toString().getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { ok = false; }
        SaveStore.deleteTree(saveCopy(a, id));
        cancel(a);
        AlertDialog.Builder b = new AlertDialog.Builder(a).setTitle(R.string.s2_diag_done).setMessage(sum + "\n" + a.getString(ok ? R.string.s2_diag_done_note : R.string.s2_diag_no_report))
                .setNegativeButton(android.R.string.ok, null);
        if (ok) b.setPositiveButton(R.string.s2_diag_share, (d, w) -> a.shareLog(report));
        b.show();
    }
}
