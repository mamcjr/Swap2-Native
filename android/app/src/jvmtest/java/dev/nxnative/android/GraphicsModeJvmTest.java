package dev.nxnative.android;
import java.util.Arrays;

/** The local host's old 1=FSR convention must not be confused with BananaNative 0.2.5's 1=bilinear convention. */
public final class GraphicsModeJvmTest {
    private static void check(boolean b) { if (!b) throw new AssertionError(); }
    public static void main(String[] args) {
        for (String mode : new String[] {"nearest", "bilinear", "fsr", "net"}) {
            java.util.List<String> lines = Arrays.asList(GraphicsMode.lines(mode, .25f));
            check(lines.contains("DM_VK_NDSR=" + mode));
            check(lines.contains("DM_VK_FSR=" + (mode.equals("fsr") ? 1 : 0)));
            check(lines.contains("DM_VK_PRESENT_NEAREST=" + (mode.equals("nearest") ? 1 : 0)));
        }
        check(Arrays.asList(GraphicsMode.lines("fsr", -1)).contains("DM_VK_FSR_SHARP=-1.0"));
        check(Arrays.asList(GraphicsMode.lines("unknown", .25f)).contains("DM_VK_NDSR=net"));
        System.out.println("ok: scaling choices, named host modes, nearest filtering, sharpness off and invalid preference fallback");
    }
}
