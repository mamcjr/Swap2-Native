# Swap2-Native test APK — BananaNative v0.2.5 integration

Built on 2026-11-09 from the current working tree, including Argon's checkpoint at `C:/Users/junio/.gemini/antigravity/brain/a974c89a-31c6-4bab-a412-163749790f8a/checkpoint_banananative_v0.2.5.md` and the integration fixes below. No commit, installation or publication was performed.

## Artifact

- APK: [swap2-native-0.1.1-bn025-test-2026-11-09.apk](../android/app/out/swap2-native-0.1.1-bn025-test-2026-11-09.apk)
- Package: `com.swap2native.test`; version `0.1.1-bn025-test`, code `3`.
- Test signing certificate SHA-256: `811d1eb6b92c294693b161d51a92460b472e26e6611be7b41bbc81e131c9601f`.
- Size: 41,381,774 bytes.
- APK SHA-256: `9f8442e64d7ba5977a0b6cedc54ff42113af1180ef4dfa3f33cfe662f249b800`.
- Native preparer enabled; universal host with `NX_RUNTIME_PROFILE=ON`. No fake preparation engine.
- Shader chain `fdd0dcd96fd9`, Vulkan chain `d274c85fc9f9`, Vulkan ABI `674db0a4ff79`; computed chain versions match the headers.

This is a test-package update, separate from the publicly signed `com.swap2native` application. BananaNative v0.2.5 describes the imported improvements, not the Swap2-Native application version.

## Integration fixes required by the Android build

1. `nvn_dirty.h`: declare `nvn_dirty_block_at_least` as returning `size_t`, matching its implementation. The previous `void` declaration prevented compilation.
2. `vka.h`: include `vka_gkey.h` instead of declaring a second, older pipeline-key layout. Runtime pipeline keys and prewarm serialization now use the same structure; the prewarm fingerprint rejects incompatible older lists.
3. `vka_pso.c`: call the existing `dm_module_dir()` accessor when locating the pack's `vk_prewarm.bin`. The imported code incorrectly declared it as a global pointer.
4. `PackService.java`: obtain the picked NUP descriptor through the implemented `PickedNup.detachedFd`, adopt ownership in a `ParcelFileDescriptor`, and retain it in the process-owned descriptor map. The imported call to `PickedNup.descriptor` referred to a nonexistent method.

No feature was disabled to make the APK compile. The shared key header retains the imported texture-mask fields; this build does not add texture-retyping behavior beyond the checkpoint.

## Validation

- Full Gradle `apk` build succeeded with the native preparation stage and Android ARM64 host. Build log: `output/build/bn025-gradle.log`.
- `apksigner verify --print-certs` passed; package/version confirmed with `aapt dump badging`.
- APK ZIP integrity passed. Native host, launcher, preparer, translator, Python and SPIRV-Tools libraries are present.
- Embedded `shader_split.py`, `nup.py`, `pack_android.py`, Zelda `shader_map.tsv` and Zelda `vk_prewarm.bin` match the working-tree files byte for byte. Verification script: `output/build/verify-bn025-apk.py`.
- Linux Python checkpoint suite: 61 tests ran, 54 passed, 7 skipped because optional tooling/fixtures were unavailable. Suites: BNSH NVN2, NUP, Vulkan prewarm, Android pack builds, log digest, readiness and shader ZIP.
- Additional Windows/JDK run: six readiness/shader-ZIP tests passed. NUP JVM test compiled but failed at the `spec` assertion, which expects Unix `/s/G.nup` paths while Windows `File` normalizes separators. Later assertions in that JVM test were not reached; this is not a complete JVM NUP validation. Log: `output/build/bn025-jvm-tests.log`.
- All five portable C tests passed: `t_android_mapbudget`, `t_android_pwlist`, `t_pack_modules`, `t_android_imgmem`, `t_android_zshare`. The prewarm test validated Zelda's 865 seed records against the shared key layout. Runner: `output/build/verify-bn025-native.py`.
- Requested regression harness attempted for Zelda OOT, DK Bananza, Mario Kart World, Pokopia and Mario Kart 8 Deluxe. It cannot run here: Windows lacks `fcntl`, and Linux fails loading the harness's macOS `/usr/lib/libproc.dylib`. No gameplay regression result is claimed.

## Device validation still required

Test application startup, native preparation from a document-picked `.nup`, reopening the selected NUP after a process restart, and gameplay with the chosen GPU driver. Update/reprepare Zelda's shaders when the app requests it so the new profile inputs can take effect. FPS, scene correctness, driver FD-alias support and long-session stability have not been measured in this build session.
