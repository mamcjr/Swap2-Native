package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The entry point for emulator frontends (FRONTENDS.md), reached by the exported alias <application ID>.Launch: an ACTION_VIEW of a game (a .nup, or a game
 * folder) by a file:// or content:// URI, or the extras {@code game} (a path) or {@code title_id} (a game already on Home). Draws nothing of its own.
 * <p>
 * A game whose card and pack are ready to play starts at once (its card is added first when the frontend's file is new and its pack is found and fits), and when
 * it stops the frontend comes back (GameActivity's "frontend" extra). Anything that needs the user (no pack, Prepare, shaders to make or update, a pack that does
 * not fit, All files access) goes to Home with the game, which asks as it does for a picked game. A frontend never chooses the pack or the save folder: those
 * extras stay adb's (MainActivity.fromIntent).
 */
public final class FrontendLaunch extends Activity {
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) { finish(); return; }   // recreated after it handed the game on
        Intent i = getIntent();
        if (!Environment.isExternalStorageManager()) {
            Toast.makeText(this, getString(R.string.s2l_frontend_access, getString(R.string.app_name)), Toast.LENGTH_LONG).show();
            home(null);
            return;
        }
        String tid = Frontend.titleId(i.getStringExtra("title_id"));
        if (tid != null) { byTitleId(tid); return; }
        String path = pathOf(i);
        if (path == null) {
            fail(i.getData() != null
                    ? getString(R.string.s2l_frontend_not_file, String.valueOf(i.getData()))
                    : getString(R.string.s2l_frontend_no_game));
            return;
        }
        File game = Switch2Layout.moved(Switch2Layout.root(), new File(Frontend.normal(path)));   // a frontend set up before the move to Switch2/Games sends the old path
        if (!game.exists()) { fail(getString(R.string.s2l_frontend_missing, game.getPath(), path)); return; }
        if (!game.canRead()) { fail(getString(R.string.s2l_frontend_unreadable, game.getPath())); return; }
        new Thread(() -> {
            String id = null, err = null;
            try { id = Pack.titleIdOf(game); } catch (IOException e) { err = e.getMessage(); }   // a .nup's TOC is hashed: off the main thread
            String fid = id, ferr = err;
            Game g = ferr == null ? ready(game, fid) : null;
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (ferr != null) fail(ferr);
                else if (g != null) play(g);
                else home(game);
            });
        }, "nx-frontend").start();
    }

    /** The game's path: the intent's data (file://, a storage document, or a provider URI that carries a /storage path), else the extra "game" */
    private String pathOf(Intent i) {
        Uri u = i.getData();
        if (u != null) {
            if ("file".equalsIgnoreCase(u.getScheme())) return Frontend.fileUri(u.toString());
            if ("content".equalsIgnoreCase(u.getScheme())) {
                String p = PathUtil.documentToPath(this, u);   // ES-DE's %ROMSAF%: a document of a picked tree
                if (p != null) return p;
                p = Frontend.embeddedPath(u.getPath());
                return p != null && Switch2Layout.moved(Switch2Layout.root(), new File(Frontend.normal(p))).exists() ? p : null;
            }
            return null;
        }
        String p = i.getStringExtra("game");
        return p != null && p.startsWith("/") ? p : null;
    }

    /**
     * The game's card when it can start now: its card (or a new one, saved, when its pack is found), a game this APK has, no save backup or restore working, a pack
     * that reads, is of this title and of this game build and rewritten with the mods there are now (PackCheck.playBlocking), has a RomFS for it and this version's shader library. null: Home asks (launch)
     */
    private Game ready(File game, String id) {
        if (Profile.missing(id, "This game") != null) return null;
        List<Game> list = Game.load(this);
        List<String> dirs = new ArrayList<>();
        for (Game g : list) dirs.add(g.gameDir);
        int at = Frontend.card(dirs, game.getPath());
        Game card = at >= 0 ? list.get(at) : null;
        File packDir = card != null ? new File(card.packDir) : Folders.packFor(this, game, id);
        if (packDir == null || SaveUi.busy(id)) return null;
        Pack p;
        try {
            p = Pack.read(packDir);
            if (!p.titleId.equalsIgnoreCase(id)) return null;
            if (Nup.isNup(game) ? p.romfsPath(game) == null : p.romfs(game) == null) return null;
        } catch (IOException e) {
            return null;
        }
        if (PackCheck.playBlocking(this, p, game) != null || Pack.libraryProblem(packDir, id) != null) return null;
        if (card != null) return card;
        Game g = new Game(id, p.name.isEmpty() ? id : p.name, game.getPath(), packDir.getPath());
        synchronized (Game.class) {
            List<Game> now = Game.load(this);
            now.removeIf(x -> Frontend.same(x.gameDir, game.getPath()));
            now.add(g);
            Game.save(this, now);
        }
        return g;
    }

    private void byTitleId(String tid) {
        for (Game g : Game.load(this)) {
            if (!g.titleId.equalsIgnoreCase(tid)) continue;
            File game = new File(g.gameDir);
            new Thread(() -> {
                Game r = ready(game, g.titleId);
                runOnUiThread(() -> { if (isFinishing()) return; if (r != null) play(r); else home(game); });
            }, "nx-frontend").start();
            return;
        }
        fail(getString(R.string.s2l_frontend_no_card, tid, getString(R.string.app_name)));
    }

    private void play(Game g) {
        startActivity(new Intent(this, GameActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra("title_id", g.titleId).putExtra("name", g.name).putExtra("game", g.gameDir).putExtra("pack", g.packDir).putExtra("frontend", true));
        finish();
    }

    /** Home, with the game when there is one (MainActivity.fromFrontend: its card's Play, or the steps a picked game takes) */
    private void home(File game) {
        Intent h = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (game != null) h.putExtra("frontend_game", game.getPath());
        startActivity(h);
        finish();
    }

    private void fail(String why) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle(R.string.s2l_frontend_cannot_start).setMessage(why)
                .setPositiveButton(android.R.string.ok, null).setOnDismissListener(d -> finish()).show();
    }
}
