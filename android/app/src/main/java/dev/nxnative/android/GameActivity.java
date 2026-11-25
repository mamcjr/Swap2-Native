package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Runs one game in this process (":game"): sets up its folders and environment, starts nxhost_main, and shows a SurfaceView (handed to the host's Vulkan replay with
 * nxhost_set_window) with the newest log lines and the on-screen controls (TouchControls) over it. Gamepad keys and axes, touches, the Activity lifecycle, audio focus and audio route changes go to the host through
 * Native (libnxlaunch.so → libnxhost.so's C interface); all of it reaches the host instance of this process. Leaving ends the process: the host starts once per process.
 */
public final class GameActivity extends Activity implements SurfaceHolder.Callback, InputManager.InputDeviceListener {
    private static final int KEEP_LOGS = 10;

    private TextView overlay;
    private TextView tag;
    private PerformanceOverlay fps;   // the build tag (top left) and the frame rate with the other items (top right, Overlay), drawn by the launcher over the game (no GPU work in the host)
    private Overlay stats;
    private static Boolean overlayEnv;   // DM_OVERLAY in env.txt: overrides the setting for this run (static: a screen created again keeps it)
    private TouchControls controls;
    private SidePanel panel;
    private final HashSet<Integer> pads = new HashSet<>();   // the gamepads connected: the on-screen controls and the panel's handle hide while there is one
    private View panelHandle;   // the in-game panel's arrow at the right edge: for touch only (a gamepad opens the panel with Home)
    private File log;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean input, started;
    private InputManager inputManager;
    private AudioManager audio;
    private AudioFocusRequest focus;
    private volatile AudioDeviceCallback route;
    private boolean routeReady;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Ui.immersive(this);
        // This game's own host (libnxhost_<title ID>.so, with its profile compiled in): a game this APK has no profile for gets a sentence, not a crash
        String id = String.valueOf(getIntent().getStringExtra("title_id"));
        // The run's exit record and the uncaught-exception handler (ExitWatch): first, so even a failure below reaches the launcher's notice. A new record only for a new run
        // The automatic diagnosis (AutoDiag): its run comes from its own file, so a screen Android restored after the run before was killed (saved != null, the
        // intent of that run) starts the next one all the same; with no run waiting such a screen has nothing to show and leaves (diagStale, below)
        boolean diagIntent = getIntent().getBooleanExtra("diag", false) && !hostRunning;
        diagRun = diagIntent ? AutoDiag.takeRun(this) : null;
        boolean diagStale = diagIntent && diagRun == null;
        if (diagRun != null) for (String kv : diagRun.env) if (kv.startsWith("DM_VK_DRIVER=")) {   // a driver run: as if the game's settings picked it (GpuDriver.runChoice)
            String v = kv.substring("DM_VK_DRIVER=".length());
            GpuDriver.runChoice = v.equals("system") ? GpuDriver.SYSTEM : new File(v).getName();
        }
        AutoDiag.screen(this);
        ExitWatch.game(this, id, getIntent().getStringExtra("name"), (saved == null || diagRun != null) && !hostRunning && !diagStale);
        if (saved == null && !hostRunning && !diagIntent) Game.played(this, getIntent().getStringExtra("game"));   // the library's Last played (a player's run, not a diagnosis)
        try {
            Native.load(this, id);
        } catch (IllegalStateException e) {
            setContentView(new FrameLayout(this));
            fail(e.getMessage());
            return;
        }
        FrameLayout root = new FrameLayout(this);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface);
        // The log's last lines (tail()): bottom left, one block only as high as its text, so it stays clear of the build tag in the corner above
        overlay = overlayText(Gravity.BOTTOM | Gravity.START, 1f);
        root.addView(overlay, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.START));
        // The on-screen controls take every touch and pass it to the host, which tells controls from touchscreen fingers
        controls = new TouchControls(this, Native.hasInput(), id);
        root.addView(controls, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        // The build tag: always shown, whatever the overlay switches say, so every screenshot and recording names its build; bold, flush in the top-left corner
        tag = overlayText(Gravity.TOP | Gravity.START, 1f);
        tag.setText(Prefs.buildTagShown(this));
        tag.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(tag, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
        if (diagRun != null) tag.setText(getString(R.string.s2_diag_tag, diagRun.ordinal, diagRun.key));
        tag.setVisibility(diagRun != null || Prefs.overlayItem(this, "tag") ? View.VISIBLE : View.GONE);   // off by default since 2026-11-08 (the overlay items' "Build name"); the log's first line still names the build
        // The log block may grow at most up to the tag's bottom edge (its top margin), so a long log never runs under the tag
        tag.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) overlay.getLayoutParams();
            if (lp.topMargin != b) { lp.topMargin = b; overlay.post(overlay::requestLayout); }
        });
        fps = new PerformanceOverlay(this);
        root.addView(fps, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.END));
        stats = new Overlay(this);
        panel = new SidePanel(this, (ctx, box) -> controls.controlRows(ctx, box));   // the panel's themed context (its Controls tab)
        // an open panel takes every pad key: it must never open under the curtain (the guide button, Back, the controls' settings), so the curtain leaves first
        panel.onVisibility = open -> { if (open) curtainEnd(true); controls.setSuspended(open); };
        root.addView(panel, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        controls.openSettings = panel::open;
        // The layout editor needs the whole screen: the panel closes, and Paused (over the controls: its first tap would continue the game) hides while it is open;
        // the game stays paused
        controls.onEditStart = () -> { panel.close(); pausedView.setVisibility(View.GONE); };
        controls.onEditEnd = () -> { if (paused) pausedView.setVisibility(View.VISIBLE); };
        // The panel's handle: a small translucent arrow at the right edge, a quarter of the way down (above the shoulder and face buttons of the automatic layout on both of
        // the PNM-N49's screens; half way down it covered A on the outer screen and ZR on the inner one), over the controls so it keeps its own touches wherever they are dragged
        TextView handle = new TextView(this);
        handle.setText("\u2039");
        handle.setTextColor(Color.WHITE);
        handle.setTextSize(22);
        handle.setGravity(Gravity.CENTER);
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        float dp = getResources().getDisplayMetrics().density;
        bg.setColor(0x99000000);
        bg.setCornerRadii(new float[] {12 * dp, 12 * dp, 0, 0, 0, 0, 12 * dp, 12 * dp});
        handle.setBackground(bg);
        // Faint while idle (0.4: it sits over the game picture), fully opaque while a finger is on it so the tap is seen
        handle.setAlpha(0.4f);
        handle.setOnTouchListener((v, e) -> {
            int a = e.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) v.setAlpha(1f);
            else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) v.setAlpha(0.4f);
            return false;   // the click listener still opens the panel
        });
        handle.setContentDescription(getString(R.string.s2g_ga_open_panel));
        handle.setOnClickListener(v -> panel.open());
        root.addView(handle, root.indexOfChild(panel), new FrameLayout.LayoutParams(Math.round(22 * dp), Math.round(56 * dp), Gravity.END | Gravity.TOP));
        panelHandle = handle;
        handle.setVisibility(pads.isEmpty() ? View.VISIBLE : View.GONE);   // touch only: hidden while a gamepad is connected (padsChanged)
        root.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> handle.setTranslationY((b - t) * 0.25f - 28 * dp));
        // Paused (pause(): the in-game panel's Pause, or the screen coming back with pause-on-return on): the game is suspended until a tap, or A / Start on a gamepad.
        // Under the panel and its handle, so Stop stays reachable
        pausedView = new TextView(this);
        pausedView.setText(R.string.s2g_ga_paused);
        pausedView.setTextColor(Color.WHITE);
        pausedView.setTextSize(22);
        pausedView.setGravity(Gravity.CENTER);
        pausedView.setBackgroundColor(0x99000000);
        pausedView.setVisibility(View.GONE);
        pausedView.setOnClickListener(v -> unpause());
        root.addView(pausedView, root.indexOfChild(handle), new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        setContentView(root);
        TextInput.screen(this);   // the keyboard applet's input box shows on this screen (again, when the game was waiting on it as the screen was recreated)
        // The host starts once per process. A screen created again while it runs (Android recreated the activity: "Don't keep activities", a font or language
        // change while in the background) takes its window over; a screen restored into a new process (Android ended the old one while it was in the background)
        // has no game to show: the game was stopped, and the user starts it again
        if (hostRunning) attach();
        else if (diagStale) {   // the launcher reads how that run ended and starts the next (AutoDiag.resume)
            finish();
            new Handler(Looper.getMainLooper()).postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
        }
        else if (saved == null || diagRun != null) { curtainShow(root); start(); }
        else {
            String why = ExitWatch.restoredCause(this);   // what Android said about the end, told here once (the launcher does not tell it again)
            fail(getString(R.string.s2g_ga_bg_stopped) + (why != null ? " " + why : "") + " " + getString(R.string.s2g_ga_bg_lost));
        }
    }

    /** The overlay's text (build tag, stats, log lines): all one small size, 8 sp (at 11-12 sp they covered the touch buttons games put in the corners), white with a
     *  shadow on a faint dark pill with tight padding, so it reads over any scene; touches pass through to the controls */
    static final int OVERLAY_SP = 8;
    private TextView overlayText(int gravity, float alpha) {
        TextView t = new TextView(this);
        float dp = getResources().getDisplayMetrics().density;
        t.setTextColor(Color.WHITE);
        t.setShadowLayer(2, 0, 0, Color.BLACK);
        t.setTextSize(OVERLAY_SP);
        t.setGravity(gravity);
        t.setAlpha(alpha);
        t.setPadding(Math.round(3 * dp), Math.round(1 * dp), Math.round(3 * dp), Math.round(1 * dp));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setColor(0x80000000);
        bg.setCornerRadius(3 * dp);
        t.setBackground(bg);
        t.setClickable(false);
        t.setFocusable(false);
        return t;
    }

    private boolean overlayOn() { return overlayEnv != null ? overlayEnv : Prefs.overlay(this); }

    /** The overlay, twice a second: top right the frame rate and the other items that are on (Overlay); nothing is read while it is off */
    private void fpsTick() {
        if (stopped || paused) { ui.postDelayed(this::fpsTick, 2000); return; }   // nothing to show: no readings (thermal, battery, the host's counters) while suspended
        boolean on = overlayOn();
        CharSequence t = on ? stats.tick() : null;
        fps.setVisibility(t != null ? View.VISIBLE : View.GONE);
        if (t != null) fps.update(stats.sample());
        ui.postDelayed(this::fpsTick, 500);
    }

    private String saveHow = "new", saveNote;

    /**
     * The game's save folder: the one picked in Folders, else the app's own saves/&lt;id&gt; (AppStorage.save; the device save beside it). An older version's save in
     * the old default folder beside the game (&lt;Switch2 root&gt;/saves/&lt;id&gt; for the play app, saves-test/&lt;id&gt; for the test app: Prefs.oldSaveDefault) is
     * copied in once, when the app's save is empty (defaultSave, SaveMigrate). An empty folder is then filled from the private save of the first versions
     * (&lt;app files&gt;/save/&lt;id&gt;, "migrated"), else from &lt;Switch2 root&gt;/save-seed/&lt;id&gt; (seeded). Falls back to that private folder.
     */
    private File saveDir(File files, File game, String id) throws IOException {
        File priv = new File(files, "save/" + id);
        String chosen = Prefs.folder(this, "save_" + id);   // the user's pick (Folders…): used as it is, never migrated into or seeded (the launcher asked about copying)
        File dir = testSave != null ? new File(testSave) : chosen != null ? new File(chosen) : defaultSave(game, id);
        if (diagRun != null) {   // the automatic diagnosis never runs on the player's save: a copy of it, made by its first run
            File copy = AutoDiag.saveCopy(this, id);
            if (!copy.isDirectory() && dir.isDirectory() && !isEmpty(dir)) copyTree(dir, copy);
            testSave = copy.getPath(); dir = copy;
        }
        // The save's lock first (kept while the game runs), then a restore (Restore saves…) or swap stopped part way is finished (SaveStore's journal, the host's
        // rules); one that cannot be finished stops the start: never a half save
        holdLock(dir, getString(R.string.s2g_ga_save_busy, dir));
        try {
            if (SaveStore.recoverLocked(dir)) saveNote = "a save restore or swap that was stopped was finished first";
            // A copy a stopped import or restore left beside the save (no marker names it after recover) goes before the game starts: one filled a phone, and the
            // game's save commits then failed with ENOSPC at every write (a black title screen); the launcher's own pass skips a save whose game already runs
            long freed = SaveStore.clearLeftoversLocked(dir);
            if (freed > 0) saveNote = (saveNote == null ? "" : saveNote + "; ") + "an unfinished save copy beside it was removed first (" + SaveStore.size(freed) + " freed)";
        } catch (IOException e) {
            throw new IOException(getString(R.string.s2g_ga_save_unfinished, e.getMessage()));
        }
        if (testSave != null) {   // a test run's own save folder (env.txt, test app only): used as it is
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create the test save folder " + dir);
            saveHow = diagRun != null ? "a copy for the automatic diagnosis" : "DM_SAVE from env-" + id + ".txt (test app)";
            return dir;
        }
        try {
            File root = Switch2Layout.rootOf(game);   // the Switch2 root: the game's folder's parent, or the one above Games/
            for (String x : new String[] {".old", ".pend1", ".pend2"})   // a commit was interrupted: the host finishes it at mount (plat_swap_recover), before anything is migrated or seeded
                if (new File(dir.getPath() + x).exists()) { saveHow = "recovering"; return dir; }
            if (!dir.isDirectory() && !dir.mkdirs() || !dir.canWrite()) throw new IOException("cannot create " + dir);
            if (chosen != null) { saveHow = isEmpty(dir) ? "chosen, new" : "chosen"; return dir; }
            if (!isEmpty(dir)) { saveHow = "existing"; return dir; }
            if (!isEmpty(priv)) { copyTree(priv, dir); saveHow = "migrated"; return dir; }
            File seed = new File(root, "save-seed/" + id);
            if (!isEmpty(seed)) { copyTree(seed, dir); saveHow = "seeded"; return dir; }
            return dir;
        } catch (IOException | RuntimeException e) {
            saveNote = "save folder " + dir + " unusable (" + e.getMessage() + "), using the app's private folder";
            saveHow = isEmpty(priv) ? "new" : "existing";
            return mkdirs(priv);
        }
    }

    /**
     * The default save: the app's saves/&lt;id&gt;, after the one-time copy of the old default's save into it (SaveMigrate: checked file by file, under both saves'
     * locks, never over a save the app has, the old folder only read). When the copy fails, this start plays the old folder, as before, and says so; the next start
     * tries again. Once copied (Prefs.migratedFrom), the app's save is the game's for good
     */
    private File defaultSave(File game, String id) {
        File app = AppStorage.save(this, id), old = Prefs.oldSaveDefault(game, id);
        if (Prefs.migratedFrom(this, id) != null) return app;
        SaveMigrate.Result r = SaveMigrate.migrate(old, app);
        switch (r.state) {
            case COPIED:
                Prefs.migratedFrom(this, id, old.getPath());
                saveNote = "copied once from " + old + " (" + r.copied.files + " files, " + SaveStore.size(r.copied.bytes) + ", checked); that folder is kept as it was";
                Toast.makeText(this, getString(R.string.s2g_ga_save_copied, app, old), Toast.LENGTH_LONG).show();
                return app;
            case FAILED:
                saveNote = "not copied into the app's storage (" + r.message + "): this start plays the save in " + old + "; the next start tries again";
                Toast.makeText(this, getString(R.string.s2g_ga_save_not_copied, r.message, old), Toast.LENGTH_LONG).show();
                return old;
            default:   // the app has a save already, or there is none to copy
                return app;
        }
    }

    private static boolean isEmpty(File d) { String[] l = d.list(); return l == null || l.length == 0; }

    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("cannot create " + to);
            File[] l = from.listFiles();
            if (l != null) for (File f : l) copyTree(f, new File(to, f.getName()));
        } else Files.copy(from.toPath(), to.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }

    // ---- The readiness check before the host starts (Readiness.java, android/README.md "Readiness check"): the Vulkan facts of the driver this game starts with, against the
    // host's needs and the game's readiness.tsv. Required items missing: a dialog names them with the GPU and driver, and the player goes back or tries anyway; degraded
    // ones: a notice the player can turn off for this phone and driver. Its one line goes into the game's log after the start ([readiness] ...) ----
    private boolean readinessDone, triedAnyway;
    private String readinessLine;
    private static final long READINESS_WAIT_MS = 8000;

    /** true: the start waits for the check (it calls start() again when the player may go on) */
    private boolean readinessHold(String id) {
        if (readinessDone) return false;
        File files = getExternalFilesDir(null);
        List<String> drv = new ArrayList<>();   // the driver as start() will set it: the launcher's pick, then the user's lines (which win)
        drv.add("DM_TEMP=" + new File(files, "temp/" + id));   // (start's DM_TEMP: DM_QGL's settings file goes there, written when the check opens the driver)
        GpuDriver.env(this, id, drv);
        drv.addAll(quietEnv(new File(files, "env.txt")));
        drv.addAll(envLines(Prefs.gameEnv(this, id)));
        drv.addAll(quietEnv(new File(files, "env-" + id + ".txt")));
        String[] denv = Readiness.driverEnv(drv);
        overlay.setText(getString(R.string.s2g_ga_checking_gpu, getIntent().getStringExtra("name")));
        if (curtain != null) curtain.caption(R.string.s2_play_checking_gpu);
        final boolean[] answered = {false};
        Runnable decide = () -> {   // on the UI thread, once: with the facts, or without them after READINESS_WAIT_MS
            if (answered[0] || isFinishing()) return;
            answered[0] = true;
            readinessDecide(id, probeFacts);
        };
        new Thread(() -> { probeFacts = Native.readiness(denv); probeDone = true; ui.post(decide); }, "nxreadiness").start();
        ui.postDelayed(() -> { if (!probeDone) probeFacts = "unchecked=the GPU check did not answer in " + READINESS_WAIT_MS / 1000 + " s"; decide.run(); }, READINESS_WAIT_MS);
        return true;
    }
    private volatile String probeFacts;
    private volatile boolean probeDone;

    private List<String> quietEnv(File f) {
        try { return f.isFile() ? envLines(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8)) : new ArrayList<>(); }
        catch (IOException | SecurityException e) { return new ArrayList<>(); }
    }

    private void readinessDecide(String id, String vulkan) {
        Profile p = Profile.of(id);
        Map<String, String> f = Readiness.merge(vulkan, ReadinessActivity.ramGib(this), Build.VERSION.SDK_INT);
        List<Readiness.Check> l = new ArrayList<>();
        Readiness.Check st = Readiness.status(p != null ? p.status : null);
        if (st != null) l.add(st);
        l.addAll(Readiness.evaluate(f, Readiness.parseReqs(p != null ? p.reqs : null)));
        rFacts = f;
        fps.gpu(f.get("gpu.name"), f.get("driver.name"), f.get("driver.id"), f.get("driver.custom"));
        rChecks = l;
        readinessLine = Readiness.summary(id, f, l);
        String report = Readiness.report(ReadinessActivity.header(this, (p != null ? p.name + " " : "") + id + " (start-up check)"), f, l);
        try { Files.write(new File(getExternalFilesDir(null), "readiness-" + id + ".txt").toPath(), report.getBytes(StandardCharsets.UTF_8)); } catch (IOException ignored) { /* the screen's report leaves it out */ }
        readinessDone = true;
        if (diagRun != null) { start(); return; }   // the automatic diagnosis asks nothing: a driver that lacks something is a run's result (its line is in the log)
        List<Readiness.Check> fails = Readiness.of(l, Readiness.Status.FAIL), warns = Readiness.of(l, Readiness.Status.WARNING);
        // The notice's wording in the phone's language (ReadinessActivity.words); the log line, the report and the "seen" key (warnKey) keep the English checks
        Readiness.Words words = ReadinessActivity.words(this);
        List<Readiness.Check> shown = new ArrayList<>();
        Readiness.Check shownSt = Readiness.status(p != null ? p.status : null, words);
        if (shownSt != null) shown.add(shownSt);
        shown.addAll(Readiness.evaluate(f, Readiness.parseReqs(p != null ? p.reqs : null), words));
        List<Readiness.Check> shownFails = Readiness.of(shown, Readiness.Status.FAIL), shownWarns = Readiness.of(shown, Readiness.Status.WARNING);
        String gpu = getString(R.string.s2g_ga_ready_gpu, Readiness.gpu(f), f.get("ram_gib"));
        // The notice is short (what is missing, by name); Details has each item's sentence, the GPU and Copy report
        if (!fails.isEmpty()) {
            StringBuilder m = new StringBuilder();
            for (Readiness.Check c : shownFails) m.append(c.title).append(", ");
            m.setLength(m.length() - 2);
            // Without placed memory maps the GPU sees none of the game's memory: draws are off (vka_draw.c), so the result is certain, not "may"
            boolean placed = false;
            for (Readiness.Check c : fails) placed |= "ext.VK_KHR_map_memory2".equals(c.fact) || "ext.VK_EXT_map_memory_placed".equals(c.fact) || "feature.memoryMapPlaced".equals(c.fact);
            GpuDriver.Info drv = GpuDriver.selected(this, id);
            String msg = !placed ? getString(R.string.s2g_ga_ready_missing, m)
                    : drv == null ? getString(R.string.s2g_ga_ready_black_system, m) : getString(R.string.s2g_ga_ready_black_custom, m, drv.label());
            readinessDialog(getString(R.string.s2g_ga_not_ready), msg, details(shownFails, shownWarns, gpu), report,
                    getString(placed ? R.string.s2g_ga_start_black : R.string.s2g_ga_try_anyway), true);
        } else if (!warns.isEmpty() && !Prefs.of(this).getString(Prefs.gameKey("readiness_seen", id), "").equals(warnKey(f, warns))) {
            StringBuilder m = new StringBuilder();
            for (Readiness.Check c : shownWarns) m.append(c.title).append(", ");
            m.setLength(m.length() - 2);
            readinessDialog(getString(R.string.s2g_ga_ready_limits), getString(R.string.s2g_ga_ready_less, m), details(shownFails, shownWarns, gpu), report, getString(R.string.s2g_ga_continue), false);
        } else start();
    }

    /** The Details of the start-up notice: each failure and warning with its sentence, then the GPU and memory */
    private static String details(List<Readiness.Check> fails, List<Readiness.Check> warns, String gpu) {
        StringBuilder m = new StringBuilder();
        for (Readiness.Check c : fails) m.append("✕ ").append(c.title).append(": ").append(c.message).append("\n\n");
        for (Readiness.Check c : warns) m.append("! ").append(c.title).append(": ").append(c.message).append("\n\n");
        return m.append(gpu).toString();
    }

    /** What the notice was about (the warnings, the GPU and driver): "Don't show again" holds until one of them changes */
    private static String warnKey(Map<String, String> f, List<Readiness.Check> warns) {
        StringBuilder s = new StringBuilder(Readiness.gpu(f));
        for (Readiness.Check c : warns) s.append('|').append(c.title);
        return Integer.toHexString(s.toString().hashCode());
    }

    /**
     * The start-up notice. Required items missing: Try anyway, Back, Details. Only degraded items: Continue (one tap), with "Don't show this again" (until the GPU,
     * the driver or the warnings change), Back and Details; Settings › Readiness check shows the details again at any time.
     */
    private void readinessDialog(String title, String msg, String details, String report, String go, boolean required) {
        String id = titleId();
        android.widget.CheckBox again = required ? null : new android.widget.CheckBox(this);
        AlertDialog.Builder b = new AlertDialog.Builder(this).setTitle(title).setCancelable(false).setNeutralButton(R.string.s2g_ga_details, null);
        if (again == null) b.setMessage(msg);
        else {
            int p = Math.round(20 * getResources().getDisplayMetrics().density);
            android.widget.LinearLayout box = new android.widget.LinearLayout(this);
            box.setOrientation(android.widget.LinearLayout.VERTICAL);
            box.setPadding(p, p / 2, p, 0);
            TextView t = new TextView(this);
            t.setText(msg + "\n\n" + getString(R.string.s2g_ga_details_where));
            t.setTextSize(15);
            box.addView(t);
            again.setText(R.string.s2g_ga_dont_show);
            box.addView(again);
            android.widget.ScrollView sv = new android.widget.ScrollView(this);   // a short screen with several warnings still reaches the box
            sv.addView(box);
            b.setView(sv);
        }
        b.setPositiveButton(go, (d, w) -> {
            if (again != null && again.isChecked())
                Prefs.of(this).edit().putString(Prefs.gameKey("readiness_seen", id), warnKey(rFacts, Readiness.of(rChecks, Readiness.Status.WARNING))).apply();
            triedAnyway = required;
            start();
        });
        b.setNegativeButton(R.string.s2g_ga_back, (d, w) -> end());
        AlertDialog d = b.create();
        // Details opens over the notice and keeps it open; Copy report there keeps Details open
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
            AlertDialog dd = new AlertDialog.Builder(this).setTitle(R.string.s2g_ga_readiness).setMessage(details).setPositiveButton(R.string.s2g_ga_close, null).setNeutralButton(R.string.s2g_ga_copy_report, null).create();
            dd.setOnShowListener(y -> dd.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(u -> ReadinessActivity.copy(this, report)));
            dd.show();
        }));
        d.show();
    }
    private void noteOwnRate() {
        try {
            String[] f = Native.fpsCapStatus().split(" ");
            if (Integer.parseInt(f[0]) >= 0 && Integer.parseInt(f[2]) > 0) Prefs.ownRate(this, titleId(), Integer.parseInt(f[1]) == 0);   // presenting: main is loaded
        } catch (RuntimeException e) { /* no answer: left as it was */ }
    }

    private Map<String, String> rFacts;
    private List<Readiness.Check> rChecks;

    private void start() {
        String id = getIntent().getStringExtra("title_id"), name = getIntent().getStringExtra("name");
        File game = new File(getIntent().getStringExtra("game")), packDir = new File(getIntent().getStringExtra("pack"));
        File files = getExternalFilesDir(null);
        try {
            Pack pack = Pack.read(packDir);
            String romfs = pack.romfsPath(game);   // a folder's RomFS folder, or <file>.nup:<selector>/romfs (NUP.md)
            if (romfs == null) throw new IOException(Nup.isNup(game) ? getString(R.string.s2g_ga_no_romfs_nup, game.getName(), pack.romfsSelector()) : getString(R.string.s2g_ga_no_romfs, game));
            if (readinessHold(id)) return;   // after the pack's own checks (a missing pack says so first), before the save is locked or anything is written
            PackMemoryUpdate.apply(this, pack);
            // A .nup used where it is (PickedNup): the host gets a descriptor of it, fd:<n>/<name>, wherever it would get the file's path (DM_ROMFS, the add-on links);
            // it stays open for this process's life, and the host duplicates it for each image it opens (nup_image.h)
            int nupFd = PickedNup.is(this, game.getPath()) ? PickedNup.detachedFd(this, game) : -1;
            if (nupFd >= 0) romfs = PickedPath.forHost(romfs, game, nupFd);
            // The user's lines, read first: the paths the launcher sets cannot be changed by them (guard), but the test app's env-<id>.txt may name a test save folder (one game's file only: a global DM_SAVE would point every game at one save)
            List<String> user = new ArrayList<>(guard(userEnv(new File(files, "env.txt")), "env.txt", false));   // diagnostic switches (every game: never DM_SAVE) (DM_SVC=1, DM_FS=1, ...), as the iPhone's Documents/env.txt
            user.addAll(guard(envLines(Prefs.gameEnv(this, id)), "the game's settings", false));   // the game's own lines (its Game settings): after env.txt, so they win for this game
            user.addAll(guard(userEnv(new File(files, "env-" + id + ".txt")), "env-" + id + ".txt", testApp()));   // and env-<title ID>.txt (adb), the same for one game
            File save = saveDir(files, game, id), temp = mkdirs(new File(files, "temp/" + id)), root = mkdirs(new File(files, "root"));
            // The DLC links live in internal app storage: the external files folder is shared storage (FUSE), which refuses symbolic links (errno not yet confirmed on a phone; a refused link is now a [RED] line), so
            // every link failed there and no game saw its DLC (docs/reference/packing.md "DLC links in shared storage"). Their targets stay where they are
            File aoc = aocLinks(mkdirs(new File(getFilesDir(), "aoc/" + id)), game, pack, nupFd);
            Pack.removeDir(new File(files, "aoc/" + id));   // the folder earlier builds used (empty, or links on a device that took them)
            log = new File(LogProvider.dir(this), id + "-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log");
            pruneLogs(id);
            ExitWatch.log(log);
            List<String> env = new ArrayList<>(List.of(
                    "DM_ROMFS=" + romfs, "DM_SAVE=" + save, "DM_TEMP=" + temp, "DM_ROOT=" + root, "DM_AOC_DIR=" + aoc,
                    "HOME=" + files, "TMPDIR=" + getCacheDir(), "DM_BUILD_COMMIT=" + BuildConfig.NX_COMMIT,
                    "DM_CTL=" + new File(files, "ctl.txt"), "NX_EXIT_RECORD=" + ExitWatch.record(this)));   // live control lines (android/TESTING.md "Live control", android/tools/nxctl.sh); DM_CTL= in env.txt turns it off
            String prev = prevExit();   // how the game process before this one ended, for the host (vka_pso.c bb_open: a kill from outside while the driver compiled a pipeline)
            if (!prev.isEmpty()) env.add("NX_PREV_EXIT=" + prev);
            // Shaders the game binds that its library does not have are saved in the game's work folder (<Switch2>/Work/<title ID>/nvn_shdump, or an older
            // <title ID>-work beside the dump: Switch2Layout.workOf, as DumpCheck.workOf names it), where the next Prepare's split takes them in (PackService
            // DM_NVN_SHDUMP_DIR): only for a game prepared on the phone (its work folder is there)
            File work = Switch2Layout.workOf(game, id);
            if (work.isDirectory()) env.add("DM_NVN_SHDUMP=" + new File(work, "nvn_shdump"));
            env.add(0, "NX_BUILD_TAG=" + Prefs.buildTag(this) + " \u00b7 signer " + Prefs.signerId(this));   // nxlaunch.c writes it as the log's first line; the signer says which key signed the APK (Signer)
            // Game resolution: the game's own setting, else the main screen's: the window size the host gives the game (window_fn), or the graphics scale and operation
            // mode that give that size (graphics_scale rows), as its profile says (Profile.resolutionEnv); nothing for a game with neither
            Profile prof = Profile.of(id);
            if (prof != null) env.addAll(prof.resolutionEnv(Prefs.gameWindow(this, id)));
            // Upscaler (Game settings): FSR 1 in place of agl's NDSR networks, with the profile's input encoding (Profile.ndsrEnv); nothing for a game without them
            if (prof != null) env.addAll(prof.ndsrEnv(Prefs.ndsr(this, id)));
            // Scene resolution 720p (Game settings): the RomFS overlay made on the phone from the player's files, while it matches this pack (SceneOverlay)
            String sceneNote = null;
            List<String> overlays = new ArrayList<>();   // DM_ROMFS_OVERLAY's folders (they hold different files: the scene's Bootup pack, the foliage pack)
            String sceneScale = Prefs.sceneScale(this, id, prof);
            if (sceneScale != null) {
                if (SceneOverlay.current(files, id, sceneScale, prof.sceneExpect(), pack.version)) overlays.add(SceneOverlay.dir(files, id).getPath());
                else sceneNote = "[RED] [nxlaunch] Scene resolution " + Prefs.sceneLabel(prof, sceneScale) + ": the overlay " + SceneOverlay.dir(files, id) + " was not made for this pack (version "
                        + pack.version + ", scale " + sceneScale + "): the game's own scene size this run; choosing it again in its Game settings makes it again";
            }
            // Ground foliage (Game settings): the level's RomFS overlay made on the phone from the player's files, while it matches this pack (FoliageOverlay)
            String foliage = Prefs.foliage(this, id, prof);
            if (foliage != null) {
                String keys = prof.foliageLevels().get(foliage);
                if (FoliageOverlay.current(files, id, foliage, keys, pack.version)) overlays.add(FoliageOverlay.dir(files, id, foliage).getPath());
                else {
                    String n = "[RED] [nxlaunch] Ground foliage " + foliage + ": the overlay " + FoliageOverlay.dir(files, id, foliage) + " was not made for this pack (version " + pack.version
                            + "): the game's own foliage this run; choosing the level again in its Game settings makes it again";
                    sceneNote = sceneNote == null ? n : sceneNote + "\n" + n;
                }
            }
            if (!overlays.isEmpty()) env.add("DM_ROMFS_OVERLAY=" + String.join(":", overlays));
            Lsfg.env(this, env);   // frame generation: the user's Lossless.dll and the settings
            Fsr.env(this, env);   // FSR 1 upscaling (the in-game panel)
            GameGraphics.env(this, id, env);   // per-title choices, before the player's environment overrides
            GameGraphics.foliageEnv(this, id, prof, env);   // Foliage (Advanced graphics): the profile's grass and plant programs and the chosen level (nothing without them)
            GameGraphics.effectsEnv(this, id, prof, env);   // the effects turned off in Advanced graphics (Fog): their programs left out (nothing without graphics_off rows)
            String lang = Prefs.gameLangCode(Prefs.gameLang(this, id));   // Game language (Game settings): the system language the host gives the game, else its default (US English)
            if (lang != null) env.add("DM_LANG=" + lang);
            // the shared-font stand-in (dm/third_party/fonts, GameFonts): without a standard font the host has none on a phone (hle_sdk16.c looks for macOS's), and a game that
            // reads the shared font crashes in its font code (Splatoon Raiders at boot); a DM_FONT_DIR line of the user's comes later and wins
            try (java.io.InputStream fin = getAssets().open(GameFonts.ASSET)) {
                File fontDir = new File(getFilesDir(), "fonts");
                java.io.ByteArrayOutputStream buf = new java.io.ByteArrayOutputStream(); byte[] b = new byte[65536]; int n;
                while ((n = fin.read(b)) > 0) buf.write(b, 0, n);
                GameFonts.place(fontDir, GameFonts.STANDARD, buf.toByteArray());
                env.add(GameFonts.envLine(fontDir));
            } catch (java.io.IOException e) { android.util.Log.w("Swap2Native", "the shared font stand-in is not placed: " + e.getMessage()); }
            // The frame hold of the APK's profile (fps_hold; DM_FPS_HOLD): a pack's game_profile.tsv is written once, when it is prepared, so a pack made before the row
            // changed would keep the old one. env.txt and the game's own lines still override it (they come after)
            if (prof != null && !prof.fpsHold.isEmpty()) env.add("DM_FPS_HOLD=" + prof.fpsHold);
            // The low-RAM keys of the APK's profile, for the same reason (vka_lowram.h: the host applies them only behind its gate and DM_VK_LOW_RAM); env.txt still overrides
            if (prof != null && !prof.lowRamPart.isEmpty()) env.add("DM_VK_LOW_RAM_PART_MB=" + prof.lowRamPart);
            if (prof != null && !prof.lowRamImg.isEmpty()) env.add("DM_VK_LOW_RAM_IMG_MAX_MB=" + prof.lowRamImg);
            int cap = Prefs.fpsCap(this, id);   // the game's own frame rate cap (its in-game panel, its Game settings), else the main screen's (unset: the profile's fps_target)
            if (cap >= 0) env.add("DM_FPS_TARGET=" + cap);
            String gpuNote = GpuDriver.env(this, id, env);   // the game's GPU driver, else the main screen's (DM_VK_DRIVER)
            // DM_TEST_CRASH (a test crash, ExitWatch): the test app only
            boolean refusedCrash = !testApp() && user.removeIf(kv -> kv.startsWith("DM_TEST_CRASH="));
            env.addAll(user);
            if (diagRun != null) {   // the run's switches last: they win (its driver is already there, through GpuDriver.env)
                for (String kv : diagRun.env) if (!kv.startsWith("DM_VK_DRIVER=")) env.add(kv);
                env.add("DM_VK_MEM_CENSUS_S=5");
                env.add("DM_VK_PLACED_KILLS=0");   // a run of the diagnosis is what it says it is: no switch to the alias by itself, and its deaths are not counted (vka_placedkill.h)
            }
            Fsr.started(this, env);   // the FSR values this run starts with (env.txt and env-<id>.txt win), which the in-game panel shows
            String testCrash = null;
            for (String kv : env) {
                if (kv.startsWith("DM_OVERLAY=")) overlayEnv = !kv.substring(11).trim().equals("0");
                if (kv.startsWith("DM_TEST_CRASH=")) testCrash = kv.substring(14).trim();
            }
            String[] args = {"nxhost", packDir.getPath()};
            int r = Native.start(args, env.toArray(new String[0]), log.getPath(), files.getPath());
            if (r != 0) throw new IOException(getString(r == -1 ? R.string.s2g_ga_host_tried : R.string.s2g_ga_host_not_started, r));
            // Whether this game picks its own frame rate (the host's cap status: no rate it can hold), remembered for its Game settings, which then offer no cap.
            // Read once the game has run a while (the host finds the game's limiter after it starts); the in-game panel updates it too
            ui.postDelayed(this::noteOwnRate, 30000);
            hostRunning = true;
            if (diagRun != null) AutoDiag.started(this, diagRun);
            if ("java".equals(testCrash)) { Native.note("[RED] DM_TEST_CRASH=java: an exception on the main thread in 10 s"); ExitWatch.testCrash(ui); }   // =native: nxlaunch.c
            if (refusedCrash) Native.note("[nxlaunch] DM_TEST_CRASH not taken: the test app only");
            if (readinessLine != null) Native.note(readinessLine + (triedAnyway ? "; the player chose Try anyway" : ""));   // the readiness check (readinessHold)
            Native.note("[nxlaunch] game: " + game + (nupFd >= 0 ? " (.nup used where it is, descriptor " + nupFd + ")" : Nup.isNup(game) ? " (.nup)" : " (folder)") + ", DM_ROMFS " + romfs + ", pack " + packDir);
            Native.note("[nxlaunch] save: " + save + " (" + saveHow + ")" + (saveNote != null ? "; " + saveNote : ""));
            // Its size and the room beside it: the host's commit copies the whole save (clonefile is a full copy on the phone's storage), so a save of several GB (a
            // wrong import) or a full storage fails every commit with errno 28 (a friend's run, 2026-11-08)
            long saveBytes = SaveStore.sizeUpTo(save, Long.MAX_VALUE), roomBytes = save.getAbsoluteFile().getParentFile().getUsableSpace();
            Native.note("[nxlaunch] save size " + SaveStore.size(saveBytes) + ", free beside it " + SaveStore.size(roomBytes)
                    + (saveBytes > SaveStore.MAX_SAVE_BYTES ? " -- [RED] far bigger than a save: a folder imported by mistake? (Restore saves holds the backup made before the import)" : "")
                    + (roomBytes < 2 * saveBytes + (64L << 20) ? " -- [RED] not enough free space for the game's save commits (each copies the whole save)" : ""));
            Native.note("Swap2-Native " + BuildConfig.VERSION_NAME + " (" + BuildConfig.NX_COMMIT + "), host libnxhost_" + id + ".so; " + name + " " + id + " version " + pack.version);
            noteLastExit();
            Native.note("device " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + "), " + Build.SUPPORTED_ABIS[0]
                    + "; game " + game + "; pack " + packDir);
            if (envProblem != null) Native.note("[RED] env.txt: " + envProblem);
            if (aocLine != null) Native.note(aocLine);
            for (String n : aocProblems) Native.note(n);
            if (sceneNote != null) {
                Native.note(sceneNote);
                android.widget.Toast.makeText(this, getString(R.string.s2_scene_not_ready), android.widget.Toast.LENGTH_LONG).show();
            }
            if (!refused.isEmpty()) {
                String msg = "not taken: " + String.join(", ", refused) + ": the launcher sets the game's paths (DM_SAVE, DM_ROMFS, DM_TEMP, DM_ROOT, DM_AOC_DIR, HOME)"
                        + (testApp() ? "; DM_SAVE is taken from env-<title ID>.txt only" : "");
                Native.note("[RED] env: " + msg);
                android.widget.Toast.makeText(this, getString(testApp() ? R.string.s2g_ga_env_refused_test : R.string.s2g_ga_env_refused, String.join(", ", refused)), android.widget.Toast.LENGTH_LONG).show();
            }
            if (testSave != null && diagRun == null) Native.note("[nxlaunch] test app: DM_SAVE from env-" + id + ".txt: " + testSave);
            Native.note("frame generation: " + (Lsfg.on(this) ? "on" : "off") + "; " + Lsfg.note);
            Native.note(gpuNote + " (picked in the launcher; a DM_VK_DRIVER line in env.txt wins, the host's [vk] driver line says which ran)");
            input = Native.hasInput();
            started = true;
            hostSetup();
            overlay.setText(getString(R.string.s2g_ga_starting, name, log));
            ui.postDelayed(this::tail, 500);
            ui.postDelayed(this::fpsTick, 500);
            ui.postDelayed(this::curtainPoll, CURTAIN_POLL_MS);
        } catch (IOException | RuntimeException e) {
            fail(String.valueOf(e.getMessage()));
        }
    }

    /** Native.start returned 0 in this process: the game runs (until the process ends), and a new screen attaches to it instead of starting it */
    private static boolean hostRunning;

    /**
     * This screen takes over the running game's window (the activity was created again while the game runs). Nothing of start() runs: the save is locked and in use
     * by the host (its recovery must not run beside the host's commits), the DLC links and the log are the first screen's
     */
    private void attach() {
        Native.note("the game's screen opened again; the running game carries on");
        log = null;   // the run's log is the first screen's; this one shows no log
        overlay.setVisibility(View.GONE);
        input = Native.hasInput();
        started = true;
        hostSetup();
        // The host is still stopped (the screen that went sent onStop; nothing runs before onStart). It stays paused when it was (the in-game panel's Pause, or a return
        // not yet continued), and is paused when pause-on-return is on, as a return from the background is: the overlay shows whenever the host is paused, a tap continues
        int hostPaused = Native.paused();
        if (hostPaused < 0) Native.lifecycle(5);   // a host without nxhost_paused: its pause is not known, so it is continued and paused again by the setting alone
        // Pause-on-return only after a real trip to the background: a screen recreated while it showed (a wallpaper colour or theme change) does not pause the game
        if (hostPaused == 1 || hostInBackground && Prefs.pauseOnReturn(this, titleId())) { paused = true; Native.lifecycle(4); pausedView.setVisibility(View.VISIBLE); }
        ui.postDelayed(this::fpsTick, 500);
    }

    /** The game's screen was stopped (the game in the background) and not started again; a stop for a configuration change is no trip to the background */
    private static boolean hostInBackground;

    /**
     * A start that cannot go on: the reason, then the activity and its process end, as Stop does. A failed start must not leave an idle :game process: it would hold
     * this game's host (the next Play of another game would land in it, Native.load) and the save's lock
     */
    private void fail(String why) {
        curtainEnd(true);
        new AlertDialog.Builder(this).setTitle(getString(R.string.s2g_ga_cannot_start, getIntent().getStringExtra("name"))).setMessage(why)
                .setPositiveButton(android.R.string.ok, (d, w) -> end()).setCancelable(false).show();
    }

    /** The automatic diagnosis: a run that lived its time ends as Stop does */
    void diagEnd() { end(); }
    private AutoDiag.Run diagRun;   // this screen's run of the automatic diagnosis (onCreate), else null

    private void end() {
        ExitWatch.stopped();
        backToFrontend();
        finish();
        // On its own handler: onDestroy clears ui's callbacks
        new Handler(Looper.getMainLooper()).postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
    }

    /** A game a frontend started (FrontendLaunch): the app's task goes behind the frontend, so its stop shows the frontend, not Home */
    void backToFrontend() { if (getIntent().getBooleanExtra("frontend", false)) moveTaskToBack(true); }

    /**
     * The screen goes (Stop, or Android recreating the activity while the game runs, which then attaches a new one): what hostSetup registered is let go, and the
     * overlay and log loops stop, so a screen created again does not double them (two deviceAdded per pad, two route resets per change) or keep this one alive.
     * The game itself is left as it is: onStop has suspended it
     */
    @Override protected void onDestroy() {
        TextInput.gone(this);
        ui.removeCallbacksAndMessages(null);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        if (audio != null) {
            if (focus != null) audio.abandonAudioFocusRequest(focus);
            if (route != null) { audio.unregisterAudioDeviceCallback(route); route = null; }
        }
        super.onDestroy();
    }

    /** Play on another game while this one runs reaches this activity (singleTask, the same :game process): this game stays, and the user is told to stop it first */
    @Override protected void onNewIntent(android.content.Intent i) {
        super.onNewIntent(i);
        String want = i.getStringExtra("title_id"), have = getIntent().getStringExtra("title_id");
        if (want == null || want.equals(have)) return;
        new AlertDialog.Builder(this).setTitle(getString(R.string.s2g_ga_running_title, getIntent().getStringExtra("name")))
                .setMessage(getString(R.string.s2g_ga_running_message, i.getStringExtra("name")))
                .setPositiveButton(android.R.string.ok, null).show();
    }

    // ---- The save's lock (SaveStore.Lock on <save>.lock): taken before the save is recovered and kept while this process lives, so a backup or restore of this save
    // never runs beside the game (SaveUi asks SaveStore.inUse); the process ending releases it ----
    private static SaveStore.Lock saveLock;
    private static File saveLocked;

    /** busy: the failure's sentence for the player (the save is in use) */
    private static synchronized void holdLock(File save, String busy) throws IOException {
        if (saveLock != null && SaveStore.same(saveLocked, save)) return;   // this process holds it already (the game's screen opened again)
        SaveStore.Lock l = SaveStore.tryLock(save);
        if (l == null) throw new IOException(busy);
        if (saveLock != null) saveLock.close();
        saveLock = l;
        saveLocked = save;
    }

    /**
     * Why the previous run of a game ended, as Android recorded it (ActivityManager.getHistoricalProcessExitReasons, API 30): a process killed from outside
     * (low memory, the OEM's background or heat limits, a signal from the driver) writes nothing into its own log, so the next start writes the reason here.
     * Only the newest exit of the :game process, and each exit once (its timestamp is remembered)
     */
    private void noteLastExit() {
        try {
            android.app.ActivityManager am = getSystemService(android.app.ActivityManager.class);
            if (am == null) return;
            for (android.app.ApplicationExitInfo x : am.getHistoricalProcessExitReasons(getPackageName(), 0, 8)) {
                if (x.getProcessName() == null || !x.getProcessName().endsWith(":game") || x.getPid() == android.os.Process.myPid()) continue;
                android.content.SharedPreferences p = getSharedPreferences("launcher", MODE_PRIVATE);
                if (x.getTimestamp() <= p.getLong("last_game_exit_noted", 0)) return;
                p.edit().putLong("last_game_exit_noted", x.getTimestamp()).apply();
                Native.note(String.format(java.util.Locale.ROOT, "[nxlaunch] the previous run ended %s: reason %d (%s), status %d, importance %d, PSS %d MiB, RSS %d MiB%s",
                        new java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.ROOT).format(new java.util.Date(x.getTimestamp())), x.getReason(), exitReason(x.getReason()),
                        x.getStatus(), x.getImportance(), x.getPss() / 1024, x.getRss() / 1024, x.getDescription() != null ? "; " + x.getDescription() : ""));
                return;
            }
        } catch (RuntimeException e) { Native.note("[nxlaunch] the previous run's exit reason: not readable (" + e + ")"); }
    }

    /** The newest earlier game process's end as "reason,status,time in ms" (ApplicationExitInfo), "" when Android has none: read before the host starts, noted nowhere */
    private String prevExit() {
        try {
            android.app.ActivityManager am = getSystemService(android.app.ActivityManager.class);
            if (am == null) return "";
            for (android.app.ApplicationExitInfo x : am.getHistoricalProcessExitReasons(getPackageName(), 0, 8)) {
                if (x.getProcessName() == null || !x.getProcessName().endsWith(":game") || x.getPid() == android.os.Process.myPid()) continue;
                return x.getReason() + "," + x.getStatus() + "," + x.getTimestamp() + "," + x.getImportance();   // (the importance: vka_placedkill.h counts a kill in the foreground only)
            }
        } catch (RuntimeException e) { /* no history: the host then takes nothing from the run before */ }
        return "";
    }

    private static String exitReason(int r) {
        switch (r) {
            case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "low memory: killed by the system";
            case android.app.ApplicationExitInfo.REASON_CRASH: return "Java crash";
            case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "native crash";
            case android.app.ApplicationExitInfo.REASON_SIGNALED: return "killed by a signal";
            case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "exited by itself";
            case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "stopped by the user";
            case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "excessive resource use";
            case android.app.ApplicationExitInfo.REASON_ANR: return "not responding";
            case android.app.ApplicationExitInfo.REASON_OTHER: return "other (often the OEM's own limits)";
            case android.app.ApplicationExitInfo.REASON_FREEZER: return "frozen by the system";
            default: return "code " + r;
        }
    }

    /** Environment keys only the launcher sets: a line of env.txt, env-<id>.txt or the game's own lines that names one is refused (DM_SAVE: see allowSave) */
    static final java.util.Set<String> PROTECTED = new java.util.HashSet<>(java.util.Arrays.asList("DM_SAVE", "DM_ROMFS", "DM_TEMP", "DM_ROOT", "DM_AOC_DIR", "HOME"));
    private final List<String> refused = new ArrayList<>();
    private String testSave;   // DM_SAVE from env-<id>.txt in the test app: a test run's own save folder (a copied seed), never the user's

    /** lines without the protected keys; each one refused is noted (refused). allowSave: DM_SAVE may stay (the test app's files, which only adb writes) */
    private List<String> guard(List<String> lines, String from, boolean allowSave) {
        List<String> out = new ArrayList<>();
        for (String kv : lines) {
            String k = kv.substring(0, kv.indexOf('=')).trim();
            if (k.equals("DM_SAVE") && allowSave) { testSave = kv.substring(kv.indexOf('=') + 1).trim(); continue; }   // taken as the save folder (locked, recovered) below
            if (PROTECTED.contains(k)) { refused.add(k + " (" + from + ")"); continue; }
            out.add(kv);
        }
        return out;
    }

    static boolean testApp() { return !BuildConfig.NX_PLAY; }

    private static File mkdirs(File d) throws IOException {
        if (!d.isDirectory() && !d.mkdirs()) throw new IOException("cannot create " + d);
        return d;
    }

    /**
     * DM_AOC_DIR/<index> → each DLC's RomFS folder in the game folder, or DM_AOC_DIR/<index>.nup → the .nup (hle_fs.c aoc_dir; Pack.aocLinks), as symbolic links in
     * app storage. A .nup used where it is (nupFd ≥ 0): the link's target is fd:<n>/<name>, which the host reads with readlink (nothing is at that path).
     * A link that cannot be made is a [RED] line in the run's log (aocProblems): the game runs without that DLC
     */
    private File aocLinks(File dir, File game, Pack pack, int nupFd) {
        Map<String, String> links = pack.aocLinks(game);
        for (String p : Pack.makeAocLinks(dir, links, t -> nupFd >= 0 ? PickedPath.forHost(t, game, nupFd) : t, (target, path) -> {
            try { Os.symlink(target, path); } catch (ErrnoException e) { throw new IOException(android.system.OsConstants.errnoName(e.errno) + " (" + e.getMessage() + ")"); }
        })) aocProblems.add("[RED] [nxlaunch] DLC: " + p);
        aocLine = "[nxlaunch] DLC: " + links.size() + " link" + (links.size() == 1 ? "" : "s") + " in DM_AOC_DIR " + dir + (links.isEmpty() ? "" : " " + links.keySet());
        return dir;
    }
    private final List<String> aocProblems = new ArrayList<>();
    private String aocLine;

    // A file `adb shell` writes here belongs to the shell user with mode 0660 and group ext_data_rw, which the app is not in: it exists but cannot be read
    // (TESTING.md, Diagnostic switches: chmod 666 after writing it). The run goes on without the switches and the log says why
    private String envProblem;
    private List<String> userEnv(File f) {
        if (!f.exists()) return new ArrayList<>();
        try {
            return envLines(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
        } catch (IOException | SecurityException e) {
            envProblem = f + " exists but cannot be read (" + e + "): no switches from it this run; after writing it with adb, chmod 666 it";
            return new ArrayList<>();
        }
    }

    /** KEY=VALUE lines; blank lines and # comments skipped */
    static List<String> envLines(String text) {
        List<String> out = new ArrayList<>();
        for (String l : text.split("\n")) {
            l = l.trim();
            if (!l.isEmpty() && !l.startsWith("#") && l.indexOf('=') > 0) out.add(l);
        }
        return out;
    }

    /** The newest KEEP_LOGS logs of this game are kept (each game's logs are <title ID>-<time>.log: one game's runs never push another's out) */
    private void pruneLogs(String id) {
        File[] logs = LogProvider.dir(this).listFiles((d, n) -> n.startsWith(id + "-") && n.endsWith(".log"));
        if (logs == null || logs.length < KEEP_LOGS) return;
        Arrays.sort(logs, Comparator.comparingLong(File::lastModified));
        for (int i = 0; i <= logs.length - KEEP_LOGS; i++) { logs[i].delete(); ExitWatch.digestOf(logs[i]).delete(); }   // and its digest (the notice's), if any
    }

    /** The last lines of the log over the surface, once a second: until the game's first frame, then only with the setting on (TouchControls.showLog), or after a crash */
    private boolean drawing, failed;
    private void tail() {
        if (log == null) return;
        if (stopped || paused) { ui.postDelayed(this::tail, 2000); return; }   // the log does not move while the game is suspended
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            long n = r.length(), from = Math.max(0, n - 2048);
            byte[] b = new byte[(int) (n - from)];
            r.seek(from);
            r.readFully(b);
            String text = new String(b, StandardCharsets.UTF_8);
            // the host logs these once frames are presented ("[nvn] frame <n>:", not "[nvn] frame step …", which comes before the first present)
            if (FRAME_LINE.matcher(text).find() || text.contains("[longframe]") || text.contains("window frame rate")) drawing = true;
            if (text.contains("CRASH sig=") || text.contains("FATAL")) failed = true;
            // Not while the game loads any more (2026-11-08: the lines over the opening looked like an error): only after a crash, or with Show log on
            overlay.setVisibility(failed || TouchControls.showLog(this) ? View.VISIBLE : View.GONE);
            String[] lines = text.split("\n");
            StringBuilder s = new StringBuilder();
            for (int i = Math.max(from > 0 ? 1 : 0, lines.length - 8); i < lines.length; i++) s.append(lines[i]).append('\n');
            overlay.setText(s.toString().trim());
        } catch (IOException ignored) { /* next time */ }
        ui.postDelayed(this::tail, 1000);
    }

    // ---- The loading curtain (PlayCurtain): over everything from a new run's first frame (never on attach: the game already draws), the caption and the bar from
    // the run's log, read here from where the last poll stopped so no line is missed; it fades into the game once frames are presented (tail's own signal too),
    // ends at once on a failure (the log and the dialog show) or when the game is paused, and after CURTAIN_MAX_MS whatever the log says ----
    private PlayCurtain curtain;
    private long curtainPos;
    private static final long CURTAIN_POLL_MS = 250, CURTAIN_MAX_MS = 120_000, CURTAIN_READ_MAX = 512 * 1024;
    private static final Pattern FRAME_LINE = Pattern.compile("\\[nvn\\] frame \\d+:");   // the host's line for each of the first presented frames (hle_nvn.c)

    private void curtainShow(FrameLayout root) {
        curtain = PlayCurtain.loading(this, root, titleId(), getIntent().getStringExtra("name"));
    }

    private void curtainPoll() {
        if (curtain == null || log == null || isFinishing()) return;
        if (paused) { curtainEnd(true); return; }   // never a curtain over Paused (pause() and onStart end it too; this is the backstop)
        if (stopped) {   // in the background: the cap still holds, the log is read on return
            if (curtain.shownMs() > CURTAIN_MAX_MS) curtainEnd(true); else ui.postDelayed(this::curtainPoll, 1000);
            return;
        }
        boolean done = drawing || failed;
        try (RandomAccessFile r = new RandomAccessFile(log, "r")) {
            long n = r.length();
            if (n - curtainPos > CURTAIN_READ_MAX) curtainPos = n - CURTAIN_READ_MAX;   // a burst: its last part (the newest step) is enough
            if (n > curtainPos) {
                byte[] b = new byte[(int) (n - curtainPos)];
                r.seek(curtainPos);
                r.readFully(b);
                int end = b.length;
                while (end > 0 && b[end - 1] != '\n') end--;   // whole lines only: the rest is read again next time
                if (end > 0) {
                    curtainPos += end;
                    done |= curtain.feed(new String(b, 0, end, StandardCharsets.UTF_8));
                }
            }
        } catch (IOException ignored) { /* next time */ }
        curtain.tick();
        // the host's boot hold (DM_VK_PREPARE): the count and a tap to skip, while it shows (it answers only after its first second)
        boolean holding = curtain.compiling(Native.prepareStatus(), () -> { Native.note("compiling shaders: skipped by the player"); Native.prepareSkip(); });
        if (failed || curtain.failed() || ((done || curtain.shownMs() > CURTAIN_MAX_MS) && !holding)) {   // never while the boot hold runs (it ends by itself: DM_VK_PREPARE_MAX_S)
            boolean bad = failed || curtain.failed();
            if (bad) failed = true;   // the log block shows (tail)
            curtainEnd(bad);
            return;
        }
        ui.postDelayed(this::curtainPoll, CURTAIN_POLL_MS);
    }

    /** The curtain leaves (fast: at once, for a failure, a pause or Back); nothing when it is gone already */
    private void curtainEnd(boolean fast) {
        if (curtain == null) return;
        PlayCurtain c = curtain;
        curtain = null;
        c.finish(fast, null);
    }

    // ---- The host's side of the Activity: input devices, audio focus and route, motion, lifecycle ----
    private void hostSetup() {
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, ui);
        for (int id : inputManager.getInputDeviceIds()) onInputDeviceAdded(id);
        Native.motionRotation(getDisplay().getRotation());
        audio = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_UNKNOWN).build())
                .setOnAudioFocusChangeListener(f -> Native.audioInterrupt(f == AudioManager.AUDIOFOCUS_LOSS || f == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT
                        || f == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK), ui)
                .build();
        // Granted at once calls no listener: an interruption the screen that went saw (a call while in the background) is over for this one
        if (audio.requestAudioFocus(focus) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED) Native.audioInterrupt(false);
        else Native.note("audio focus not granted at start");
        // The callback first reports the devices already there: only later changes reopen the streams
        route = new AudioDeviceCallback() {
            @Override public void onAudioDevicesAdded(AudioDeviceInfo[] d) { if (routeReady) Native.audioRouteChanged(); }
            @Override public void onAudioDevicesRemoved(AudioDeviceInfo[] d) { if (routeReady) Native.audioRouteChanged(); }
        };
        // Off the UI thread (Banana Native 4299044a, its issue 251): the call lists the audio ports in system_server and can take seconds (an ANR in
        // AudioSystem.listAudioPatches on an Exynos phone)
        final AudioDeviceCallback cb = route; final AudioManager am = audio;
        Thread rt = new Thread(() -> {
            am.registerAudioDeviceCallback(cb, ui);
            if (route != cb) am.unregisterAudioDeviceCallback(cb);   // the screen went meanwhile
            else ui.post(() -> routeReady = true);
        }, "audio-route");
        rt.start();
    }

    // ---- Paused: the game suspended while its screen shows (nxhost_lifecycle 4 / 5); on its return from the background with pause-on-return on (the game's setting, on by
    // default), and by the in-game panel's Pause. The host keeps it suspended (clock, graphics, guest threads, audio, focus Background) until unpause() ----
    private boolean paused, stopped;
    private TextView pausedView;

    private String titleId() { return String.valueOf(getIntent().getStringExtra("title_id")); }

    /** The in-game panel's Pause */
    void pause() {
        if (!started || paused) return;
        paused = true;
        curtainEnd(true);   // a suspended game draws no first frame: Paused must be reachable
        Native.lifecycle(4);
        Native.motionStop();
        pausedView.setVisibility(View.VISIBLE);
    }

    private void unpause() {
        if (!paused) return;
        paused = false;
        pausedView.setVisibility(View.GONE);
        Native.lifecycle(5);
        if (resumedNow) Native.motionStart();
    }

    @Override protected void onStart() {
        super.onStart();
        if (!started) return;
        // back from the background: stays suspended (4 before 2, so the host never lets it run in between) until the tap
        if (stopped && !paused && Prefs.pauseOnReturn(this, titleId())) {
            paused = true; Native.lifecycle(4);
            if (!controls.editing()) pausedView.setVisibility(View.VISIBLE);   // the layout editor stays usable; its Done shows Paused (onEditEnd)
        }
        if (paused) curtainEnd(true);   // a suspended game draws no first frame: Paused must be reachable
        stopped = false;
        hostInBackground = false;
        Native.lifecycle(2);
    }
    @Override protected void onResume() {
        super.onResume();
        Ui.immersive(this);
        if (!started) return;
        resumedNow = true;
        Native.lifecycle(3);
        Native.motionRotation(getDisplay().getRotation());
        if (!paused) Native.motionStart();   // paused: the sensors stay off (they would wake the host at their rate for a frozen game); unpause starts them
    }
    private boolean resumedNow;
    @Override protected void onPause() {
        resumedNow = false;
        if (controls != null) controls.cancelInput();
        if (started) { Native.motionStop(); Native.lifecycle(0); }
        super.onPause();
    }
    @Override protected void onStop() {
        if (started) { stopped = true; if (!isChangingConfigurations()) hostInBackground = true; Native.lifecycle(1); }
        super.onStop();
    }
    @Override public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        if (started) Native.motionRotation(getDisplay().getRotation());
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) Ui.immersive(this);
    }

    @Override public void surfaceCreated(SurfaceHolder h) { Native.setSurface(h.getSurface()); }
    // The replay letterboxes the game's 16:9 picture into the window (vka_present.c); TouchControls gives the host that rectangle (nxhost_touch_layout) with its own size
    @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int ht) { Native.setSurface(h.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder h) { Native.setSurface(null); }

    private static boolean gamepad(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private static boolean realPad(InputDevice d) {
        if (d == null || d.isVirtual()) return false;
        String n = d.getName();
        return n == null || !n.startsWith("uinput-");
    }

    @Override public void onInputDeviceAdded(int id) {
        InputDevice d = InputDevice.getDevice(id);
        // Gamepads and joysticks only: a keyboard with arrow keys (SOURCE_DPAD) gets a player from its first pad key, if it ever sends one
        if (realPad(d) && (d.supportsSource(InputDevice.SOURCE_GAMEPAD) || d.supportsSource(InputDevice.SOURCE_JOYSTICK))) {
            Native.deviceAdded(id, d.getName(), false);
            padConnected(id);
        }
        deviceAxes.remove(id);
    }
    @Override public void onInputDeviceRemoved(int id) {
        Native.deviceRemoved(id); deviceAxes.remove(id);
        if (pads.remove(id)) padsChanged(!pads.isEmpty());
    }
    private void padConnected(int id) { if (pads.add(id)) padsChanged(true); }

    /** A gamepad connected, or the last one gone: the on-screen controls and the panel's handle hide or come back (the pad's Home opens the panel meanwhile) */
    private void padsChanged(boolean connected) {
        controls.setGamepad(connected);
        if (panelHandle != null) panelHandle.setVisibility(connected ? View.GONE : View.VISIBLE);
    }
    @Override public void onInputDeviceChanged(int id) { deviceAxes.remove(id); }

    /*
     * Minus + Plus (BUTTON_SELECT + BUTTON_START) held together for a second also opens the in-game panel: the way in for a pad whose Home never reaches the app
     * (its key layout makes it KEYCODE_HOME, which the system keeps: a Switch Pro Controller's Home, a HID pad's consumer Home key). The game gets both presses
     * as usual meanwhile (a short press of either is the game's); when the hold opens the panel the game gets both releases at once, and the real releases
     * that follow are swallowed
     */
    private final HashSet<Integer> holdKeys = new HashSet<>();   // Minus and Plus down now
    private boolean holdOpened;   // the hold opened the panel: Minus and Plus are swallowed until both are up
    private int holdDevice;
    private final Runnable holdOpen = () -> {
        holdOpened = true;
        if (input) { Native.key(holdDevice, KeyEvent.KEYCODE_BUTTON_SELECT, false); Native.key(holdDevice, KeyEvent.KEYCODE_BUTTON_START, false); }
        if (panel != null && !panel.isOpen()) panel.open();
    };

    /** Minus + Plus held (holdOpen): true when the key is swallowed */
    private boolean holdToOpen(KeyEvent e) {
        int k = e.getKeyCode();
        if (k != KeyEvent.KEYCODE_BUTTON_SELECT && k != KeyEvent.KEYCODE_BUTTON_START || !gamepad(e.getSource())) return false;
        if (e.getAction() == KeyEvent.ACTION_DOWN) {
            if (e.getRepeatCount() == 0 && holdKeys.add(k) && holdKeys.size() == 2 && !holdOpened && !panel.isOpen()) {
                holdDevice = e.getDeviceId();
                ui.postDelayed(holdOpen, 1000);
            }
            return holdOpened;
        }
        if (e.getAction() != KeyEvent.ACTION_UP) return false;
        holdKeys.remove(k);
        ui.removeCallbacks(holdOpen);
        boolean swallow = holdOpened;
        if (holdKeys.isEmpty()) holdOpened = false;
        return swallow;
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (panel == null) return super.dispatchKeyEvent(e);   // no host for this game (onCreate): only the message is shown
        // A gamepad's Home / guide button opens and closes the in-game panel; while it is open the pad moves through it and the game gets no pad keys
        if (e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_MODE) { if (e.getAction() == KeyEvent.ACTION_UP) panel.toggle(); return true; }
        if (holdToOpen(e)) return true;   // or Minus + Plus held for a second
        if (panel.isOpen() && gamepad(e.getSource())) return panel.padKey(PadNav.labelled(this, e)) || super.dispatchKeyEvent(e);   // its A and B as the launcher's (PadNav)
        if (paused && gamepad(e.getSource())) {   // paused: A or Start continues; no pad key reaches the game
            if (e.getAction() == KeyEvent.ACTION_UP && (PadNav.label(this, e) == KeyEvent.KEYCODE_BUTTON_A || e.getKeyCode() == KeyEvent.KEYCODE_BUTTON_START)) unpause();
            return true;
        }
        // Back from the phone and keyboard keys stay with the launcher: the host takes only pad buttons
        if (input && gamepad(e.getSource()) && e.getDevice() != null && !e.getDevice().isVirtual()
                && (e.getAction() == KeyEvent.ACTION_DOWN || e.getAction() == KeyEvent.ACTION_UP)
                && Native.key(e.getDeviceId(), e.getKeyCode(), e.getAction() == KeyEvent.ACTION_DOWN)) { padConnected(e.getDeviceId()); return true; }
        return super.dispatchKeyEvent(e);
    }

    // The axes the host reads (input_map.h), and per device the ones it has (InputDevice.getMotionRange): absent axes would read 0 and pull a stick or trigger back
    private static final int[] AXES = {MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ, MotionEvent.AXIS_RX, MotionEvent.AXIS_RY,
            MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS, MotionEvent.AXIS_BRAKE};
    private final HashMap<Integer, int[]> deviceAxes = new HashMap<>();
    private final float[] axisValues = new float[AXES.length];

    private int[] axesOf(InputDevice d, int source) {
        int[] have = deviceAxes.get(d.getId());
        if (have != null) return have;
        int n = 0;
        int[] tmp = new int[AXES.length];
        for (int a : AXES) if (d.getMotionRange(a, source) != null) tmp[n++] = a;
        have = Arrays.copyOf(tmp, n);
        deviceAxes.put(d.getId(), have);
        return have;
    }

    @Override public boolean onGenericMotionEvent(MotionEvent e) {
        if (!input || panel.isOpen() || paused || (e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK || e.getAction() != MotionEvent.ACTION_MOVE || e.getDevice() == null)
            return super.onGenericMotionEvent(e);
        int[] have = axesOf(e.getDevice(), e.getSource());
        for (int i = 0; i < have.length; i++) axisValues[i] = e.getAxisValue(have[i]);
        return Native.axes(e.getDeviceId(), have, axisValues) || super.onGenericMotionEvent(e);   // the host reads have.length values
    }


    /** Back (the phone's button or its edge swipe) opens and closes the in-game panel, which has Stop */
    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { curtainEnd(true); if (panel != null) panel.toggle(); else end(); }   // during the curtain: the panel (with Stop) shows, uncovered
}
