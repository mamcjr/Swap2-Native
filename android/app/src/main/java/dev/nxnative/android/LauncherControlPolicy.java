package dev.nxnative.android;

/** Diagnostic extras are not a public launcher protocol. Frontend launch aliases remain separate. */
final class LauncherControlPolicy {
    static final String[] EXTRAS = {"game", "add_game", "pack", "save", "run", "ondevice_dump", "ondevice_start", "ondevice_pause", "ondevice_pack", "show_when_locked",
            "gpu_driver_zip", "gpu_driver", "pack_title", "pack_export", "pack_import", "shaders_title", "shaders_export", "shaders_import"};
    static boolean blocked(boolean allowed, Iterable<String> keys) {
        if (allowed) return false;
        for (String key : keys) for (String control : EXTRAS) if (control.equals(key)) return true;
        return false;
    }
}
