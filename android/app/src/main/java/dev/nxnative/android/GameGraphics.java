package dev.nxnative.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.widget.LinearLayout;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** BananaNative 0.2.5 graphics choices in Swap2's settings UI, backed by the local renderer. */
final class GameGraphics {
    private static final String MODE = "graphics_mode", SHARP = "graphics_sharp", ASYNC = "graphics_async", TIMING = "graphics_timing", MEMORY = "graphics_memory", MESH = "graphics_mesh",
            FOLIAGE = "graphics_foliage", EFFECT = "graphics_effect_";
    private static final int[] MODES = {R.string.s2_up_original, R.string.s2_up_fsr1, R.string.s2_up_sgsr2, R.string.s2_up_fsr3};
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
    /** Whether the game's task/mesh geometry is drawn (default on). Off leaves every mesh draw out (DM_VK_MESH_DRAWS=0, live): in Ocarina of Time much of the foliage, the
     *  falling leaves and some models go, for about 10 fps in Kokiri Forest on the POCO F7 Ultra (GPU 48 to 30 ms a frame by the scarecrow) */
    static boolean meshDraws(Context c, String id) { return Prefs.of(c).getBoolean(key(MESH, id), Prefs.of(c).getBoolean(MESH, true)); }
    /** Foliage (Advanced graphics; a game whose profile has grass_mesh or plant_mesh rows, Profile.grass / Profile.plants): 3 high (the default, as the game draws
     *  it), 2 medium (half the grass), 1 low (no grass, half the plants), 0 none. The host's DM_VK_FOLIAGE (live) with the profile's programs as
     *  DM_VK_FOLIAGE_GRASS and DM_VK_FOLIAGE_PLANTS (vka_mesh.inc foliage_action). The setting's row is the interface's; this is its value */
    static int foliage(Context c, String id) { int v = Prefs.of(c).getInt(key(FOLIAGE, id), Prefs.of(c).getInt(FOLIAGE, 3)); return v < 0 ? 0 : v > 3 ? 3 : v; }
    static boolean hasFoliage(Profile prof) { return prof != null && !(prof.grass.isEmpty() && prof.plants.isEmpty()); }
    static void foliageEnv(Context c, String id, Profile prof, List<String> env) {
        if (!hasFoliage(prof)) return;
        if (!prof.grass.isEmpty()) env.add("DM_VK_FOLIAGE_GRASS=" + prof.grass);
        if (!prof.plants.isEmpty()) env.add("DM_VK_FOLIAGE_PLANTS=" + prof.plants);
        env.add("DM_VK_FOLIAGE=" + foliage(c, id));
    }
    /** The effects Advanced graphics may turn off (a profile's graphics_off rows, Profile.effects "<name>:<dispatch|draw>:<keys>|…"; Ocarina of Time: volume_fog, the
     *  Fog option), by name in the profile's order */
    static List<String> effects(Profile prof) {
        List<String> out = new ArrayList<>();
        if (prof != null && !prof.effects.isEmpty()) for (String e : prof.effects.split("\\|")) out.add(e.substring(0, e.indexOf(':')));
        return out;
    }
    /** Whether an effect is drawn (default on). Off: the host leaves its programs out, DM_VK_DISPATCH_OFF (compute, its storage images cleared to zero) or DM_VK_DRAW_OFF
     *  (draws), both live. The setting's row is the interface's; this is its value */
    static boolean effect(Context c, String id, String name) { return Prefs.of(c).getBoolean(key(EFFECT + name, id), Prefs.of(c).getBoolean(EFFECT + name, true)); }
    static void effect(Context c, String id, String name, boolean on, boolean live) {
        Prefs.of(c).edit().putBoolean(key(EFFECT + name, id), on).commit();
        if (live) push(c, id);
    }
    /** DM_VK_DISPATCH_OFF and DM_VK_DRAW_OFF from the effects turned off, each "" when none (so a live line clears what an earlier one set) */
    private static String[] effectsOff(Context c, String id, Profile prof) {
        StringBuilder disp = new StringBuilder(), draw = new StringBuilder();
        if (prof != null && !prof.effects.isEmpty()) for (String e : prof.effects.split("\\|")) {
            String[] f = e.split(":", 3);
            if (f.length < 3 || effect(c, id, f[0])) continue;
            StringBuilder b = f[1].equals("draw") ? draw : disp;
            b.append(b.length() > 0 ? "," : "").append(f[2]);
        }
        return new String[] {disp.toString(), draw.toString()};
    }
    static void effectsEnv(Context c, String id, Profile prof, List<String> env) {
        String[] off = effectsOff(c, id, prof);
        if (!off[0].isEmpty()) env.add("DM_VK_DISPATCH_OFF=" + off[0]);
        if (!off[1].isEmpty()) env.add("DM_VK_DRAW_OFF=" + off[1]);
    }
    static void reset(Context c, String id) {
        SharedPreferences.Editor e = Prefs.of(c).edit();
        for (String n : effects(id == null ? null : Profile.of(id))) e.remove(key(EFFECT + n, id));
        for (String k : new String[] {MODE, SHARP, ASYNC, TIMING, MEMORY, MESH, FOLIAGE, "graphics_stretch", "graphics_ram_saver"}) e.remove(key(k, id));
        e.commit();
    }
    static void env(Context c, String id, List<String> env) {
        env.addAll(Arrays.asList(GraphicsMode.lines(mode(c, id), sharp(c, id))));
        env.add("DM_VK_PSO_ASYNC=" + (async(c, id) ? 2 : 0));
        env.add("DM_VK_STRETCH=" + (stretch(c, id) ? 1 : 0));
        if (!meshDraws(c, id)) env.add("DM_VK_MESH_DRAWS=0");
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
    /** Image: the Upscaler, its level (Game resolution's setting offered as Quality, Balanced, Performance: levelRow, made by the page), its sharpness and, outside a run,
     *  Fit or Stretch. Live rows (the in-game panel) are only what the renderer changes during a run */
    static void imageRows(Context c, String id, LinearLayout box, android.view.View level, Runnable again, boolean live) {
        Runnable changed = () -> { if (live) push(c, id); again.run(); };
        int m = GraphicsMode.index(mode(c, id));
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_upscaler), v ->
                choose(c, id, MODE, R.string.s2_upscaler, names(c, MODES), m,
                    n -> Prefs.of(c).edit().putString(key(MODE, id), GraphicsMode.VALUES[n]).commit(), changed))
                .value(c.getString(MODES[m]) + " · " + c.getString(R.string.s2_up_note)).chevron(c).view);
        if (level != null) box.addView(level);
        if (m != 0) {
            final int selected = GraphicsMode.sharpIndex(sharp(c, id));
            box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_gfx_sharp), v ->
                    choose(c, id, SHARP, R.string.s2_gfx_sharp, names(c, SHARPS), selected,
                        n -> Prefs.of(c).edit().putFloat(key(SHARP, id), GraphicsMode.SHARPS[n]).commit(), changed))
                    .value(c.getString(SHARPS[selected]) + " · " + c.getString(R.string.s2_gfx_sharp_note)).chevron(c).view);
        }
        if (live) return;
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_display, c.getString(R.string.s2_gfx_display), v ->
                choose(c, id, "graphics_stretch", R.string.s2_gfx_display, names(c, new int[] {R.string.s2_gfx_fit, R.string.s2_gfx_stretch}), stretch(c, id) ? 1 : 0,
                    n -> Prefs.of(c).edit().putBoolean(key("graphics_stretch", id), n == 1).commit(), again))
                .value(c.getString(stretch(c, id) ? R.string.s2_gfx_stretch : R.string.s2_gfx_fit) + " · " + c.getString(R.string.s2_gfx_restart)).chevron(c).view);
    }

    /** The Upscaler level's game resolutions (Quality, Balanced, Performance): Game resolution's own sizes (Prefs.WINDOWS), which the game renders at before the upscaler */
    private static final String[] LEVELS = {"1920x1080", "1280x720", "960x540"};
    private static final int[] LEVEL_NAMES = {R.string.s2_up_quality, R.string.s2_up_balanced, R.string.s2_up_performance};

    /**
     * The Upscaler level row, in place of Game resolution while an upscaler is chosen: the same setting (Prefs.window, or the game's own Prefs.gameWindow), offered as
     * Quality 1080p, Balanced 720p and Performance 540p (where the game takes 540p). null: Original is chosen, or no game here has a resolution lever (the page then
     * shows Game resolution as before, or nothing)
     */
    static android.view.View levelRow(Context c, String id, Runnable again) {
        if (GraphicsMode.index(mode(c, id)) == 0) return null;
        Profile prof = id == null ? null : Profile.of(id);
        if (id == null ? !Profile.anyResolution() : prof == null || !prof.hasResolution()) return null;
        int n = id != null && !prof.supports540p() ? 2 : 3, off = id == null ? 0 : 1;
        String cur = id == null ? Prefs.window(c) : Prefs.gameWindow(c, id);
        int at = Arrays.asList(LEVELS).subList(0, n).indexOf(cur);
        String shown = at >= 0 ? c.getString(LEVEL_NAMES[at]) : Prefs.windowName(c, cur, true);
        Ui.Setting row = new Ui.Setting(c, R.drawable.s2_ic_display, c.getString(R.string.s2_up_level), v -> {
            String[] labels = new String[n + off];
            if (id != null) labels[0] = c.getString(R.string.s2_inherit_choice, Prefs.windowName(c, Prefs.defaultWindow(c, id), false));
            for (int i = 0; i < n; i++) labels[i + off] = c.getString(LEVEL_NAMES[i]);
            int checked = id != null && !Prefs.ownWindow(c, id) ? 0 : at < 0 ? -1 : at + off;
            new AlertDialog.Builder(c).setTitle(R.string.s2_up_level).setSingleChoiceItems(labels, checked, (d, w) -> {
                String size = w < off ? null : LEVELS[w - off];
                if (id == null) Prefs.window(c, size); else Prefs.gameWindow(c, id, size);
                d.dismiss();
                again.run();
            }).setNegativeButton(android.R.string.cancel, null).show();
        });
        if (id != null) row.override(c, Prefs.ownWindow(c, id), shown);
        else row.value(shown + " · " + c.getString(R.string.s2_up_level_note));
        return row.chevron(c).view;
    }

    private static final int[] FOLIAGE_NAMES = {R.string.s2_gfx_foliage_none, R.string.s2_gfx_foliage_low, R.string.s2_gfx_foliage_medium, R.string.s2_gfx_foliage_high};

    /** The effects that can be turned off here: the game's own (Profile.effects), or for the general settings every profile's, each name once */
    private static List<String> effectNames(String id) {
        if (id != null) return effects(Profile.of(id));
        List<String> out = new ArrayList<>();
        for (Profile p : Profile.all()) for (String n : effects(p)) if (!out.contains(n)) out.add(n);
        return out;
    }

    /** An effect's name on screen: s2_gfx_effect_&lt;name&gt; when the app has it, else the profile's name */
    private static String effectLabel(Context c, String name) {
        int rid = c.getResources().getIdentifier("s2_gfx_effect_" + name, "string", c.getPackageName());
        return rid == 0 ? name.replace('_', ' ') : c.getString(rid);
    }

    /** Scene: Mesh geometry, Grass and plants (a game whose profile has grass_mesh or plant_mesh rows; for the general settings, while any game has them) and the
     *  effects that can be turned off (graphics_off rows: Ocarina of Time's Fog), every one live */
    static void sceneRows(Context c, String id, LinearLayout box, Runnable again, boolean live) {
        box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_gfx_mesh), null)
                .value(c.getString(R.string.s2_gfx_mesh_note))
                .toggle(c, meshDraws(c, id), (button, on) -> { Prefs.of(c).edit().putBoolean(key(MESH, id), on).commit(); if (live) push(c, id); }).view);
        boolean foliage = false;
        if (id != null) foliage = hasFoliage(Profile.of(id));
        else for (Profile p : Profile.all()) foliage |= hasFoliage(p);
        if (foliage) {
            int f = foliage(c, id);
            String[] items = new String[4];
            for (int i = 0; i < 4; i++) items[i] = c.getString(FOLIAGE_NAMES[3 - i]);   // High first
            box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, c.getString(R.string.s2_gfx_foliage), v ->
                    choose(c, id, FOLIAGE, R.string.s2_gfx_foliage, items, 3 - f, n -> Prefs.of(c).edit().putInt(key(FOLIAGE, id), 3 - n).commit(),
                        () -> { if (live) push(c, id); again.run(); }))
                    .value(c.getString(FOLIAGE_NAMES[f]) + " · " + c.getString(R.string.s2_gfx_foliage_note)).chevron(c).view);
        }
        for (String n : effectNames(id))
            box.addView(new Ui.Setting(c, R.drawable.s2_ic_image, effectLabel(c, n), null)
                    .value(c.getString(R.string.s2_gfx_effect_note))
                    .toggle(c, effect(c, id, n), (button, on) -> effect(c, id, n, on, live)).view);
    }

    /** Advanced, outside a run: RAM saver (a game's own), shaders compiled in the background, how the game's memory reaches the GPU, GPU timing */
    static void advancedRows(Context c, String id, LinearLayout box, Runnable again) {
        if (id != null) box.addView(new Ui.Setting(c, R.drawable.s2_ic_chip, c.getString(R.string.s2_gfx_ram_saver), null)
                .value(c.getString(R.string.s2_gfx_ram_saver_note))
                .toggle(c, Prefs.of(c).getBoolean(key("graphics_ram_saver", id), false),
                    (button, on) -> Prefs.of(c).edit().putBoolean(key("graphics_ram_saver", id), on).commit()).view);
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
            w.write("env DM_VK_MESH_DRAWS=" + (meshDraws(c, id) ? 1 : 0) + "\n");
            w.write("env DM_VK_FOLIAGE=" + foliage(c, id) + "\n");   // (live; without the launch's DM_VK_FOLIAGE_GRASS / _PLANTS it changes nothing)
            Profile prof = id == null ? null : Profile.of(id);
            if (!effects(prof).isEmpty()) {   // (the live override holds 255 characters: 15 keys)
                String[] off = effectsOff(c, id, prof);
                w.write("env DM_VK_DISPATCH_OFF=" + off[0] + "\nenv DM_VK_DRAW_OFF=" + off[1] + "\n");
            }
        } catch (IOException e) { Native.note("[RED] graphics settings: could not apply live: " + e); }
    }
}
