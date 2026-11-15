package dev.nxnative.android;

/** Shared work folders survive switching app variants; Android/data destinations do not transfer write permission. */
final class PackDestination {
    static boolean missingCode(java.io.File directory, Iterable<String> modules) {
        for (String module : modules)
            if (!new java.io.File(directory, module + ".patched.bin").isFile() || !new java.io.File(directory, module + ".android.key").isFile()) return true;
        return false;
    }
    static boolean foreignVariant(String path, String currentPackage, String titleId) {
        String marker = "/Android/data/";
        int at = path.lastIndexOf(marker);
        if (at < 0) return false;
        String[] parts = path.substring(at + marker.length()).split("/", -1);
        return parts.length == 4 && (parts[0].equals("com.swap2native") || parts[0].startsWith("com.swap2native."))
                && !parts[0].equals(currentPackage) && parts[1].equals("files") && parts[2].equals("packs")
                && titleId.equalsIgnoreCase(parts[3]);
    }
}
