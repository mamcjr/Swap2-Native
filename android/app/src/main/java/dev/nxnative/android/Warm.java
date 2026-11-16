package dev.nxnative.android;

import android.content.Context;
import android.os.Build;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * The pipeline warm's bookkeeping (WarmActivity): whether a game's pipeline cache needs warming before Play, the environment the warm runs with, and the record of the
 * last complete warm. The host fills the cache of the GPU driver the game will run on (vka_pso.c "Warming with the game closed"), so a warm holds for one driver, one shader
 * library, one host build and one system build: a change of any of them (a new GPU driver picked or installed, a system update, a shader update, a new APK) asks for another.
 * Another warm after a change is short where the cache still holds the pipelines (a pipeline already in it is found, not compiled). The idea is SCSKiller's: compile with
 * the game closed (docs/ANALISE-SCSKILLER-2026-11-16.md).
 */
final class Warm {
    private Warm() {}

    /** The record of the last complete warm: temp/&lt;title ID&gt;/pipelines.warm in the app's external files, beside the cache it describes (DM_TEMP) */
    static File record(Context c, String id) { return new File(c.getExternalFilesDir(null), "temp/" + id + "/pipelines.warm"); }

    /** What a warm holds for: the system build, the GPU driver the game runs on, its shader library (path, size, time) and this host build */
    static String stamp(Context c, String id, File pack) {
        GpuDriver.Info d = GpuDriver.selected(c, id);
        File lib = Pack.playableLibrary(pack, id);
        if (lib == null) lib = Pack.legacyLibrary(pack, id);
        return "system " + Build.FINGERPRINT + "\ndriver " + (d == null ? "system" : d.dir.getName()) + "\nlibrary " + (lib == null ? "none" : lib.getPath() + " " + lib.length() + " " + lib.lastModified())
                + "\nhost " + BuildConfig.VERSION_CODE + " " + BuildConfig.NX_COMMIT + "\n";
    }

    /** Whether Play warms first: on (Prefs "warm_before_play", default on), a playable library, and no complete warm of this very stamp */
    static boolean wanted(Context c, String id, File pack) {
        if (!Prefs.of(c).getBoolean("warm_before_play", true)) return false;
        if (Pack.playableLibrary(pack, id) == null) return false;   // nothing to compile from yet (the shader update or Prepare comes first)
        return !stamp(c, id, pack).equals(read(record(c, id)));
    }

    static void done(Context c, String id, File pack) {
        File f = record(c, id);
        try { File p = f.getParentFile(); if (p != null) p.mkdirs(); Files.write(f.toPath(), stamp(c, id, pack).getBytes(StandardCharsets.UTF_8)); } catch (IOException e) { /* asked again next time */ }
    }

    private static String read(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (IOException e) { return null; }
    }

    /**
     * The game's environment as far as it decides the pipeline cache and its pipelines: DM_TEMP (the cache file's folder) and HOME as GameActivity sets them, the game's
     * graphics settings (GameGraphics), its GPU driver (GpuDriver), then env.txt, the game's own lines and env-&lt;title ID&gt;.txt (the keys only the launcher sets left out)
     */
    static List<String> env(Context c, String id) {
        File files = c.getExternalFilesDir(null), temp = new File(files, "temp/" + id);
        temp.mkdirs();
        List<String> env = new ArrayList<>(List.of("DM_TEMP=" + temp, "HOME=" + files, "TMPDIR=" + c.getCacheDir(), "DM_BUILD_COMMIT=" + BuildConfig.NX_COMMIT));
        env.add(0, "NX_BUILD_TAG=" + Prefs.buildTag(c) + " · signer " + Prefs.signerId(c));
        GameGraphics.env(c, id, env);
        GpuDriver.env(c, id, env);
        List<String> user = new ArrayList<>(lines(new File(files, "env.txt")));
        user.addAll(GameActivity.envLines(Prefs.gameEnv(c, id)));
        user.addAll(lines(new File(files, "env-" + id + ".txt")));
        for (String kv : user) if (!GameActivity.PROTECTED.contains(kv.substring(0, kv.indexOf('=')).trim())) env.add(kv);
        return env;
    }

    private static List<String> lines(File f) {
        String s = f.isFile() ? read(f) : null;
        return s == null ? new ArrayList<>() : GameActivity.envLines(s);
    }
}
