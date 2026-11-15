package dev.nxnative.android;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Predicate;

/**
 * The library's own search, with no Android classes (the JVM test, android/app/src/jvmtest's LibraryScanJvmTest). Once All files access is on, the launcher's
 * start and every return to it (MainActivity.libraryScan) make Switch2/Games when it is not there and look for games in the Switch2 folder (its Games/ and, until
 * the move of the older layout, its root: PackFind.gamesIn) and in the library folder last picked; each game that has no card yet goes through Add game's own
 * checks (MainActivity.addAll, quietly: no dialog). A copy still being written (changed less than {@link #SETTLE_MS} ago, or a .nup shorter than its header
 * says: Nup.Incomplete) is left for the next look. A game that was refused (another version, no profile, …) is remembered by its path, size and time
 * ({@link #mark}) so it is not checked again until the file changes; one the user removed from the list is remembered by its path ({@link #removedMark}) so the
 * search does not bring it back (Add game and Refresh still do).
 */
final class LibraryScan {
    /** A game changed less than this long ago may still be being copied (MTP, a file manager, a download): the next look checks it */
    static final long SETTLE_MS = 30_000;
    /** At most this many remembered games (the oldest are forgotten first) */
    static final int MAX_MARKS = 256;

    private LibraryScan() {}

    /** The games of the folders (PackFind.gamesIn each), each once by its path in its primary form (Frontend.normal), in the folders' order; a missing folder holds none */
    static List<File> games(List<File> dirs) {
        Map<String, File> out = new LinkedHashMap<>();
        Set<String> seen = new LinkedHashSet<>();
        for (File d : dirs) {
            if (d == null || !seen.add(Frontend.normal(d.getAbsolutePath()))) continue;
            for (File g : PackFind.gamesIn(d)) out.putIfAbsent(Frontend.normal(g.getAbsolutePath()), g);
        }
        return new ArrayList<>(out.values());
    }

    /** A refused game's mark: its path (primary form), its size (0 for a folder) and its time, so a file copied again or finished is checked again */
    static String mark(File g) {
        return Frontend.normal(g.getAbsolutePath()) + "|" + (g.isFile() ? g.length() : 0) + "|" + g.lastModified();
    }

    /** A removed game's mark: its path alone (removed stays removed while the file is there, whatever its size or time) */
    static String removedMark(String path) { return "-" + Frontend.normal(new File(path).getAbsolutePath()); }

    /** Whether a game was changed so recently that it may still be being written; for a folder, its own time or that of its base/ or updates/ */
    static boolean settling(File g, long now) {
        long t = g.lastModified();
        if (g.isDirectory()) for (String sub : new String[] {"base", "updates", "base/romfs", "base/exefs"}) t = Math.max(t, new File(g, sub).lastModified());
        return t > now - SETTLE_MS && t <= now + SETTLE_MS;
    }

    /**
     * The found games the library's search checks: not on a card (cards: their game paths), not remembered (a refusal of the same file, or removed by the user:
     * marks), and not still being written (settling)
     */
    static List<File> fresh(List<File> found, List<String> cards, Set<String> marks, long now) {
        List<File> out = new ArrayList<>();
        for (File g : found) {
            if (Frontend.card(cards, g.getPath()) >= 0 || Frontend.card(cards, g.getAbsolutePath()) >= 0) continue;
            if (marks.contains(mark(g)) || marks.contains(removedMark(g.getPath()))) continue;
            if (settling(g, now)) continue;
            out.add(g);
        }
        return out;
    }

    /**
     * Whether a found game is already in the list under another path: a card of the same title whose game has the same file or folder name and is still there
     * (alive: a .nup picked in place without All files access, whose card holds PickedNup's stand-in path, or a file that exists). The search does not add such
     * a copy on its own (two cards of one game); Add game and Refresh still do. cardIds and cardDirs: the cards' title IDs and game paths, in the same order
     */
    static boolean copyOnCard(List<String> cardIds, List<String> cardDirs, String titleId, File g, Predicate<String> alive) {
        if (titleId == null) return false;
        for (int i = 0; i < cardIds.size() && i < cardDirs.size(); i++) {
            String dir = cardDirs.get(i);
            if (dir == null || !titleId.equalsIgnoreCase(cardIds.get(i)) || Frontend.same(dir, g.getAbsolutePath())) continue;
            if (new File(dir).getName().equals(g.getName()) && alive.test(dir)) return true;
        }
        return false;
    }

    /**
     * The saved marks' first line: the value never starts with a path, so the Switch2 folder's move (Switch2Migration.repointPrefs, which repoints values that
     * are paths) leaves it alone; a moved game's old mark no longer matches, and it is checked once more
     */
    static final String HEADER = "#library-scan 1";

    /** The remembered marks, one per line (oldest first), as a set */
    static Set<String> parse(String saved) {
        Set<String> out = new LinkedHashSet<>();
        if (saved != null) for (String s : saved.split("\n")) if (!s.isEmpty() && !s.startsWith("#")) out.add(s);
        return out;
    }

    /**
     * The marks with these added (or moved to the newest end) and those removed taken out (a game added again by the user: its removed mark goes), at most
     * MAX_MARKS (the oldest forgotten), one per line. Older marks of the same path with another size or time are dropped: only its current state counts
     */
    static String remember(String saved, Collection<String> add, Collection<String> remove) {
        Set<String> s = parse(saved);
        for (String m : remove) s.remove(m);
        for (String m : add) {
            String path = m.startsWith("-") ? null : m.substring(0, m.indexOf('|') < 0 ? m.length() : m.indexOf('|'));
            if (path != null) s.removeIf(x -> x.startsWith(path + "|"));
            s.remove(m);
            s.add(m);
        }
        List<String> l = new ArrayList<>(s);
        if (l.size() > MAX_MARKS) l = l.subList(l.size() - MAX_MARKS, l.size());
        return l.isEmpty() ? "" : HEADER + "\n" + String.join("\n", l);
    }

    /** One game as Add game's check left it, for {@link #cover} */
    static final class Item {
        final File game;
        final String titleId;
        final boolean usable;     // added as it is: a pack that plays, or a dump Prepare starts from (DumpCheck.identify passed)
        final boolean baseOnly;   // the base game alone, where the game's profile needs an update (DumpCheck.Result.baseOnly)

        Item(File game, String titleId, boolean usable, boolean baseOnly) { this.game = game; this.titleId = titleId; this.usable = usable; this.baseOnly = baseOnly; }
    }

    /**
     * Base and update in two .nup files of one game (the user dumped them apart): a base-only .nup is not needed when an update .nup of the same title in the
     * same folder can be added on its own (an update that carries the whole ExeFS and RomFS: DumpLayout's "merged" dump). → that one, or null. This app does
     * not combine the two files into one game (android/NUP.md, open): an update .nup that needs the base's RomFS is refused with its reason
     */
    static Item cover(Item k, List<Item> all) {
        if (!k.baseOnly || k.titleId == null) return null;
        File dir = k.game.getAbsoluteFile().getParentFile();
        for (Item o : all) {
            if (o == k || !o.usable || !k.titleId.equals(o.titleId) || o.game.getAbsoluteFile().equals(k.game.getAbsoluteFile())) continue;
            File od = o.game.getAbsoluteFile().getParentFile();
            if (dir != null && od != null && Frontend.same(dir.getPath(), od.getPath())) return o;
        }
        return null;
    }

    /** The other .nup files directly beside a .nup (its own folder), sorted by name: where the update .nup of a base-only one would be */
    static List<File> siblings(File nup) {
        File dir = nup.getAbsoluteFile().getParentFile();
        File[] l = dir == null ? null : dir.listFiles(f -> !f.getName().startsWith(".") && Nup.isNup(f) && f.isFile() && !f.getAbsoluteFile().equals(nup.getAbsoluteFile()));
        if (l == null) return new ArrayList<>();
        Arrays.sort(l);
        return new ArrayList<>(Arrays.asList(l));
    }
}
