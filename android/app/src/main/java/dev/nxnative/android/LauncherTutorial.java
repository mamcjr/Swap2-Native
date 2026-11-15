package dev.nxnative.android;

import android.app.AlertDialog;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** Replayable local first-game guide. Actions use the normal import and permission flows. */
final class LauncherTutorial {
    private LauncherTutorial() {}

    static void firstRun(MainActivity a) {
        if (a.isFinishing() || a.isDestroyed() || !Game.load(a).isEmpty()) return;
        android.content.SharedPreferences prefs = Prefs.open(a, "tutorial");
        if (prefs.getBoolean("introduced_v1", false)) return;
        prefs.edit().putBoolean("introduced_v1", true).apply();
        show(a);
    }

    static void show(MainActivity a) {
        final int[] titles = {R.string.s2_guide_files_title, R.string.s2_guide_picker_title,
                R.string.s2_guide_prepare_title, R.string.s2_guide_help_title};
        final int[] bodies = {R.string.s2_guide_files, R.string.s2_guide_picker,
                R.string.s2_guide_prepare, R.string.s2_guide_help};
        final int[] step = {0};
        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(a, 20);
        content.setPadding(pad, Ui.dp(a, 8), pad, Ui.dp(a, 12));
        TextView progress = Ui.small(a, "");
        TextView heading = Ui.text(a, "", 20, R.color.bn_text, 700);
        heading.setAccessibilityHeading(true);
        TextView body = Ui.text(a, "", 16, R.color.bn_text, 400);
        content.addView(progress);
        Ui.add(content, heading, 12);
        Ui.add(content, body, 12);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(content);
        AlertDialog dialog = new AlertDialog.Builder(a).setTitle(R.string.s2_guide_title).setView(scroll)
                .setNegativeButton(R.string.s2_guide_later, null)
                .setNeutralButton(R.string.s2_guide_previous, null)
                .setPositiveButton(R.string.s2_guide_next, null).create();
        Runnable render = () -> {
            progress.setText(a.getString(R.string.s2_guide_step, step[0] + 1, titles.length));
            heading.setText(titles[step[0]]);
            body.setText(bodies[step[0]]);
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(step[0] > 0);
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setText(step[0] == titles.length - 1
                    ? R.string.s2_guide_add : R.string.s2_guide_next);
            scroll.scrollTo(0, 0);
            heading.announceForAccessibility(heading.getText());
        };
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> { if (step[0] > 0) step[0]--; render.run(); });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                if (step[0] < titles.length - 1) { step[0]++; render.run(); }
                else { dialog.dismiss(); a.addGame(); }
            });
            render.run();
        });
        dialog.show();
    }

    /** Preserve the actual diagnostic and offer recovery without changing the selected file. */
    static void importProblem(MainActivity a, String detail, Runnable retry) {
        new AlertDialog.Builder(a).setTitle(R.string.s2_guide_import_problem)
                .setMessage(a.getString(R.string.s2_guide_problem_help) + "\n\n" + detail)
                .setPositiveButton(R.string.s2_guide_retry, (d, w) -> retry.run())
                .setNeutralButton(R.string.s2_guide_title, (d, w) -> show(a))
                .setNegativeButton(android.R.string.cancel, null).show();
    }
}
