package dev.nxnative.android;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

final class PackMemoryUpdate {
    static void apply(Context c, Pack pack) throws IOException {
        String path = "memory-policy/" + pack.titleId + ".tsv";
        // User-provided profiles have no built-in policy; their values remain theirs.
        String[] names = c.getAssets().list("memory-policy");
        if (names == null || !java.util.Arrays.asList(names).contains(pack.titleId + ".tsv")) return;
        String policy;
        try (InputStream in = c.getAssets().open(path)) { policy = new String(in.readAllBytes(), StandardCharsets.UTF_8); }
        File file = new File(pack.dir, "game_profile.tsv");
        String old = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        String updated = PackMemoryPolicy.apply(old, policy, pack.builds);
        if (updated.equals(old)) return;
        File backup = new File(pack.dir, "game_profile.before-memory-policy.tsv");
        if (!backup.exists()) Files.copy(file.toPath(), backup.toPath());
        File temp = new File(pack.dir, "game_profile.memory-policy.tmp");
        try {
            Files.write(temp.toPath(), updated.getBytes(StandardCharsets.UTF_8));
            Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } finally { Files.deleteIfExists(temp.toPath()); }
        android.util.Log.i("Swap2Native", "Updated build-gated pack memory policy: " + pack.titleId);
    }
}
