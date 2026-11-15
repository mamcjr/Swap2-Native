package dev.nxnative.android;

import java.io.IOException;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Build-gated memory-only metadata updates. Never replaces modules, NPDM facts, shader inputs or code outputs. */
final class PackMemoryPolicy {
    static String apply(String runtime, String policy, List<String> builds) throws IOException {
        List<String> lines = List.of(policy.split("\\R"));
        boolean known = lines.stream().filter(s -> s.startsWith("BUILD\t")).anyMatch(s -> builds.contains("main=" + s.substring(6)));
        if (!known) return runtime;
        Map<String, String> values = new HashMap<>();
        for (String line : runtime.split("\\R")) {
            String[] c = line.split("\t", 2);
            if (c.length == 2 && List.of("MEM_MAP_LIMIT", "MEM_RESERVE", "MEM_ALIAS").contains(c[0]))
                if (values.put(c[0], c[1]) != null) throw new IOException("Duplicate memory-profile row: " + c[0]);
        }
        Map<String, String> replace = new HashMap<>();
        for (String line : lines) {
            String[] c = line.split("\t");
            if (c.length == 2 && c[0].equals("MEM_MAP_LIMIT")) {
                if (!c[1].matches("[01]")) throw new IOException("Invalid application-pool policy");
                replace.put(c[0], c[1]);
            } else if (c.length == 5 && c[0].equals("LAYOUT")) {
                for (int i = 1; i < c.length; i++) if (!c[i].matches("[0-9]+")) throw new IOException("Invalid memory-layout policy");
                if (c[1].equals(values.get("MEM_RESERVE")) && c[2].equals(values.get("MEM_ALIAS"))) {
                    replace.put("MEM_RESERVE", c[3]); replace.put("MEM_ALIAS", c[4]);
                }
            }
        }
        for (String key : replace.keySet()) if (!values.containsKey(key)) throw new IOException("Missing memory-profile row: " + key);
        String updated = runtime;
        for (Map.Entry<String, String> e : replace.entrySet())
            updated = updated.replaceAll("(?m)^" + e.getKey() + "\\t[^\\r\\n]*", e.getKey() + "\t" + e.getValue());
        return updated;
    }
}
