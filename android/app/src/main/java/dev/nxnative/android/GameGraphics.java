package dev.nxnative.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.widget.LinearLayout;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.Arrays;
import java.util.List;

/** BananaNative 0.2.5 graphics choices in Swap2's settings UI, backed by the local renderer. */
final class GameGraphics {
    private static final String MODE = "graphics_mode", SHARP = "graphics_sharp", ASYNC = "graphics_async", TIMING = "graphics_timing", MEMORY = "graphics_memory";
    private static final int[] MODES = {R.string.s2_gfx_nearest, R.string.s2_gfx_bilinear, R.string.s2_upscaler_fsr, R.string.s2_upscaler_net};
    private static final int[] SHARPS = {R.string.s2_gfx_off, R.string.s2_fsr_sharp_softer, R.string.s2_fsr_sharp_soft, R.string.s2_fsr_sharp_default, R.string.s2_fsr_sharp_max};
    private static String key(String key, String id) { return id == null ? key : Prefs.gameKey(key, id); }
    static String mode(Context c, String id) {
        SharedPreferences p = Prefs.of(c);
        if (id != null && p.contains(key(MODE, id))) return GraphicsMode.VALUES[GraphicsMode.index(p.getString(key(MODE, id), "net"))];
        // Preserve the earlier explicit per-game upscaler choice before adopting a new global default.
        if (id != null && p.contains(Prefs.gameKey("ndsr", id))) return Prefs.ndsr(c, id);
        if (p.contains(MODE)) return GraphicsMode.VALUES[GraphicsMode.index(p.getString(MODE, "net"))];
        return id == null ? (Fsr.on(c) ? "fsr" : "net") : Prefs.ndsr(c, id);
    }
    static float sharp(Context c, String id) { return Prefs.of(c).getFloat(key(SHARP, id), Prefs.of(c).getFloat(SHARP, Fsr.sharp(c))); }
    static boolean async(Context c, String id) { return Prefs.of(c).getBoolean(key(ASYNC, id), Prefs.of(c).getBoolean(ASYNC, true)); }
    static boolean stretch(Context c, String id) { return Prefs.of(c).getBoolean(key("graphics_stretch", id), Prefs.of(c).getBoolean("graphics_stretch", false)); }
    /** How the game's memory reaches the GPU: "alias" (the default since 2026-11-14: a shared buffer the host maps at the memory's addresses, DM_VK_ALIAS=prefer)
     *  or "placed" (the driver maps GPU memory over it, the host's own default). A Redmagic 11 Air closes the game on every driver's placed maps and runs it on
     *  the alias (docs/reference/memory.md "A game killed from outside on placed guest memory") */
    static String memory(Context c, String id) { return Prefs.of(c).getString(key(MEMORY, id), Prefs.of(c).getString(MEMORY, "alias")); }
    static String timing(Context c, String id) { return Prefs.of(c).getString(key(TIMING, id), Prefs.of(c).getString(TIMING, null)); }
    static void reset(Context c, String id) {
        SharedPreferences.Editor e = Prefs.of(c).edit();
        for (String k : new String[] {MODE, SHARP, ASYNC, TIMING, MEMORY, "graphics_stretch", "graphics_ram_saver"}) e.remove(key(k, id));
        e.commit();
    }
    static void env(Context c, String id, List<String> env) {
        env.addAll(Arrays.asList(GraphicsMode.lines(mode(c, id), sharp(c, id))));
        env.add("DM_VK_PSO_ASYNC=" + (async(c, id) ? 2 : 0));
        env.add("DM_VK_STRETCH=" + (stretch(c, id) ? 1 : 0));
        if (id != null && Prefs.of(c).getBoolean(key("graphics_ram_saver", id), false)) {
            env.add("DM_MEM_FILEBACK=1");
            env.add("DM_MEM_FILEBACK_KINDS=shader,texture");
        }
        String ts = timing(c, id);
        if (ts != null && !ts.equals("profile")) env.add("DM_NVN_GPU_TS=" + ts);
        if (!memory(c, id).equals("placed")) env.add("DM_VK_ALIAS=prefer");
    }
    private static String[] names(Context c, int[] resources) {
        String[] out = new String[resources.length];
        for (int i = 0; i < out.length; i++) out[i] = c.getString(resources[i]);
        return out;
    }
    private static void choose(Context c, String id, String key, int title, String[] items, int checked, java.util.function.IntConsumer set, Runnable done) {
        boolean own = Prefs.of(c).contains(key(key, id)) || (key.equals(MODE) && id != null && Prefs.of(c).contains(Prefs.gameKey("ndsr", id)));
        String[] labels = items;
        if (id != null) {
            labels = new String[items.length + 1];
            labels[0] = c.getString(R.string.s2_gfx_inherit);
            System.arraycopy(items, 0, labels, 1, items.length);
            checked = own ? checked + 1 : 0;
        }
        new AlertDialog.Builder(c).setTitle(title).setSingleChoiceItems(labels, checked, (d, which) -> {
            int v = id == null ? which : which - 1;
            if (v < 0) Prefs.of(c).edit().remove(key(key, id)).commit(); else set.accept(v);
            if (key.equals(MODE) && id != null) Prefs.of(c).edit().remove(Prefs.gameKey("ndsr", id)).commit();
            d.dismiss(); done.run();
        }).setNegativeButton(android.R.string.cancel, null).show();
    }
    /** Global defaults or per-title overrides; live rows contain only settings the renderer can change during a run. */
    static void rows(Context c, String id, LinearLayout box, Runnable again, boolean live) {
        Runnable changed = () -> { if (live) push(c, id); again.run(); };
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_upscaler), v ->
                choose(c, id, MODE, R.string.s2_upscaler, names(c, MODES), GraphicsMode.index(mode(c, id)),
                    n -> Prefs.of(c).edit().putString(key(MODE, id), GraphicsMode.VALUES[n]).commit(), changed))
                .value(c.getString(MODES[GraphicsMode.index(mode(c, id))]) + " · " + c.getString(R.string.s2_gfx_scaling_note)).chevron(c).view);
        if (mode(c, id).equals("fsr")) {
            int cur = 3; for (int i = 0; i < GraphicsMode.SHARPS.length; i++) if (GraphicsMode.SHARPS[i] == sharp(c, id)) cur = i;
            final int selected = cur;
            box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_gfx_sharp), v ->
                    choose(c, id, SHARP, R.string.s2_gfx_sharp, names(c, SHARPS), selected,
                        n -> Prefs.of(c).edit().putFloat(key(SHARP, id), GraphicsMode.SHARPS[n]).commit(), changed))
                    .value(c.getString(SHARPS[selected]) + " · " + c.getString(R.string.s2_gfx_sharp_note)).chevron(c).view);
        }
        if (live) return;
        if (id != null) box.addView(new Ui.Setting(c, R.drawable.s2_ic_chip, c.getString(R.string.s2_gfx_ram_saver), null)
                .value(c.getString(R.string.s2_gfx_ram_saver_note))
                .toggle(c, Prefs.of(c).getBoolean(key("graphics_ram_saver", id), false),
                    (button, on) -> Prefs.of(c).edit().putBoolean(key("graphics_ram_saver", id), on).commit()).view);
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_display, c.getString(R.string.s2_gfx_display), v ->
                choose(c, id, "graphics_stretch", R.string.s2_gfx_display, names(c, new int[] {R.string.s2_gfx_fit, R.string.s2_gfx_stretch}), stretch(c, id) ? 1 : 0,
                    n -> Prefs.of(c).edit().putBoolean(key("graphics_stretch", id), n == 1).commit(), again))
                .value(c.getString(stretch(c, id) ? R.string.s2_gfx_stretch : R.string.s2_gfx_fit) + " · " + c.getString(R.string.s2_gfx_restart)).chevron(c).view);
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_chip, c.getString(R.string.s2_gfx_async), v ->
                choose(c, id, ASYNC, R.string.s2_gfx_async, names(c, new int[] {R.string.s2_gfx_off, R.string.s2_gfx_on}), async(c, id) ? 1 : 0,
                    n -> Prefs.of(c).edit().putBoolean(key(ASYNC, id), n == 1).commit(), again))
                .value(c.getString(async(c, id) ? R.string.s2_gfx_on : R.string.s2_gfx_off) + " · " + c.getString(R.string.s2_gfx_async_note)).chevron(c).view);
        boolean placed = memory(c, id).equals("placed");
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_chip, c.getString(R.string.s2_gfx_memory), v ->
                choose(c, id, MEMORY, R.string.s2_gfx_memory, names(c, new int[] {R.string.s2_gfx_memory_alias, R.string.s2_gfx_memory_placed}), placed ? 1 : 0,
                    n -> Prefs.of(c).edit().putString(key(MEMORY, id), n == 1 ? "placed" : "alias").commit(), again))
                .value(c.getString(placed ? R.string.s2_gfx_memory_placed : R.string.s2_gfx_memory_alias) + " · " + c.getString(R.string.s2_gfx_memory_note)).chevron(c).view);
        String ts = timing(c, id);
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_gauge, c.getString(R.string.s2_gfx_timing), v ->
                choose(c, id, TIMING, R.string.s2_gfx_timing, names(c, new int[] {R.string.s2_gfx_game_default, R.string.s2_gfx_segment, R.string.s2_gfx_submission}),
                    ts == null || ts.equals("profile") ? 0 : ts.equals("segment") ? 1 : 2, n -> {
                        SharedPreferences.Editor e = Prefs.of(c).edit();
                        e.putString(key(TIMING, id), n == 0 ? "profile" : n == 1 ? "segment" : "submission");
                        e.commit();
                    }, again)).value(c.getString(ts == null || ts.equals("profile") ? R.string.s2_gfx_game_default : ts.equals("segment") ? R.string.s2_gfx_segment : R.string.s2_gfx_submission)
                        + " · " + c.getString(R.string.s2_gfx_timing_note)).chevron(c).view);
    }
    private static void push(Context c, String id) {
        File ext = c.getExternalFilesDir(null);
        if (ext == null) return;
        try (FileWriter w = new FileWriter(new File(ext, "ctl.txt"), true)) {
            for (String line : GraphicsMode.lines(mode(c, id), sharp(c, id))) w.write("env " + line + "\n");
        } catch (IOException e) { Native.note("[RED] graphics settings: could not apply live: " + e); }
    }
}
