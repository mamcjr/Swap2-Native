package dev.nxnative.android;

import android.app.Application;

/**
 * Installed in every process of the app (the launcher, {@code :game}, {@code :pack}, {@code :readiness}, {@code :selftest}): hooks {@link PickedNup} into
 * {@link Nup#opener} before any component runs, so a {@code .nup} picked in place opens through Android's document provider wherever {@link Nup} is called.
 */
public final class NxApp extends Application {
    @Override public void onCreate() {
        super.onCreate();
        PickedNup.install(this);
        PadNav.install(this);   // the gamepad's A and B in every window, the dialogs' too (the Nintendo layout)
    }
}
