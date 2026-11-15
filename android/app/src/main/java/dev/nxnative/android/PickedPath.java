package dev.nxnative.android;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * The paths of a .nup used where it is (PickedNup: picked with the system file picker, its read permission kept with takePersistableUriPermission; nothing copied).
 * Every place in the launcher, the packer and the list names a game by a {@link File}: a picked .nup's is {@code <app storage>/picked/<key>/<display name>}, a stand-in
 * path in the app's own folder where no file is written. Its parent is {@code <app storage>/picked/<key>} (created empty), so {@code <stand-in>/../<id>-work} is a
 * writable work folder per picked file in app storage and {@code game.getName()} is the user's file name.
 * <p>
 * Pure Java (no Android classes): NupJvmTest checks it.
 */
final class PickedPath {
    private PickedPath() {}

    /** Where stand-in folders live: {@code <app storage>/picked} */
    static File dir(File appRoot) { return new File(appRoot, "picked"); }

    /** 16 lower-case hex digits of SHA-256(uri): the folder name of one picked document */
    static String key(String uri) {
        try {
            byte[] d = MessageDigest.getInstance("SHA-256").digest(uri.getBytes(StandardCharsets.UTF_8));
            StringBuilder s = new StringBuilder(16);
            for (int i = 0; i < 8; i++) s.append(String.format(Locale.ROOT, "%02x", d[i] & 0xff));
            return s.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        }
    }

    /** The stand-in path for a picked .nup: {@code <app storage>/picked/<key>/<displayName>} */
    static File standIn(File appRoot, String key, String displayName) { return new File(new File(dir(appRoot), key), displayName); }

    /**
     * If {@code f} is a stand-in path under {@code appRoot/picked/<16 hex>/<name>.nup}, that {@code <16 hex>} key; else null. Never touches the filesystem (the
     * stand-in file does not exist).
     */
    static String keyOf(File appRoot, File f) {
        if (appRoot == null || f == null) return null;
        File p = f.getAbsoluteFile().getParentFile();
        if (p == null || !p.getParentFile().equals(dir(appRoot)) || !Nup.hasExt(f.getName())) return null;
        String k = p.getName();
        if (k.length() != 16) return null;
        for (int i = 0; i < 16; i++) {
            char c = k.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return null;
        }
        return k;
    }

    /**
     * Why a picked document's display name cannot be used as a .nup in place, or null when it can (a safe single file name ending in {@code .nup}, with no
     * {@code .nup:} inside it).
     */
    static String nameProblem(String displayName) {
        String n = CopyPlan.safeName(displayName);
        if (n == null || n.equalsIgnoreCase(Nup.EXT) || Nup.lastSplit(n) >= 0) return "That file's name cannot be used.";
        if (Nup.hasExt(n)) return null;
        return n + " is not a .nup file (its name must end in .nup).";
    }

    /**
     * The path the C host opens for a detached file descriptor ({@code nup_image.h nup_open_file}): {@code fd:<fd>/<name>.nup}, which ends in {@code .nup} so
     * {@code nup_split} and {@code hle_fs.c aoc_dir} treat it like any {@code .nup} path, and carries the descriptor number where no file path can.
     */
    static String fdSpec(int fd, String fileName) { return "fd:" + fd + "/" + fileName; }

    /**
     * Rewrites {@code spec} ({@code <standIn>:update/<v>/romfs} from {@code Pack.romfsPath}, or {@code <standIn>} from {@code Pack.aocLinks}) so its file part is
     * {@code fd:<fd>/<name>.nup} when it names {@code standIn}; any other path is returned as it is.
     */
    static String forHost(String spec, File standIn, int fd) {
        if (spec == null || standIn == null || fd < 0) return spec;
        String p = standIn.getAbsolutePath(), f = fdSpec(fd, standIn.getName());
        if (spec.equals(p)) return f;
        if (spec.startsWith(p + ":")) return f + spec.substring(p.length());
        return spec;
    }
}
