package dev.nxnative.android;

/** Scaling policy shared by the launcher and live controls. Use this host's named NDSR modes, not upstream's incompatible numeric order. */
final class GraphicsMode {
    static final String[] VALUES = {"nearest", "bilinear", "fsr", "net"};
    static final float[] SHARPS = {-1f, 1f, .5f, .25f, 0f};
    static int index(String mode) {
        for (int i = 0; i < VALUES.length; i++) if (VALUES[i].equals(mode)) return i;
        return 3;
    }
    static String[] lines(String mode, float sharp) {
        mode = VALUES[index(mode)];
        return new String[] {"DM_VK_NDSR=" + mode, "DM_VK_FSR=" + (mode.equals("fsr") ? 1 : 0),
                "DM_VK_FSR_SHARP=" + sharp, "DM_VK_PRESENT_NEAREST=" + (mode.equals("nearest") ? 1 : 0)};
    }
}
