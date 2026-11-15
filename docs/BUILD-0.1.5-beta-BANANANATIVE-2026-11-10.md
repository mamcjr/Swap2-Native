# Swap2-Native 0.1.5-beta: BananaNative 0.2.8 runtime fixes

## Scope

Adapted from BananaNative `7ea6a966e4be51d5e2ac0be08543ff44c036e447`, retaining the local 0.1.3 transfer lifetime fixes and 0.1.4 memory/compiler improvements. The [comparison](ANALISE-BANANANATIVE-0.2.8-2026-11-10.md) is the pre-implementation audit.

- NVN queue and command-buffer event/sync waits now record real dependencies. The replay parks a consumer's logical queue and lets other queues produce its inputs; sync state is snapshotted at submission. Split/out-of-order work uses per-slot fences and carries completion effects to a covering timeline signal. Present waits for its frame outside the Vulkan queue lock. This targets Zelda lighting order; no universal lighting/FPS claim.
- Images named by generation-valid texture-pool slots remain resident even if shader-side bindless reads are absent from CPU draw tracking. Copied NVN texture objects cannot finalize an original object's image while it remains alive. Descriptor construction takes a temporary hold across unlocked upgrades. Existing transfer marking, retirement accounting and dead-pool CPU-image rejection remain.
- Shader-code pools defer Vulkan placement until an actual GPU data use and can use unlinked file backing in the app cache. Per-game Graphics > RAM saver adds large eligible texture pools (off by default). EN/PT strings explain storage writes and next-launch behavior; resetting game graphics also resets it. No storage backing is enabled for texture pools without a choice.
- AV1 uses the recoverable MediaCodec/dav1d backend through the existing nn::codec adapter. Hardware-init refusal and midstream failure fall back to software with bounded key-frame packet replay, timestamps and duplicate-picture suppression. H.264 retains its path. Vendored dav1d 1.5.4 and MIT upstream/Anka notices are retained.
- Existing localized storage management already lists categories/games, requires confirmations, protects active operations and keeps save deletion separate. It was retained instead of replacing it with upstream's English-only interface. Unlinked RAM-saver files disappear when their process closes.
- Default-off diagnostic launcher commands remain as implemented in 0.1.4.

No shader-chain/ABI or profile shader-table change, no re-preparation or cache deletion, no commit or publication. Purple-glow shader work remains separate. Dead-pool GPU-image takeover is not newly enabled: the local conservative rejection remains.

## Validation

- ARM64 NDK tests on the connected POCO: `t_android_qsched`, `t_android_wait_records`, `t_android_texown`, `t_android_fileback`, extended production `t_android_texlife`, `t_android_zero_adopt`, `t_android_compile_gate`, `t_android_pending_pipeline`, `t_android_cachewrite`, `t_android_av` passed.
- Scheduler coverage includes consumer-before-producer, queue ordering, event predicates/wrapping, queued event writes, sync snapshots and reuse, nested command lists, timeline signals, completion order and inherited safety valves. Production record-walker coverage confirms two waits around a same-list fence retain their distinct submitted values after later re-fencing.
- Production texture tests cover live-slot residency, overwrite/unregistration, dead pool generation, actual init/release of copied objects, alias holds, pending uploads, writeback, same-frame pressure and fence-aware retirement. Unit tests use mock Vulkan calls, not a real GPU scene.
- Synthetic 24-frame 640x360 AV1 clip generated with ffmpeg testsrc2/libaom, key interval 12. On the actual Qualcomm hardware decoder, `DM_AV1_MC_FAIL=8` switches to dav1d. All 24 frames match software-only output in order, timestamp and luma hash; EOS/reset/free pass. Evidence: `output/build/015-av1-{software,recovery}-test.log`. The standalone test starts Binder callbacks, as an APK already does. Earlier runs without them stalled; a 96x64 clip was refused by hardware and tested initialization fallback rather than midstream recovery.
- Full Java/resources/native APK build passed (`output/build/015-banananative-build.log`). Public package `com.swap2native`, version 0.1.5-beta/code 6, NX_PLAY true.
- Five-game regression attempted for Zelda, DK, MKW, Pokopia and MK8 Deluxe. The harness cannot run on this Windows/WSL setup because it imports macOS `libproc.dylib`; no five-game gameplay pass.
- Extra ASan/UBSan host tests could not link because the installed WSL Clang lacks sanitizer runtime archives. Native on-device tests above passed; do not mark sanitizer validation passed.

## Integration issue found and corrected

The first signed build installed but aborted while loading `libnxhost.so`, before game logging. Symbolization identified `nvn_nops_register`: upstream added a no-op registration for a workload-priority function that already has a local implementation. Removed the unrelated imported no-op additions, preserving the local function. A new `t_android_host_load` loads the actual built library and runs its registry constructors before a game/save is opened. The corrected built library passes `t_android_host_load` on ARM64 Android: 743 NVN names, 455 implemented (443 semantic + 12 empty), constructors initialized successfully. Evidence: `output/build/015-host-load-test.log`. The corrected release was signed again and installed successfully; see device checks below.

## Installation and device checks

The original release signing certificate is required: SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1`. Update uses `adb install -r`, never uninstall or the test package. Before/after package metadata, save-file hashes and Zelda pack listings are retained in `output/analysis/banananative-v0.2.8/`. The first update preserved every save-file hash and the first-install time. Final corrected artifact: `android/app/out/swap2-native-0.1.5-beta.apk`, 41,909,774 bytes, SHA-256 `1bdd42e97c79962d2c8fcae7e607bfdb974053a6d3360ead933253281f204565`. Original release certificate verified, `adb install -r` successful, package `com.swap2native`, version 0.1.5-beta/code 6. First-install time remains `2026-11-08 14:11:23`, data directory `/data/user/0/com.swap2native`. Every pre-install save-file hash matches after the final replacement. Prepared Zelda pack listing also remains unchanged.

Real-device Zelda run: `0400486024DEC000-20261110-072448.log` (copied as `output/analysis/banananative-v0.2.8/zelda-015.log`). The game renders its opening, reports hardware AV1 around 60 pictures/s for that movie, proceeds through loading and reaches Link's house with textured walls/floor/objects and visible scene lighting. Capture `zelda-015-scene.png` shows the scene at 26.9 FPS; this is one screenshot, not an FPS comparison or a stability benchmark. Left running in this scene for the user's test.

Runtime confirmation: at present 300, 997 waits parked whole and 997 resumed after their producer, with no forced early releases; later house frames also show producer-driven releases. Shader file backing reports approximately 331 MiB after opening and 0 failures in the recorded moves. Only shader pools are file-backed by default; texture RAM saver was not enabled for this test. The initial loader conflict is absent in the corrected APK.

Memory still needs care: later scene samples show roughly 2.0 GiB resident PSS and 4.4 GiB swap PSS, with image accounting around 3.1–3.3 GiB. These are different quantities and must not be summed as physical use. Available RAM was around 1.1 GiB; this is not proof that low-memory or longer-session failures are resolved. Existing unimplemented shading-rate image/palette calls still log red lines; their local behavior was retained.

## Reproducing the AV1 test

Generate an IVF clip with `ffmpeg -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 24 -c:v libaom-av1 -cpu-used 8 -crf 32 -g 12 -f ivf <clip.ivf>`. Build `t_android_av1_recovery.c` with the Android NDK, runtime-profile/host/dav1d include directories, the Android CMake build's `libdav1d.a`, `-DPLAT_ANDROID=1 -DPLAT_APPLE=0`, section garbage collection and `-lmediandk -ldl -lm -pthread`. Run separate processes as `<test> <clip.ivf> software` and `<test> <clip.ivf> recover`; compare every `picture` line, not just the final aggregate hash.

## Remaining validation and limits

The initial house and opening movie were observed on the device as described above. Confirm window artifacts, outdoor transitions and longer-session memory separately; no baseline comparison or multi-game gameplay pass was performed. Upstream's queue scheduler retains logged compatibility fallbacks at CPU waits, present, finalization, backlog pressure and missing-producer timeout; inline replay does not park queues. File backing adds storage I/O and can trade RAM for load latency; software AV1 can be slower at 1080p. No performance gain is promised without a controlled scene comparison.
