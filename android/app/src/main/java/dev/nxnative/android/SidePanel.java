package dev.nxnative.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Color;
import android.graphics.Insets;
import android.text.TextUtils;
import android.view.ContextThemeWrapper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.function.BiConsumer;
import java.util.function.IntConsumer;
import java.util.function.IntFunction;

/**
 * The in-game panel: slides in from the right over the running game, which goes on behind it (docs/SWAP2-NATIVE-SIDE-PANEL-v1.md). It holds the frame rate cap
 * (Native.fpsCapSet), frame generation (Native.frameGenSet through Lsfg.push), the live graphics rows (GameGraphics.rows), the overlay's switches and sliders, the
 * on-screen controls' rows (TouchControls.controlRows), Pause and Stop. Every change applies at once, without going back to the launcher, and is kept for the next
 * start. Opened by Back (the phone's button or its edge swipe), a long press on the on-screen controls' show / hide button, the handle at the right edge (hidden
 * while a gamepad is connected), or on a gamepad its Home / guide button (KEYCODE_BUTTON_MODE) or Minus and Plus held together for a second (for a pad whose Home
 * the system keeps); closed the same ways, by its close button, by a tap beside it, or by B on a gamepad, whose D-pad and A move through it meanwhile and whose L and
 * R change its tab (GameActivity.dispatchKeyEvent, its A and B as PadNav names them).
 *
 * <p>Its look is the launcher's Console revision 6 (Theme.Banana, res/values[-night]/colors.xml, Ui), light or dark as Settings › Theme says, on this panel and its
 * dialogs only: a themed context made at each opening ({@link #themed}); the game's window and the touch controls' artwork keep their own. A fixed header (the
 * brand mark, the running game's name, a close button) and Graphics / Monitor / Controls tabs above, fixed actions below (Back to the game, Pause, Stop), and only
 * the selected tab's content scrolls; the tab and each tab's scroll position are kept while the game runs.
 */
final class SidePanel extends FrameLayout {
    private static final int TABS = 3;
    private final Activity act;
    private final BiConsumer<Context, LinearLayout> fill;   // GameActivity: TouchControls.controlRows, given the panel's themed context
    private final View scrim;
    private LinearLayout sheet;
    private Context ui;
    private Segments tabs;
    private final ScrollView[] pages = new ScrollView[TABS];
    private final int[] scrollY = new int[TABS];
    private int tab;
    private Ui.Setting fpsRow, lsfgRow;
    private TextView fpsChip;
    private View fpsChevron;
    private String lsfgMissing = "";
    private boolean open;
    java.util.function.Consumer<Boolean> onVisibility;
    private final Runnable tick = this::refresh;

    SidePanel(Activity a, BiConsumer<Context, LinearLayout> fill) {
        super(a);
        act = a;
        this.fill = fill;
        scrim = new View(a);   // a modest scrim: the game stays visible beside the panel; a tap there closes it
        scrim.setBackgroundColor(0x66000000);
        scrim.setOnClickListener(v -> close());
        addView(scrim, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
        setOnApplyWindowInsetsListener((v, in) -> { inset(in); return in; });
        setVisibility(GONE);
    }

    boolean isOpen() { return open; }
    void toggle() { if (open) close(); else open(); }

    void open() {
        if (open) return;
        open = true;
        if (onVisibility != null) onVisibility.accept(true);
        if (sheet != null) { sheet.animate().cancel(); removeView(sheet); }
        build();
        setVisibility(VISIBLE);
        if (LauncherTheme.still(act)) { sheet.setTranslationX(0); scrim.setAlpha(1f); }
        else {
            sheet.setTranslationX(sheet.getLayoutParams().width > 0 ? sheet.getLayoutParams().width : getWidth());
            sheet.animate().translationX(0).setDuration(180).start();
            scrim.setAlpha(0f);
            scrim.animate().alpha(1f).setDuration(180).start();
        }
        tabs.items[tab].post(() -> tabs.items[tab].requestFocus());   // a gamepad starts on the tabs; a touch leaves no focus (touch mode)
        removeCallbacks(tick);
        refresh();
    }

    void close() {
        if (!open) return;
        open = false;
        removeCallbacks(tick);   // the one refresh loop ends with the panel
        keepScroll();
        Runnable end = () -> { if (!open) { setVisibility(GONE); if (onVisibility != null) onVisibility.accept(false); } };
        if (LauncherTheme.still(act)) { end.run(); return; }
        scrim.animate().alpha(0f).setDuration(150).start();
        sheet.animate().translationX(sheet.getWidth()).setDuration(150).withEndAction(end).start();
    }

    @Override protected void onDetachedFromWindow() { removeCallbacks(tick); super.onDetachedFromWindow(); }

    /** A gamepad key while the panel is open: B closes it, A presses what has focus, L and R change the tab, the D-pad moves the focus (the view system's own handling) */
    boolean padKey(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_B) { if (e.getAction() == KeyEvent.ACTION_UP) close(); return true; }
        if (k == KeyEvent.KEYCODE_BUTTON_A) {
            View f = findFocus();
            if (e.getAction() == KeyEvent.ACTION_UP && f != null) f.performClick();
            return true;
        }
        boolean dpad = k == KeyEvent.KEYCODE_DPAD_UP || k == KeyEvent.KEYCODE_DPAD_DOWN || k == KeyEvent.KEYCODE_DPAD_LEFT || k == KeyEvent.KEYCODE_DPAD_RIGHT;
        if (dpad && tabs != null && findFocus() == null) {   // the focus outside the panel (on the game's views): it comes back to the selected tab
            if (e.getAction() == KeyEvent.ACTION_DOWN) tabs.items[tab].requestFocus();
            return true;
        }
        if (k == KeyEvent.KEYCODE_BUTTON_L1 || k == KeyEvent.KEYCODE_BUTTON_R1) {
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0 && tabs != null) {
                show((tab + (k == KeyEvent.KEYCODE_BUTTON_R1 ? 1 : TABS - 1)) % TABS);
                tabs.items[tab].requestFocus();
            }
            return true;
        }
        return false;
    }

    /** The running game's title ID (GameActivity's intent): the panel's settings are that game's own */
    private String game() { return String.valueOf(act.getIntent().getStringExtra("title_id")); }

    /**
     * The launcher's theme (Theme.Banana) for this panel and its dialogs, light or dark as Settings › Theme says (LauncherTheme; System: the system's), over the
     * game's activity, whose own window, theme and configuration stay as they are. Still the activity underneath, so dialogs open from it
     */
    private Context themed() {
        Configuration cfg = new Configuration(act.getResources().getConfiguration());
        String m = LauncherTheme.mode(act);
        int night = LauncherTheme.LIGHT.equals(m) ? Configuration.UI_MODE_NIGHT_NO : LauncherTheme.DARK.equals(m) ? Configuration.UI_MODE_NIGHT_YES
                : Resources.getSystem().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        cfg.uiMode = (cfg.uiMode & ~Configuration.UI_MODE_NIGHT_MASK) | night;
        ContextThemeWrapper w = new ContextThemeWrapper(act, R.style.Theme_Banana);
        w.applyOverrideConfiguration(cfg);
        return w;
    }

    // ---- Layout ----

    private void build() {
        ui = themed();
        sheet = new LinearLayout(ui);
        sheet.setOrientation(LinearLayout.VERTICAL);
        sheet.setBackgroundColor(Ui.col(ui, R.color.bn_bg));   // opaque, edge to edge; only the content is inset (inset)
        sheet.setClickable(true);   // a touch on the panel never reaches the game or the scrim
        boolean low = ui.getResources().getConfiguration().screenHeightDp < 480;   // a landscape phone: the three actions share one row, so the content keeps its height

        // Header: the brand mark, Swap2-Native and Quick settings; the running game's name on its own line; the close button
        LinearLayout head = Ui.row(ui);
        head.addView(Ui.badge(ui, 18));
        LinearLayout names = new LinearLayout(ui);
        names.setOrientation(LinearLayout.VERTICAL);
        names.setPadding(Ui.dp(ui, 10), 0, Ui.dp(ui, 8), 0);
        LinearLayout brand = Ui.row(ui);
        TextView title = Ui.text(ui, "Swap2-Native", 19, R.color.bn_text, 800);
        title.setSingleLine(true);
        title.setIncludeFontPadding(false);
        title.setAccessibilityHeading(true);
        brand.addView(title, Ui.wrap());
        TextView quick = Ui.text(ui, ui.getString(R.string.s2g_panel_quick), 12, R.color.bn_text_2, 600);
        quick.setSingleLine(true);
        quick.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams ql = Ui.weight();
        ql.leftMargin = Ui.dp(ui, 8);
        brand.addView(quick, ql);
        names.addView(brand, Ui.fill());
        String name = act.getIntent().getStringExtra("name");
        LinearLayout gameLine = Ui.row(ui);
        ImageView pad = new ImageView(ui);
        pad.setImageDrawable(Ui.icon(ui, R.drawable.s2_ic_gamepad, R.color.bn_text_2));
        pad.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        gameLine.addView(pad, new LinearLayout.LayoutParams(Ui.dp(ui, 18), Ui.dp(ui, 18)));
        TextView gameName = Ui.text(ui, name != null && !name.isEmpty() ? name : game(), 15, R.color.bn_text, 700);
        gameName.setSingleLine(true);
        gameName.setEllipsize(TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams gl = Ui.weight();
        gl.leftMargin = Ui.dp(ui, 6);
        gameLine.addView(gameName, gl);
        LinearLayout.LayoutParams gll = Ui.fill();
        gll.topMargin = Ui.dp(ui, 3);
        names.addView(gameLine, gll);
        head.addView(names, Ui.weight());
        head.addView(Ui.iconButton(ui, R.drawable.s2_ic_close, ui.getString(R.string.s2g_ga_close), Ui.Kind.TONAL, v -> close()));
        sheet.addView(head, Ui.fill());

        // Tabs, then the pages: only the selected one is shown (the others are gone, so they take no focus), and only it scrolls
        tabs = segmented(ui, new int[] {R.drawable.s2_ic_image, R.drawable.s2_ic_display, R.drawable.s2_ic_gamepad},
                new CharSequence[] {ui.getString(R.string.s2g_panel_tab_graphics), ui.getString(R.string.s2g_panel_tab_monitor), ui.getString(R.string.s2g_panel_tab_controls)},
                tab, this::show);
        tabs.byDefault = true;
        tabs.select(tab);
        LinearLayout.LayoutParams tl = Ui.fill();
        tl.topMargin = Ui.dp(ui, low ? 8 : 12);
        sheet.addView(tabs.view, tl);
        FrameLayout body = new FrameLayout(ui);
        for (int i = 0; i < TABS; i++) {
            ScrollView sv = new ScrollView(ui);
            sv.setClipToPadding(false);
            sv.setScrollBarStyle(View.SCROLLBARS_OUTSIDE_OVERLAY);
            LinearLayout content = new LinearLayout(ui);
            content.setOrientation(LinearLayout.VERTICAL);
            content.setPadding(0, Ui.dp(ui, 2), 0, Ui.dp(ui, 12));
            sv.addView(content, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            body.addView(sv, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            pages[i] = sv;
            if (i == 0) graphics(content);
            else if (i == 1) monitor(content);
            else fill.accept(ui, content);
        }
        LinearLayout.LayoutParams bl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1);
        bl.topMargin = Ui.dp(ui, 8);
        sheet.addView(body, bl);

        // Fixed actions under a hairline: Back to the game, then Pause and Stop (one row on a short window, when the labels fit)
        View line = Ui.divider(ui);
        sheet.addView(line);
        Ui.Action back = action(Ui.Kind.PRIMARY, R.drawable.s2_ic_play, ui.getString(R.string.s2g_panel_back), v -> close());
        // Pause: the game suspended (clock, graphics, threads, audio) until a tap on the screen, or A / Start: GameActivity.pause
        Ui.Action pause = action(Ui.Kind.TONAL, R.drawable.s2_ic_pause, ui.getString(R.string.s2g_panel_pause_short), v -> { close(); if (act instanceof GameActivity) ((GameActivity) act).pause(); });
        pause.setContentDescription(ui.getString(R.string.s2g_panel_pause));
        Ui.Action stop = action(Ui.Kind.DANGER, R.drawable.s2_ic_stop, ui.getString(R.string.s2g_panel_stop_short), v -> confirmStop());
        stop.setContentDescription(ui.getString(R.string.s2g_panel_stop));
        LinearLayout foot = new LinearLayout(ui);
        foot.setOrientation(LinearLayout.VERTICAL);
        sheet.addView(foot, Ui.fill());
        arrange(foot, back, pause, stop, low);
        // One row only while every label fits whole (the actual text: a translation, a large font size); else Back to the game over Pause and Stop
        if (low) foot.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                if (r - l <= 0) return;
                foot.removeOnLayoutChangeListener(this);
                int need = Ui.dp(ui, 16);
                for (View x : new View[] {back, pause, stop}) need += natural(x);
                if (need > r - l) foot.post(() -> arrange(foot, back, pause, stop, false));
            }
        });
        addView(sheet, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END));
        select(tab);   // not show: the new pages' scroll positions are 0 until restored below
        WindowInsets in = getRootWindowInsets();
        inset(in);
    }

    /** The footer's actions: one row, each as wide as its label needs (shared out), or Back to the game full width over Pause and Stop */
    private void arrange(LinearLayout foot, View back, View pause, View stop, boolean oneRow) {
        foot.removeAllViews();
        for (View x : new View[] {back, pause, stop}) if (x.getParent() != null) ((ViewGroup) x.getParent()).removeView(x);
        int gap = Ui.dp(ui, 8);
        if (oneRow) {
            LinearLayout row = Ui.row(ui);
            View[] all = {back, pause, stop};
            for (int i = 0; i < 3; i++) {
                LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, natural(all[i]));
                if (i > 0) p.leftMargin = gap;
                row.addView(all[i], p);
            }
            LinearLayout.LayoutParams rl = Ui.fill();
            rl.topMargin = gap;
            foot.addView(row, rl);
        } else {
            LinearLayout.LayoutParams b1 = Ui.fill();
            b1.topMargin = Ui.dp(ui, 10);
            foot.addView(back, b1);
            LinearLayout.LayoutParams rl = Ui.fill();
            rl.topMargin = gap;
            foot.addView(Ui.pair(ui, pause, stop), rl);
        }
    }

    /** A view's width with nothing limiting it (its label whole) */
    private static int natural(View v) {
        v.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        return v.getMeasuredWidth();
    }

    /** A footer action: 48 dp high, its icon and a one-line label */
    private Ui.Action action(Ui.Kind k, int icon, CharSequence label, View.OnClickListener l) {
        Ui.Action a = Ui.action(ui, icon, label, k, l);
        a.setMinimumHeight(Ui.dp(ui, 48));
        a.setPadding(Ui.dp(ui, 10), 0, Ui.dp(ui, 10), 0);
        a.label.setTextSize(15);
        a.label.setEllipsize(TextUtils.TruncateAt.END);
        ((LinearLayout.LayoutParams) a.label.getLayoutParams()).leftMargin = Ui.dp(ui, 8);
        a.icon.setLayoutParams(new LinearLayout.LayoutParams(Ui.dp(ui, 18), Ui.dp(ui, 18)));
        return a;
    }

    /**
     * The panel's width and padding: min(440 dp, half the usable width) at the right edge, the whole width on a narrow window; its background reaches the
     * window's edges, its content stays clear of a display cutout and of the system bars while they show
     */
    private void inset(WindowInsets in) {
        if (sheet == null) return;
        Insets i = in == null ? Insets.NONE : in.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        int w = getWidth() > 0 ? getWidth() : act.getWindow().getDecorView().getWidth();
        if (w <= 0) w = getResources().getDisplayMetrics().widthPixels;
        int usable = w - i.left - i.right;
        boolean narrow = usable < Ui.dp(ui, 600);
        int want = narrow ? ViewGroup.LayoutParams.MATCH_PARENT : Math.min(Ui.dp(ui, 440), usable / 2) + i.right;
        LayoutParams lp = (LayoutParams) sheet.getLayoutParams();
        if (lp != null && lp.width != want) { lp.width = want; sheet.setLayoutParams(lp); }
        int g = Ui.dp(ui, 16);
        sheet.setPadding(g + (narrow ? i.left : 0), i.top + Ui.dp(ui, 10), g + i.right, i.bottom + Ui.dp(ui, 10));
    }

    /** The shown tab's scroll position, kept for the next time it shows (hidden pages are gone and keep theirs) */
    private void keepScroll() { if (pages[tab] != null && pages[tab].getVisibility() == VISIBLE) scrollY[tab] = pages[tab].getScrollY(); }

    /** Shows tab i (its page; the others gone) */
    private void show(int i) {
        keepScroll();
        select(i);
    }

    private void select(int i) {
        tab = i;
        tabs.select(i);
        for (int k = 0; k < TABS; k++) pages[k].setVisibility(k == i ? VISIBLE : GONE);
        ScrollView sv = pages[i];
        int y = scrollY[i];
        if (y == 0) return;
        // Restored once the page has its height (a page just built or shown again is laid out at the next pass)
        sv.getViewTreeObserver().addOnGlobalLayoutListener(new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
            @Override public void onGlobalLayout() {
                sv.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                sv.scrollTo(0, y);
            }
        });
    }

    // ---- Graphics: the frame rate cap, frame generation, the live image rows, pause on return ----

    private void graphics(LinearLayout box) {
        section(ui, box, ui.getString(R.string.s2g_panel_sec_performance));
        LinearLayout perf = card(ui);
        // The game's own frame rate; frame generation (below) adds frames on top. Its value opens a choice of the rates the host offers for this game (refresh)
        fpsRow = row(ui, 0, ui.getString(R.string.s2g_panel_fps_cap), "", v -> pickCap());
        fpsChip = Ui.chip(ui, "");
        fpsChip.setSingleLine(true);
        fpsChip.setEllipsize(TextUtils.TruncateAt.END);
        fpsChip.setMaxWidth(Ui.dp(ui, 150));
        fpsRow.view.addView(fpsChip, Ui.wrap());
        fpsRow.chevron(ui);
        fpsChevron = fpsRow.view.getChildAt(fpsRow.view.getChildCount() - 1);
        perf.addView(fpsRow.view);
        add(box, perf);
        add(box, frameGeneration());

        section(ui, box, ui.getString(R.string.s2g_panel_sec_image));
        LinearLayout image = card(ui);
        Runnable[] redraw = new Runnable[1];   // a choice redraws the rows (the sharpness row shows only with FSR); only the options the renderer applies live
        redraw[0] = () -> { image.removeAllViews(); GameGraphics.rows(ui, game(), image, redraw[0], true); unclip(image); };
        redraw[0].run();
        add(box, image);

        LinearLayout session = card(ui);
        session.addView(toggleRow(ui, R.drawable.s2_ic_pause, ui.getString(R.string.s2g_panel_pause_return), null, Prefs.pauseOnReturn(act, game()),
                (b, on) -> Prefs.pauseOnReturn(act, game(), on)).view);
        add(box, session);
    }

    /** GameGraphics' rows (Ui.Setting) in the panel: their titles may wrap and their explanations show whole (the launcher keeps them to two lines) */
    private static void unclip(LinearLayout rows) {
        for (int i = 0; i < rows.getChildCount(); i++) {
            if (!(rows.getChildAt(i) instanceof LinearLayout)) continue;
            LinearLayout r = (LinearLayout) rows.getChildAt(i);
            if (r.getChildCount() < 2 || !(r.getChildAt(1) instanceof LinearLayout)) continue;
            LinearLayout col = (LinearLayout) r.getChildAt(1);
            if (col.getChildCount() < 2) continue;
            TextView title = (TextView) col.getChildAt(0), value = (TextView) col.getChildAt(1);
            title.setSingleLine(false);
            title.setMaxLines(3);
            value.setMaxLines(8);
        }
    }

    /** The host's bit of an offered cap in fpsCapStatus's rates: 15, 20, 30, (40, 45,) 60 */
    private static int capBit(int v) { return v == 15 ? 0 : v == 20 ? 1 : v == 30 ? 2 : 5; }

    /** The cap's choice: the game's own, and only the caps the host can hold for this game (fpsCapStatus's rates) */
    private void pickCap() {
        String[] f = Native.fpsCapStatus().split(" ");
        int cap = Integer.parseInt(f[0]), rates = Integer.parseInt(f[1]);
        int cur = cap >= 0 ? cap : Prefs.fpsCap(act, game());   // the cap in effect (env.txt or DM_CTL may have set another)
        List<Integer> values = new ArrayList<>();
        List<String> labels = new ArrayList<>();
        int checked = -1;   // a cap the launcher does not offer (40, 45 from env.txt or DM_CTL): none checked, the row's line says it
        for (int v : Prefs.FPS_CAPS) {
            if (v != 0 && !(cap >= 0 && (rates & 1 << capBit(v)) != 0)) continue;
            if (v == cur || v == 0 && cur < 0) checked = values.size();
            values.add(v);
            labels.add(v == 0 ? ui.getString(R.string.s2g_panel_fps_own) : v + " fps");
        }
        choose(ui, ui.getString(R.string.s2g_panel_fps_cap), labels.toArray(new String[0]), checked, w -> {
            int v = values.get(w);
            Prefs.fpsCap(act, game(), v);   // the running game's own cap from now on (the main screen's stays the default of the others)
            Native.fpsCapSet(v);
            refresh();
        });
    }

    /** Frame generation (Lossless Scaling, the user's own Lossless.dll): the switch with its live state, then the multiplier, the target and the motion estimation */
    private LinearLayout frameGeneration() {
        LinearLayout c = card(ui);
        lsfgRow = null;
        if (!BuildConfig.NX_LSFG) {
            c.addView(row(ui, R.drawable.s2_ic_gauge, ui.getString(R.string.s2g_lsfg_on), ui.getString(R.string.s2g_lsfg_not_built), null).view);
            return c;
        }
        lsfgMissing = Lsfg.find(act) == null ? " · " + ui.getString(R.string.s2_lsfg_dll_missing) : "";
        Runnable apply = () -> { Lsfg.push(act); refresh(); };
        // Lsfg's own preferences (Lsfg.on, multiplier, target, flow read them; Lsfg.env passes them at the next start)
        lsfgRow = toggleRow(ui, R.drawable.s2_ic_gauge, ui.getString(R.string.s2g_lsfg_on), "Lossless Scaling" + lsfgMissing, Lsfg.on(act),
                (b, v) -> { Prefs.of(act).edit().putBoolean("lsfg", v).commit(); apply.run(); });
        c.addView(lsfgRow.view);
        View d = Ui.divider(ui);
        LinearLayout.LayoutParams dl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(ui, 1));
        dl.leftMargin = dl.rightMargin = Ui.dp(ui, 10);
        c.addView(d, dl);
        LinearLayout pair = Ui.row(ui);
        pair.addView(cell(R.string.s2g_panel_lsfg_mult, R.string.s2g_lsfg_mult, new String[] {"2×", "3×", "4×"}, Lsfg.MULTS, Lsfg.multiplier(act), "lsfg_mult", apply), Ui.weight());
        View split = new View(ui);
        split.setBackgroundColor(Ui.col(ui, R.color.bn_card_stroke));
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(Ui.dp(ui, 1), Ui.dp(ui, 32));
        sl.leftMargin = sl.rightMargin = Ui.dp(ui, 4);
        pair.addView(split, sl);
        pair.addView(cell(R.string.s2g_panel_lsfg_target, R.string.s2g_lsfg_target, new String[] {ui.getString(R.string.s2g_lsfg_fixed), "60", "90", "120"}, Lsfg.TARGETS,
                Lsfg.target(act), "lsfg_target", apply), Ui.weight());
        c.addView(pair, Ui.fill());
        c.addView(cell(R.string.s2g_panel_lsfg_flow, R.string.s2g_lsfg_flow, new String[] {ui.getString(R.string.s2g_lsfg_auto), "25%", "50%", "75%", "100%"}, Lsfg.FLOWS,
                Lsfg.flow(act), "lsfg_flow", apply), Ui.fill());
        TextView mode = Ui.small(ui, ui.getString(R.string.s2g_lsfg_mode));   // Eden ports only the performance shader set (lossless_dll.h PerformanceShader)
        mode.setTextSize(12);
        mode.setPadding(Ui.dp(ui, 12), 0, Ui.dp(ui, 12), Ui.dp(ui, 8));
        c.addView(mode);
        return c;
    }

    /** A compact value cell of the frame generation card: its short name over its value and a ▾; a tap opens the choice, titled with the option's whole explanation */
    private View cell(int name, int explained, String[] labels, int[] values, int current, String key, Runnable apply) {
        LinearLayout v = Ui.row(ui);
        v.setMinimumHeight(Ui.dp(ui, 52));
        v.setPadding(Ui.dp(ui, 12), Ui.dp(ui, 6), Ui.dp(ui, 10), Ui.dp(ui, 6));
        v.setBackground(Ui.ripple(ui, Ui.round(Color.TRANSPARENT, Ui.dp(ui, 12), 0, 0)));
        v.setFocusable(true);
        LinearLayout col = new LinearLayout(ui);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView n = Ui.text(ui, ui.getString(name), 12, R.color.bn_text_2, 500);
        n.setSingleLine(true);
        n.setEllipsize(TextUtils.TruncateAt.END);
        col.addView(n);
        int at = -1;
        for (int i = 0; i < values.length; i++) if (values[i] == current) at = i;
        TextView val = Ui.text(ui, at >= 0 ? labels[at] : String.valueOf(current), 15, R.color.bn_text, 700);
        val.setSingleLine(true);
        val.setEllipsize(TextUtils.TruncateAt.END);
        col.addView(val);
        v.addView(col, Ui.weight());
        ImageView down = new ImageView(ui);
        down.setImageDrawable(Ui.icon(ui, R.drawable.s2_ic_chevron_down, R.color.bn_text_2));
        down.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        v.addView(down, new LinearLayout.LayoutParams(Ui.dp(ui, 18), Ui.dp(ui, 18)));
        v.setContentDescription(ui.getString(name) + ", " + val.getText());
        int[] sel = {at};
        v.setOnClickListener(x -> choose(ui, ui.getString(explained), labels, sel[0], w -> {
            sel[0] = w;
            Prefs.of(act).edit().putInt(key, values[w]).commit();
            val.setText(labels[w]);
            v.setContentDescription(ui.getString(name) + ", " + labels[w]);
            apply.run();
        }));
        return v;
    }

    // ---- Monitor: the performance overlay (GameActivity polls it while it is on; nothing new is polled here) ----

    private void monitor(LinearLayout box) {
        LinearLayout c = card(ui);
        List<Runnable> dependents = new ArrayList<>();
        boolean[] on = {Prefs.overlay(act)};
        Ui.Setting master = toggleRow(ui, R.drawable.s2_ic_display, ui.getString(R.string.s2g_ov_show), null, on[0], (b, v) -> {
            Prefs.overlay(act, v);
            on[0] = v;
            for (Runnable r : dependents) r.run();
        });
        c.addView(master.view);
        Segments modes = segmented(ui, null, new CharSequence[] {ui.getString(R.string.s2g_panel_ov_compact), ui.getString(R.string.s2g_panel_ov_detailed)},
                Prefs.of(act).getBoolean("overlay_detail", false) ? 1 : 0, i -> Prefs.of(act).edit().putBoolean("overlay_detail", i == 1).commit());
        LinearLayout.LayoutParams ml = Ui.fill();
        ml.setMargins(Ui.dp(ui, 10), Ui.dp(ui, 4), Ui.dp(ui, 10), Ui.dp(ui, 4));
        c.addView(modes.view, ml);
        dependents.add(() -> modes.setEnabled(on[0]));
        SeekBar scale = overlaySlider(c, "overlay_scale", R.string.s2_overlay_scale, 40, 150, 75);
        SeekBar opacity = overlaySlider(c, "overlay_opacity", R.string.s2_overlay_opacity, 20, 100, 100);
        dependents.add(() -> { enable(scale, on[0]); enable(opacity, on[0]); });
        add(box, c);
        TextView hint = Ui.small(ui, ui.getString(R.string.s2_overlay_gesture));
        hint.setPadding(Ui.dp(ui, 4), Ui.dp(ui, 6), Ui.dp(ui, 4), 0);
        box.addView(hint, Ui.fill());

        section(ui, box, ui.getString(R.string.s2g_panel_sec_items));
        LinearLayout items = card(ui);
        for (int k = 0; k < Overlay.ITEMS.length; k++) {
            String key = Overlay.ITEMS[k][0];
            Ui.Setting s = toggleRow(ui, 0, ui.getString(Overlay.LABELS[k]), null, Prefs.overlayItem(act, key), (b, v) -> Prefs.overlayItem(act, key, v));
            items.addView(s.view);
            dependents.add(() -> enable(s, on[0]));
        }
        add(box, items);
        for (Runnable r : dependents) r.run();
    }

    /** The overlay's size or opacity (Overlay's own keys and ranges): stored while dragged, committed when let go */
    private SeekBar overlaySlider(LinearLayout box, String key, int title, int min, int max, int initial) {
        int value = Math.max(min, Math.min(max, Math.round(Prefs.of(act).getFloat(key, initial / 100f) * 100)));
        return slider(ui, box, p -> ui.getString(title, p), min, max, value,
                (p, user) -> { if (user) Prefs.of(act).edit().putFloat(key, p / 100f).apply(); },
                p -> Prefs.of(act).edit().putFloat(key, p / 100f).commit());
    }

    // ---- Actions ----

    /** Stop, after the confirmation (unsaved progress is lost); Keep running is the default */
    private void confirmStop() {
        AlertDialog d = new AlertDialog.Builder(ui).setTitle(R.string.s2g_panel_stop_title).setMessage(R.string.s2g_panel_stop_message)
                // The run's exit record says the player stopped it before the process ends (as GameActivity.end does): without it the launcher found a "running"
                // run killed by a signal and showed "The game stopped unexpectedly"
                .setPositiveButton(R.string.s2g_panel_stop_yes, (x, w) -> { Native.note("stopped by the user"); ExitWatch.stopped(); if (act instanceof GameActivity) ((GameActivity) act).backToFrontend(); android.os.Process.killProcess(android.os.Process.myPid()); })
                .setNegativeButton(R.string.s2g_panel_keep_running, null).create();
        d.setOnShowListener(x -> {
            Button b = d.getButton(AlertDialog.BUTTON_POSITIVE);
            if (b != null) b.setTextColor(Ui.col(ui, R.color.bn_danger));
            Button n = d.getButton(AlertDialog.BUTTON_NEGATIVE);
            if (n != null) n.requestFocus();
        });
        d.show();
    }

    /** Twice a second while open: what the game runs at, which caps it can hold, and the frame generation's state */
    private void refresh() {
        if (!open) return;
        String[] f = Native.fpsCapStatus().split(" ");
        int cap = Integer.parseInt(f[0]), rates = Integer.parseInt(f[1]), now = Integer.parseInt(f[2]);
        // No cap in this build, or a game that picks its own frame rate: no choice (it would do nothing), the row says so and its chip reads Auto
        // (rates is 0 too until the game's main is loaded: only a game that presents, now > 0, is known to pick its own)
        // (bit 6: the host holds the cap by pacing presents alone, hle_fpslock.c dm_fps_lock_paced: the game is not told, so the line says it may slow down)
        boolean own = cap >= 0 && rates == 0 && now > 0, none = cap < 0 || own, paced = (rates & 1 << 6) != 0;
        if (cap >= 0 && now > 0) Prefs.ownRate(act, game(), own);   // its Game settings hide the cap too
        String s = cap < 0 ? ui.getString(R.string.s2g_fps_no_cap)
                : now <= 0 ? ui.getString(R.string.s2g_fps_starting)
                : own ? ui.getString(R.string.s2g_fps_own_rate)
                : cap > 0 ? ui.getString(paced ? R.string.s2g_fps_now_paced : R.string.s2g_fps_now_capped, now, cap) : ui.getString(R.string.s2g_fps_now_own, now);
        fpsRow.value(s);
        fpsChip.setText(own ? ui.getString(R.string.s2g_panel_fps_auto) : cap > 0 ? cap + " fps" : ui.getString(R.string.s2g_panel_fps_own));
        fpsChip.setVisibility(cap < 0 ? GONE : VISIBLE);
        fpsChevron.setVisibility(none ? GONE : VISIBLE);
        fpsRow.view.setEnabled(!none);   // not clickable, and no focus for the D-pad (a disabled view takes none)
        if (lsfgRow != null) {
            String st = Native.frameGenStatus();
            int sp = st.indexOf(' ');
            lsfgRow.value("Lossless Scaling · " + ui.getString(R.string.s2g_lsfg_now, sp > 0 ? st.substring(sp + 1) : st) + lsfgMissing);
        }
        removeCallbacks(tick);   // a change calls refresh too: one loop, not one more per change
        postDelayed(tick, 500);
    }

    // ---- The panel's building blocks (Console revision 6 through Ui), for its tabs and TouchControls.controlRows ----

    /** A card of rows: the surface color, a hairline edge, 18 dp corners */
    static LinearLayout card(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setBackground(Ui.round(Ui.col(c, R.color.bn_card), Ui.dp(c, 18), Ui.col(c, R.color.bn_card_stroke), Ui.dp(c, 1)));
        int p = Ui.dp(c, 4);
        l.setPadding(p, p, p, p);
        return l;
    }

    private static final Object SECTION = new Object();

    /** A section label (12 sp caps) above the next card */
    static void section(Context c, LinearLayout box, CharSequence s) {
        TextView t = Ui.section(c, s);
        t.setTag(SECTION);
        LinearLayout.LayoutParams lp = Ui.fill();
        lp.topMargin = Ui.dp(c, box.getChildCount() > 0 ? 18 : 4);
        lp.bottomMargin = Ui.dp(c, 6);
        box.addView(t, lp);
    }

    /** Adds a card (or a button) to a page: 12 dp below the one before, right under a section label */
    static <V extends View> V add(LinearLayout box, V v) {
        LinearLayout.LayoutParams lp = Ui.fill();
        int n = box.getChildCount();
        lp.topMargin = n == 0 || box.getChildAt(n - 1).getTag() == SECTION ? 0 : Ui.dp(box.getContext(), Ui.GAP);
        box.addView(v, lp);
        return v;
    }

    /** A small explanation under a card */
    static TextView note(Context c, LinearLayout box, CharSequence s) {
        TextView t = Ui.small(c, s);
        t.setPadding(Ui.dp(c, 4), Ui.dp(c, 6), Ui.dp(c, 4), 0);
        box.addView(t, Ui.fill());
        return t;
    }

    /** A row (Ui.Setting) whose title may wrap (large font sizes, long translations); icon 0: none; sub: the line under the title, or null */
    static Ui.Setting row(Context c, int icon, CharSequence title, CharSequence sub, View.OnClickListener l) {
        Ui.Setting s = icon != 0 ? new Ui.Setting(c, icon, title.toString(), l) : new Ui.Setting(c, "", title.toString(), l);
        LinearLayout col = (LinearLayout) s.view.getChildAt(1);
        if (icon == 0) {   // hidden rather than removed: Ui.Setting.toggle finds the title column at its index
            s.view.getChildAt(0).setVisibility(GONE);
            col.setPadding(Ui.dp(c, 6), 0, Ui.dp(c, 8), 0);
        }
        TextView t = (TextView) col.getChildAt(0);
        t.setSingleLine(false);
        t.setMaxLines(4);
        t.setTextSize(15);
        s.value.setMaxLines(4);
        s.view.setMinimumHeight(Ui.dp(c, 52));
        if (sub != null) s.value(sub);
        return s;
    }

    /** A row that is a switch: the whole row toggles it (one touch and focus target) */
    static Ui.Setting toggleRow(Context c, int icon, CharSequence title, CharSequence sub, boolean on, CompoundButton.OnCheckedChangeListener l) {
        Ui.Setting s = row(c, icon, title, sub, null);
        s.toggle(c, on, l);
        return s;
    }

    /** A row whose value opens a themed single choice (no radio list in the panel); pick gets the index chosen */
    static Ui.Setting choiceRow(Context c, int icon, CharSequence title, String[] labels, int checked, IntConsumer pick) {
        Ui.Setting[] s = new Ui.Setting[1];
        int[] cur = {checked};
        s[0] = row(c, icon, title, checked >= 0 ? labels[checked] : "", v -> choose(c, title, labels, cur[0], w -> {
            cur[0] = w;
            s[0].value(labels[w]);
            pick.accept(w);
        }));
        s[0].chevron(c);
        return s[0];
    }

    /** A single choice in the launcher's dialog (the context's theme: Theme.Banana.Dialog); a pick closes it */
    static void choose(Context c, CharSequence title, String[] labels, int checked, IntConsumer pick) {
        new AlertDialog.Builder(c).setTitle(title).setSingleChoiceItems(labels, checked, (d, w) -> { d.dismiss(); pick.accept(w); })
                .setNegativeButton(android.R.string.cancel, null).show();
    }

    /** A row enabled or not: faded, not clickable and taking no focus when not */
    static void enable(Ui.Setting s, boolean on) {
        s.view.setEnabled(on);
        s.view.setAlpha(on ? 1f : 0.42f);
        if (s.toggle != null) s.toggle.setEnabled(on);
    }

    static void enable(SeekBar s, boolean on) {
        s.setEnabled(on);
        ((View) s.getParent()).setAlpha(on ? 1f : 0.42f);
    }

    /**
     * A slider with its label (which shows the value, label.apply(value)) in neutral colors; change gets each new value and whether the user moved it, stop
     * (or null) the value when the finger lets go. Returns the SeekBar (setProgress takes value - min)
     */
    static SeekBar slider(Context c, LinearLayout box, IntFunction<CharSequence> label, int min, int max, int value, BiConsumer<Integer, Boolean> change, IntConsumer stop) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(Ui.dp(c, 12), Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 2));
        TextView t = Ui.text(c, label.apply(value), 15, R.color.bn_text, 600);
        l.addView(t, Ui.fill());
        SeekBar s = new SeekBar(c);
        s.setMax(max - min);
        s.setProgress(value - min);
        int ink = Ui.col(c, R.color.bn_banana);
        s.setProgressTintList(ColorStateList.valueOf(ink));
        s.setThumbTintList(ColorStateList.valueOf(ink));
        s.setProgressBackgroundTintList(ColorStateList.valueOf(Ui.col(c, R.color.bn_card_stroke)));
        s.setBackground(Ui.ripple(c, Ui.round(Color.TRANSPARENT, Ui.dp(c, 14), 0, 0)));   // the focus ring for the D-pad (left and right move it)
        s.setMinimumHeight(Ui.dp(c, 44));
        s.setContentDescription(label.apply(value));
        s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar b, int p, boolean user) {
                CharSequence now = label.apply(min + p);
                t.setText(now);
                b.setContentDescription(now);
                change.accept(min + p, user);
            }
            @Override public void onStartTrackingTouch(SeekBar b) {}
            @Override public void onStopTrackingTouch(SeekBar b) { if (stop != null) stop.accept(min + b.getProgress()); }
        });
        l.addView(s, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Ui.dp(c, 44)));
        box.addView(l, Ui.fill());
        return s;
    }

    /** A segmented choice (the tabs, the overlay's mode): the selected segment in the primary fill with inverse text */
    static final class Segments {
        final LinearLayout view;
        final View[] items;
        private final ImageView[] icons;
        private final TextView[] labels;
        private final int[] iconIds;
        private int selected = -1;
        boolean byDefault;   // the selected segment is the window's default focus (the panel's tabs)

        Segments(Context c, int[] iconIds, CharSequence[] names, IntConsumer pick) {
            this.iconIds = iconIds;
            view = Ui.row(c);
            view.setBackground(Ui.round(Ui.col(c, R.color.bn_card), Ui.dp(c, 999), Ui.col(c, R.color.bn_card_stroke), Ui.dp(c, 1)));
            int p = Ui.dp(c, 3);
            view.setPadding(p, p, p, p);
            items = new View[names.length];
            icons = new ImageView[names.length];
            labels = new TextView[names.length];
            for (int i = 0; i < names.length; i++) {
                LinearLayout it = Ui.row(c);
                it.setGravity(Gravity.CENTER);
                it.setMinimumHeight(Ui.dp(c, 42));
                it.setPadding(Ui.dp(c, 8), 0, Ui.dp(c, 8), 0);
                it.setFocusable(true);
                it.setClickable(true);
                it.setContentDescription(names[i]);
                if (iconIds != null) {
                    icons[i] = new ImageView(c);
                    icons[i].setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
                    it.addView(icons[i], new LinearLayout.LayoutParams(Ui.dp(c, 18), Ui.dp(c, 18)));
                }
                labels[i] = Ui.text(c, names[i], 14, R.color.bn_text, 600);
                labels[i].setSingleLine(true);
                labels[i].setEllipsize(TextUtils.TruncateAt.END);
                labels[i].setIncludeFontPadding(false);
                LinearLayout.LayoutParams ll = Ui.wrap();
                ll.weight = 0;
                if (iconIds != null) ll.leftMargin = Ui.dp(c, 6);
                it.addView(labels[i], ll);
                int k = i;
                it.setOnClickListener(v -> { if (k != selected) { select(k); pick.accept(k); } });
                LinearLayout.LayoutParams lp = Ui.weight();
                if (i > 0) lp.leftMargin = Ui.dp(c, 2);
                view.addView(it, lp);
                items[i] = it;
            }
        }

        void select(int i) {
            selected = i;
            Context c = view.getContext();
            for (int k = 0; k < items.length; k++) {
                boolean on = k == i;
                items[k].setSelected(on);
                if (byDefault) items[k].setFocusedByDefault(on);   // the tabs: where the focus goes when a D-pad press leaves touch mode (the window's default focus)
                items[k].setBackground(Ui.ripple(c, Ui.round(on ? Ui.col(c, R.color.bn_banana) : Color.TRANSPARENT, Ui.dp(c, 999), 0, 0)));
                labels[k].setTextColor(Ui.col(c, on ? R.color.bn_on_banana : R.color.bn_text));
                labels[k].setTypeface(Ui.face(on ? 700 : 600));
                if (icons[k] != null) icons[k].setImageDrawable(Ui.icon(c, iconIds[k], on ? R.color.bn_on_banana : R.color.bn_text_2));
            }
        }

        void setEnabled(boolean on) {
            view.setAlpha(on ? 1f : 0.42f);
            for (View v : items) v.setEnabled(on);
        }
    }

    static Segments segmented(Context c, int[] icons, CharSequence[] names, int selected, IntConsumer pick) {
        Segments s = new Segments(c, icons, names, pick);
        s.select(selected);
        return s;
    }
}
