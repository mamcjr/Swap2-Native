package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Where a dump keeps each version's code and RomFS (after Banana Native's DumpLayout, eden/v0.3.11). Dumps come in three layouts: the base alone (base/, or a
 * .nup's base package: a game whose version is its base, such as Mario Kart World 1.8.0), an update alone that carries the whole ExeFS and RomFS
 * (updates/&lt;v&gt;/, or a .nup with only update/&lt;v&gt;: a "merged" dump), or both. A version is one that has an exefs/main of its own (the base is version
 * 0); its ExeFS and its RomFS are its own when it has them, else the base's (an update overrides the base, part by part). A version folder's RomFS is its
 * romfs/ with files or a romfs.bin image beside it (Pack.romfsIn). No Android classes: the JVM tests run it.
 */
final class DumpLayout {
    private DumpLayout() {}

    /** The versions a dump holds (each with its own exefs/main), lowest first; the base is version 0. A one-version folder (exefs/ at its top) has none: it needs a pack made elsewhere */
    static List<Long> versions(File dump) {
        List<Long> out = new ArrayList<>();
        try {
            if (Nup.isNup(dump)) {
                Nup n = Nup.open(dump);
                for (Nup.Pkg p : n.packages)
                    if ((p.kind == Nup.BASE || p.kind == Nup.UPDATE) && isFile(n, p, "exefs/main")) out.add(p.kind == Nup.BASE ? 0L : p.version);
            } else {
                if (new File(dump, "base/exefs/main").isFile()) out.add(0L);
                File[] ups = new File(dump, "updates").listFiles(File::isDirectory);
                if (ups != null) for (File u : ups) if (u.getName().matches("\\d+") && new File(u, "exefs/main").isFile()) out.add(Long.parseLong(u.getName()));
            }
        } catch (IOException | RuntimeException e) {
            // an unreadable dump holds no version here (DumpCheck says why)
        }
        out.sort(null);
        return out;
    }

    private static boolean isFile(Nup n, Nup.Pkg p, String path) throws IOException {
        Nup.Node x = n.lookup(p, path);
        return x != null && !x.dir;
    }

    /** The newest version a dump holds, -1 when none */
    static long newest(File dump) {
        List<Long> v = versions(dump);
        return v.isEmpty() ? -1 : v.get(v.size() - 1);
    }

    /** A folder dump's folder of a version: updates/&lt;v&gt;, else base/ */
    static File dir(File dump, long v) { return new File(dump, v == 0 ? "base" : "updates/" + v); }

    /**
     * Where a part of a version is, as the tools take it: "exefs[/…]" or "romfs[/…]" of the version itself, else of the base. A folder's path (its romfs/
     * folder or romfs.bin image for "romfs"), or a .nup's &lt;file&gt;.nup:&lt;selector&gt;/&lt;sub&gt; (NUP.md "Naming a package"). For a part neither has,
     * the version's own path (whose absence the caller reports)
     */
    static String path(File dump, long v, String sub) {
        boolean romfs = sub.equals("romfs") || sub.startsWith("romfs/");
        if (Nup.isNup(dump)) return Nup.spec(dump, selector(dump, v, romfs), sub);
        File own = dir(dump, v), base = dir(dump, 0);
        File from = v != 0 && !has(own, romfs, sub) && has(base, romfs, sub) ? base : own;
        if (sub.equals("romfs")) {
            File r = Pack.romfsIn(from);
            if (r != null) return r.getPath();
        }
        return new File(from, sub).getPath();
    }

    private static boolean has(File verDir, boolean romfs, String sub) {
        return romfs ? Pack.romfsIn(verDir) != null : new File(verDir, sub).exists();
    }

    /** The .nup package of a version's code (romfs false) or RomFS: the version's own when it has that part, else the base's */
    static String selector(File nup, long v, boolean romfs) {
        String own = Nup.versionSelector(v);
        if (v == 0) return own;
        try {
            Nup n = Nup.open(nup);
            Nup.Pkg p = n.find(own), b = n.base();
            if (p != null && (romfs ? hasRomfs(n, p) : p.exefsDir != Nup.NONE)) return own;
            if (p != null && b != null && (romfs ? hasRomfs(n, b) : b.exefsDir != Nup.NONE)) return "base";
        } catch (IOException e) {
            // the version's own: reading it reports the error
        }
        return own;
    }

    private static boolean hasRomfs(Nup n, Nup.Pkg p) throws IOException {
        Nup.Node d = n.dir(p.romfsDir);
        return d != null && d.childCount > 0;
    }

    /** Whether a version has a RomFS (its own or the base's) with files */
    static boolean hasRomfs(File dump, long v) {
        try {
            if (Nup.isNup(dump)) {
                Nup n = Nup.open(dump);
                Nup.Pkg p = n.find(selector(dump, v, true));
                return p != null && hasRomfs(n, p);
            }
            return Pack.romfsIn(dir(dump, v)) != null || v != 0 && Pack.romfsIn(dir(dump, 0)) != null;
        } catch (IOException e) {
            return false;
        }
    }

    /** Whether a file of a version (its own, else the base's) exists */
    static boolean exists(File dump, long v, String sub) {
        if (!Nup.isNup(dump)) return new File(path(dump, v, sub)).isFile();
        try {
            Nup n = Nup.open(dump);
            Nup.Pkg p = n.find(selector(dump, v, sub.startsWith("romfs")));
            return p != null && isFile(n, p, sub);
        } catch (IOException e) {
            return false;
        }
    }

    /** Whether a dump is one Prepare can start from: some version with exefs/main, exefs/main.npdm and a RomFS (its own or the base's) */
    static boolean preparable(File dump) {
        for (long v : versions(dump)) if (exists(dump, v, "exefs/main.npdm") && hasRomfs(dump, v)) return true;
        return false;
    }

    /** "the base", "update 458752" */
    static String name(long v) { return v == 0 ? "the base" : "update " + v; }

    /** What a dump holds, for a log line or a message's detail: "the base and update 131072", or that it holds no game code */
    static String describe(File dump) {
        return describe(dump, "no game code (no exefs/main in the base or an update)", "the base", "update %1$s", "%1$s (no RomFS)", "and");
    }

    /**
     * describe in the words given (for the launcher's message, from its string resources; the log keeps the English above): none when it holds no game code, base,
     * update (%1$s: the version), noRomfs (%1$s: a version's name) and the word between the last two names
     */
    static String describe(File dump, String none, String base, String update, String noRomfs, String and) {
        List<Long> vs = versions(dump);
        if (vs.isEmpty()) return none;
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < vs.size(); i++) {
            long v = vs.get(i);
            String n = v == 0 ? base : String.format(java.util.Locale.ROOT, update, v);
            s.append(i == 0 ? "" : i == vs.size() - 1 ? " " + and + " " : ", ").append(hasRomfs(dump, v) ? n : String.format(java.util.Locale.ROOT, noRomfs, n));
        }
        return s.toString();
    }

    /** The program ID of a version's main.npdm (its own, else the base's) */
    static String npdmTitle(File dump, long v) throws IOException {
        if (!Nup.isNup(dump)) return Pack.programId(new File(path(dump, v, "exefs/main.npdm")));
        Nup n = Nup.open(dump);
        String sel = selector(dump, v, false);
        Nup.Pkg p = n.find(sel);
        Nup.Node x = p == null ? null : n.lookup(p, "exefs/main.npdm");
        if (x == null || x.dir) throw new IOException("no exefs/main.npdm");
        return Pack.programId(n.read(x, -1), dump.getName() + ":" + sel + "/exefs/main.npdm");
    }
}
