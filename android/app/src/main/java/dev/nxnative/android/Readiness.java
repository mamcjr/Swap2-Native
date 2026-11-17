package dev.nxnative.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The readiness check's decisions (android/README.md "Readiness check"): what this phone and its GPU driver report, against what the host needs for every game
 * ({@link #HOST}, the checks vka_dev.c makes) and what the game's profile asks for (its req rows, dm/games/README.md "Keys"). Each requirement is "required" (the game
 * will not run, or will draw nothing, without it) or "degraded" (it runs, with something missing or at lower detail). The facts are "key=value" lines: the host's
 * nxhost_vk_readiness (vka_readiness.c, on the driver the game would load: whatever it reports, a custom driver's too) and the launcher's own (ram_gib, android.sdk).
 * Nothing here knows a vendor or a game: the numbers come from the driver and the profile. No Android classes: the JVM test runs it on the Mac.
 * The screen (ReadinessActivity) and the start-up check (GameActivity) show these; the screen's pass / warning / fail cards with a fix button, Run again and the shareable
 * report are adapted from the author's earlier readiness check.
 */
final class Readiness {
    enum Status { PASS, WARNING, FAIL, INFO }

    /**
     * The wording a check shows on the screen, in the phone's language (ReadinessActivity.words: the launcher's string resources): key names one of this class's
     * fixed texts, args its values. Null for a key it has no wording for: the English one is shown. The report and the log line always take the English texts.
     */
    interface Words { String get(String key, Object... args); }

    /** The shown text of a fixed text: the English one without words or a wording for key */
    private static String say(Words w, String key, String english, Object... args) {
        if (w == null) return english;
        String s = w.get(key, args);
        return s != null ? s : english;
    }

    /** One requirement: the fact (a key of the facts) must be at least min (a number, a dotted version, or 0x hex). what: "title: why" */
    static final class Req {
        final boolean required;
        final String fact, min, title, why;
        /** The host's own requirements' key for their shown wording (Words "req_&lt;key&gt;", "title: why" as what); null for a profile's rows (their text is the game's data) */
        final String key;

        Req(boolean required, String fact, String min, String what) { this(required, fact, min, what, null); }

        Req(boolean required, String fact, String min, String what, String key) {
            this.required = required;
            this.key = key;
            this.fact = fact;
            this.min = min;
            int c = what.indexOf(": ");
            this.title = c > 0 ? what.substring(0, c) : what;
            this.why = c > 0 ? what.substring(c + 2) : "";
        }
    }

    static final class Check {
        final String title, message, fix, fact;
        final Status status;
        /** What the screen shows in the phone's language (null: title and message, the English the report takes) */
        private final String shownTitle, shownMessage;

        Check(String title, Status status, String message, String fix) { this(title, status, message, fix, null); }

        /** fact: the requirement's fact key (Req.fact) when the check is one, so a caller can tell what failed without matching titles */
        Check(String title, Status status, String message, String fix, String fact) { this(title, status, message, fix, fact, null, null); }

        /** shownTitle, shownMessage: the same in the phone's language, for the screen (null: the English) */
        Check(String title, Status status, String message, String fix, String fact, String shownTitle, String shownMessage) {
            this.fact = fact;
            this.title = title;
            this.status = status;
            this.message = message;
            this.fix = fix;
            this.shownTitle = shownTitle;
            this.shownMessage = shownMessage;
        }

        String shownTitle() { return shownTitle != null ? shownTitle : title; }
        String shownMessage() { return shownMessage != null ? shownMessage : message; }

        @Override public String toString() { return "[" + status + "] " + title + ": " + message; }
    }

    /** What the host needs whatever the game: vka_dev.c stops without the first ones (Vulkan 1.3, the queue, timeline semaphores, dynamic rendering), and without
     *  placed memory maps, update-after-bind texture tables or a swapchain it runs but draws or shows nothing (its [RED] lines say so) */
    static final List<Req> HOST = Collections.unmodifiableList(List.of(
            new Req(true, "android.sdk", "33", "Android 13 or newer: the app is built for it", "android"),
            new Req(true, "vk.instance", "1.3", "Vulkan 1.3 loader: the graphics replay is written for Vulkan 1.3", "vk_instance"),
            new Req(true, "vk.api", "1.3", "Vulkan 1.3 GPU driver: the graphics replay is written for Vulkan 1.3", "vk_api"),
            new Req(true, "queue.graphicsCompute", "1", "A graphics and compute queue: everything is submitted to one such queue", "queue"),
            new Req(true, "feature.timelineSemaphore", "1", "Timeline semaphores: the game's fences are built on them", "timeline"),
            new Req(true, "feature.dynamicRendering", "1", "Dynamic rendering: every render pass is recorded with it", "dynamic_rendering"),
            new Req(true, "ext.VK_KHR_swapchain", "1", "Swapchain: without it no picture reaches the screen", "swapchain"),
            new Req(true, "ext.VK_KHR_map_memory2", "1", "Memory map 2: placed memory maps need it", "map_memory2"),
            new Req(true, "ext.VK_EXT_map_memory_placed", "1", "Placed memory maps: the game's GPU memory is mapped at the game's own addresses; without them nothing is drawn", "placed"),
            new Req(true, "feature.memoryMapPlaced", "1", "Placed memory maps: the game's GPU memory is mapped at the game's own addresses; without them nothing is drawn", "placed"),
            new Req(true, "feature.descriptorBindingSampledImageUpdateAfterBind", "1", "Texture tables (update after bind): the game's texture pools live in them", "tex_update_after_bind"),
            new Req(true, "feature.descriptorBindingPartiallyBound", "1", "Texture tables (partially bound): the game's texture pools live in them", "tex_partially_bound"),
            new Req(true, "feature.runtimeDescriptorArray", "1", "Texture tables (runtime arrays): the game's texture pools live in them", "tex_runtime_arrays")));

    /** A profile's req rows as build.gradle hands them over (Profile.reqs): "level|fact|min|what" rows, ';' between them; a malformed row is skipped */
    static List<Req> parseReqs(String table) {
        List<Req> out = new ArrayList<>();
        if (table == null) return out;
        for (String row : table.split(";")) {
            String[] c = row.split("\\|", -1);
            if (c.length < 4 || !(c[0].equals("required") || c[0].equals("degraded")) || c[1].isEmpty() || c[2].isEmpty()) continue;
            out.add(new Req(c[0].equals("required"), c[1].trim(), c[2].trim(), c[3].trim().isEmpty() ? c[1].trim() : c[3].trim()));
        }
        return out;
    }

    /** "key=value" lines (the first '=' splits) */
    static Map<String, String> facts(String text) {
        Map<String, String> out = new LinkedHashMap<>();
        if (text == null) return out;
        for (String l : text.split("\n")) {
            int eq = l.indexOf('=');
            if (eq > 0) out.put(l.substring(0, eq).trim(), l.substring(eq + 1).trim());
        }
        return out;
    }

    /** a compared with b: numbers, 0x hex, or dotted versions part by part (1.3 &lt; 1.3.204 &lt; 1.4); a value that is no number reads as 0 */
    static int compare(String a, String b) {
        String[] x = a.trim().split("\\."), y = b.trim().split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            double p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }

    private static double num(String s) {
        try {
            return s.startsWith("0x") || s.startsWith("0X") ? Long.parseLong(s.substring(2), 16) : Double.parseDouble(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** merge's reason when this host cannot report the GPU (shown in the phone's language: Words "unchecked_host") */
    static final String UNCHECKED_HOST = "this build's host cannot report the GPU's features";

    /** The facts this game is checked against: the host's Vulkan facts (null: this host cannot report them) and the launcher's */
    static Map<String, String> merge(String vulkan, double ramGib, int sdk) {
        Map<String, String> f = facts(vulkan);
        if (vulkan == null && !f.containsKey("unchecked")) f.put("unchecked", UNCHECKED_HOST);
        f.put("ram_gib", String.format(Locale.ROOT, "%.1f", ramGib));
        f.put("android.sdk", String.valueOf(sdk));
        return f;
    }

    /** Checked: the host's needs, then the game's rows. A degraded row of a fact whose required row failed is left out (one message per missing thing) */
    static List<Check> evaluate(Map<String, String> facts, List<Req> game) { return evaluate(facts, game, null); }

    /** The same, each check also worded for the screen by w (null: English only) */
    static List<Check> evaluate(Map<String, String> facts, List<Req> game, Words w) {
        List<Check> out = new ArrayList<>();
        // error: the driver answered and failed (no Vulkan library, no instance, no GPU): the game cannot run either. unchecked: nobody answered (the check timed out,
        // or the host cannot report): nothing is known, so nothing is called missing; one warning says the GPU was not checked
        String err = facts.get("error"), unchecked = facts.get("unchecked");
        List<Req> all = new ArrayList<>(HOST);
        all.addAll(game);
        List<String> failed = new ArrayList<>(), seen = new ArrayList<>();
        for (Req r : all) {
            String have = facts.get(r.fact);
            boolean vulkan = !r.fact.equals("ram_gib") && !r.fact.startsWith("android.");
            if (have == null && vulkan && (err != null || unchecked != null)) {   // nothing to compare with: said once
                if (!seen.contains("error")) {
                    seen.add("error");
                    String gt = say(w, "gpu_features", "GPU features");
                    if (err != null) {
                        String m = "The GPU driver could not be used (" + err + "): the game needs Vulkan 1.3.";
                        out.add(new Check("GPU features", Status.FAIL, m, "driver", null, gt, say(w, "gpu_error", m, err)));
                    } else {
                        String m = "Not checked (" + unchecked + "): whether this GPU has what the game needs is not known.";
                        String why = unchecked.equals(UNCHECKED_HOST) ? say(w, "unchecked_host", unchecked) : unchecked;
                        out.add(new Check("GPU features", Status.WARNING, m, null, null, gt, say(w, "gpu_unchecked", m, why)));
                    }
                }
                continue;
            }
            if (!r.required && failed.contains(r.fact) || failed.contains("title " + r.title)) continue;
            String key = r.fact + " " + r.min + " " + r.title;
            if (seen.contains(key)) continue;   // the same requirement twice (the host's and a profile's)
            seen.add(key);
            // the shown title and why: the host's own requirements in the phone's language, a profile's rows as the game's data has them
            String what = r.key == null ? null : say(w, "req_" + r.key, null);
            int wc = what == null ? -1 : what.indexOf(": ");
            String st = what == null ? r.title : wc > 0 ? what.substring(0, wc) : what, sw = what == null ? r.why : wc > 0 ? what.substring(wc + 2) : "";
            if ("1".equals(facts.get("memory.fdAlias")) && compare("1", r.min) >= 0 &&
                    (r.fact.equals("ext.VK_EXT_map_memory_placed") || r.fact.equals("feature.memoryMapPlaced") || r.fact.equals("ext.VK_KHR_map_memory2"))) {
                String m = "Guest-address memory is available through a tested exported-memory alias (memory.fdAlias=1).";
                out.add(new Check(r.title, Status.PASS, m, null, null, st, say(w, "alias", m)));
                continue;
            }
            boolean ok = have != null && compare(have, r.min) >= 0;
            String shown = have == null ? "not reported" : "reports " + have;
            String sl = shownLabel(w, r.fact), ss = have == null ? say(w, "not_reported", shown) : say(w, "reports", shown, have);
            if (ok) {
                String m = "Present (" + label(r.fact) + " " + have + ", needs " + r.min + ").";
                out.add(new Check(r.title, Status.PASS, m, null, null, st, say(w, "present", m, sl, have, r.min)));
            } else {
                if (r.required) { failed.add(r.fact); failed.add("title " + r.title); }
                String msg = (r.required ? "This device lacks it; this game needs it. " : "Missing: this game runs without it, with less. ")
                        + (r.why.isEmpty() ? "" : r.why.substring(0, 1).toUpperCase(Locale.ROOT) + r.why.substring(1) + ". ")
                        + "(" + label(r.fact) + " " + shown + ", needs " + r.min + ")";
                String swhy = sw.isEmpty() ? "" : sw.substring(0, 1).toUpperCase(Locale.ROOT) + sw.substring(1) + ". ";
                out.add(new Check(r.title, r.required ? Status.FAIL : Status.WARNING, msg, r.fact.equals("ram_gib") ? "memory" : "driver", r.fact,
                        st, say(w, r.required ? "lacks_required" : "lacks_degraded", msg, swhy, sl, ss, r.min)));
            }
        }
        return out;
    }

    /** The game's status on Android (its readiness.tsv status row): a game in bring-up gets a warning of its own, whatever the phone has */
    static Check status(String status) { return status(status, null); }

    /** The same, worded for the screen by w (null: English only) */
    static Check status(String status, Words w) {
        if (status == null || status.isEmpty() || status.equals("playable")) return null;
        String t = say(w, "status_title", "Status on Android");
        if (status.equals("bring-up")) {
            String m = "This game is still being brought up on Android: it may stop or draw wrongly on any phone, this one included.";
            return new Check("Status on Android", Status.WARNING, m, null, null, t, say(w, "bring_up", m));
        }
        return new Check("Status on Android", Status.INFO, status, null, null, t, null);
    }

    /** Of a game's environment (KEY=VALUE, in start's order), the lines that pick its GPU driver and what its loader reads (vka_driver.c): what the readiness facts must be read on */
    static String[] driverEnv(List<String> env) {
        List<String> out = new ArrayList<>();
        // (the check opens the driver once for the process, so whatever the driver loader reads must be here: DM_QGL writes its settings file under DM_TEMP)
        for (String kv : env) if (kv.startsWith("DM_VK_DRIVER=") || kv.startsWith("DM_VK_HOOKDIR=") || kv.startsWith("DM_QGL=") || kv.startsWith("DM_TEMP=")) out.add(kv);
        return out.toArray(new String[0]);
    }

    /** label in the phone's language (the fact keys stay as they are) */
    private static String shownLabel(Words w, String fact) { return fact.equals("ram_gib") ? say(w, "label_ram", label(fact)) : label(fact); }

    private static String label(String fact) {
        if (fact.equals("ram_gib")) return "memory in GiB:";
        if (fact.equals("android.sdk")) return "Android SDK";
        return fact + ":";
    }

    static boolean any(List<Check> l, Status s) { for (Check c : l) if (c.status == s) return true; return false; }

    static List<Check> of(List<Check> l, Status s) {
        List<Check> out = new ArrayList<>();
        for (Check c : l) if (c.status == s) out.add(c);
        return out;
    }

    /** The GPU and driver in one line: name, driver name and info, driver ID and version, and a custom driver's name */
    static String gpu(Map<String, String> f) {
        if (!f.containsKey("gpu.name")) return "GPU not reported" + (f.containsKey("error") ? " (" + f.get("error") + ")" : f.containsKey("unchecked") ? " (" + f.get("unchecked") + ")" : "");
        StringBuilder s = new StringBuilder(f.get("gpu.name"));
        s.append(", Vulkan ").append(f.getOrDefault("vk.api", "?"));
        s.append("; driver ").append(f.getOrDefault("driver.name", "?"));
        String info = f.getOrDefault("driver.info", "");
        if (!info.isEmpty()) s.append(" (").append(info).append(')');
        s.append(", driver ID ").append(f.getOrDefault("driver.id", "?")).append(", version ").append(f.getOrDefault("driver.version", "?"));
        String custom = f.getOrDefault("driver.custom", "");
        s.append(custom.isEmpty() ? ", the system's driver" : ", custom driver " + custom);
        return s.toString();
    }

    /** The log's one line ("[readiness] ..."): every player's log says what the device has and what the check decided */
    static String summary(String titleId, Map<String, String> f, List<Check> l) {
        StringBuilder s = new StringBuilder("[readiness] ").append(titleId).append(": ");
        s.append(any(l, Status.FAIL) ? "FAIL" : any(l, Status.WARNING) ? "WARN" : "PASS");
        s.append("; ").append(gpu(f)).append("; RAM ").append(f.get("ram_gib")).append(" GiB, Android SDK ").append(f.get("android.sdk"));
        for (Status st : new Status[] {Status.FAIL, Status.WARNING}) {
            List<Check> m = of(l, st);
            if (m.isEmpty()) continue;
            s.append("; ").append(st == Status.FAIL ? "missing (required): " : "missing (degraded): ");
            for (int i = 0; i < m.size(); i++) s.append(i == 0 ? "" : ", ").append(m.get(i).title);
        }
        return s.toString();
    }

    /** The report a player copies or shares: the device, the GPU and driver, the checks, then every fact */
    static String report(String header, Map<String, String> f, List<Check> l) {
        StringBuilder s = new StringBuilder(header).append('\n');
        s.append("GPU: ").append(gpu(f)).append('\n');
        s.append("RAM: ").append(f.get("ram_gib")).append(" GiB; Android SDK ").append(f.get("android.sdk")).append('\n');
        for (Check c : l) s.append(c).append('\n');
        s.append("-- facts --\n");
        for (Map.Entry<String, String> e : f.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        return s.toString();
    }
}
