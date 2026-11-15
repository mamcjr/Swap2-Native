package dev.nxnative.android;
public final class PackDestinationJvmTest {
    private static void check(boolean b) { if (!b) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        String id = "040074A01BF12000", current = "com.swap2native";
        String old = "/storage/emulated/0/Android/data/com.swap2native.test/files/packs/" + id;
        check(PackDestination.foreignVariant(old, current, id));
        check(!PackDestination.foreignVariant(old, "com.swap2native.test", id));
        check(!PackDestination.foreignVariant(old, current, "0400486024DEC000"));
        check(!PackDestination.foreignVariant(old + "/../other", current, id));
        check(!PackDestination.foreignVariant("/sdcard/Switch2/MyPack", current, id));
        check(!PackDestination.foreignVariant(old.replace("com.swap2native.test", "some.other.app"), current, id));
        java.nio.file.Path dir = java.nio.file.Files.createTempDirectory("pack-code-check");
        try {
            java.util.List<String> modules = java.util.List.of("main", "sdk");
            check(PackDestination.missingCode(dir.toFile(), modules));
            for (String module : modules) {
                java.nio.file.Files.write(dir.resolve(module + ".patched.bin"), new byte[] {1});
                java.nio.file.Files.writeString(dir.resolve(module + ".android.key"), "key");
            }
            check(!PackDestination.missingCode(dir.toFile(), modules));
            java.nio.file.Files.delete(dir.resolve("sdk.android.key"));
            check(PackDestination.missingCode(dir.toFile(), modules));
        } finally {
            try (java.util.stream.Stream<java.nio.file.Path> files = java.nio.file.Files.list(dir)) { files.forEach(p -> { try { java.nio.file.Files.delete(p); } catch (java.io.IOException e) { throw new RuntimeException(e); } }); }
            java.nio.file.Files.delete(dir);
        }
        System.out.println("PackDestination: cross-variant output detected; current, shared and unrelated packs preserved");
    }
}
