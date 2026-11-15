package dev.nxnative.android;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.MediaStore;

import java.io.File;

/**
 * A Storage Access Framework tree (or document) URI → the /storage path the host can open (All files access lets it read there). The document ID is read by
 * Frontend.documentIdPath (plain Java, FrontendJvmTest), which knows the phone's storage provider, the Downloads provider's paths and a path as the ID; a
 * MediaStore number (the Downloads provider's msf:&lt;n&gt;, the media provider's document:&lt;n&gt;) is looked up in MediaStore's _data column (Frontend.mediaStoreId).
 */
final class PathUtil {
    private PathUtil() {}

    /** null when the tree is not on a local volume (a cloud provider, say). */
    static String treeToPath(Uri tree) {
        String id;
        try {
            id = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException e) {
            return null;
        }
        return idToPath(tree.getAuthority(), id);
    }

    /** A picked document (ACTION_OPEN_DOCUMENT: a .nup) → its path; null when it is not on a local volume or its provider gives no path (see the other form) */
    static String documentToPath(Uri doc) { return documentToPath(null, doc); }

    /**
     * The same, with MediaStore's numbers resolved through c (null: not): a file picked from Downloads or Recent on Android 10 and later has the ID msf:&lt;n&gt;,
     * whose path is MediaStore's _data (readable with All files access). Last, a provider URI that carries a /storage path (a FileProvider's "root" path) when
     * that file is there
     */
    static String documentToPath(Context c, Uri doc) {
        String id;
        try {
            id = DocumentsContract.getDocumentId(doc);
        } catch (IllegalArgumentException e) {
            id = null;
        }
        String p = id == null ? null : idToPath(doc.getAuthority(), id);
        if (p != null) return p;
        Long n = id == null ? null : Frontend.mediaStoreId(doc.getAuthority(), id);
        if (n != null && c != null) p = mediaStorePath(c, n);
        if (p != null) return p;
        p = Frontend.embeddedPath(doc.getPath());
        return p != null && new File(p).exists() ? p : null;
    }

    private static String idToPath(String authority, String id) {
        return Frontend.documentIdPath(authority, id, Environment.getExternalStorageDirectory().getAbsolutePath(), d -> new File(d).isDirectory(), f -> new File(f).exists());
    }

    /** MediaStore's path of a file by its number, null when it has none or cannot be read */
    private static String mediaStorePath(Context c, long n) {
        try (Cursor q = c.getContentResolver().query(MediaStore.Files.getContentUri("external"), new String[] {MediaStore.MediaColumns.DATA}, MediaStore.MediaColumns._ID + "=?",
                new String[] {Long.toString(n)}, null)) {
            if (q == null || !q.moveToFirst() || q.isNull(0)) return null;
            String p = q.getString(0);
            return p != null && p.startsWith("/") ? p : null;
        } catch (RuntimeException e) {   // SecurityException without All files access, IllegalArgumentException on a provider without the column
            android.util.Log.i("Swap2Native", "MediaStore " + n + ": " + e);
            return null;
        }
    }
}
