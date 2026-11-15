package dev.nxnative.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * A folder of a step's outputs that the tool writes in place, on shared storage (Switch2/Work/&lt;id&gt;/ondevice), and that counts as complete only once its
 * marker file is there (ported from Banana Native v0.1.2's Outputs.java, MIT). The marker is written last, after every output is synced; it is deleted
 * first, before anything else in the folder is deleted or written again. So a kill or a reboot at any point leaves either a folder without the marker
 * (redone) or every output on the disk with it.
 *
 * Why not a temporary folder renamed into place: on Android 11 and later a folder renamed under /storage/emulated goes through MediaProvider
 * (renameForFuse, renameDirectoryUncheckedForFuse), which moves the database row of every indexed file in it in one transaction and answers EPERM when
 * one of them does not move exactly one row; a file's rename falls back for an app with all files access. A device refused the split's split.tmp -> split
 * with EPERM (Banana Native issue #1, HyperOS; which row did not move is not known). Only files are renamed here (the marker), as job.json is.
 *
 * Older builds wrote the split into split.tmp/ and renamed it to split/ only once it was complete and synced, so a split/ with no marker of either kind is
 * such a folder and still counts as complete when it holds the outputs it names (complete(dir, legacy)): an update does not split again. A folder this
 * class writes always carries a marker of one kind: BEGUN from begin() until finish() writes MARKER, and BEGUN again before a delete starts, so a folder
 * half written or half deleted is never taken for an older build's.
 */
final class Outputs {
    static final String MARKER = "complete";
    /** Written by begin() and before a delete: the folder is being written or deleted, and is not an older build's renamed folder */
    static final String BEGUN = "begun";

    private Outputs() {}

    /** Whether the folder's outputs were all written and synced (its marker is there) */
    static boolean complete(File dir) { return new File(dir, MARKER).isFile(); }

    /**
     * complete(dir), or an older build's folder that was renamed into place once complete: no marker of either kind, and every name in legacy there
     * (a name ending in '/' a folder, else a file)
     */
    static boolean complete(File dir, String... legacy) {
        if (complete(dir)) return true;
        if (Files.isSymbolicLink(dir.toPath()) || !dir.isDirectory() || new File(dir, BEGUN).exists() || new File(dir, MARKER + ".tmp").exists()) return false;
        if (legacy.length == 0) return false;
        for (String n : legacy) {
            boolean folder = n.endsWith("/");
            File f = new File(dir, folder ? n.substring(0, n.length() - 1) : n);
            if (folder ? !f.isDirectory() : !f.isFile()) return false;
        }
        return true;
    }

    /** Empties the folder for a new run of its tool (the marker goes first), makes it, and writes BEGUN */
    static void begin(File dir) throws IOException {
        delete(dir);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
        write(new File(dir, BEGUN), "begun\n");
    }

    /** Syncs every output to the disk, then writes the marker: to a temporary name, synced, then renamed (a file's rename) */
    static void finish(File dir) throws IOException {
        syncTree(dir);
        File m = new File(dir, MARKER), tmp = new File(dir, MARKER + ".tmp");
        write(tmp, "complete\n");
        Files.move(tmp.toPath(), m.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    /** Deletes the folder: BEGUN in first, then its marker (an error when it stays), then the rest */
    static void delete(File dir) throws IOException {
        if (Files.isSymbolicLink(dir.toPath())) { Files.delete(dir.toPath()); return; }
        if (!dir.isDirectory()) { if (dir.exists() && !dir.delete()) throw new IOException("cannot delete " + dir); return; }
        write(new File(dir, BEGUN), "deleting\n");
        File m = new File(dir, MARKER);
        if (m.exists() && !m.delete()) throw new IOException("cannot delete " + m);
        deleteTree(dir);
        if (dir.exists()) throw new IOException("cannot delete " + dir);
    }

    /** delete(dir) that never throws (a folder left behind keeps BEGUN when it could be written, so it is not taken as complete) */
    static void deleteQuietly(File dir) {
        try {
            delete(dir);
        } catch (IOException e) {
            deleteTree(dir);
        }
    }

    /** Writes a small file and syncs it */
    private static void write(File f, String text) throws IOException {
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
    }

    /** Syncs every file under f to the disk */
    static void syncTree(File f) throws IOException {
        File[] kids = Files.isSymbolicLink(f.toPath()) ? null : f.listFiles();
        if (kids != null) for (File k : kids) {
            if (k.isDirectory()) syncTree(k);
            else try (java.nio.channels.FileChannel ch = java.nio.channels.FileChannel.open(k.toPath(), java.nio.file.StandardOpenOption.WRITE)) { ch.force(true); }
        }
    }

    /** Deletes a folder and what is in it (a link goes, never what it points to); its marker goes before the rest of it, BEGUN last */
    private static void deleteTree(File f) {
        boolean link = Files.isSymbolicLink(f.toPath());
        if (!link) new File(f, MARKER).delete();
        File[] kids = link ? null : f.listFiles();
        if (kids != null) for (File k : kids) {
            if (k.getName().equals(BEGUN)) continue;
            if (k.isDirectory() && !Files.isSymbolicLink(k.toPath())) deleteTree(k);
            else k.delete();
        }
        if (!link) new File(f, BEGUN).delete();
        f.delete();
    }
}
