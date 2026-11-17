package dev.nxnative.android;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The Ground foliage's levels (Game settings › Ground foliage; a profile with foliage_off rows, Profile.foliageLevels): a RomFS overlay folder per level whose one
 * file is the game's foliage pack (Zelda: Ocarina of Time's Pack/Foliage.pack.zs) with those layers' booleans false in every scene, so the game places none of them.
 * It is made on the phone from the player's own RomFS by dm/tools/gsys_foliage.py in the packer's Python (SceneService, in the :pack process), never shipped with
 * the app. It lives in the app's files folder, overlay/&lt;title ID&gt;/foliage_&lt;level&gt;/, with foliage_&lt;level&gt;.txt beside it naming what it was made
 * from (the keys and the pack's version): GameActivity gives it to the host (DM_ROMFS_OVERLAY, after the Scene resolution's when both are on: they hold other files)
 * only while that still matches. No Android classes: the JVM test runs it.
 */
final class FoliageOverlay {
    private FoliageOverlay() {}

    static File dir(File files, String titleId, String level) { return new File(files, "overlay/" + titleId + "/foliage_" + level); }
    static File stamp(File files, String titleId, String level) { return new File(files, "overlay/" + titleId + "/foliage_" + level + ".txt"); }
    static File log(File files, String titleId, String level) { return new File(files, "overlay/" + titleId + "/foliage_" + level + ".log"); }

    /** What foliage_&lt;level&gt;.txt holds for an overlay made with these inputs */
    static String stampText(String keys, String version) { return "off\t" + keys + "\nversion\t" + version + "\n"; }

    /** Whether the level's overlay is there and was made from these inputs (its folder has files, its stamp matches) */
    static boolean current(File files, String titleId, String level, String keys, String version) {
        String[] l = dir(files, titleId, level).list();
        File s = stamp(files, titleId, level);
        if (l == null || l.length == 0 || !s.isFile()) return false;
        try {
            return new String(Files.readAllBytes(s.toPath()), StandardCharsets.UTF_8).equals(stampText(keys, version));
        } catch (IOException e) {
            return false;
        }
    }

    /** gsys_foliage.py's arguments: the RomFS as the host takes it (DM_ROMFS: a folder or &lt;file&gt;.nup:&lt;selector&gt;/romfs), the keys, the folder to write */
    static String[] argv(String romfs, String keys, File out) { return new String[] {"--romfs", romfs, "--off", keys, "--out", out.getPath()}; }
}
