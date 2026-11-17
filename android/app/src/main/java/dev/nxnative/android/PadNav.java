package dev.nxnative.android;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;

import java.util.Collections;
import java.util.Set;
import java.util.WeakHashMap;

/**
 * The app's own gamepad buttons: the launcher's pages, its dialogs, the other activities' windows and the in-game panel. Never the game: the host maps the
 * pad to the Switch (dm/host/android/input_map.c), by position, so the right face button is the Switch's A there.
 *
 * Android names a gamepad's face buttons by position, as an Xbox pad prints them: KEYCODE_BUTTON_A is the bottom one, B the right one, X the left one, Y the
 * top one (its key layouts do so for Nintendo pads too). With the Nintendo layout (Settings › Interface, on by default) the app reads them as the Switch labels
 * them, the same buttons the game reads as A and B: the right one is A (confirm), the bottom one B (back), the left one Y, the top one X. Off, it takes
 * Android's names as they are (the Xbox layout: the bottom button confirms, the right one goes back).
 *
 * Without help, Android turns an unhandled BUTTON_A into DPAD_CENTER (a press) and BUTTON_B into BACK (Generic.kcm's fallbacks); a window that does not read
 * the buttons itself (a dialog, another activity) gets this through {@link #install}: with the Nintendo layout the right button presses and the bottom one goes
 * back there too. MainActivity reads the buttons itself ({@link #label}): A on a cover plays it, Y opens its settings, L and R move between the dock's pages.
 */
final class PadNav {
    private PadNav() {}

    private static final String NINTENDO = "pad_nintendo";

    static boolean nintendo(Context c) { return Prefs.open(c, "interface").getBoolean(NINTENDO, true); }
    static void nintendo(Context c, boolean on) { Prefs.open(c, "interface").edit().putBoolean(NINTENDO, on).apply(); }

    static boolean gamepad(KeyEvent e) { return (e.getSource() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD; }

    /** The button as the app names it (KEYCODE_BUTTON_A is A, which confirms): the event's own key, its face buttons swapped with the Nintendo layout */
    static int label(Context c, KeyEvent e) {
        int k = e.getKeyCode();
        if (!gamepad(e) || k < KeyEvent.KEYCODE_BUTTON_A || k > KeyEvent.KEYCODE_BUTTON_Y || !nintendo(c)) return k;
        switch (k) {
            case KeyEvent.KEYCODE_BUTTON_A: return KeyEvent.KEYCODE_BUTTON_B;
            case KeyEvent.KEYCODE_BUTTON_B: return KeyEvent.KEYCODE_BUTTON_A;
            case KeyEvent.KEYCODE_BUTTON_X: return KeyEvent.KEYCODE_BUTTON_Y;
            case KeyEvent.KEYCODE_BUTTON_Y: return KeyEvent.KEYCODE_BUTTON_X;
            default: return k;
        }
    }

    /** The event with its key named as the app names it ({@link #label}) */
    static KeyEvent labelled(Context c, KeyEvent e) {
        int k = label(c, e);
        return k == e.getKeyCode() ? e : as(e, k);
    }

    /** The same press (its times, action, repeat, device and source) as another key */
    static KeyEvent as(KeyEvent e, int code) {
        return new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), code, e.getRepeatCount(), e.getMetaState(), e.getDeviceId(), e.getScanCode(),
                e.getFlags(), e.getSource());
    }

    // ---- every window of the process: the activities' and their dialogs' ----

    private static final Set<View> hooked = Collections.newSetFromMap(new WeakHashMap<>());

    /**
     * Once per process (NxApp): each activity's window and every window shown over it (a dialog, found among the process's windows when the window under it
     * loses the focus) gets the Nintendo layout's A and B. Views never take BUTTON_A or B themselves (they are not confirm keys), so the buttons reach the
     * window's unhandled-key listener, which presses (DPAD_CENTER) or goes back (BACK) as the label says; with the Xbox layout it leaves them to Android
     */
    static void install(Application app) {
        app.registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityResumed(Activity a) { hook(a.getWindow().getDecorView()); }
            @Override public void onActivityCreated(Activity a, Bundle b) {}
            @Override public void onActivityStarted(Activity a) {}
            @Override public void onActivityPaused(Activity a) {}
            @Override public void onActivityStopped(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
            @Override public void onActivityDestroyed(Activity a) {}
        });
    }

    private static void hook(View root) {
        if (root == null || !hooked.add(root)) return;
        root.addOnUnhandledKeyEventListener(PadNav::unhandled);
        // A window over this one took the focus (a dialog, a dialog from a dialog): it is among the process's windows by then
        root.getViewTreeObserver().addOnWindowFocusChangeListener(focus -> { if (!focus) root.post(PadNav::scan); });
    }

    private static void scan() {
        for (View v : android.view.inspector.WindowInspector.getGlobalWindowViews()) hook(v);
    }

    private static boolean unhandled(View root, KeyEvent e) {
        if (!gamepad(e)) return false;
        int k = e.getKeyCode();
        if (k != KeyEvent.KEYCODE_BUTTON_A && k != KeyEvent.KEYCODE_BUTTON_B) return false;
        if (!nintendo(root.getContext())) return false;   // Android's own fallbacks: the bottom button presses, the right one goes back
        root.dispatchKeyEvent(as(e, label(root.getContext(), e) == KeyEvent.KEYCODE_BUTTON_A ? KeyEvent.KEYCODE_DPAD_CENTER : KeyEvent.KEYCODE_BACK));
        return true;
    }
}
