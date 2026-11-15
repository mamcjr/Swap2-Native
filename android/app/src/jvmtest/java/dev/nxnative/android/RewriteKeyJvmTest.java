package dev.nxnative.android;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Stream;

/**
 * RewriteKey on the JVM, driven by dm/tools/tests/test_android_rewritekey.py. One run:
 * <pre>
 * &lt;tree&gt; &lt;module file&gt; &lt;game dir in the tree, ending in /&gt; &lt;module name&gt; &lt;build ID (32 hex) or -&gt; &lt;REWRITER files...&gt;  → one key a line, the current formula's first
 * --rows &lt;file&gt;  → RewriteKey.buildRows of the file, as it is
 * </pre>
 * The tree stands for the APK's tools.zip: every file under it by its relative path. -Dpack.game=&lt;file&gt;: the pack's copy of game.tsv; -Dexefs=1: the module has
 * exefs patches (exefs_patches.py is hashed); -Dsuffix=&lt;text&gt;: what follows the digest (default " target=android").
 */
public final class RewriteKeyJvmTest {
    public static void main(String[] a) throws IOException {
        if (a[0].equals("--rows")) {
            System.out.write(RewriteKey.buildRows(Files.readAllBytes(Paths.get(a[1]))));
            System.out.flush();
            return;
        }
        Path root = Paths.get(a[0]), mod = Paths.get(a[1]);
        Map<String, byte[]> tools = new HashMap<>();
        try (Stream<Path> s = Files.walk(root)) {
            for (Path p : (Iterable<Path>) s::iterator) if (Files.isRegularFile(p)) tools.put(root.relativize(p).toString().replace('\\', '/'), Files.readAllBytes(p));
        }
        String[] rewriter = Arrays.copyOfRange(a, 5, a.length);
        String pg = System.getProperty("pack.game");
        byte[] packGame = pg == null ? null : Files.readAllBytes(Paths.get(pg));
        List<String> keys = RewriteKey.keys(tools, rewriter, "1".equals(System.getProperty("exefs")), a[2], a[3], h -> {
            try (InputStream in = Files.newInputStream(mod)) {
                byte[] b = new byte[1 << 12];
                int k;
                while ((k = in.read(b)) > 0) h.update(b, 0, k);
            }
        }, a[4].equals("-") ? null : a[4], System.getProperty("suffix", " target=android"), packGame);
        for (String k : keys) System.out.println(k);
    }
}
