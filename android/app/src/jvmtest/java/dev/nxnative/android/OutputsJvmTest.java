package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Outputs on the JVM (ported from Banana Native v0.1.2's test, MIT): a step's outputs written in place in their folder count as complete only once
 * Outputs.finish wrote the marker, and a new run, a stop at any point before the marker, or a delete never leaves them accepted; an older build's split/
 * (split.tmp/ renamed into place, no marker) with every output still counts. {@code <empty folder>}: prints "ok" lines, exits 1 on the first failed check.
 * Run by dm/tools/tests/test_android_outputs.py.
 */
public final class OutputsJvmTest {
    private static final String[] SPLIT = {"pkg/", "shader_map.tsv", "shader_facts.tsv", "shader_pairs.tsv", "pso_pairs.tsv"};

    private static void check(boolean ok, String what) {
        if (!ok) { System.out.println("FAIL " + what); System.exit(1); }
        System.out.println("ok " + what);
    }

    private static void write(File f, String text) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    /** What the split writes at its end: the package folder and the tables */
    private static void tool(File split) throws IOException {
        write(new File(split, "pkg/vs_0.bin"), "vs");
        write(new File(split, "pkg/ps_0.bin"), "ps");
        for (String t : new String[] {"shader_map.tsv", "shader_facts.tsv", "shader_pairs.tsv", "pso_pairs.tsv"}) write(new File(split, t), "# " + t + "\n");
    }

    public static void main(String[] a) throws Exception {
        File root = new File(a[0]), split = new File(root, "split");

        Outputs.begin(split);
        check(split.isDirectory() && !Outputs.complete(split) && !Outputs.complete(split, SPLIT), "begin makes the folder, not complete");
        tool(split);
        check(!Outputs.complete(split) && !Outputs.complete(split, SPLIT), "the tool's outputs alone are not complete (a kill before finish), nor taken for an older build's");
        Outputs.finish(split);
        check(Outputs.complete(split) && Outputs.complete(split, SPLIT) && new File(split, "shader_map.tsv").isFile() && new File(split, "pkg/ps_0.bin").isFile(),
                "finish marks it complete, outputs in place");
        check(!new File(split, Outputs.MARKER + ".tmp").exists(), "no temporary marker left");

        // a rerun of the step: the marker goes first, the old outputs with it, then a stop before finish
        Outputs.begin(split);
        check(!Outputs.complete(split, SPLIT) && split.isDirectory() && !new File(split, "pkg").exists(), "begin on a complete folder empties it, marker first");
        write(new File(split, "shader_map.tsv"), "# cut short");
        check(!Outputs.complete(split, SPLIT), "a stopped rerun is not complete");

        // a temporary marker a kill left behind is not the marker
        write(new File(split, Outputs.MARKER + ".tmp"), "complete\n");
        check(!Outputs.complete(split, SPLIT), "a temporary marker is not complete");
        tool(split);
        Outputs.finish(split);
        check(Outputs.complete(split), "finish over a temporary marker");

        // the job's end and a refused profile check delete it
        Outputs.delete(split);
        check(!split.exists() && !Outputs.complete(split, SPLIT), "delete removes the folder and its marker");

        // an older build's split/ (made by renaming split.tmp/ once complete, no marker): still complete, so an update does not split again
        File old = new File(root, "old-split");
        tool(old);
        check(!Outputs.complete(old) && Outputs.complete(old, SPLIT), "an older build's renamed folder with every output is complete");
        check(!Outputs.complete(old), "without the legacy names, a folder with no marker is not complete");
        new File(old, "pso_pairs.tsv").delete();
        check(!Outputs.complete(old, SPLIT), "an older build's folder missing an output is not complete");
        tool(old);
        // a delete stopped half way: BEGUN went in first, so what is left is never taken for an older build's folder
        write(new File(old, Outputs.BEGUN), "deleting\n");
        check(!Outputs.complete(old, SPLIT), "a folder with BEGUN is not an older build's");
        Outputs.begin(old);   // the next split begins by deleting it
        check(old.isDirectory() && !new File(old, "pkg").exists() && !Outputs.complete(old, SPLIT), "begin over a half deleted folder empties it");
        Outputs.deleteQuietly(old);
        check(!old.exists(), "deleteQuietly removes it");

        // a link is never followed into: its target stays
        File target = new File(root, "target");
        tool(target);
        File link = new File(root, "link");
        boolean linked = true;
        try {
            Files.createSymbolicLink(link.toPath(), target.toPath());
        } catch (IOException | UnsupportedOperationException e) {
            linked = false;   // no symbolic links here (Windows without the privilege)
        }
        if (linked) {
            check(!Outputs.complete(link, SPLIT), "a link is not taken as an older build's folder");
            Outputs.delete(link);
            check(!Files.exists(link.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS) && new File(target, "pkg/vs_0.bin").isFile(), "delete takes the link, not its target");
        }

        // finish then delete of a complete folder
        Outputs.begin(split);
        tool(split);
        Outputs.finish(split);
        Outputs.deleteQuietly(split);
        check(!split.exists(), "deleteQuietly removes a complete folder");
        System.out.println("all ok");
    }
}
