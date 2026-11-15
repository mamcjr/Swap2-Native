# Swap2-Native 0.1.3-beta: pending texture-copy lifetime

## Captured failure

The application-pool/8 GiB alias revision was installed successfully, but the subsequent Zelda session ended with a native failure. Android records `com.swap2native:game` APP CRASH(NATIVE), signal 11, at 18:57:57 on 2026-11-09; the launcher separately ended LOW_MEMORY two seconds later. Do not label the game's native crash itself as an LMK termination.

Evidence:

- `output/analysis/zelda-policy-crash-logcat.txt`: fault `0x70`, `vk-enc`, Qualcomm Vulkan driver; `vkCmdCopyBufferToImage2` -> `vkCmdCopyBufferToImage` -> host offset `0x1794a0`.
- Matching unstripped host BuildId `dd11fbb42c3231a5810a295e159e843760c8b4a7`, addr2line identifies `vka_enc.c:342`, the buffer-to-image copy dispatch (inlined into the encoder loop).
- Full `0400486024DEC000-20261109-183554.log`: CRASH at line 6156, followed by waits for encoder record 6. Last sampled Vulkan totals reach 6,365 MiB (1,967 pools, 4,286 images, 112 scratch), 43,545 maps of 65,530. Memory pressure persists; map-count exhaustion was not demonstrated.
- Configured driver name is Qualcomm Adreno 842.8; the tombstone names `/vendor/lib64/hw/vulkan.adreno.so`, BuildId `cc9b13ef89a33cbcf2d6776c75b23189`. Actual driver selection/loading should be checked separately before attributing behavior to the picker's label.

## Root-cause correction

[CPU texture re-upload lifetime](reference/memory.md#cpu-texture-re-uploads-do-not-refresh-image-lifetime): dirty-tracker uploads did not update the lifetime that pressure pruning uses. Full uploads, partial row uploads, sparse upload parts and sparse refresh now mark use before recording commands. This prevents an otherwise-old image from being reclaimed while a new copy still uses it. The exact image handle at the original driver failure is unavailable, so the lifetime defect is fixed but a matched GPU reproduction remains required to establish whether it explains that particular crash.

The previous application-pool enforcement, 8 GiB alias layout, migration and allocation-burst policy stay in place. GPU-only content retention is unchanged. No shader-chain tools, tables or ABI were changed by this fix.

## Tests

- Updated `t_android_texlife`, compiled with NDK and run on ARM64 Android, passed: transfer-only pending-fence retention, collection only after completion, two same-frame allocation bursts, aliases, holders/writeback, finalized pools and live GPU-content preservation.
- Real partial-upload function tested with mock scratch/Vulkan entry points: verifies the lifetime mark before recording, correct staging bytes and deferred destruction. This is a production-code unit test without a real GPU.
- Native/Java/resources release build passed in 24 seconds; `output/build/013-transfer-lifetime-build.log`.
- Existing five-game macOS regression limitation remains as documented in prior reports. No five-game or long gameplay pass claimed.

## Release

- APK [swap2-native-0.1.3-beta.apk](../android/app/out/swap2-native-0.1.3-beta.apk), 41,250,318 bytes, SHA-256 `fd4b2487543384aafef3dd7b097d3788ed308cbdfef9b14fe447a0bdb4fc1c0c`.
- Original release certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` verified. Installed successfully with `adb install -r` on POCO serial 98d66058, main package `com.swap2native`, version 0.1.3-beta/code 4.
- Data path and first-install time (2026-11-08 14:11:23) unchanged. Main/SDK code files and the shader library retain their sizes/dates (`output/analysis/zelda-pack-after-transfer-fix.txt` versus the pre-policy snapshot: no differences). Installed memory policy remains enabled with an 8 GiB alias region.
- Launcher opened for the user's gameplay test. No matched long gameplay stability pass is claimed for this APK. No uninstall, data clearing, Git commit or GitHub publication.

Subsequently published at the user's request: [v0.1.3-beta](https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.3-beta), with the final APK above, corresponding-source ZIP and checksums. GitHub asset digests match local artifacts. No local source commit or push; the attached source ZIP corresponds to the APK rather than the automatic tag archive.
