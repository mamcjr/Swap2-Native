package dev.nxnative.android;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/**
 * A {@code .nup} file used where it is, without All files access and without copying it into the app: the user picks it with {@link Intent#ACTION_OPEN_DOCUMENT},
 * the app keeps the provider's read permission across reboots ({@link ContentResolver#takePersistableUriPermission}), and records its document URI under
 * {@code <app storage>/picked/<key>/uri.txt}. The game's {@link File} in the launcher, the packer and the list is the stand-in path
 * {@code <app storage>/picked/<key>/<display name>} ({@link PickedPath}); no bytes of the {@code .nup} are written there.
 * <ul>
 * <li>Java ({@link Nup}) opens it through {@link Nup#opener}, installed in every process of the app ({@link NxApp}): {@code openFileDescriptor(uri, "r")} gives a
 *     seekable {@code FileChannel} over the provider's file</li>
 * <li>The on-device packer ({@code PackService}) detaches one read descriptor before a step and sets {@code DM_NUP_FD=<fd>:<stand-in path>}; {@code dm/tools/nup.py}
 *     reads that path from {@code dup(fd)} with {@code os.pread}</li>
 * <li>The C host ({@code GameActivity}) detaches one read descriptor and passes {@code fd:<fd>/<name>.nup:<selector>/romfs} in {@code DM_ROMFS} (and in
 *     {@code DM_AOC_DIR/<index>.nup}'s symlink target); {@code dm/host/nup_image.h} reads from {@code dup(fd)} with {@code pread}</li>
 * </ul>
 */
final class PickedNup {
    private PickedNup() {}

    /** A provider whose descriptor is a pipe or socket (not a local file the host and the packer can seek in): the caller can offer Copy into the app instead */
    static final class Streamed extends IOException {
        Streamed(String message) { super(message); }
    }

    /** Installs {@link Nup#opener} for this process (every process of the app calls this in {@link NxApp#onCreate}) */
    static void install(Context c) {
        Context app = c.getApplicationContext();
        Nup.opener = new Nup.Opener() {
            @Override public boolean owns(File file) { return is(app, file); }
            @Override public Nup.Source open(File file) throws IOException { return openSource(app, file); }
        };
    }

    static boolean is(Context c, File f) { return PickedPath.keyOf(AppStorage.root(c), f) != null; }

    static boolean is(Context c, String path) { return path != null && is(c, new File(path)); }

    /** The system file picker for one .nup, asking for a read permission the app can keep across reboots */
    static Intent pickIntent() {
        return new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    }

    /**
     * Takes a document the user picked: keeps its read permission, checks its name and that its descriptor is a seekable local file, writes {@code uri.txt} in
     * {@code <app storage>/picked/<key>/}, and returns the stand-in {@link File}. Throws {@link IOException} with a user-facing sentence when it cannot be used
     * where it is ({@link Streamed} when the provider streams the file instead of giving a seekable descriptor).
     */
    static File picked(Context c, Uri uri) throws IOException {
        if (uri == null) throw new IOException(c.getString(R.string.s2l_nup_none_picked));
        ContentResolver cr = c.getContentResolver();
        try {
            cr.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            throw new IOException(c.getString(R.string.s2l_nup_no_grant, String.valueOf(e.getMessage())));
        }
        String name = displayName(cr, uri);
        String bad = PickedPath.nameProblem(name);
        if (bad != null) throw new IOException(bad.equals(PickedPath.BAD_NAME) ? c.getString(R.string.s2l_nup_bad_name) : c.getString(R.string.s2l_nup_not_nup, String.valueOf(CopyPlan.safeName(name))));
        File root = AppStorage.root(c);
        String key = PickedPath.key(uri.toString());
        File standIn = PickedPath.standIn(root, key, name);
        File dir = standIn.getParentFile();
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException(c.getString(R.string.s2l_nup_cannot_create, dir.getPath()));
        Files.write(uriFile(dir).toPath(), (uri + "\n" + name + "\n").getBytes(StandardCharsets.UTF_8));
        // Open it once now: a pipe-backed provider (Streamed) or a file that is not a .nup fails here, before a card is made
        try (Nup.Source s = openSource(c, standIn)) {
            if (s.size < 128) throw new IOException(c.getString(R.string.s2l_nup_too_small, name, String.valueOf(s.size)));
        } catch (IOException e) {
            uriFile(dir).delete();
            dir.delete();
            throw e;
        }
        return standIn;
    }

    private static File uriFile(File keyDir) { return new File(keyDir, "uri.txt"); }

    /** The persisted {@link Uri} for a stand-in path, or null when it has none */
    static Uri uriOf(Context c, File standIn) {
        String key = PickedPath.keyOf(AppStorage.root(c), standIn);
        if (key == null) return null;
        File f = uriFile(new File(PickedPath.dir(AppStorage.root(c)), key));
        try {
            List<String> l = Files.readAllLines(f.toPath(), StandardCharsets.UTF_8);
            return l.isEmpty() || l.get(0).trim().isEmpty() ? null : Uri.parse(l.get(0).trim());
        } catch (IOException e) {
            return null;
        }
    }

    /**
     * Opens the picked .nup for {@link Nup}: a seekable {@code FileChannel}, its byte size and a cache identity that changes when the file does. The returned
     * {@link Nup.Source#close()} closes both the channel and the {@link ParcelFileDescriptor}.
     */
    private static Nup.Source openSource(Context c, File standIn) throws IOException {
        Uri uri = uriOf(c, standIn);
        if (uri == null) throw new IOException(c.getString(R.string.s2l_nup_unlinked, standIn.getName()));
        ParcelFileDescriptor pfd = openPfd(c, uri, standIn.getName());
        boolean ok = false;
        try {
            long size = seekableSize(c, pfd, standIn.getName());
            FileInputStream in = new FileInputStream(pfd.getFileDescriptor());
            String id = uri + "|" + size + "|" + queryLong(c.getContentResolver(), uri, DocumentsContract.Document.COLUMN_LAST_MODIFIED);
            ok = true;
            return new Nup.Source(in.getChannel(), size, id, () -> {
                try { in.close(); } finally { pfd.close(); }
            });
        } finally {
            if (!ok) try { pfd.close(); } catch (IOException ignored) {}
        }
    }

    /**
     * Detaches a read-only file descriptor for the C host ({@code fd:<fd>/<name>.nup}) or the Python packer ({@code DM_NUP_FD=<fd>:<stand-in path>}); the caller
     * closes it with {@code ParcelFileDescriptor. adoptFd(fd).close()} when the step or the process is done. Returns -1 when {@code game} is not a picked .nup.
     */
    static int detachedFd(Context c, File game) throws IOException {
        if (!is(c, game)) return -1;
        Uri uri = uriOf(c, game);
        if (uri == null) throw new IOException(c.getString(R.string.s2l_nup_unlinked, game.getName()));
        ParcelFileDescriptor pfd = openPfd(c, uri, game.getName());
        try {
            seekableSize(c, pfd, game.getName());
            return pfd.detachFd();
        } catch (Throwable t) {
            try { pfd.close(); } catch (IOException ignored) {}
            throw t;
        }
    }

    /** Why a picked .nup cannot be opened now (for {@code MainActivity.gone}); null when it opens and seeks */
    static String problem(Context c, File standIn) {
        try (Nup.Source ignored = openSource(c, standIn)) {
            return null;
        } catch (IOException e) {
            return e.getMessage();
        }
    }

    /**
     * Releases the provider permission and removes {@code uri.txt} when a card is removed and no other card still uses this stand-in path. The work folder beside
     * {@code picked/<key>} is untouched.
     */
    static void forget(Context c, File standIn) {
        if (!is(c, standIn)) return;
        for (Game g : Game.load(c)) if (g.gameDir.equals(standIn.getPath())) return;
        Uri uri = uriOf(c, standIn);
        if (uri != null) {
            try { c.getContentResolver().releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) {}
        }
        File dir = standIn.getParentFile();
        if (dir != null) { uriFile(dir).delete(); dir.delete(); }
    }

    private static ParcelFileDescriptor openPfd(Context c, Uri uri, String name) throws IOException {
        try {
            ParcelFileDescriptor pfd = c.getContentResolver().openFileDescriptor(uri, "r");
            if (pfd == null) throw new IOException(c.getString(R.string.s2l_nup_provider_none, name));
            return pfd;
        } catch (SecurityException | IllegalArgumentException e) {
            throw new IOException(c.getString(R.string.s2l_nup_no_longer_read, name, String.valueOf(e.getMessage())));
        } catch (IOException e) {
            throw new IOException(c.getString(R.string.s2l_nup_cannot_open, name, String.valueOf(e.getMessage())));
        }
    }

    /** Checks that the descriptor is a seekable file (pread at 0 works) and returns its byte size */
    private static long seekableSize(Context c, ParcelFileDescriptor pfd, String name) throws IOException {
        try {
            long end = Os.lseek(pfd.getFileDescriptor(), 0, OsConstants.SEEK_END);
            Os.lseek(pfd.getFileDescriptor(), 0, OsConstants.SEEK_SET);
            if (end > 0) return end;
        } catch (ErrnoException e) {
            throw new Streamed(c.getString(R.string.s2l_nup_streamed, name, Os.strerror(e.errno)));
        }
        long s = pfd.getStatSize();
        if (s > 0) return s;
        throw new Streamed(c.getString(R.string.s2l_nup_no_size, name));
    }

    private static String displayName(ContentResolver cr, Uri uri) {
        try (Cursor cur = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cur != null && cur.moveToFirst() && !cur.isNull(0)) return cur.getString(0);
        } catch (RuntimeException ignored) {}
        String last = uri.getLastPathSegment();
        if (last == null) return null;
        int sl = Math.max(last.lastIndexOf('/'), last.lastIndexOf(':'));
        return sl >= 0 ? last.substring(sl + 1) : last;
    }

    private static long queryLong(ContentResolver cr, Uri uri, String col) {
        try (Cursor cur = cr.query(uri, new String[] {col}, null, null, null)) {
            if (cur != null && cur.moveToFirst() && !cur.isNull(0)) return cur.getLong(0);
        } catch (RuntimeException ignored) {}
        return 0;
    }
}
