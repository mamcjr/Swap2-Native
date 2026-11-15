package dev.nxnative.android;
import java.util.List;
import java.io.IOException;

public final class PackMemoryPolicyJvmTest {
    private static void check(boolean v) { if (!v) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        String build = "e0f3ccfaf50065ae4ea01eb5aeb13b1a";
        String original = "FORMAT\t1\nMEM_MAP_LIMIT\t0\nMEM_RESERVE\t15032385536\nMEM_ALIAS\t4831838208\nCORE_MASK\t63\nMODULE\tmain\nMODULE\tsdk\n";
        String policy = "FORMAT\t1\nBUILD\t" + build + "\nMEM_MAP_LIMIT\t1\nLAYOUT\t15032385536\t4831838208\t18924699648\t8589934592\n";
        List<String> builds = List.of("main=" + build, "sdk=123");
        String updated = PackMemoryPolicy.apply(original, policy, builds);
        check(updated.equals(original.replace("MEM_MAP_LIMIT\t0", "MEM_MAP_LIMIT\t1").replace("15032385536", "18924699648").replace("4831838208", "8589934592")));
        check(PackMemoryPolicy.apply(updated, policy, builds).equals(updated));
        check(PackMemoryPolicy.apply(original, policy, List.of("main=other")).equals(original));
        String custom = original.replace("MEM_ALIAS\t4831838208", "MEM_ALIAS\t123456");
        String result = PackMemoryPolicy.apply(custom, policy, builds);
        check(result.contains("MEM_ALIAS\t123456") && result.contains("MEM_RESERVE\t15032385536") && result.contains("MEM_MAP_LIMIT\t1"));
        check(PackMemoryPolicy.apply(updated, policy.replace("MEM_MAP_LIMIT\t1", "MEM_MAP_LIMIT\t0"), builds).contains("MEM_MAP_LIMIT\t0"));
        for (String invalid : List.of(original + "MEM_MAP_LIMIT\t0\n", original.replace("MEM_MAP_LIMIT\t0\n", ""))) {
            try { PackMemoryPolicy.apply(invalid, policy, builds); throw new AssertionError("Invalid profile accepted"); }
            catch (IOException expected) { }
        }
        System.out.println("PackMemoryPolicyJvmTest: known/unknown builds, legacy/custom layout, idempotence, explicit opt-out and unrelated metadata preservation passed");
    }
}
