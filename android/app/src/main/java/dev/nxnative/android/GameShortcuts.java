package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.drawable.Icon;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The phone's Home screen shortcuts to a game (ShortcutManager): a pinned one from the game's Manage sheet (Add to Home screen) and the app icon's long-press
 * menu of the last played games (dynamic). Both open ShortcutLaunch with the game's title ID (never its path, which a move or a new card changes): it shows the
 * opening and starts the game, or opens Home with it when the game needs the user (Prepare, shaders, a pack that does not fit). The shortcut's id is
 * "game_<title ID>", so pinning a game again updates its one shortcut, and a cover downloaded later reaches it at the next refresh of the recent list.
 */
final class GameShortcuts {
    private GameShortcuts() {}

    private static final String TAG = "Swap2Native";
    static final String ACTION = "dev.nxnative.android.action.PLAY_SHORTCUT";
    static final int RECENT = 4;   // the app icon's menu: the last played games, at most (Android shows 4 or 5 of an app's shortcuts)
    private static String recentDone;   // what the last refresh set (its ids and names), so Home's resumes do not set the same list again

    static String id(String titleId) { return "game_" + titleId; }

    /** The intent a shortcut runs: ShortcutLaunch with the title ID (the name only for its message when the game is gone) */
    static Intent intent(Context c, Game g) {
        return new Intent(c, ShortcutLaunch.class).setAction(ACTION).putExtra("title_id", g.titleId).putExtra("name", g.name);
    }

    static boolean pinSupported(Context c) {
        ShortcutManager m = c.getSystemService(ShortcutManager.class);
        return m != null && m.isRequestPinShortcutSupported();
    }

    /**
     * Add to Home screen: the system's own dialog adds it (a game pinned before gets its shortcut updated instead). false: this Home screen app takes none.
     * Xiaomi's launchers (MIUI, HyperOS, POCO Launcher) add it only with their own permission "Home screen shortcuts" (app op 10017), whose first prompt
     * refuses by itself after about 3 s; refused, requestPinShortcut still returns true and nothing appears (the launcher only logs "has no permission").
     * There the pin's callback is watched: no callback within WATCH_MS says how to allow it (blocked)
     */
    static boolean pin(Activity a, Game g) {
        ShortcutManager m = a.getSystemService(ShortcutManager.class);
        if (m == null || !m.isRequestPinShortcutSupported()) return false;
        ShortcutInfo s = info(a, g, 0);
        try {
            boolean already = false;
            for (ShortcutInfo p : m.getPinnedShortcuts())
                if (p.getId().equals(s.getId())) { m.updateShortcuts(List.of(s)); already = true; Log.i(TAG, "Shortcut: " + g.titleId + " already pinned, updated"); }
            IntentSender done = already || !xiaomi() ? null : watch(a, g);
            boolean ok = m.requestPinShortcut(s, done);
            Log.i(TAG, "Shortcut: pin " + g.titleId + " (" + g.name + ") asked: " + ok + (done != null ? ", watched" : ""));
            return ok;
        } catch (IllegalStateException | IllegalArgumentException e) {   // the app is not in the foreground, a disabled id
            Log.w(TAG, "Shortcut: pin " + g.titleId + " refused: " + e.getMessage());
            return false;
        }
    }

    /** A Xiaomi, Redmi or POCO phone (their launchers gate pinning behind app op 10017) */
    static boolean xiaomi() {
        for (String s : new String[] {Build.MANUFACTURER, Build.BRAND})
            if (s != null && (s.equalsIgnoreCase("xiaomi") || s.equalsIgnoreCase("redmi") || s.equalsIgnoreCase("poco"))) return true;
        return false;
    }

    private static final long WATCH_MS = 6000;   // the permission's own prompt refuses after about 3 s, the launcher answers at once once it is allowed
    private static final String PINNED = "dev.nxnative.android.action.SHORTCUT_PINNED";

    /** The pin's callback: a broadcast to this process only; none by WATCH_MS (and the screen still up) → blocked */
    private static IntentSender watch(Activity a, Game g) {
        Context app = a.getApplicationContext();
        boolean[] pinned = {false};
        BroadcastReceiver r = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) { pinned[0] = true; Log.i(TAG, "Shortcut: " + g.titleId + " pinned"); }
        };
        app.registerReceiver(r, new IntentFilter(PINNED), Context.RECEIVER_NOT_EXPORTED);
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            try { app.unregisterReceiver(r); } catch (IllegalArgumentException ignored) {}
            if (pinned[0]) return;
            Log.w(TAG, "Shortcut: " + g.titleId + " not pinned after " + WATCH_MS + " ms (the launcher's Home screen shortcuts permission?)");
            if (!a.isFinishing() && !a.isDestroyed()) blocked(a);
        }, WATCH_MS);
        Intent i = new Intent(PINNED).setPackage(app.getPackageName());
        return PendingIntent.getBroadcast(app, 0, i, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT).getIntentSender();
    }

    /** Says how to allow Home screen shortcuts, with the app's permission page (MIUI's own editor, else the app's details) */
    private static void blocked(Activity a) {
        String app = a.getString(R.string.app_name);
        new AlertDialog.Builder(a).setTitle(R.string.s2_shortcut_blocked_title).setMessage(a.getString(R.string.s2_shortcut_blocked, app))
                .setPositiveButton(R.string.s2_shortcut_permissions, (d, w) -> {
                    Intent miui = new Intent("miui.intent.action.APP_PERM_EDITOR").putExtra("extra_pkgname", a.getPackageName());
                    try {
                        a.startActivity(miui);
                    } catch (RuntimeException e) {   // not MIUI's security app (or it refuses): the app's details, Permissions from there
                        a.startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + a.getPackageName())));
                    }
                })
                .setNegativeButton(android.R.string.ok, null).show();
    }

    private static ShortcutInfo info(Context c, Game g, int rank) {
        String name = g.name == null || g.name.isEmpty() ? g.titleId : g.name;
        return new ShortcutInfo.Builder(c, id(g.titleId)).setShortLabel(name).setLongLabel(name).setIcon(icon(c, g.titleId)).setIntent(intent(c, g)).setRank(rank).build();
    }

    /** The game's cover, cut to a square that fills an adaptive icon (the launcher's mask takes the edges), else the app's icon */
    private static Icon icon(Context c, String titleId) {
        File f = Covers.file(titleId);
        Bitmap b = f.isFile() ? BitmapFactory.decodeFile(f.getPath()) : null;
        if (b == null || b.getWidth() <= 0 || b.getHeight() <= 0) return Icon.createWithResource(c, R.mipmap.ic_launcher);
        int side = Math.min(b.getWidth(), b.getHeight());
        Rect src = new Rect((b.getWidth() - side) / 2, (b.getHeight() - side) / 2, (b.getWidth() + side) / 2, (b.getHeight() + side) / 2);
        int out = 432;   // 108 dp at 4x: an adaptive icon's full layer
        Bitmap sq = Bitmap.createBitmap(out, out, Bitmap.Config.ARGB_8888);
        new Canvas(sq).drawBitmap(b, src, new Rect(0, 0, out, out), new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG));
        b.recycle();
        return Icon.createWithAdaptiveBitmap(sq);
    }

    /**
     * The app icon's menu: the last played games (Game.playedTimes), newest first, at most RECENT; and the pinned shortcuts of the games on Home get their
     * name and cover again. Off the main thread (covers are decoded); Home's onResume calls it, so a game just played moves to the top on return
     */
    static void refresh(Context c) {
        Context app = c.getApplicationContext();
        new Thread(() -> {
            try {
                refreshNow(app);
            } catch (RuntimeException e) {   // the shortcut service's rate limit, a launcher that takes none: the menu stays as it was
                Log.w(TAG, "Shortcut: refresh failed: " + e);
            }
        }, "nx-shortcuts").start();
    }

    private static synchronized void refreshNow(Context c) {
        ShortcutManager m = c.getSystemService(ShortcutManager.class);
        if (m == null) return;
        List<Game> games = Game.load(c);
        Map<String, Long> played = Game.playedTimes(c);
        List<Game> recent = new ArrayList<>();
        Set<String> ids = new HashSet<>();
        games.stream().filter(g -> played.containsKey(g.gameDir) && Profile.of(g.titleId) != null)
                .sorted((a, b) -> Long.compare(played.get(b.gameDir), played.get(a.gameDir)))
                .forEach(g -> { if (recent.size() < RECENT && ids.add(g.titleId)) recent.add(g); });   // one entry per title (two cards of one game: the newer)
        StringBuilder key = new StringBuilder();
        for (Game g : recent) key.append(g.titleId).append('=').append(g.name).append(';');
        key.append('|');
        List<ShortcutInfo> pinned = m.getPinnedShortcuts();
        for (ShortcutInfo p : pinned) key.append(p.getId()).append(';');
        key.append(Covers.dir().list() == null ? 0 : Covers.dir().list().length);   // a cover downloaded since
        if (key.toString().equals(recentDone)) return;
        List<ShortcutInfo> dyn = new ArrayList<>();
        for (int i = 0; i < recent.size(); i++) dyn.add(info(c, recent.get(i), i));
        m.setDynamicShortcuts(dyn);
        // Pinned shortcuts of games still on Home: their name and cover now (the first card of the title, as ShortcutLaunch finds it)
        List<ShortcutInfo> upd = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (ShortcutInfo p : pinned)
            for (Game g : games) if (p.getId().equals(id(g.titleId)) && seen.add(g.titleId) && !ids.contains(g.titleId)) upd.add(info(c, g, 0));
        if (!upd.isEmpty()) m.updateShortcuts(upd);
        recentDone = key.toString();
        Log.i(TAG, "Shortcut: recent " + recent.size() + " set, " + upd.size() + " pinned updated");
    }
}
