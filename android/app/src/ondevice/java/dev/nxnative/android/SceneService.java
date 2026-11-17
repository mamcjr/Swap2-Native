package dev.nxnative.android;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ResultReceiver;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Makes the Scene resolution's 720p overlay (SceneOverlay) in the packer's process (:pack, where the embedded Python runs; the launcher's process never loads it):
 * dm/tools/gsys_dynres.py on the player's RomFS, a few seconds (the first run in the process also unpacks the packer). Started by the game's settings with
 * startService while the launcher is in front; one run at a time, and none while a Prepare step runs (PackNative.packTryRun: BUSY, the launcher says so). It writes
 * into scene_720p.tmp/, which becomes scene_720p/ with its stamp once the tool has checked its own output, and answers on the caller's ResultReceiver: 0 and
 * "message" the overlay's folder, else the exit code (PackNative.BUSY, or the tool's) and "message" why (SceneOverlay.why from its log).
 * It also makes the Ground foliage's overlays (FoliageOverlay: dm/tools/gsys_foliage.py, started with startFoliage), one run at a time with the 720p's.
 */
public final class SceneService extends Service {
    static final String EXTRA_ID = "title_id", EXTRA_ROMFS = "romfs", EXTRA_SCALE = "scale", EXTRA_EXPECT = "expect", EXTRA_VERSION = "version",
            EXTRA_RECEIVER = "receiver", MESSAGE = "message", EXTRA_LEVEL = "foliage_level", EXTRA_OFF = "foliage_off";
    static final int BUSY = PackNative.BUSY;
    /** Another SceneService run is still making an overlay (one at a time: they share the tool and their folders); BUSY now means only a Prepare step */
    static final int RUNNING = 76;
    private static final java.util.concurrent.atomic.AtomicBoolean ACTIVE = new java.util.concurrent.atomic.AtomicBoolean();
    private static final String TAG = "nxscene", TOOL = "dm/tools/gsys_dynres.py", FOLIAGE_TOOL = "dm/tools/gsys_foliage.py";

    /** Starts a run (the launcher, in front); the answer comes on r */
    static void start(Context c, String titleId, String romfs, String scale, String expect, String version, ResultReceiver r) {
        c.startService(new Intent(c, SceneService.class).putExtra(EXTRA_ID, titleId).putExtra(EXTRA_ROMFS, romfs).putExtra(EXTRA_SCALE, scale)
                .putExtra(EXTRA_EXPECT, expect).putExtra(EXTRA_VERSION, version).putExtra(EXTRA_RECEIVER, r));
    }

    /** Starts a Ground foliage run: the level's overlay with keys false (FoliageOverlay); the answer comes on r as for start */
    static void startFoliage(Context c, String titleId, String romfs, String level, String keys, String version, ResultReceiver r) {
        c.startService(new Intent(c, SceneService.class).putExtra(EXTRA_ID, titleId).putExtra(EXTRA_ROMFS, romfs).putExtra(EXTRA_LEVEL, level).putExtra(EXTRA_OFF, keys)
                .putExtra(EXTRA_VERSION, version).putExtra(EXTRA_RECEIVER, r));
    }

    @Override public IBinder onBind(Intent i) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) { stopSelf(startId); return START_NOT_STICKY; }
        new Thread(() -> {
            ResultReceiver r = intent.getParcelableExtra(EXTRA_RECEIVER, ResultReceiver.class);
            int code;
            String msg;
            try {
                String[] got = intent.hasExtra(EXTRA_LEVEL)
                        ? runFoliage(intent.getStringExtra(EXTRA_ID), intent.getStringExtra(EXTRA_ROMFS), intent.getStringExtra(EXTRA_LEVEL), intent.getStringExtra(EXTRA_OFF),
                                     intent.getStringExtra(EXTRA_VERSION))
                        : run(intent.getStringExtra(EXTRA_ID), intent.getStringExtra(EXTRA_ROMFS), intent.getStringExtra(EXTRA_SCALE),
                              intent.getStringExtra(EXTRA_EXPECT), intent.getStringExtra(EXTRA_VERSION));
                code = Integer.parseInt(got[0]);
                msg = got[1];
            } catch (Throwable e) {   // never this process's end: a Prepare may share it
                Log.e(TAG, "scene overlay", e);
                code = 1;
                msg = String.valueOf(e);
            }
            Bundle b = new Bundle();
            b.putString(MESSAGE, msg);
            if (r != null) r.send(code, b);
            stopSelf(startId);
        }, "nx-scene").start();
        return START_NOT_STICKY;
    }

    /** {exit code, message}: RUNNING before any file work while another run is on (it owns scene_720p.tmp/ and the log) */
    private String[] run(String id, String romfs, String scale, String expect, String version) throws IOException {
        if (id == null || romfs == null || scale == null || version == null) return new String[] {"2", "incomplete request"};
        if (!ACTIVE.compareAndSet(false, true)) return new String[] {String.valueOf(RUNNING), "the 720p overlay is already being made"};
        try { return runOne(id, romfs, scale, expect, version); } finally { ACTIVE.set(false); }
    }

    /** {exit code, message} of a Ground foliage run, as run's */
    private String[] runFoliage(String id, String romfs, String level, String keys, String version) throws IOException {
        if (id == null || romfs == null || level == null || keys == null || version == null || !level.matches("[A-Za-z0-9_]+")) return new String[] {"2", "incomplete request"};
        if (!ACTIVE.compareAndSet(false, true)) return new String[] {String.valueOf(RUNNING), "an overlay is already being made"};
        try {
            File files = getExternalFilesDir(null);
            if (files == null) return new String[] {"1", "the app's files folder is not available"};
            File out = FoliageOverlay.dir(files, id, level), tmp = new File(out.getPath() + ".tmp");
            return runTool(FOLIAGE_TOOL, FoliageOverlay.argv(romfs, keys, tmp), out, tmp, FoliageOverlay.stamp(files, id, level), FoliageOverlay.log(files, id, level),
                           FoliageOverlay.stampText(keys, version));
        } finally { ACTIVE.set(false); }
    }

    private String[] runOne(String id, String romfs, String scale, String expect, String version) throws IOException {
        if (expect == null) expect = "";
        File files = getExternalFilesDir(null);
        if (files == null) return new String[] {"1", "the app's files folder is not available"};
        File out = SceneOverlay.dir(files, id), tmp = new File(out.getPath() + ".tmp");
        return runTool(TOOL, SceneOverlay.argv(romfs, scale, expect, tmp), out, tmp, SceneOverlay.stamp(files, id), SceneOverlay.log(files, id), SceneOverlay.stampText(scale, expect, version));
    }

    /** Runs one overlay tool into tmp/ (argv names it), then makes tmp/ the overlay out/ with its stamp once the tool checked its own output */
    private String[] runTool(String tool, String[] argv, File out, File tmp, File stamp, File log, String stampText) throws IOException {
        Job.deleteTree(tmp);
        if (!tmp.mkdirs()) return new String[] {"1", "cannot create " + tmp};
        String head = "== " + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) + " " + Prefs.buildTag(this) + ": " + tool + " " + String.join(" ", argv) + "\n";
        Files.write(log.toPath(), head.getBytes(StandardCharsets.UTF_8), StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING);
        PackEngine e = PackEngine.create(this);
        PackEngine.setenv("TMPDIR", getCacheDir().getPath());
        int code = e.simulated() ? e.run(tool, argv, log.getPath()) : PackNative.packTryRun(tool, argv, log.getPath());
        Log.i(TAG, tool + ": exit " + code + " (log " + log + ")");
        if (code == BUSY) { Job.deleteTree(tmp); return new String[] {String.valueOf(code), "a preparation is running"}; }   // only a job step holds the lock now (ACTIVE)
        String[] made = tmp.list();
        if (code != 0 || made == null || made.length == 0) {
            Job.deleteTree(tmp);
            List<String> l = Files.readAllLines(log.toPath(), StandardCharsets.UTF_8);
            String why = SceneOverlay.why(l);
            return new String[] {String.valueOf(code == 0 ? 1 : code), (why != null ? why : "exit " + code) + " (log: " + log + ")"};
        }
        // the stamp goes first, so a folder without a matching stamp is never taken (SceneOverlay.current), then the new folder in place of the old
        stamp.delete();
        Job.deleteTree(out);
        if (!tmp.renameTo(out)) return new String[] {"1", "cannot rename " + tmp + " to " + out};
        Files.write(stamp.toPath(), stampText.getBytes(StandardCharsets.UTF_8));
        return new String[] {"0", out.getPath()};
    }
}
