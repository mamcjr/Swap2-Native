package dev.nxnative.android;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * LibraryOrder on the JVM, driven by dm/tools/tests/test_android_library_order.py: {@code order <id>} prints "order " and the sample library's names in that
 * order, ',' between them (the sample: names, release dates and last starts, some missing); {@code ids} prints every order's id.
 */
public final class LibraryOrderJvmTest {
    public static void main(String[] a) {
        if (a[0].equals("ids")) {
            List<String> ids = new ArrayList<>();
            for (LibraryOrder o : LibraryOrder.ALL) ids.add(o.id());
            System.out.println("ids " + String.join(",", ids) + " " + (LibraryOrder.of("nope") == null) + " " + (LibraryOrder.of(null) == null));
            return;
        }
        String[][] sample = {   // name, release, last start
                {"zelda", "2026-11-05", "300"}, {"Mario", "2025-06-05", ""}, {"donkey", "2025-07-17", "100"}, {"Yoshi", "", "200"}, {"unknown", "", ""}, {"apple", "2025-06-05", "100"}};
        List<String[]> l = new ArrayList<>(List.of(sample));
        Map<String[], Long> played = new HashMap<>();
        for (String[] g : sample) if (!g[2].isEmpty()) played.put(g, Long.parseLong(g[2]));
        LibraryOrder.of(a[1]).sort(l, g -> g[0], g -> g[1], played::get);
        List<String> names = new ArrayList<>();
        for (String[] g : l) names.add(g[0]);
        System.out.println("order " + String.join(",", names));
    }
}
