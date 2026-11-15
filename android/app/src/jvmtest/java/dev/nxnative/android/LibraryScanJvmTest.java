package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;

/**
 * The library's own search and Add game's checks on the JVM, driven by dm/tools/tests/test_android_library_scan.py (javac with LibraryScan, PackFind, Pack,
 * Nup, Zstd, Profile, ShaderLibs, Switch2Layout, Frontend, DumpLayout, GameFlow, PackFacts, DumpCheck, a stub BuildConfig, DKB's profile needing update 458752 with
 * main's build aabbccdd…, and a stub android.os.StatFs). Arguments: the repository, an empty work folder and a Python to run dm/tools/nup_pack.py with.
 * Checks:
 * <ul>
 * <li>LibraryScan: the games of several folders each once; which ones the search checks (not on a card, not refused before with the same size and time, not
 *     removed by the user, not still being copied); the remembered marks (header, the same path's older mark replaced, removal, the bound); a base-only .nup
 *     covered by its game's update .nup in the same folder (not by one elsewhere, of another title, or one that is not usable); the .nup files beside one; a
 *     copy of a game already on a card under another path (copyOnCard)</li>
 * <li>Nup.Incomplete: a .nup cut short (still being copied) is told apart from a damaged one, also through Pack.titleIdOf, and from one cut at FAT32's
 *     largest file (Nup.copying)</li>
 * <li>DumpCheck.identify (Add game's check, the same as Play's): base and update, an update-only "merged" .nup, a base-only .nup and folder (another version,
 *     baseOnly), an update-only .nup without RomFS, another update, another build</li>
 * </ul>
 * Prints "ok &lt;checks&gt;"; exits non-zero on the first failure.
 */
public final class LibraryScanJvmTest {
    static final String DKB = "040074A01BF12000", OTHER = "0100ABCD00010000";
    private static int n;
    private static String python;
    private static File repo;

    static void check(boolean ok, String what) {
        n++;
        if (!ok) { System.out.println("FAIL " + what); System.exit(1); }
    }

    static void write(File f, byte[] b) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), b);
    }

    static void write(File f, String s) throws IOException { write(f, s.getBytes(StandardCharsets.UTF_8)); }

    static byte[] npdm(String id) {
        long v = Long.parseUnsignedLong(id, 16);
        byte[] b = new byte[0x100];
        b[0] = 'M'; b[1] = 'E'; b[2] = 'T'; b[3] = 'A';
        b[0x70] = (byte) 0x80;
        b[0x80] = 'A'; b[0x81] = 'C'; b[0x82] = 'I'; b[0x83] = '0';
        for (int i = 0; i < 8; i++) b[0x90 + i] = (byte) (v >>> (8 * i));
        return b;
    }

    /** An NSO whose build ID starts with the given byte (0xaa: the profile's build, see the stub) */
    static byte[] nso(int first) {
        byte[] b = new byte[0x200];
        b[0] = 'N'; b[1] = 'S'; b[2] = 'O'; b[3] = '0';
        int[] want = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
        for (int i = 0; i < 32; i++) b[0x40 + i] = (byte) (i < want.length ? want[i] : i);
        b[0x40] = (byte) first;
        return b;
    }

    /** One version's folder: exefs/main (build first byte), exefs/main.npdm, and a RomFS file unless romfs is false */
    static void version(File d, String id, int build, boolean romfs) throws IOException {
        write(new File(d, "exefs/main.npdm"), npdm(id));
        write(new File(d, "exefs/main"), nso(build));
        if (romfs) write(new File(d, "romfs/data.bin"), "romfs of " + d.getName());
    }

    static File nup(File src, File out, String only) throws IOException, InterruptedException {
        out.getParentFile().mkdirs();
        List<String> cmd = new ArrayList<>(List.of(python, "-I", new File(repo, "dm/tools/nup_pack.py").getPath(), src.getPath(), out.getPath(), "--raw"));
        if (only != null) { cmd.add("--only"); cmd.add(only); }
        Process p = new ProcessBuilder(cmd).redirectErrorStream(true).start();
        String o = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        if (p.waitFor() != 0 || !out.isFile()) throw new AssertionError("nup_pack.py failed: " + o);
        return out;
    }

    static void copy(File from, File to) throws IOException {
        to.getParentFile().mkdirs();
        Files.copy(from.toPath(), to.toPath(), StandardCopyOption.REPLACE_EXISTING);
    }

    static void old(File f) { check(f.setLastModified(System.currentTimeMillis() - 10 * 60_000L), "set the time of " + f); }

    public static void main(String[] args) throws Exception {
        repo = new File(args[0]);
        File t = new File(args[1]);
        python = args[2];
        File src = new File(t, "src");
        // The dumps: base alone, base + the profile's update, the update alone ("merged"), the update alone without RomFS, another update, another build
        version(new File(src, "base/base"), DKB, 0x10, true);
        version(new File(src, "full/base"), DKB, 0x10, true);
        version(new File(src, "full/updates/458752"), DKB, 0xaa, true);
        version(new File(src, "merged/updates/458752"), DKB, 0xaa, true);
        version(new File(src, "bare/updates/458752"), DKB, 0xaa, false);
        version(new File(src, "otherup/base"), DKB, 0x10, true);
        version(new File(src, "otherup/updates/393216"), DKB, 0xcc, true);   // (another build: a build is known by its modules, so 393216 with the profile's modules would pass)
        version(new File(src, "otherbuild/base"), DKB, 0x10, true);
        version(new File(src, "otherbuild/updates/458752"), DKB, 0xbb, true);
        version(new File(src, "other/base"), OTHER, 0x10, true);
        File made = new File(t, "made");
        File base = nup(new File(src, "base"), new File(made, "base.nup"), null), full = nup(new File(src, "full"), new File(made, "full.nup"), null);
        File merged = nup(new File(src, "merged"), new File(made, "merged.nup"), "update"), bare = nup(new File(src, "bare"), new File(made, "bare.nup"), "update");
        File otherUp = nup(new File(src, "otherup"), new File(made, "otherup.nup"), null), otherBuild = nup(new File(src, "otherbuild"), new File(made, "otherbuild.nup"), null);
        File other = nup(new File(src, "other"), new File(made, "other.nup"), null);

        identify(base, full, merged, bare, otherUp, otherBuild, new File(src, "base"));
        incomplete(full, new File(t, "cut"));
        scan(new File(t, "scan"), base, merged, other);
        marks();
        copies(new File(t, "copies"));
        System.out.println("ok " + n);
    }

    // ---- Add game's check (DumpCheck.identify, the same as Play's) ----

    static void identify(File base, File full, File merged, File bare, File otherUp, File otherBuild, File baseFolder) {
        DumpCheck.Result r = DumpCheck.identify(full);
        check(r.ok() && r.version == 458752 && DKB.equals(r.titleId), "base and the profile's update: ok " + r.error);
        r = DumpCheck.identify(merged);
        check(r.ok() && r.version == 458752, "an update-only .nup with its own ExeFS and RomFS: ok " + r.error);
        check(Pack.isDump(merged), "an update-only merged .nup is a dump Prepare starts from");

        r = DumpCheck.identify(base);
        check(!r.ok() && DumpCheck.BUILD.equals(r.code) && r.baseOnly, "a base-only .nup: another version, the base alone " + r.code + " " + r.baseOnly);
        check(r.error.contains("base.nup has no update.") && r.error.contains("update 458752") && r.error.contains("does not combine"), "its sentence names the file, the update and the pairing: " + r.error);
        check(Pack.isDump(base), "a base-only .nup is a dump by its layout (what Add game used to add, and Play refused)");

        r = DumpCheck.identify(baseFolder);
        check(!r.ok() && DumpCheck.BUILD.equals(r.code) && r.baseOnly && r.error.contains("no update"), "a base-only folder: the same " + r.error);

        r = DumpCheck.identify(bare);
        check(!r.ok() && !r.baseOnly && r.error.contains("bare.nup is an update-only .nup (update/458752) without the game's RomFS"), "an update-only .nup without RomFS: " + r.error);
        check(!Pack.isDump(bare) && !DumpLayout.versions(bare).isEmpty(), "it has game code but is not a dump Prepare starts from");

        r = DumpCheck.identify(otherUp);
        check(!r.ok() && DumpCheck.BUILD.equals(r.code) && !r.baseOnly && r.error.contains("has update 393216") && r.error.contains("update 458752"), "another update: " + r.error);

        r = DumpCheck.identify(otherBuild);
        check(!r.ok() && DumpCheck.BUILD.equals(r.code) && !r.baseOnly && r.error.contains("is build bb"), "another build of the update: " + r.error);
    }

    // ---- Incomplete copies ----

    static void incomplete(File full, File t) throws IOException {
        File cut = new File(t, "cut.nup");
        copy(full, cut);
        try (RandomAccessFile f = new RandomAccessFile(cut, "rw")) { f.setLength(f.length() - 16); }
        IOException e = null;
        try { Pack.titleIdOf(cut); } catch (IOException x) { e = x; }
        check(e != null && Nup.incomplete(e) && e.getMessage().contains("exFAT"), "a .nup cut short: incomplete, through Pack.titleIdOf, with the exFAT hint " + e);
        File tiny = new File(t, "tiny.nup");
        write(tiny, "NUP");
        e = null;
        try { Pack.titleIdOf(tiny); } catch (IOException x) { e = x; }
        check(e != null && Nup.incomplete(e), "a .nup of a few bytes (its copy just started): incomplete " + e);
        File longer = new File(t, "longer.nup");
        copy(full, longer);
        try (RandomAccessFile f = new RandomAccessFile(longer, "rw")) { f.setLength(f.length() + 16); }
        e = null;
        try { Nup.open(longer); } catch (IOException x) { e = x; }
        check(e != null && !Nup.incomplete(e), "a .nup longer than its header says is damaged, not incomplete " + e);
        File junk = new File(t, "junk.nup");
        write(junk, new byte[0x200]);
        e = null;
        try { Nup.open(junk); } catch (IOException x) { e = x; }
        check(e != null && !Nup.incomplete(e), "not a .nup at all: not incomplete " + e);
        // a copy that stopped at FAT32's largest file never grows: said at once (with its exFAT hint), not left for a later look
        check(Nup.copying(1000, 2000) && !Nup.copying(Nup.FAT32_MAX, 6L << 30) && !Nup.copying(2000, 2000) && !Nup.copying(3000, 2000), "copying: shorter, not cut at 4 GiB - 1");
    }

    /** The same game already on a card under another path (a .nup picked in place: PickedNup's stand-in; a copy elsewhere): the search does not add it twice */
    static void copies(File t) throws IOException {
        File games = new File(t, "Switch2/Games"), a = new File(games, "Zelda.nup"), elsewhere = new File(t, "Download/Zelda.nup");
        write(a, "x");
        write(elsewhere, "x");
        String standIn = new File(t, "app/picked/k1/Zelda.nup").getPath();   // no file there: alive by PickedNup.is
        List<String> ids = List.of(DKB), dirs = List.of(standIn);
        check(LibraryScan.copyOnCard(ids, dirs, DKB, a, d -> d.equals(standIn)), "a .nup picked in place (stand-in path) of the same title and name: on a card");
        check(!LibraryScan.copyOnCard(ids, dirs, DKB, a, d -> false), "not when that card's game is gone (moved away)");
        check(!LibraryScan.copyOnCard(ids, dirs, OTHER, a, d -> true), "not for another title");
        check(!LibraryScan.copyOnCard(ids, dirs, DKB, new File(games, "Zelda-1.nup"), d -> true), "not for another name");
        check(!LibraryScan.copyOnCard(ids, dirs, null, a, d -> true), "not when the title is unknown");
        check(LibraryScan.copyOnCard(List.of(DKB.toLowerCase(java.util.Locale.ROOT)), List.of(elsewhere.getPath()), DKB, a, d -> new File(d).exists()), "a copy elsewhere that exists");
        check(!LibraryScan.copyOnCard(List.of(DKB), List.of(a.getPath()), DKB, a, d -> true), "its own card is not a copy");
    }

    // ---- The search ----

    static void scan(File t, File base, File merged, File other) throws IOException {
        File s2 = new File(t, "Switch2"), games = new File(s2, "Games"), extra = new File(t, "MyGames");
        File a = new File(games, "A.nup"), b = new File(games, "Zelda/B.nup"), c = new File(s2, "Old.nup"), d = new File(extra, "D.nup"), e = new File(games, "E.nup");
        copy(merged, a);
        copy(merged, b);
        copy(merged, c);
        copy(merged, d);
        copy(merged, e);
        for (File f : new File[] {a, b, c, d, e}) old(f);
        List<File> found = LibraryScan.games(List.of(s2, extra, s2, games, new File(t, "missing")));
        List<String> names = new ArrayList<>();
        for (File f : found) names.add(f.getName());
        check(names.equals(List.of("Old.nup", "A.nup", "E.nup", "B.nup", "D.nup")), "the games of the Switch2 folder (root, Games/, a folder of its own) and the library folder, each once " + names);

        long now = System.currentTimeMillis();
        List<String> cards = List.of(a.getPath());
        Set<String> marks = LibraryScan.parse(LibraryScan.remember("", List.of(LibraryScan.mark(c), LibraryScan.removedMark(d.getPath())), List.of()));
        List<File> fresh = LibraryScan.fresh(found, cards, marks, now);
        check(fresh.equals(List.of(e, b)), "not on a card, not refused before, not removed " + fresh);
        // the refused file copied again (another size): checked again; one still being copied (just changed): left for the next look
        try (RandomAccessFile f = new RandomAccessFile(c, "rw")) { f.setLength(f.length() + 16); }
        old(c);
        check(e.setLastModified(now - 5_000), "touch E");
        fresh = LibraryScan.fresh(found, cards, marks, now);
        check(fresh.equals(List.of(c, b)), "a refused file that changed is checked again; one changed seconds ago waits " + fresh);
        check(LibraryScan.settling(e, now) && !LibraryScan.settling(b, now), "settling");
        File dumpDir = new File(games, "Dump");
        version(new File(dumpDir, "base"), DKB, 0x10, true);
        check(LibraryScan.settling(dumpDir, System.currentTimeMillis()), "a dump folder just written is settling (its base/ is new)");

        // Base and update in two .nup files of one game, in the same folder
        File pair = new File(t, "pair"), pb = new File(pair, "game-base.nup"), pu = new File(pair, "game-update.nup"), po = new File(pair, "other.nup");
        copy(base, pb);
        copy(merged, pu);
        copy(other, po);
        File elsewhere = new File(t, "elsewhere/game-update.nup");
        copy(merged, elsewhere);
        LibraryScan.Item kb = new LibraryScan.Item(pb, DKB, false, true), ku = new LibraryScan.Item(pu, DKB, true, false);
        LibraryScan.Item kx = new LibraryScan.Item(elsewhere, DKB, true, false), ko = new LibraryScan.Item(po, OTHER, true, false);
        check(LibraryScan.cover(kb, List.of(kb, kx, ko, ku)) == ku, "a base-only .nup is covered by its game's usable update .nup beside it");
        check(LibraryScan.cover(kb, List.of(kb, kx, ko)) == null, "not by one in another folder, nor by another title's");
        check(LibraryScan.cover(kb, List.of(kb, new LibraryScan.Item(pu, DKB, false, false))) == null, "not by an update .nup that is not usable alone");
        check(LibraryScan.cover(ku, List.of(kb, ku)) == null, "only a base-only one is covered");
        List<String> sib = new ArrayList<>();
        for (File f : LibraryScan.siblings(pb)) sib.add(f.getName());
        check(sib.equals(List.of("game-update.nup", "other.nup")), "the .nup files beside a .nup " + sib);
        // What Add game's check says of them, which cover() is given (MainActivity.pair)
        check(DumpCheck.identify(pb).baseOnly && DumpCheck.identify(pu).ok(), "the base .nup is base-only, the update .nup usable alone");
    }

    static void marks() {
        String s = LibraryScan.remember(null, List.of("/x/a.nup|10|1", "/x/b.nup|5|1", LibraryScan.removedMark("/x/c.nup")), List.of());
        check(s.startsWith(LibraryScan.HEADER + "\n") && !s.startsWith("/"), "the saved marks start with their header, never a path (the Switch2 move repoints values that are paths) " + s);
        Set<String> m = LibraryScan.parse(s);
        check(m.size() == 3 && !m.contains(LibraryScan.HEADER), "parsed without the header " + m);
        s = LibraryScan.remember(s, List.of("/x/a.nup|20|2"), List.of("/x/b.nup|5|1"));
        m = LibraryScan.parse(s);
        check(m.contains("/x/a.nup|20|2") && !m.contains("/x/a.nup|10|1") && !m.contains("/x/b.nup|5|1") && m.contains(LibraryScan.removedMark("/x/c.nup")),
                "a newer mark of a path replaces the older one; a game added again loses its mark " + m);
        check(LibraryScan.remember(s, List.of(), List.of("/x/a.nup|20|2", LibraryScan.removedMark("/x/c.nup"))).isEmpty(), "no marks: nothing saved");
        List<String> many = new ArrayList<>();
        for (int i = 0; i < LibraryScan.MAX_MARKS + 10; i++) many.add("/x/g" + i + ".nup|1|1");
        m = LibraryScan.parse(LibraryScan.remember(s, many, List.of()));
        check(m.size() == LibraryScan.MAX_MARKS && m.contains("/x/g" + (LibraryScan.MAX_MARKS + 9) + ".nup|1|1") && !m.contains("/x/g0.nup|1|1"), "at most MAX_MARKS, the oldest forgotten " + m.size());
    }
}
