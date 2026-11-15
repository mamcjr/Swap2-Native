package dev.nxnative.android;
import java.util.List;
public final class LauncherControlPolicyJvmTest {
    public static void main(String[] args) {
        for (String key : LauncherControlPolicy.EXTRAS) {
            if (!LauncherControlPolicy.blocked(false, List.of(key)) || LauncherControlPolicy.blocked(true, List.of(key))) throw new AssertionError(key);
        }
        if (LauncherControlPolicy.blocked(false, List.of()) || LauncherControlPolicy.blocked(false, List.of("frontend_game"))) throw new AssertionError("Frontend launch blocked");
        if (!LauncherControlPolicy.blocked(false, List.of("frontend_game", "gpu_driver_zip"))) throw new AssertionError("Mixed control request accepted");
        System.out.println("LauncherControlPolicy: diagnostic extras denied by default, opt-in and frontend separation passed");
    }
}
