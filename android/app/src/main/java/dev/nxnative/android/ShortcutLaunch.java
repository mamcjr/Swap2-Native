package dev.nxnative.android;

import android.app.Activity;
import android.app.ActivityOptions;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.File;
import java.util.Map;

/**
 * A Home screen shortcut to a game (GameShortcuts): the opening (BrandIntro, when Settings › Opening animation is on), then the game's own loading screen,
 * without Home. The extra title_id names the game; its card is found when the shortcut fires (of two cards of the title, the one played last), so a game moved
 * or added again keeps its shortcut. The card must be ready to play as for a frontend (FrontendLaunch.ready: pack, code, shader library); anything that needs
 * the user (Prepare, shaders, a pack that does not fit, All files access) opens Home with the game, whose Play asks as it does from its card. A game no longer
 * on Home, or whose file is gone, is said in a Toast. The pipeline warm runs first when Play would run it (Warm.wanted). When the game stops, the app's task
 * goes behind the Home screen the shortcut was on (GameActivity's "frontend" extra). In a task of its own (taskAffinity ""), so the launcher's clearing of the
 * shortcut's task never reaches Home or a running game; a game running already comes to the front (GameActivity.onNewIntent says when it is another).
 */
public final class ShortcutLaunch extends Activity {
    private static final String TAG = "Swap2Native";
    private BrandIntro intro;
    private boolean introDone, checked, handed;
    private Game card, ready;

    @Override protected void attachBaseContext(android.content.Context base) {
        super.attachBaseContext(base);
        LauncherTheme.apply(this);
    }

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (saved != null) { finish(); return; }   // recreated after it handed the game on
        Ui.immersive(this);
        String tid = Frontend.titleId(getIntent().getStringExtra("title_id"));
        Log.i(TAG, "Shortcut: start " + tid);
        if (tid == null) { finish(); return; }
        if (!Environment.isExternalStorageManager()) {
            Toast.makeText(this, getString(R.string.s2l_frontend_access, getString(R.string.app_name)), Toast.LENGTH_LONG).show();
            home(null);
            return;
        }
        card = card(tid);
        if (card == null) {
            String name = getIntent().getStringExtra("name");
            Toast.makeText(this, getString(R.string.s2_shortcut_gone, name != null ? name : tid, getString(R.string.app_name)), Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        File game = new File(card.gameDir);
        if (!game.exists()) {
            Toast.makeText(this, getString(R.string.s2_shortcut_missing, card.name, game.getPath()), Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Ui.col(this, R.color.bn_bg));
        setContentView(frame, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (LauncherTheme.opening(this)) intro = BrandIntro.show(this, frame, () -> { intro = null; introDone = true; next(); });
        else introDone = true;
        Game c = card;
        new Thread(() -> {   // a .nup's pack check reads and hashes: off the main thread, beside the opening
            Game r = FrontendLaunch.ready(this, game, c.titleId);
            runOnUiThread(() -> { ready = r; checked = true; next(); });
        }, "nx-shortcut").start();
        if (!introDone) return;
        next();
    }

    /** The title's card: the one played last when Home has it twice (two folders of one game), else the first */
    private Game card(String tid) {
        Map<String, Long> played = Game.playedTimes(this);
        Game best = null;
        long bestAt = Long.MIN_VALUE;
        for (Game g : Game.load(this)) {
            if (!g.titleId.equalsIgnoreCase(tid)) continue;
            long at = played.getOrDefault(g.gameDir, Long.MIN_VALUE + 1);
            if (best == null || at > bestAt) { best = g; bestAt = at; }
        }
        return best;
    }

    /** Once both the opening and the check are done: the game (its loading screen, no window animation over the opening's background) or Home */
    private void next() {
        if (!introDone || !checked || handed || isFinishing()) return;
        handed = true;
        if (ready == null) { Log.i(TAG, "Shortcut: " + card.titleId + " needs Home"); home(new File(card.gameDir)); return; }
        Game g = ready;
        Log.i(TAG, "Shortcut: play " + g.titleId + " " + g.gameDir);
        Intent i;
        if (Warm.wanted(this, g.titleId, new File(g.packDir)))   // as Play: the pipeline warm first, the game at its end
            i = new Intent(this, WarmActivity.class).putExtra("title_id", g.titleId).putExtra("name", g.name).putExtra("game", g.gameDir).putExtra("pack", g.packDir)
                    .putExtra("play", true);
        else
            i = new Intent(this, GameActivity.class).putExtra("title_id", g.titleId).putExtra("name", g.name).putExtra("game", g.gameDir).putExtra("pack", g.packDir)
                    .putExtra("frontend", true);
        startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK), ActivityOptions.makeCustomAnimation(this, 0, 0).toBundle());
        finish();
        overridePendingTransition(0, 0);
    }

    /** Home, with the game when there is one (MainActivity.fromFrontend: its card's Play says what is missing and offers the fix) */
    private void home(File game) {
        Intent h = new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        if (game != null) h.putExtra("frontend_game", game.getPath());
        startActivity(h);
        finish();
    }

    /** Left during the opening (Home, Recents): the start is dropped, never made from the background */
    @Override protected void onStop() {
        super.onStop();
        if (!handed && !isFinishing()) { handed = true; if (intro != null) intro.finish(); finish(); }
    }

    @Override public void onWindowFocusChanged(boolean focus) { super.onWindowFocusChanged(focus); if (focus) Ui.immersive(this); }
}
