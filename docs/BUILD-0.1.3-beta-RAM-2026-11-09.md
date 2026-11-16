# Swap2-Native 0.1.3-beta: RAM, 540p and preparation recovery

Historical build report: the APK path below now contains the later [application-pool/alias/allocation-burst revision](BUILD-0.1.3-beta-MEMORY-POLICY-2026-11-09.md). Its current hash and installation results are in that report; the hashes below identify preceding builds.

## Installed release

- APK: swap2-native-0.1.3-beta.apk.
- Package `com.swap2native`, version `0.1.3-beta`, code 4, not debuggable.
- SHA-256: `b0a8d026028e8ac68dd2a0285c2165cb0d5eb84730582bfbbda8fb7913abde87` (additional completed-image retirement fix).
- 41,233,586 bytes; original release certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` verified before installation.
- Installed with `adb install -r` on the user's POCO. Main package/data path unchanged; Android still reports first installation at 2026-11-08 14:11:23. No uninstall, app-data clearing, Git commit or GitHub publication.
- Final build log: `output/build/013-reclaim-build.log`; signed with the original release key and installed with `adb install -r` again. Native preparer enabled from the verified local stage. `DMSH_CHAIN=fdd0dcd96fd9` and `DMSH_VK_CHAIN=d274c85fc9f9` unchanged.

## Changes

- [RAM ownership/cache implementation](reference/memory.md#android-image-cache-ownership-under-ram-pressure): recording-thread cleanup for finalized pools, actual serial-owner references rather than forced refcount reset, preservation of presentation/capture/writeback owners, GPU-fenced destruction, pending-retirement accounting and pressure-aware eviction of unused CPU-backed images. GPU-only live content and linear/video images are preserved. Cache pressure checks run at least once per second during slow frames, and below-reserve available memory reduces the cache limit rather than accepting its full existing size.
- 540p (960x540) appended to existing resolution choices. Fixed-scale profiles without that size do not offer a fictitious mapping; Mario Kart World uses its verified 720p mapping when inheriting global 540p. Zelda's scene-palette mechanism remains separate.
- Preparation page uses explicit safe-area positioning instead of asymmetric margins with FrameLayout.CENTER. Observed the corrected landscape screen on the phone: full right edge/logo/buttons visible. Capture: `output/analysis/prepare-fixed-013.png`.
- [Cross-variant preparation recovery](reference/packing.md#shared-preparation-retains-another-app-variants-private-pack-path): a shared DK job pointed to the removed test app's private pack destination. Re-home output, preserve completed shared shaders, rebuild missing module outputs, and complete final packing. The first on-device test exposed missing code outputs after relocation; the final APK includes that additional recovery.

## Validation

- Full native/Java/resources release build and signature verification passed. Package/version and embedded memory-fix diagnostics, 540p and real preparer verified.
- Production texture implementation tested in `t_android_texlife.c`, built with the Android NDK and executed on ARM64 Android without a GPU. Passed serial aliases, external holds, writeback refs, delayed destruction/accounting, queued pool cleanup, same-address pool reuse, pressure eviction, in-flight/recent-image retention and live GPU-output protection.
- `t_android_texlimit`: passed pressure/budget calculations, below-reserve deficit and working-set thrash checks.
- Existing native tests passed: map budget, prewarm (including Zelda's 865 seed records), build-specific modules, image-memory allocator and zero sharing.
- PackDestination JVM tests passed foreign/current/shared/unrelated path checks, wrong title/traversal, absent modules, complete modules and missing SDK key.
- Preparation/output/build suites: eight Linux Python tests passed, one JVM test skipped there for missing JDK. The two output tests then passed with the Windows JDK/UTF-8.
- Five-game regression command attempted for Zelda, DK, Mario Kart World, Pokopia and Mario Kart 8 Deluxe. It cannot run here because it imports macOS `libproc.dylib`; no five-game gameplay pass is claimed.

## Device status and limits

Final APK installed and launcher opened with four library entries. DK preparation completed successfully on the device at 17:32 on 2026-11-09. The persisted job reports `state=done`, an empty error and every stage done. All 22,850 shaders were retained; final packing explicitly recognized the existing library as current. Missing main/SDK outputs were rebuilt in 91.1/26.8 seconds, and the 428.1 MiB pack was finalized under the main application's private pack directory. No game launch was performed as part of this completion check. The foreground had subsequently moved to Android's file picker, so it was left undisturbed.

The first 0.1.3-beta image (SHA-256 `f04d2dbeaeb60a357ff11b1eece0c24e60786d5c0d8074a4da5e06b87db6d218`) still ended in Zelda LOW_MEMORY. A follow-up removes unnecessary future-fence waiting only for already-complete pressure-prune candidates. The updated ARM64 ownership test passes and on-device playback confirms 308 MiB reclaimed with zero pending bytes. However, a sampled Zelda process still reached approximately 7.5 GiB total PSS including 5.2 GiB swap PSS, with severe paging and low frame rate. The full memory problem is not resolved by this additional fix. See the RAM investigation (`ANALISE-ZELDA-RAM-2026-11-09.md`, kept local). No shader-table/driver mesh-support fix was included implicitly.

The follow-up run ended LOW_MEMORY at 17:45:31.873. [BananaNative comparison and next validation](COMPARISON-BANANANATIVE-MEMORY-2026-11-09.md) identify further missing semantics, especially the disabled installed application-pool mapping limit. No additional game launch or driver change followed that termination.
