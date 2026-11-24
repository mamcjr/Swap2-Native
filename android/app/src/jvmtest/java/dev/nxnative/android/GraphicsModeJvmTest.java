package dev.nxnative.android;
import java.util.Arrays;
import java.util.List;

/** The Upscaler choice's host lines: the named NDSR modes (not BananaNative 0.2.5's numeric order), the DLSS stand-in for each, FSR 1 at the present, the sharpness steps */
public final class GraphicsModeJvmTest {
    private static void check(boolean b, String what) { if (!b) throw new AssertionError(what); }
    public static void main(String[] args) {
        String[][] want = {   // choice, DM_VK_NDSR, DM_VK_FSR, DM_VK_DLSS
            {"net", "net", "0", "3"}, {"fsr", "fsr", "1", "2"}, {"gsr2", "gsr2", "1", "3"}, {"fsr3", "fsr", "1", "4"}};
        for (String[] w : want) {
            List<String> lines = Arrays.asList(GraphicsMode.lines(w[0], .25f));
            check(lines.contains("DM_VK_NDSR=" + w[1]), w[0] + ": NDSR");
            check(lines.contains("DM_VK_FSR=" + w[2]), w[0] + ": FSR 1 at the present");
            check(lines.contains("DM_VK_DLSS=" + w[3]), w[0] + ": DLSS stand-in");
            check(lines.contains("DM_VK_PRESENT_NEAREST=0"), w[0] + ": no nearest filtering");
            check(lines.contains("DM_VK_DLSS_SHARP=15") && lines.contains("DM_VK_FSR3_SHARP=0.5"), w[0] + ": the default sharpness is the host's");
        }
        List<String> off = Arrays.asList(GraphicsMode.lines("fsr", -1));
        check(off.contains("DM_VK_FSR_SHARP=-1.0") && off.contains("DM_VK_DLSS_SHARP=0") && off.contains("DM_VK_FSR3_SHARP=0.0"), "sharpness off everywhere");
        for (String old : new String[] {"nearest", "bilinear", "unknown"}) check(Arrays.asList(GraphicsMode.lines(old, .25f)).contains("DM_VK_NDSR=net"), old + ": Original");
        check(GraphicsMode.sharpIndex(.3f) == 3 && GraphicsMode.sharpIndex(-5f) == 0, "nearest sharpness step");
        System.out.println("ok: the four upscalers' host lines, sharpness steps, earlier choices read as Original");
    }
}
