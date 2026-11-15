package dev.nxnative.android;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * scripts/pack_android.py's rewrite_keys, plain Java (PackCheck.code; checked on the JVM against the Python by dm/tools/tests/test_android_rewritekey.py with
 * RewriteKeyJvmTest): SHA-256 over name\0bytes of the REWRITER files (and exefs_patches.py when the module has exefs patches), the module, the profile's
 * patches.py (when there is one), every other dm/games/&lt;id&gt;/patches.py that names the module's build ID (under its path), and game.tsv's build rows, the only
 * part of game.tsv the rewriter reads (buildRows, hashed under the name ROWS; the rest is the host's, and a change there must not make the code stale).
 * Also the earlier (v1) formula, which hashed the whole game.tsv and no other patches.py, when no other patches.py names the build: with the APK's game.tsv,
 * and with the pack's copy (&lt;pack&gt;/game.tsv) when its build rows are the APK's: the key proves the copy is the file it was made with, and the rows the
 * rewrite read are the same.
 */
final class RewriteKey {
    private RewriteKey() {}

    /** The name the build rows are hashed under (pack_android.py ROWS) */
    static final String ROWS = "game.tsv build rows";

    /**
     * game.tsv's build rows (pack_android.py build_rows; bytes kept as they are): each line (split at \n, one trailing \r dropped) of printable ASCII and tabs whose
     * first cell is "build", its cells stripped of spaces and joined by tabs, one a line; a line holding "build" and any other byte is kept whole, as it is
     * (patch_nso.py's str.splitlines and str.strip break and strip at more characters than these).
     */
    static byte[] buildRows(byte[] tsv) {
        StringBuilder out = new StringBuilder();
        for (String line : new String(tsv, StandardCharsets.ISO_8859_1).split("\n", -1)) {
            if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
            boolean text = true;
            for (int i = 0; i < line.length() && text; i++) {
                char ch = line.charAt(i);
                text = ch == '\t' || ch >= 0x20 && ch < 0x7f;
            }
            if (!text) {
                if (line.contains("build")) out.append(line).append('\n');
                continue;
            }
            String[] c = line.split("\t", -1);
            for (int i = 0; i < c.length; i++) c[i] = strip(c[i]);
            if (c[0].equals("build")) out.append(String.join("\t", c)).append('\n');
        }
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }

    private static String strip(String s) {   // Python's bytes.strip(b' ')
        int a = 0, b = s.length();
        while (a < b && s.charAt(a) == ' ') a++;
        while (b > a && s.charAt(b - 1) == ' ') b--;
        return s.substring(a, b);
    }

    /** Feeds the module's bytes (PackCheck.Mod.hash) */
    interface Module { void hash(MessageDigest h) throws IOException; }

    /**
     * Every key that counts as the current rewriter's, its own first. tools: the APK's tools by path (dm/tools/&lt;file&gt;, dm/games/&lt;id&gt;/patches.py, &lt;gameDir&gt;game.tsv);
     * rewriter: the REWRITER files; exefs: whether exefs_patches.py is hashed (the module has exefs patches); buildId: the module's build ID as 32 lowercase hex
     * digits, or null when it has none; suffix: what follows the digest (" target=android" and, with mods, " mods=&lt;digest&gt;"); packGameTsv: the pack's copy of
     * game.tsv, or null. The module is read once.
     */
    static List<String> keys(Map<String, byte[]> tools, String[] rewriter, boolean exefs, String gameDir, String module, Module nso, String buildId, String suffix,
                             byte[] packGameTsv) throws IOException {
        MessageDigest h = sha256();
        for (String f : rewriter) part(h, f, need(tools, "dm/tools/" + f));
        if (exefs) part(h, "exefs_patches.py", need(tools, "dm/tools/exefs_patches.py"));
        h.update((module + "\0").getBytes(StandardCharsets.UTF_8));
        nso.hash(h);
        byte[] pp = tools.get(gameDir + "patches.py");
        if (pp != null) part(h, "patches.py", pp);
        byte[] gt = need(tools, gameDir + "game.tsv");
        List<MessageDigest> hs = new ArrayList<>();
        MessageDigest cur = copy(h);
        boolean others = false;
        if (buildId != null) {
            byte[] bid = buildId.getBytes(StandardCharsets.ISO_8859_1);
            for (Map.Entry<String, byte[]> e : new TreeMap<>(tools).entrySet()) {   // pack_android.py other_patches: sorted by the path hashed
                String n = e.getKey();
                if (!n.startsWith("dm/games/") || !n.endsWith("/patches.py") || n.equals(gameDir + "patches.py")) continue;
                if (n.indexOf('/', "dm/games/".length()) != n.length() - "/patches.py".length()) continue;   // dm/games/<id>/patches.py only
                if (indexOf(e.getValue(), bid) < 0) continue;
                part(cur, n, e.getValue());
                others = true;
            }
        }
        part(cur, ROWS, buildRows(gt));
        hs.add(cur);
        if (!others) {
            MessageDigest v1 = copy(h);
            part(v1, "game.tsv", gt);
            hs.add(v1);
            if (packGameTsv != null && !Arrays.equals(packGameTsv, gt) && Arrays.equals(buildRows(packGameTsv), buildRows(gt))) {
                MessageDigest v1p = copy(h);
                part(v1p, "game.tsv", packGameTsv);
                hs.add(v1p);
            }
        }
        List<String> out = new ArrayList<>();
        for (MessageDigest d : hs) {
            StringBuilder s = new StringBuilder();
            for (byte b : d.digest()) s.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            out.add(s + suffix);
        }
        return out;
    }

    private static byte[] need(Map<String, byte[]> tools, String path) throws IOException {
        byte[] b = tools.get(path);
        if (b == null) throw new IOException("the APK's tools have no " + path);
        return b;
    }

    private static int indexOf(byte[] hay, byte[] needle) {
        outer:
        for (int i = 0; i + needle.length <= hay.length; i++) {
            for (int j = 0; j < needle.length; j++) if (hay[i + j] != needle[j]) continue outer;
            return i;
        }
        return -1;
    }

    private static MessageDigest sha256() throws IOException {
        try { return MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IOException(e); }
    }

    private static MessageDigest copy(MessageDigest h) throws IOException {
        try { return (MessageDigest) h.clone(); } catch (CloneNotSupportedException e) { throw new IOException(e); }
    }

    private static void part(MessageDigest h, String name, byte[] b) {
        h.update((name + "\0").getBytes(StandardCharsets.UTF_8));
        h.update(b);
    }
}
