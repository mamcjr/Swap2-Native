package dev.nxnative.android;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;

import java.io.File;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The Play transition and the game's loading curtain (one view, used on both sides of the hand-off):
 * <ul>
 * <li>Launcher (MainActivity.start, {@link #launch}): over the whole window, the stage colour fades in over the HUD (header, dock, the other cards, the shelf),
 * the chosen card's cover lifts off its slot and travels and scales to the window's centre with the brand's halo (CoverFrame's cyan / coral offset edges and a
 * soft glow that breathes), and the game's name, an empty bar and the first caption fade in under it. Then GameActivity starts with no window animation.</li>
 * <li>Game (GameActivity, {@link #loading}): the same picture at the same place (the cover centred, at the size the launcher passed in the intent), fully shown
 * from the first frame of the game's window, so the hand-off looks continuous. GameActivity feeds it the run's log ({@link #feed}): the caption and the bar follow
 * the host's start-up lines (LoadLog), the bar never goes backwards, and the curtain fades into the game once frames are presented ({@link #finish}).</li>
 * </ul>
 * Reduced motion (Settings › Interface, or the system's animator scale off: LauncherTheme.still): nothing travels, scales or breathes; the curtain only fades in
 * and out. The stage is the dark brand stage in both themes: the game's window behind it is dark (its theme's background is the same colour), so a light
 * launcher dims into it rather than flashing. Animators end when the view leaves its window; the curtain takes every touch while it shows.
 */
final class PlayCurtain extends FrameLayout {
    static final String X_SIZE = "curtain_size", X_ROUND = "curtain_round";   // the cover's side in px at the centre, and its corner radius over its side

    private static final float DEFAULT_SIDE = 0.36f, DEFAULT_ROUND = 0.075f;
    private static final long LAUNCH_MS = 460, LAUNCH_GO_MS = 520, FADE_MS = 200, OUT_MS = 320;

    private final boolean still;
    private final View stage;
    private final Halo halo;
    private final FrameLayout cover;
    private final TextView name, caption;
    private final Bar bar;
    private final int fixedSide;
    private final float round;
    private int side;
    private boolean finishing;
    private View hidden;   // the launcher's card art hidden while its copy travels (restored by dismiss)

    private PlayCurtain(Context c, Bitmap art, String title, int side, float round) {
        super(c);
        still = LauncherTheme.still(c);
        fixedSide = side;
        this.round = round > 0 && round < 0.5f ? round : DEFAULT_ROUND;
        setClickable(true);   // touches during the curtain go nowhere
        setFocusable(false);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setContentDescription(title);
        stage = new View(c);
        stage.setBackgroundColor(Ui.col(c, R.color.s2_play_stage));
        addView(stage, new LayoutParams(-1, -1));
        halo = new Halo(c, still);
        addView(halo, new LayoutParams(0, 0));
        cover = new FrameLayout(c);
        cover.setClipToOutline(true);
        cover.setElevation(Ui.dp(c, 6));
        if (art != null) {
            ImageView iv = new ImageView(c);
            iv.setImageBitmap(art);
            iv.setScaleType(ImageView.ScaleType.FIT_CENTER);
            iv.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            cover.addView(iv, new LayoutParams(-1, -1));
        } else {   // no cover yet: the game's initial, as the library's tiles (Ui.tile)
            String n = title == null ? "" : title.trim();
            TextView t = new TextView(c);
            t.setText(n.isEmpty() ? "?" : n.substring(0, n.offsetByCodePoints(0, 1)).toUpperCase(java.util.Locale.ROOT));
            t.setTextColor(Ui.col(c, R.color.s2_play_text));
            t.setTypeface(Ui.face(800));
            t.setGravity(Gravity.CENTER);
            t.setIncludeFontPadding(false);
            t.setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
            t.setTag("initial");
            cover.addView(t, new LayoutParams(-1, -1));
        }
        addView(cover, new LayoutParams(0, 0));
        name = new TextView(c);
        name.setText(title);
        name.setTextColor(Ui.col(c, R.color.s2_play_text));
        name.setTextSize(17);
        name.setTypeface(Ui.face(700));
        name.setGravity(Gravity.CENTER);
        name.setSingleLine(true);
        name.setEllipsize(TextUtils.TruncateAt.END);
        addView(name, new LayoutParams(0, LayoutParams.WRAP_CONTENT));
        bar = new Bar(c, still);
        addView(bar, new LayoutParams(0, Ui.dp(c, 4)));
        caption = new TextView(c);
        caption.setText(R.string.s2_play_preparing);
        caption.setTextColor(Ui.col(c, R.color.s2_play_text_2));
        caption.setTextSize(13);
        caption.setGravity(Gravity.CENTER);
        caption.setSingleLine(true);
        caption.setEllipsize(TextUtils.TruncateAt.END);
        addView(caption, new LayoutParams(0, LayoutParams.WRAP_CONTENT));
    }

    // ---- Layout: the cover centred in the curtain; the name, the bar and the caption under it ----

    private int sideFor(int w, int h) {
        int m = Math.min(w, h);
        return fixedSide > 0 ? Math.min(fixedSide, Math.round(m * 0.6f)) : Math.round(m * DEFAULT_SIDE);
    }

    @Override protected void onMeasure(int wSpec, int hSpec) {
        int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
        int was = side;
        side = sideFor(w, h);
        int line = Math.max(Math.round(side * 1.5f), Ui.dp(getContext(), 220));
        line = Math.min(line, w - 2 * Ui.dp(getContext(), 16));
        measure(stage, w, h);
        measure(halo, Math.round(side * 1.9f), Math.round(side * 1.9f));
        measure(cover, side, side);
        if (side != was) {
            View t = cover.getChildAt(0);
            if ("initial".equals(t.getTag())) ((TextView) t).setTextSize(android.util.TypedValue.COMPLEX_UNIT_PX, side * 0.4f);
            cover.setBackground(Ui.round(Ui.col(getContext(), R.color.s2_play_tile), side * round, 0, 0));
        }
        halo.cover(side, side * round);
        name.measure(MeasureSpec.makeMeasureSpec(line, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.AT_MOST));
        measure(bar, line, Ui.dp(getContext(), 4));
        caption.measure(MeasureSpec.makeMeasureSpec(line, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.AT_MOST));
        setMeasuredDimension(w, h);
    }

    private static void measure(View v, int w, int h) { v.measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY)); }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int w = r - l, h = b - t, cx = w / 2, cy = h / 2;
        stage.layout(0, 0, w, h);
        place(halo, cx, cy);
        place(cover, cx, cy);
        int y = cy + side / 2 + Ui.dp(getContext(), 18);
        y = stack(name, cx, y) + Ui.dp(getContext(), 12);
        y = stack(bar, cx, y) + Ui.dp(getContext(), 10);
        stack(caption, cx, y);
    }

    private static void place(View v, int cx, int cy) {
        int w = v.getMeasuredWidth(), h = v.getMeasuredHeight();
        v.layout(cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h);
    }

    private static int stack(View v, int cx, int y) {
        int w = v.getMeasuredWidth(), h = v.getMeasuredHeight();
        v.layout(cx - w / 2, y, cx - w / 2 + w, y + h);
        return y + h;
    }

    // ---- Launcher side ----

    /**
     * MainActivity.start: the transition over the whole window, then go (on the UI thread, LAUNCH_GO_MS later; never after the curtain left its window). art: the
     * chosen card's cover box (null or not laid out: the curtain only fades in, as with reduced motion). The launcher's onResume / onStop call dismiss
     */
    static PlayCurtain launch(Activity a, View art, Bitmap cover, String title, Runnable go) {
        ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
        int w = decor.getWidth(), h = decor.getHeight();
        float round = art != null && art.getWidth() > 0 ? Ui.dp(a, 12) / (float) art.getWidth() : DEFAULT_ROUND;
        PlayCurtain c = new PlayCurtain(a, cover, title, Math.round(Math.min(w, h) * DEFAULT_SIDE), round);
        decor.addView(c, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        boolean travel = !c.still && art != null && art.isShown() && art.getWidth() > 0 && w > 0 && h > 0;
        if (!travel) {
            c.setAlpha(0f);
            c.animate().alpha(1f).setDuration(FADE_MS).start();
        } else {
            int[] at = new int[2], dc = new int[2];
            art.getLocationInWindow(at);
            decor.getLocationInWindow(dc);
            float s = art.getWidth() / (float) c.sideFor(w, h);
            float dx = at[0] - dc[0] + art.getWidth() / 2f - w / 2f, dy = at[1] - dc[1] + art.getHeight() / 2f - h / 2f;
            for (View v : new View[] {c.cover, c.halo}) { v.setTranslationX(dx); v.setTranslationY(dy); v.setScaleX(s); v.setScaleY(s); }
            c.halo.setAlpha(0f);
            c.stage.setAlpha(0f);
            for (View v : new View[] {c.name, c.bar, c.caption}) v.setAlpha(0f);
            art.setAlpha(0f);   // its copy lifts off the slot
            c.hidden = art;
            DecelerateInterpolator ease = new DecelerateInterpolator(1.6f);
            c.stage.animate().alpha(1f).setDuration(260).start();
            c.cover.animate().translationX(0).translationY(0).scaleX(1).scaleY(1).setInterpolator(ease).setDuration(LAUNCH_MS).start();
            c.halo.animate().translationX(0).translationY(0).scaleX(1).scaleY(1).alpha(1f).setInterpolator(ease).setDuration(LAUNCH_MS).start();
            for (View v : new View[] {c.name, c.bar, c.caption}) v.animate().alpha(1f).setStartDelay(260).setDuration(220).start();
        }
        c.postDelayed(() -> { if (c.isAttachedToWindow()) go.run(); }, travel ? LAUNCH_GO_MS : FADE_MS + 40);
        return c;
    }

    /** The launcher's HUD as it was: the curtain gone, the card's art shown again */
    void dismiss() {
        if (hidden != null) { hidden.animate().cancel(); hidden.setAlpha(1f); hidden = null; }
        animate().cancel();
        ViewGroup p = (ViewGroup) getParent();
        if (p != null) p.removeView(this);
    }

    /** The launcher's extras for GameActivity's curtain: the cover's side and roundness at the centre, so the game's window shows the same picture */
    void extras(Intent i) {
        i.putExtra(X_SIZE, side > 0 ? side : fixedSide).putExtra(X_ROUND, round);
    }

    // ---- Game side ----

    /** GameActivity.onCreate (a new run only): the curtain fully shown over root, at the size the launcher passed (none: a frontend's start, the default size) */
    static PlayCurtain loading(Activity a, ViewGroup root, String titleId, String title) {
        Intent i = a.getIntent();
        PlayCurtain c = new PlayCurtain(a, coverOf(titleId), title, i.getIntExtra(X_SIZE, 0), i.getFloatExtra(X_ROUND, DEFAULT_ROUND));
        root.addView(c, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        c.started = SystemClock.uptimeMillis();
        return c;
    }

    /** The cover the library shows (Covers' file), decoded at most about 512 px wide; null when there is none or it cannot be read */
    private static Bitmap coverOf(String titleId) {
        try {
            File f = Covers.file(titleId);
            if (!f.isFile()) return null;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(f.getPath(), o);
            o.inSampleSize = 1;
            while (o.outWidth / (o.inSampleSize * 2) >= 512) o.inSampleSize *= 2;
            o.inJustDecodeBounds = false;
            return BitmapFactory.decodeFile(f.getPath(), o);
        } catch (RuntimeException e) {
            return null;
        }
    }

    private long started;
    private final LoadLog log = new LoadLog();

    /** A caption without a log line (the readiness check before the host starts) */
    void caption(int res) { if (!finishing) caption.setText(res); }

    /** New complete lines of the run's log; true once frames are presented or the run failed (the caller then finishes the curtain) */
    boolean feed(String lines) {
        if (finishing) return false;
        log.feed(lines);
        tick();
        return log.drawing || log.failed;
    }

    boolean failed() { return log.failed; }

    private boolean compiling;   // the host's boot hold shows (compiling): tick leaves the bar and the caption to it

    /**
     * The host's boot hold (vka_pso.c DM_VK_PREPARE, Native.prepareStatus "taken total seconds-left"): the game waits while the pipelines of earlier runs and of the
     * game's profile are compiled, at most 90 s. The bar shows taken / total and the caption the count, the time left and that a tap skips (skip: Native.prepareSkip,
     * the game goes on and the rest compiles beside it). status null or empty: no hold (any more); the caption goes back to the log's step. True while the hold shows
     */
    boolean compiling(String status, Runnable skip) {
        if (finishing) return false;
        if ("pending".equals(status)) return true;   // the hold's first second: the curtain stays as it is, the count shows after it (vka_pso.c nxhost_pso_prepare_status)
        int[] st = parseCompiling(status);
        if (st == null) {
            if (compiling) { compiling = false; setOnClickListener(null); setClickable(true); tick(); }
            return false;
        }
        if (!compiling) {
            compiling = true;
            setOnClickListener(v -> { if (compiling) { compiling = false; setOnClickListener(null); setClickable(true); skip.run(); tick(); } });
        }
        bar.indeterminate(false);
        bar.progress(st[1] > 0 ? Math.min(0.97f, st[0] / (float) st[1]) : 0f);
        String t = st[2] >= 0 ? getContext().getString(R.string.s2_play_compiling, st[0], st[1], st[2]) : getContext().getString(R.string.s2_play_compiling_n, st[0], st[1]);
        if (!TextUtils.equals(t, caption.getText())) caption.setText(t);
        return compiling;
    }

    /** The host's answer as {taken, total, seconds left or -1}; null when it is not one (no hold to show) */
    static int[] parseCompiling(String s) {
        if (s == null) return null;
        String[] p = s.trim().split("\\s+");
        if (p.length != 3) return null;
        try {
            int done = Integer.parseInt(p[0]), total = Integer.parseInt(p[1]), eta = Integer.parseInt(p[2]);
            if (total <= 0 || done < 0) return null;
            return new int[] {Math.min(done, total), total, eta < 0 ? -1 : eta};
        } catch (NumberFormatException e) {
            return null;
        }
    }

    /** Every poll: the bar creeps through the current step toward the next (never backwards, never past it), the caption names the step */
    void tick() {
        if (finishing || compiling) return;
        long now = SystemClock.uptimeMillis();
        if (log.stage == LoadLog.NONE) { bar.indeterminate(true); return; }
        bar.indeterminate(false);
        float from = LoadLog.AT[log.stage], to = log.stage + 1 < LoadLog.AT.length ? LoadLog.AT[log.stage + 1] : 0.97f;
        float v = from;
        if (log.of > 0) v = from + (to - from) * Math.min(1f, log.done / (float) log.of);
        else v = from + (to - from) * 0.85f * (1f - (float) Math.exp(-(now - log.since) / 8000.0));
        bar.progress(v);
        CharSequence t = log.caption(getContext());
        if (t != null && !TextUtils.equals(t, caption.getText())) caption.setText(t);
    }

    long shownMs() { return SystemClock.uptimeMillis() - started; }

    /** The game draws (or failed, or the wait ran out): the bar fills, then the curtain fades into the game and leaves; after (may be null) once it is gone */
    void finish(boolean fast, Runnable after) {
        if (finishing) return;
        finishing = true;
        setClickable(false);
        if (log.drawing) { bar.indeterminate(false); bar.progress(1f); caption.setText(R.string.s2_play_ready); }
        animate().alpha(0f).setStartDelay(fast ? 0 : 120).setDuration(fast || still ? 150 : OUT_MS).withEndAction(() -> {
            ViewGroup p = (ViewGroup) getParent();
            if (p != null) p.removeView(this);
            if (after != null) after.run();
        }).start();
    }

    @Override protected void onDetachedFromWindow() {
        animate().cancel();
        super.onDetachedFromWindow();
    }

    /**
     * What the run's log says about the start-up, step by step (the host's own lines, dm/host/main.c and dm/gfx/vk_android): the SDK's start (">>> nnosInitialize",
     * ">>> nninitStartup"), the game's main (">>> nnMain"), the shader library ("[vk] shader library ", with "shader library check: a / b shaders" when the check
     * runs), the pipeline prewarm ("[vk] prewarm list …: N pipeline keys", "[vk] prewarm: N pipeline keys … made ahead", "[vk] prewarm: off"), the swapchain
     * ("[vk] swapchain ": the first scene's textures and frame), then the presented frames (GameActivity.tail's own signal: "[nvn] frame <n>:", "[longframe]",
     * "window frame rate"). The prewarm goes on in the background after the first frame: the curtain never waits for it. A step only ever moves forward.
     */
    static final class LoadLog {
        static final int NONE = 0, CODE = 1, MAIN = 2, SHADERS = 3, PREWARM = 4, SCENE = 5;
        static final float[] AT = {0f, 0.06f, 0.16f, 0.30f, 0.55f, 0.74f};
        private static final Pattern CHECK = Pattern.compile("shader library check: .*?(\\d+) / (\\d+) shaders");
        private static final Pattern PW_LIST = Pattern.compile("prewarm list .*: (\\d+) pipeline keys");
        private static final Pattern PW_START = Pattern.compile("prewarm: (\\d+) pipeline keys");

        int stage = NONE, done, of, pipelines;
        long since = SystemClock.uptimeMillis();
        boolean drawing, failed;

        void feed(String text) {
            for (String l : text.split("\n")) line(l);
        }

        private void line(String l) {
            if (l.matches(".*\\[nvn\\] frame \\d+:.*") || l.contains("[longframe]") || l.contains("window frame rate")) drawing = true;   // not "[nvn] frame step …"
            if (l.contains("CRASH sig=") || l.contains("FATAL")) failed = true;
            if (l.contains(">>> nnosInitialize") || l.contains(">>> nninitStartup")) to(CODE);
            else if (l.contains(">>> nnMain")) to(MAIN);
            else if (l.contains("[vk] shader library check:")) {
                to(SHADERS);
                Matcher m = CHECK.matcher(l);
                if (stage == SHADERS && m.find()) { done = Math.max(done, parse(m.group(1))); of = Math.max(of, parse(m.group(2))); }
            } else if (l.contains("[vk] shader library ")) to(SHADERS);
            else if (l.contains("[vk] prewarm list ") || l.contains("[vk] prewarm: ")) {
                to(PREWARM);
                Matcher m = PW_LIST.matcher(l);
                if (m.find()) pipelines = Math.max(pipelines, parse(m.group(1)));
                else if ((m = PW_START.matcher(l)).find()) pipelines = Math.max(pipelines, parse(m.group(1)));
            } else if (l.contains("[vk] swapchain ")) to(SCENE);
        }

        private void to(int s) {
            if (s <= stage) return;
            stage = s;
            since = SystemClock.uptimeMillis();
            done = of = 0;   // a count belongs to its own step
        }

        private static int parse(String s) { try { return Integer.parseInt(s); } catch (NumberFormatException e) { return 0; } }

        CharSequence caption(Context c) {
            switch (stage) {
                case CODE: return c.getString(R.string.s2_play_code);
                case MAIN: return c.getString(R.string.s2_play_main);
                case SHADERS: return of > 0 ? c.getString(R.string.s2_play_shader_check, done, of) : c.getString(R.string.s2_play_shaders);
                case PREWARM: return pipelines > 0 ? c.getString(R.string.s2_play_prewarm_n, pipelines) : c.getString(R.string.s2_play_prewarm);
                case SCENE: return c.getString(R.string.s2_play_scene);
                default: return null;
            }
        }
    }

    /** The brand halo behind the cover: a soft cyan-to-coral glow and CoverFrame's two offset hairlines around the cover; it breathes unless motion is reduced */
    private static final class Halo extends View {
        private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final int cyan, coral;
        private final float edge, offset, gap;
        private final boolean still;
        private float side, radius, breath = 1f;
        private ValueAnimator pulse;

        Halo(Context c, boolean still) {
            super(c);
            this.still = still;
            cyan = Ui.col(c, R.color.s2_brand_cyan);
            coral = Ui.col(c, R.color.s2_brand_coral);
            edge = Ui.dp(c, 1.25f);
            offset = Ui.dp(c, 2f);
            gap = Ui.dp(c, 6f);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(edge);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        }

        void cover(float side, float radius) {
            if (this.side == side && this.radius == radius) return;
            this.side = side;
            this.radius = radius;
            invalidate();
        }

        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            glow.setShader(new RadialGradient(w / 2f, h / 2f, Math.max(1, w / 2f), new int[] {alpha(cyan, 0.42f), alpha(coral, 0.18f), 0},
                    new float[] {0.25f, 0.62f, 1f}, Shader.TileMode.CLAMP));
        }

        private static int alpha(int c, float a) { return (Math.round(255 * a) << 24) | (c & 0xFFFFFF); }

        @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            if (still) return;
            pulse = ValueAnimator.ofFloat(0.7f, 1f);
            pulse.setDuration(1400);
            pulse.setRepeatMode(ValueAnimator.REVERSE);
            pulse.setRepeatCount(ValueAnimator.INFINITE);
            pulse.addUpdateListener(a -> { breath = (float) a.getAnimatedValue(); invalidate(); });
            pulse.start();
        }

        @Override protected void onDetachedFromWindow() {
            if (pulse != null) { pulse.cancel(); pulse = null; }
            super.onDetachedFromWindow();
        }

        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            if (w <= 0 || side <= 0) return;
            float k = 0.92f + 0.08f * breath;
            c.save();
            c.scale(k, k, w / 2, h / 2);
            glow.setAlpha(Math.round(255 * (0.55f + 0.45f * breath)));
            c.drawCircle(w / 2, h / 2, w / 2, glow);
            c.restore();
            r.set(w / 2 - side / 2 - gap, h / 2 - side / 2 - gap, w / 2 + side / 2 + gap, h / 2 + side / 2 + gap);
            float rr = radius + gap;
            int a = Math.round(170 * (0.6f + 0.4f * breath));
            line.setColor(cyan);
            line.setAlpha(a);
            r.offset(-offset, -offset);
            c.drawRoundRect(r, rr, rr, line);
            line.setColor(coral);
            line.setAlpha(a);
            r.offset(2 * offset, 2 * offset);
            c.drawRoundRect(r, rr, rr, line);
        }
    }

    /**
     * The loading bar: a rounded track and a cyan-to-coral fill. Determinate: the fill eases toward the value it was given and never goes back. Indeterminate (no
     * step known yet): a short segment sweeps the track; with reduced motion it stays still in the middle
     */
    private static final class Bar extends View {
        private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF r = new RectF();
        private final boolean still;
        private float target, shown;
        private boolean indeterminate = true;
        private final long t0 = SystemClock.uptimeMillis();

        Bar(Context c, boolean still) {
            super(c);
            this.still = still;
            track.setColor(Ui.col(c, R.color.s2_play_track));
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        }

        void indeterminate(boolean on) {
            if (indeterminate == on) return;
            if (!on && shown < target) shown = target;   // nothing to ease from
            indeterminate = on && target <= 0;   // once a value was shown the bar never goes back to sweeping
            invalidate();
        }

        void progress(float v) {
            v = Math.max(0f, Math.min(1f, v));
            if (v <= target) return;
            target = v;
            if (still) shown = target;
            invalidate();
        }

        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            fill.setShader(new LinearGradient(0, 0, Math.max(1, w), 0, Ui.col(getContext(), R.color.s2_brand_cyan), Ui.col(getContext(), R.color.s2_brand_coral), Shader.TileMode.CLAMP));
        }

        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(), rad = h / 2;
            r.set(0, 0, w, h);
            c.drawRoundRect(r, rad, rad, track);
            if (indeterminate) {
                float seg = w * 0.28f;
                float x = still ? (w - seg) / 2 : ((SystemClock.uptimeMillis() - t0) % 1600) / 1600f * (w + seg) - seg;
                r.set(Math.max(0, x), 0, Math.min(w, x + seg), h);
                if (r.width() > 0) c.drawRoundRect(r, rad, rad, fill);
                if (!still) postInvalidateOnAnimation();
                return;
            }
            if (shown < target) {
                shown = Math.min(target, shown + Math.max(0.002f, (target - shown) * 0.12f));
                postInvalidateOnAnimation();
            }
            r.set(0, 0, Math.max(h, w * shown), h);
            c.drawRoundRect(r, rad, rad, fill);
        }
    }
}
