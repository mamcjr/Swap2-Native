package dev.nxnative.android;

import java.text.Collator;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.function.Function;

/**
 * The order of Home's library (its Sort control, MainActivity.sortOrder): by name (the title the card shows), by release date (the profile's release row,
 * Profile.release) or by when the game last started (Game.playedTimes), each ascending or descending. A game with no value for the key (no profile or no
 * release row; never started since the app recorded it) comes after every game with one, in both directions; ties, and the games without a value among
 * themselves, go by name A to Z. No choice (null) keeps the order the games were added in. No Android classes: LibraryOrderJvmTest runs it on the JVM.
 */
final class LibraryOrder {
    enum Key { NAME, RELEASE, PLAYED }

    final Key key;
    final boolean descending;

    private LibraryOrder(Key key, boolean descending) { this.key = key; this.descending = descending; }

    /** Every order, in the Sort dialog's order: each key ascending, then descending */
    static final LibraryOrder[] ALL = {
            new LibraryOrder(Key.NAME, false), new LibraryOrder(Key.NAME, true),
            new LibraryOrder(Key.RELEASE, false), new LibraryOrder(Key.RELEASE, true),
            new LibraryOrder(Key.PLAYED, false), new LibraryOrder(Key.PLAYED, true)};

    /** How the order is remembered (the "library" preferences' sort): name_asc, name_desc, release_asc, … */
    String id() { return key.name().toLowerCase(Locale.ROOT) + (descending ? "_desc" : "_asc"); }

    /** The order a remembered id names, null for none or an unknown one (the order added) */
    static LibraryOrder of(String id) {
        if (id != null) for (LibraryOrder o : ALL) if (o.id().equals(id)) return o;
        return null;
    }

    /**
     * Sorts the list in place (stable). name: the title shown; release: "YYYY-MM-DD" or "" / null; played: the last start in milliseconds, or null / 0 when
     * there is none
     */
    <T> void sort(List<T> list, Function<T, String> name, Function<T, String> release, Function<T, Long> played) {
        Collator c = Collator.getInstance();
        c.setStrength(Collator.SECONDARY);   // case does not count, accents do (after the letters)
        Comparator<T> byName = (a, b) -> c.compare(String.valueOf(name.apply(a)), String.valueOf(name.apply(b)));
        Comparator<T> cmp;
        switch (key) {
            case RELEASE: cmp = lastWhenMissing(t -> { String r = release.apply(t); return r == null || r.isEmpty() ? null : r; }, Comparator.<String>naturalOrder(), byName); break;
            case PLAYED: cmp = lastWhenMissing(t -> { Long p = played.apply(t); return p == null || p <= 0 ? null : p; }, Comparator.<Long>naturalOrder(), byName); break;
            default: cmp = descending ? byName.reversed() : byName; break;
        }
        list.sort(cmp);
    }

    /** By the value, in this order's direction; a missing value (null) after every present one; then by name A to Z */
    private <T, V> Comparator<T> lastWhenMissing(Function<T, V> value, Comparator<V> natural, Comparator<T> byName) {
        Comparator<V> dir = descending ? natural.reversed() : natural;
        return (a, b) -> {
            V va = value.apply(a), vb = value.apply(b);
            if (va == null || vb == null) {
                if (va != vb) return va == null ? 1 : -1;
                return byName.compare(a, b);
            }
            int r = dir.compare(va, vb);
            return r != 0 ? r : byName.compare(a, b);
        };
    }
}
