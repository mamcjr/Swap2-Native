package dev.nxnative.android;

/**
 * The Upscaler choice (Advanced graphics, the in-game panel), shared by the launcher and live controls: one choice the host applies wherever the game scales its picture.
 * Original keeps the game's own path (agl's NDSR network; for NVN2's DLSS the host's default stand-in, Snapdragon GSR2). FSR 1, SGSR 2 and FSR 3.1 name what runs in the
 * DLSS slot (DM_VK_DLSS 2, 3, 4: vka_exec.c dlss_eval); in the NDSR slot SGSR 2 is DM_VK_NDSR=gsr2 (temporal where the build's camera is known, EASU elsewhere) and FSR 3.1,
 * which needs the game's motion vectors, falls to FSR 1 EASU (DM_VK_NDSR=fsr); a picture smaller than the window gets FSR 1 at the present with any of the three
 * (DM_VK_FSR). Use this host's named NDSR modes, not upstream's numeric order.
 */
final class GraphicsMode {
    static final String[] VALUES = {"net", "fsr", "gsr2", "fsr3"};
    /** Sharpness steps (off, softer, soft, default, strongest) as FSR 1's RCAS stops (DM_VK_FSR_SHARP; below 0 = off), the value the settings keep */
    static final float[] SHARPS = {-1f, 1f, .5f, .25f, 0f};
    /** The same steps for the DLSS stand-ins' RCAS (DM_VK_DLSS_SHARP, 0..100, the host's default 15) and FSR 3.1's (DM_VK_FSR3_SHARP, 0..1, default 0.5) */
    private static final int[] DLSS_SHARPS = {0, 5, 10, 15, 30};
    private static final float[] FSR3_SHARPS = {0f, .2f, .35f, .5f, .8f};

    /** A kept choice's index; the earlier "nearest" and "bilinear" (the present's filters, no upscaler) are Original now */
    static int index(String mode) {
        for (int i = 0; i < VALUES.length; i++) if (VALUES[i].equals(mode)) return i;
        return 0;
    }

    /** The nearest sharpness step of a kept value */
    static int sharpIndex(float sharp) {
        int best = 3;
        for (int i = 0; i < SHARPS.length; i++) if (Math.abs(SHARPS[i] - sharp) < Math.abs(SHARPS[best] - sharp)) best = i;
        return best;
    }

    static String[] lines(String mode, float sharp) {
        int m = index(mode), s = sharpIndex(sharp);
        String ndsr = m == 0 ? "net" : m == 2 ? "gsr2" : "fsr";
        int dlss = m == 1 ? 2 : m == 3 ? 4 : 3;
        return new String[] {"DM_VK_NDSR=" + ndsr, "DM_VK_FSR=" + (m == 0 ? 0 : 1), "DM_VK_FSR_SHARP=" + SHARPS[s], "DM_VK_PRESENT_NEAREST=0",
                "DM_VK_DLSS=" + dlss, "DM_VK_DLSS_SHARP=" + DLSS_SHARPS[s], "DM_VK_FSR3_SHARP=" + FSR3_SHARPS[s]};
    }
}
