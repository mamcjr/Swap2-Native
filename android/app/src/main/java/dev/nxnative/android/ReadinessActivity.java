package dev.nxnative.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.usage.UsageStatsManager;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.StatFs;
import android.provider.Settings;
import android.text.format.DateUtils;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The Readiness check's screen (Settings › Readiness check), in a process of its own (":readiness") because it loads a host (the APK's first game's: any host reports the
 * GPU) to ask Vulkan what this phone has (Native.readiness), on the GPU driver picked for every game. Cards: the phone (Android, memory, files access, storage, battery
 * restrictions, the last game exit and memory kills of the last 7 days), the GPU and driver, then for each game of this APK the host's needs and the game's own (Readiness),
 * each pass / warning / fail with a fix button where there is one. Run again; Copy report and Share report hand the whole report on for a compatibility report.
 * The cards, Run again, the fix buttons, the last exit and the shareable report are adapted from the author's earlier readiness check. The same check runs before every
 * game starts (GameActivity), on that game's own driver, and stops a start that would fail with a sentence and Try anyway.
 */
public final class ReadinessActivity extends Activity {
    /** MainActivity's request: a result of FIX_DRIVER asks it to open the GPU driver dialog (it lives in the launcher's process) */
    static final int REQUEST = 7, FIX_DRIVER = RESULT_FIRST_USER;
    private static final long KILL_WINDOW_MS = 7 * DateUtils.DAY_IN_MILLIS;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private LinearLayout list;
    private TextView summary, verdict, mark;
    private String report = "";
    // This process's Vulkan facts and the driver they were read on: the driver is opened once per process (vka_driver_open), so they are static, kept by a screen opened
    // again in the same process (left by Home, not Back), which then sees a changed pick as stale instead of reading the old driver as the new one
    private static String vulkan;
    private static String probedDriver;

    /** The launcher's theme choice (Settings › Theme, LauncherTheme) */
    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(base);
        LauncherTheme.apply(this);
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Ui.col(this, R.color.bn_bg));
        setContentView(frame);
        LinearLayout content = Ui.page(this, frame, 640);   // the same gutter, width and header as the launcher's pages
        content.addView(Ui.header(this, Ui.back(this, v -> leave()), getString(R.string.s2s_rd_screen_title), null));
        // The verdict: a mark, one line, the counts; then Run again and the report. Each check below shows its details on a tap (failures and warnings open)
        LinearLayout top = Ui.add(content, Ui.card(this), 16);
        LinearLayout vr = Ui.row(this);
        mark = Ui.text(this, "…", 26, R.color.bn_on_banana, 900);
        mark.setGravity(android.view.Gravity.CENTER);
        mark.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        vr.addView(mark, new LinearLayout.LayoutParams(Ui.dp(this, 52), Ui.dp(this, 52)));
        LinearLayout vc = new LinearLayout(this);
        vc.setOrientation(LinearLayout.VERTICAL);
        vc.setPadding(Ui.dp(this, 14), 0, 0, 0);
        verdict = Ui.heading(this, getString(R.string.s2s_rd_checking));
        vc.addView(verdict);
        summary = Ui.small(this, "");
        vc.addView(summary);
        vr.addView(vc, Ui.weight());
        top.addView(vr);
        setMark(-1);
        Button share = Ui.button(this, getString(R.string.s2s_rd_share), Ui.Kind.TONAL, v -> share());
        share.setOnLongClickListener(v -> { copy(this, report); return true; });
        Ui.add(top, Ui.pair(this, Ui.button(this, getString(R.string.s2s_rd_run_again), Ui.Kind.TONAL, v -> run()), share), 16);
        Ui.add(top, Ui.button(this, getString(R.string.s2s_rd_copy_report), Ui.Kind.QUIET, v -> copy(this, report)), 6, Ui.wrap());
        list = Ui.add(content, new LinearLayout(this), 0);
        list.setOrientation(LinearLayout.VERTICAL);
        try {
            Native.load(Profile.first().titleId);   // any game's host reports the GPU: the APK's first game's
        } catch (RuntimeException e) {   // (IllegalStateException: no host; an APK always has a game)
            vulkan = "unchecked=" + e.getMessage();
        }
        run();
    }

    @Override protected void onResume() {
        super.onResume();
        if (list != null && list.getChildCount() > 0) run();   // back from a fix (the app's settings, files access)
    }

    /** The checks on a thread of their own (the Vulkan query loads the driver), then the cards */
    private boolean running;
    private void run() {
        if (running) return;
        running = true;
        verdict.setText(R.string.s2s_rd_checking);
        summary.setText("");
        setMark(-1);
        Context app = getApplicationContext();
        new Thread(() -> {
            GpuDriver.Info d = GpuDriver.selected(app);
            String drv = d == null ? "" : d.dir.getPath();
            if (vulkan == null || !vulkan.startsWith("unchecked=")) {
                if (probedDriver == null || probedDriver.equals(drv)) {
                    List<String> env = new ArrayList<>();
                    GpuDriver.env(app, null, env);
                    String v = Native.readiness(Readiness.driverEnv(env));
                    vulkan = v;
                    probedDriver = drv;
                }
            }
            boolean stale = probedDriver != null && !probedDriver.equals(drv);
            Result r = collect(app, vulkan, stale);
            ui.post(() -> { running = false; show(r); });
        }, "nxreadiness").start();
    }

    private static final class Section {
        final String heading;
        final List<Readiness.Check> checks;

        Section(String heading, List<Readiness.Check> checks) { this.heading = heading; this.checks = checks; }
    }

    private static final class Result {
        final List<Section> sections = new ArrayList<>();
        String report, verdict, summary;
        int level;   // 0 ready, 1 ready with less, 2 not ready
    }

    private static Result collect(Context c, String vulkan, boolean stale) {
        Result r = new Result();
        Context en = english(c);   // the report's wording; the screen's is c's
        Readiness.Words w = words(c);
        Map<String, String> f = Readiness.merge(vulkan, ramGib(c), Build.VERSION.SDK_INT);
        List<Readiness.Check> phone = phoneChecks(c, en);
        GpuDriver.Info d = GpuDriver.selected(c);
        List<Readiness.Check> gpu = new ArrayList<>();
        String gpuLine = Readiness.gpu(f);
        gpu.add(check(c, en, R.string.s2s_rd_gpu_and_driver, stale ? Readiness.Status.WARNING : f.containsKey("gpu.name") ? Readiness.Status.INFO : f.containsKey("error") ? Readiness.Status.FAIL : Readiness.Status.WARNING,
                "driver", x -> stale ? x.getString(R.string.s2s_rd_gpu_stale, gpuLine) : gpuLine));
        gpu.add(check(c, en, R.string.s2s_rd_driver_picked, Readiness.Status.INFO, "driver", x -> x.getString(R.string.s2s_rd_driver_picked_msg, d == null ? x.getString(R.string.s2s_rd_system) : d.label())));
        r.sections.add(new Section(c.getString(R.string.s2s_rd_this_phone), phone));
        r.sections.add(new Section(c.getString(R.string.s2s_rd_gpu), gpu));
        StringBuilder rep = new StringBuilder(header(c, null)).append('\n');
        int fails = 0, warns = 0;
        for (Readiness.Check x : phone) rep.append(x).append('\n');
        for (Readiness.Check x : gpu) rep.append(x).append('\n');
        List<Game> added = Game.load(c);
        for (Profile p : Profile.all()) {
            List<Readiness.Check> l = new ArrayList<>();
            Readiness.Check st = Readiness.status(p.status, w);
            if (st != null) l.add(st);
            l.addAll(Readiness.evaluate(f, Readiness.parseReqs(p.reqs), w));
            String own = GpuDriver.choice(c, p.titleId);
            if (own != null) {
                // This screen probes the main screen's driver only (the driver opens once a process, vka_driver_open): a game with another driver of its own is
                // checked on that one when it starts, and that check can differ from this screen's
                GpuDriver.Info g = GpuDriver.selected(c, p.titleId);
                boolean same = g == null ? d == null : d != null && g.dir.equals(d.dir);
                l.add(same ? check(c, en, R.string.s2s_rd_own_driver, Readiness.Status.INFO, "driver", x -> x.getString(R.string.s2s_rd_own_driver_same, driverName(x, g)))
                        : check(c, en, R.string.s2s_rd_own_driver, Readiness.Status.WARNING, "driver", x -> x.getString(R.string.s2s_rd_own_driver_other, driverName(x, g), driverName(x, d))));
            }
            String name = p.titleId;   // a game is named as the user's dump names it (its card), else by its title ID
            for (Game g : added) if (g.titleId.equals(p.titleId)) { name = g.name; break; }
            for (Game g : added) if (g.titleId.equals(p.titleId)) l.add(gameFiles(c, en, g));
            r.sections.add(new Section(name, l));
            boolean fail = Readiness.any(l, Readiness.Status.FAIL), warn = Readiness.any(l, Readiness.Status.WARNING);
            if (fail) fails++; else if (warn) warns++;
            rep.append("-- game ").append(p.titleId).append(" --\n");
            for (Readiness.Check x : l) rep.append(x).append('\n');
        }
        String last = lastReports(c);
        if (!last.isEmpty()) rep.append(last);
        rep.append("-- facts --\n");
        for (Map.Entry<String, String> e : f.entrySet()) rep.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        r.report = rep.toString();
        int phoneFails = Readiness.of(phone, Readiness.Status.FAIL).size();
        int phoneWarns = Readiness.of(phone, Readiness.Status.WARNING).size();
        r.level = fails > 0 || phoneFails > 0 ? 2 : warns > 0 ? 1 : 0;
        r.verdict = c.getString(r.level == 2 ? R.string.s2s_rd_not_ready : r.level == 1 ? R.string.s2s_rd_ready_limits : R.string.s2s_rd_ready);
        android.content.res.Resources res = c.getResources();
        r.summary = r.level == 2 ? (fails > 0 ? res.getQuantityString(R.plurals.s2s_rd_games_lack, fails, fails) : "") + (fails > 0 && phoneFails > 0 ? "; " : "")
                + (phoneFails > 0 ? res.getQuantityString(R.plurals.s2s_rd_phone_problems, phoneFails, phoneFails) : "") + "."
                : r.level == 1 ? res.getQuantityString(R.plurals.s2s_rd_games_run_less, warns, warns) : c.getString(phoneWarns > 0 ? R.string.s2s_rd_all_met_warnings : R.string.s2s_rd_all_met);
        return r;
    }

    private void show(Result r) {
        if (isFinishing() || isDestroyed()) return;
        report = r.report;
        verdict.setText(r.verdict);
        summary.setText(r.summary);
        setMark(r.level);
        list.removeAllViews();
        for (Section s : r.sections) {
            Ui.add(list, Ui.section(this, s.heading), Ui.SECTION_GAP);
            LinearLayout card = Ui.add(list, Ui.listCard(this), 8);
            boolean gpu = s == r.sections.get(1);   // the GPU section's facts are what a report needs most: open
            for (Readiness.Check x : s.checks) Ui.add(card, row(x, gpu), 2);
        }
    }

    /** The verdict's mark: ✓ on leaf green, ! on banana, ✕ on red-orange; … while checking */
    private void setMark(int level) {
        mark.setText(level == 0 ? "✓" : level == 1 ? "!" : level == 2 ? "✕" : "…");
        int fill = level == 0 ? R.color.bn_leaf : level == 2 ? R.color.bn_danger : R.color.bn_banana;
        mark.setTextColor(Ui.col(this, level == 0 ? R.color.bn_on_leaf : level == 2 ? R.color.bn_card : R.color.bn_on_banana));
        mark.setBackground(Ui.round(Ui.col(this, fill), Ui.dp(this, 999), 0, 0));
    }

    /** One check: its mark and title; the details show on a tap, and open from the start for a failure or a warning (with its fix button) */
    private View row(Readiness.Check x, boolean openInfo) {
        boolean bad = x.status == Readiness.Status.FAIL || x.status == Readiness.Status.WARNING, open0 = bad || openInfo && x.status == Readiness.Status.INFO;
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Ui.dp(this, 10), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        box.setBackground(Ui.ripple(this, Ui.round(bad ? Ui.col(this, x.status == Readiness.Status.FAIL ? R.color.bn_danger_tint : R.color.bn_warn_tint) : android.graphics.Color.TRANSPARENT,
                Ui.dp(this, 16), 0, 0)));
        LinearLayout top = Ui.row(this);
        String m = x.status == Readiness.Status.PASS ? "✓" : x.status == Readiness.Status.WARNING ? "!" : x.status == Readiness.Status.FAIL ? "✕" : "i";
        TextView st = Ui.text(this, m, 15, x.status == Readiness.Status.FAIL ? R.color.bn_danger : x.status == Readiness.Status.PASS ? R.color.bn_leaf : R.color.bn_text_2, 900);
        st.setGravity(android.view.Gravity.CENTER);
        top.addView(st, new LinearLayout.LayoutParams(Ui.dp(this, 24), ViewGroup.LayoutParams.WRAP_CONTENT));
        TextView t = Ui.text(this, x.shownTitle(), 15, R.color.bn_text, 600);
        t.setPadding(Ui.dp(this, 8), 0, 0, 0);
        top.addView(t, Ui.weight());
        android.widget.ImageView chev = new android.widget.ImageView(this);   // turned down when closed, up when open
        chev.setImageDrawable(Ui.icon(this, R.drawable.ic_chevron, R.color.bn_text_2));
        chev.setRotation(open0 ? -90 : 90);
        chev.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        top.addView(chev, new LinearLayout.LayoutParams(Ui.dp(this, 20), Ui.dp(this, 20)));
        box.addView(top);
        LinearLayout more = new LinearLayout(this);
        more.setOrientation(LinearLayout.VERTICAL);
        more.setPadding(Ui.dp(this, 32), 0, 0, 0);
        TextView msg = Ui.small(this, x.shownMessage());
        msg.setTextIsSelectable(false);
        more.addView(msg);
        View.OnClickListener fix = fix(x);
        if (fix != null && bad) Ui.add(more, Ui.button(this, fixLabel(x.fix), Ui.Kind.SOFT, fix), 8, Ui.wrap());
        Ui.add(box, more, 4);
        more.setVisibility(open0 ? View.VISIBLE : View.GONE);
        box.setOnClickListener(v -> {
            boolean open = more.getVisibility() != View.VISIBLE;
            more.setVisibility(open ? View.VISIBLE : View.GONE);
            chev.setRotation(open ? -90 : 90);
        });
        return box;
    }

    private String fixLabel(String fix) {
        switch (fix) {
            case "driver": return getString(R.string.s2s_rd_fix_driver);
            case "access": return getString(R.string.s2s_rd_fix_access);
            default: return getString(R.string.s2s_rd_fix_app);
        }
    }

    private View.OnClickListener fix(Readiness.Check x) {
        if (x.fix == null) return null;
        switch (x.fix) {
            case "driver": return v -> { setResult(FIX_DRIVER); leave(); };
            case "access": return v -> settings(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            case "app": return v -> settings(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
            default: return null;   // memory: nothing on the phone changes it
        }
    }

    private void settings(String action) {
        try { startActivity(new Intent(action, Uri.parse("package:" + getPackageName()))); }
        catch (ActivityNotFoundException e) { startActivity(new Intent(Settings.ACTION_SETTINGS)); }
    }

    private void share() {
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_SUBJECT, "nxnative readiness report").putExtra(Intent.EXTRA_TEXT, report);
        startActivity(Intent.createChooser(send, getString(R.string.s2s_rd_share_title)));
    }

    static void copy(Context c, String text) {
        ClipboardManager cm = c.getSystemService(ClipboardManager.class);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("Readiness report", text));
        Toast.makeText(c, R.string.s2s_rd_copied, Toast.LENGTH_SHORT).show();
    }

    // ---- The checks' wording: English for the report (and the log), the phone's language on the screen ----

    /** c with English resources (values/): the report's wording, whatever the phone's language */
    static Context english(Context c) {
        android.content.res.Configuration conf = new android.content.res.Configuration(c.getResources().getConfiguration());
        conf.setLocale(Locale.ENGLISH);
        return c.createConfigurationContext(conf);
    }

    /** A text of a check in one language (the context's) */
    private interface Text { String in(Context x); }

    /** A check worded twice from the same resources: in English for the report (en), in the phone's language for the screen (c) */
    private static Readiness.Check check(Context c, Context en, int title, Readiness.Status status, String fix, Text msg) {
        return new Readiness.Check(en.getString(title), status, msg.in(en), fix, null, c.getString(title), msg.in(c));
    }

    private static String driverName(Context x, GpuDriver.Info i) { return i == null ? x.getString(R.string.s2s_rd_system) : i.label(); }

    /** Readiness's fixed texts in the phone's language (Readiness.Words), for the screen and the start-up check */
    static Readiness.Words words(Context c) {
        return (key, args) -> {
            int id = wordText(key);
            if (id == 0) return null;
            return args == null || args.length == 0 ? c.getString(id) : c.getString(id, args);
        };
    }

    private static int wordText(String key) {
        switch (key) {
            case "gpu_features": return R.string.s2s_rd_w_gpu_features;
            case "gpu_error": return R.string.s2s_rd_w_gpu_error;
            case "gpu_unchecked": return R.string.s2s_rd_w_gpu_unchecked;
            case "unchecked_host": return R.string.s2s_rd_w_unchecked_host;
            case "alias": return R.string.s2s_rd_w_alias;
            case "present": return R.string.s2s_rd_w_present;
            case "lacks_required": return R.string.s2s_rd_w_lacks_required;
            case "lacks_degraded": return R.string.s2s_rd_w_lacks_degraded;
            case "not_reported": return R.string.s2s_rd_w_not_reported;
            case "reports": return R.string.s2s_rd_w_reports;
            case "label_ram": return R.string.s2s_rd_w_label_ram;
            case "status_title": return R.string.s2s_rd_w_status_title;
            case "bring_up": return R.string.s2s_rd_w_bring_up;
            case "req_android": return R.string.s2s_rd_w_req_android;
            case "req_vk_instance": return R.string.s2s_rd_w_req_vk_instance;
            case "req_vk_api": return R.string.s2s_rd_w_req_vk_api;
            case "req_queue": return R.string.s2s_rd_w_req_queue;
            case "req_timeline": return R.string.s2s_rd_w_req_timeline;
            case "req_dynamic_rendering": return R.string.s2s_rd_w_req_dynamic_rendering;
            case "req_swapchain": return R.string.s2s_rd_w_req_swapchain;
            case "req_map_memory2": return R.string.s2s_rd_w_req_map_memory2;
            case "req_placed": return R.string.s2s_rd_w_req_placed;
            case "req_tex_update_after_bind": return R.string.s2s_rd_w_req_tex_update_after_bind;
            case "req_tex_partially_bound": return R.string.s2s_rd_w_req_tex_partially_bound;
            case "req_tex_runtime_arrays": return R.string.s2s_rd_w_req_tex_runtime_arrays;
            default: return 0;
        }
    }

    // ---- What the launcher knows without Vulkan (shared with GameActivity) ----

    /** The phone's memory as Android reports it (ActivityManager totalMem, GiB): less than the label, by what the firmware and kernel keep (a 16 GB phone reports about
     *  14.5-15.5 GiB, a 12 GB one about 10.5-11.5) */
    static double ramGib(Context c) {
        ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
        c.getSystemService(ActivityManager.class).getMemoryInfo(m);
        return m.totalMem / (double) (1L << 30);
    }

    /**
     * Whether the dump of an added game can be read: a folder (readable, with base/exefs/main.npdm or exefs/main.npdm), or a .nup (Nup.open: its header and the
     * SHA-256 of its table of contents; its packages listed)
     */
    static Readiness.Check gameFiles(Context c, Context en, Game g) {
        File f = new File(g.gameDir);
        String path = f.getPath();
        int title = R.string.s2s_rd_game_files;
        if (Nup.isNup(f)) {
            try {
                Nup n = Nup.open(f);
                StringBuilder s = new StringBuilder();
                for (Nup.Pkg p : n.packages) s.append(s.length() == 0 ? "" : ", ").append(p.selector() != null ? p.selector() : "kind " + p.kind);
                String pkgs = s.toString();
                return check(c, en, title, Readiness.Status.PASS, null, x -> x.getString(R.string.s2s_rd_files_nup_ok, path, pkgs));
            } catch (IOException e) {
                String why = String.valueOf(e.getMessage());
                return check(c, en, title, Readiness.Status.FAIL, f.canRead() ? null : "access", x -> x.getString(R.string.s2s_rd_files_unusable, path, why));
            }
        }
        if (!f.isDirectory() || !f.canRead()) return check(c, en, title, Readiness.Status.FAIL, "access", x -> x.getString(R.string.s2s_rd_files_unreadable, path));
        if (Pack.npdmOf(f) == null) return check(c, en, title, Readiness.Status.FAIL, null, x -> x.getString(R.string.s2s_rd_files_no_npdm, path));
        return check(c, en, title, Readiness.Status.PASS, null, x -> x.getString(R.string.s2s_rd_files_folder_ok, path));
    }

    /** The report's first lines: the build, the phone, Android, and the game when there is one */
    static String header(Context c, String game) {
        return "nxnative readiness report: " + Prefs.buildTag(c) + "\nSigner: " + Prefs.signerId(c) + " (SHA-256 of the signing certificate, first 16 hex digits)\nPhone: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + "), Android "
                + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + "), " + Build.SUPPORTED_ABIS[0] + (game == null ? "" : "\nGame: " + game);
    }

    /** The newest start-up check of each game (GameActivity writes files/readiness-&lt;title ID&gt;.txt), for the report */
    private static String lastReports(Context c) {
        StringBuilder s = new StringBuilder();
        File files = c.getExternalFilesDir(null);
        for (Profile p : Profile.all()) {
            File f = new File(files, "readiness-" + p.titleId + ".txt");
            if (!f.isFile()) continue;
            try {
                String t = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
                String first = t.contains("\n[") ? t.substring(t.indexOf("\n[") + 1) : t;
                int end = first.indexOf("-- facts --");
                s.append("-- last start of ").append(p.titleId).append(" (").append(DateUtils.getRelativeTimeSpanString(f.lastModified())).append(") --\n")
                        .append(end >= 0 ? first.substring(0, end) : first);
            } catch (IOException ignored) { /* left out */ }
        }
        return s.toString();
    }

    private static List<Readiness.Check> phoneChecks(Context c, Context en) {
        List<Readiness.Check> l = new ArrayList<>();
        boolean arm64 = java.util.Arrays.asList(Build.SUPPORTED_64_BIT_ABIS).contains("arm64-v8a");
        l.add(check(c, en, R.string.s2s_rd_phone, arm64 ? Readiness.Status.PASS : Readiness.Status.FAIL, null,
                x -> x.getString(arm64 ? R.string.s2s_rd_phone_arm64 : R.string.s2s_rd_phone_not_arm64, Build.MANUFACTURER, Build.MODEL, Build.VERSION.RELEASE)));
        ActivityManager am = c.getSystemService(ActivityManager.class);
        ActivityManager.MemoryInfo m = new ActivityManager.MemoryInfo();
        am.getMemoryInfo(m);
        double total = m.totalMem / (double) (1L << 30), avail = m.availMem / (double) (1L << 30);
        boolean low = m.lowMemory;
        l.add(check(c, en, R.string.s2s_rd_memory, Readiness.Status.INFO, null, x -> x.getString(low ? R.string.s2s_rd_memory_low : R.string.s2s_rd_memory_msg, total, avail)));
        boolean access = Environment.isExternalStorageManager();
        l.add(check(c, en, R.string.s2s_rd_files_access, access ? Readiness.Status.PASS : Readiness.Status.FAIL, "access",
                x -> x.getString(access ? R.string.s2s_rd_access_granted : R.string.s2s_rd_access_denied)));
        try {
            File ext = c.getExternalFilesDir(null);
            long free = new StatFs((ext != null ? ext : c.getFilesDir()).getPath()).getAvailableBytes();
            double gb = free / 1e9;
            l.add(check(c, en, R.string.s2s_rd_storage, gb >= 10 ? Readiness.Status.PASS : Readiness.Status.WARNING, null,
                    x -> x.getString(gb >= 10 ? R.string.s2s_rd_storage_ok : R.string.s2s_rd_storage_low, gb)));
        } catch (IllegalArgumentException e) { /* no such folder: left out */ }
        UsageStatsManager us = c.getSystemService(UsageStatsManager.class);
        if (am.isBackgroundRestricted()) l.add(check(c, en, R.string.s2s_rd_battery, Readiness.Status.WARNING, "app", x -> x.getString(R.string.s2s_rd_battery_restricted)));
        else if (us != null && us.getAppStandbyBucket() == UsageStatsManager.STANDBY_BUCKET_RESTRICTED)
            l.add(check(c, en, R.string.s2s_rd_battery, Readiness.Status.WARNING, "app", x -> x.getString(R.string.s2s_rd_battery_standby)));
        else l.add(check(c, en, R.string.s2s_rd_battery, Readiness.Status.PASS, null, x -> x.getString(R.string.s2s_rd_battery_ok)));
        // The game's process (:game) as Android remembers its ends: the newest, and kills for memory in the last 7 days
        try {
            List<ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(c.getPackageName(), 0, 0);
            ApplicationExitInfo lastGame = null; int kills = 0; long lastKill = 0, since = System.currentTimeMillis() - KILL_WINDOW_MS;
            for (ApplicationExitInfo e : exits) {
                if (e.getProcessName() == null || !e.getProcessName().endsWith(":game")) continue;
                if (lastGame == null || e.getTimestamp() > lastGame.getTimestamp()) lastGame = e;
                if (e.getTimestamp() >= since && memoryKill(e)) { kills++; lastKill = Math.max(lastKill, e.getTimestamp()); }
            }
            if (lastGame == null) l.add(check(c, en, R.string.s2s_rd_last_exit, Readiness.Status.INFO, null, x -> x.getString(R.string.s2s_rd_no_game_yet)));
            else {
                String when = DateUtils.getRelativeTimeSpanString(lastGame.getTimestamp(), System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS).toString();
                int why = lastGame.getReason();
                boolean normal = why == ApplicationExitInfo.REASON_EXIT_SELF || why == ApplicationExitInfo.REASON_USER_REQUESTED || why == ApplicationExitInfo.REASON_USER_STOPPED;
                boolean mem = memoryKill(lastGame);
                String desc = lastGame.getDescription();
                boolean withDesc = desc != null && !desc.isEmpty() && !normal;
                l.add(check(c, en, R.string.s2s_rd_last_exit, normal ? Readiness.Status.PASS : Readiness.Status.WARNING, null, x -> {
                    String what = mem ? x.getString(R.string.s2s_rd_exit_memory) : why == ApplicationExitInfo.REASON_CRASH || why == ApplicationExitInfo.REASON_CRASH_NATIVE ? x.getString(R.string.s2s_rd_exit_crashed)
                            : why == ApplicationExitInfo.REASON_ANR ? x.getString(R.string.s2s_rd_exit_anr) : normal ? x.getString(R.string.s2s_rd_exit_normal) : x.getString(R.string.s2s_rd_exit_other, exitName(x, why));
                    return normal ? x.getString(R.string.s2s_rd_last_exit_normal, what, when)
                            : withDesc ? x.getString(R.string.s2s_rd_last_exit_desc, what, when, desc) : x.getString(R.string.s2s_rd_last_exit_bad, what, when);
                }));
            }
            if (kills > 0) {
                int k = kills;
                String last = DateUtils.getRelativeTimeSpanString(lastKill, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS).toString();
                l.add(check(c, en, R.string.s2s_rd_memory_kills, Readiness.Status.WARNING, null, x -> x.getResources().getQuantityString(R.plurals.s2s_rd_kills, k, k, last)));
            }
            else l.add(check(c, en, R.string.s2s_rd_memory_kills, Readiness.Status.PASS, null, x -> x.getString(R.string.s2s_rd_kills_none)));
        } catch (RuntimeException e) { /* not available on this phone: left out */ }
        if (GameActivity.testApp()) {   // the test app's switches (env.txt): a report says which were on
            File env = new File(c.getExternalFilesDir(null), "env.txt");
            List<String> keys = new ArrayList<>();
            try {
                if (env.isFile()) for (String kv : GameActivity.envLines(new String(Files.readAllBytes(env.toPath()), StandardCharsets.UTF_8))) keys.add(kv.substring(0, kv.indexOf('=')).trim());
            } catch (IOException | SecurityException e) { keys.add("(env.txt unreadable)"); }
            String on = String.join(", ", keys);
            l.add(check(c, en, R.string.s2s_rd_switches, Readiness.Status.INFO, null, x -> on.isEmpty() ? x.getString(R.string.s2s_rd_switches_none) : x.getString(R.string.s2s_rd_switches_on, on)));
        }
        return l;
    }

    /** An end for memory: Android's low-memory reason, or a kill whose description names the low-memory killer (phones without the reason report a signal) */
    private static boolean memoryKill(ApplicationExitInfo e) {
        if (e.getReason() == ApplicationExitInfo.REASON_LOW_MEMORY) return true;
        String d = e.getDescription();
        return d != null && (d.contains("lmk") || d.toLowerCase(Locale.ROOT).contains("low memory"));
    }

    private static String exitName(Context x, int reason) {
        switch (reason) {
            case ApplicationExitInfo.REASON_SIGNALED: return x.getString(R.string.s2s_rd_reason_signal);
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return x.getString(R.string.s2s_rd_reason_resources);
            case ApplicationExitInfo.REASON_FREEZER: return x.getString(R.string.s2s_rd_reason_frozen);
            case ApplicationExitInfo.REASON_PERMISSION_CHANGE: return x.getString(R.string.s2s_rd_reason_permission);
            case ApplicationExitInfo.REASON_DEPENDENCY_DIED: return x.getString(R.string.s2s_rd_reason_dependency);
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return x.getString(R.string.s2s_rd_reason_start_failed);
            default: return x.getString(R.string.s2s_rd_reason_other, reason);
        }
    }

    /** The host and the driver live in this process: leaving ends it, so the next check opens the driver picked then */
    private void leave() {
        finish();
        ui.postDelayed(() -> Process.killProcess(Process.myPid()), 300);
    }

    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { leave(); }
}
