package dev.nxnative.android;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * The pipeline warm, in a process of its own (":warm"): the game's prewarm list compiled into its pipeline cache with the game closed (Native.warm, vka_warm.c), so the
 * game's first minutes do not stop for compiles. Started by Play when the game's last complete warm is not for this driver, library, host and system build (Warm.wanted);
 * the game then starts when it ends. "Play now" stops it early: the compiles under way finish, what was made is saved, and the game starts (its own prewarm goes on with
 * the rest, as before). Its log is logs/&lt;title ID&gt;-warm-&lt;time&gt;.log. The process ends with the screen: the Vulkan device and the driver's memory go with it
 * before the game's process starts its own.
 * Extras: title_id, name, game, pack (as GameActivity's), play (start the game at the end).
 */
public final class WarmActivity extends Activity {
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView line;
    private ProgressBar bar;
    private Button now;
    private volatile boolean running, stopping;
    private String id;
    private File pack;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        id = getIntent().getStringExtra("title_id"); pack = new File(getIntent().getStringExtra("pack"));
        String name = getIntent().getStringExtra("name");
        boolean play = getIntent().getBooleanExtra("play", false);
        int p = Math.round(24 * getResources().getDisplayMetrics().density);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL); root.setGravity(Gravity.CENTER); root.setPadding(p, p, p, p); root.setBackgroundColor(Color.rgb(12, 16, 14));
        TextView title = new TextView(this);
        title.setText(getString(R.string.s2_warm_title, name)); title.setTextColor(Color.WHITE); title.setTextSize(22); title.setGravity(Gravity.CENTER);
        TextView explain = new TextView(this);
        explain.setText(R.string.s2_warm_explain); explain.setTextColor(Color.rgb(190, 200, 192)); explain.setTextSize(15); explain.setGravity(Gravity.CENTER); explain.setPadding(0, p / 2, 0, p / 2);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setIndeterminate(true);
        line = new TextView(this); line.setTextColor(Color.WHITE); line.setTextSize(16); line.setGravity(Gravity.CENTER); line.setPadding(0, p / 2, 0, p / 2);
        now = new Button(this); now.setText(play ? R.string.s2_warm_play_now : R.string.s2_warm_stop);
        now.setOnClickListener(v -> stop());
        root.addView(title); root.addView(explain);
        root.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        root.addView(line); root.addView(now);
        setContentView(root);
        Ui.immersive(this);   // full screen, as the game's: no navigation bar over the text
        try { Native.load(id); } catch (IllegalStateException e) { finishWith(e.getMessage(), false); return; }
        File log = new File(LogProvider.dir(this), id + "-warm-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()) + ".log");
        List<String> env = Warm.env(this, id);
        line.setText(R.string.s2_warm_starting);
        running = true;
        Thread t = new Thread(null, () -> {
            String r = Native.warm(pack.getPath(), env.toArray(new String[0]), log.getPath());
            ui.post(() -> ended(r));
        }, "nxwarm", 1 << 20);
        t.start();
        ui.postDelayed(this::poll, 300);
    }

    private void poll() {
        if (!running) return;
        int[] pr = new int[2];
        if (Native.warmProgress(pr) && pr[1] > 0) {
            bar.setIndeterminate(false); bar.setMax(pr[1]); bar.setProgress(Math.min(pr[0], pr[1]));
            line.setText(getString(stopping ? R.string.s2_warm_stopping : R.string.s2_warm_progress, Math.min(pr[0], pr[1]), pr[1]));
        }
        ui.postDelayed(this::poll, 300);
    }

    private void stop() {
        if (!running) { next(); return; }
        if (stopping) return;
        stopping = true; now.setEnabled(false);
        line.setText(R.string.s2_warm_stopping_now);
        Native.warmStop();   // the compiles under way end, the cache is saved, Native.warm returns (ended)
    }

    /** r: "<rc> <made> <had> <dropped> <total>" (Native.warm), null when this host has no warm */
    private void ended(String r) {
        running = false;
        String[] f = r == null ? new String[0] : r.split(" ");
        boolean ok = f.length == 5 && f[0].equals("0");
        if (ok && !stopping) Warm.done(this, id, pack);   // complete: the next Play goes straight to the game until something changes
        if (r == null) { finishWith(getString(R.string.s2_warm_none), true); return; }
        if (!ok) { finishWith(getString(R.string.s2_warm_failed, f.length > 0 ? f[0] : "?"), true); return; }
        bar.setIndeterminate(false); bar.setMax(1); bar.setProgress(1);
        line.setText(getString(R.string.s2_warm_done, Integer.parseInt(f[1]), Integer.parseInt(f[2]), Integer.parseInt(f[4])));
        ui.postDelayed(this::next, 800);
    }

    private void finishWith(String msg, boolean thenNext) {
        running = false; line.setText(msg); bar.setVisibility(View.INVISIBLE);
        now.setEnabled(true); now.setText(getIntent().getBooleanExtra("play", false) ? R.string.s2_warm_play_now : R.string.s2_warm_close);
        now.setOnClickListener(v -> next());
        if (thenNext) ui.postDelayed(this::next, 2500);
    }

    private boolean left;
    /** The game (play) or back to the launcher; this process ends with the screen, taking the warm's Vulkan device with it */
    private void next() {
        if (left) return;
        left = true;
        Intent in = getIntent();
        if (in.getBooleanExtra("play", false))
            startActivity(new Intent(this, GameActivity.class).putExtra("title_id", id).putExtra("name", in.getStringExtra("name")).putExtra("game", in.getStringExtra("game"))
                    .putExtra("pack", pack.getPath()));
        finish();
        ui.postDelayed(() -> Process.killProcess(Process.myPid()), 500);
    }

    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) Ui.immersive(this); }

    @SuppressWarnings("deprecation")
    @Override public void onBackPressed() { if (running) stop(); else next(); }
}
