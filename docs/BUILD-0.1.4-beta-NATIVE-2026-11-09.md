# Swap2-Native 0.1.4-beta: selected AnkaNative/Lichii improvements

## Implemented scope

First host/launcher batch following the source comparison (`ANALISE-LICHII130-ANKANATIVE022-2026-11-09.md`, kept local), not a wholesale renderer import.

- Public launcher diagnostic commands default off, with an explicit setting under Settings > About. Initial launch and onNewIntent use the gate. Frontend launch requests remain separate; mixed requests are blocked. EN/PT strings explain the setting and confirmation. Referrer strings are not trusted as authorization.
- A zero-shared image that solely owns its allocation adopts that same allocation on first write, without a new VkImage/allocation or handle replacement. Shared/dedicated accounting changes under the allocator mutex; genuinely shared allocations retain copy-on-write behavior.
- Entirely empty 64 MiB image blocks are returned when occupied blocks have sufficient free space and a suitable run. One empty block is retained when useful to avoid churn. Partially occupied memory is not compacted/discarded.
- Native draws wait for pending graphics pipelines by default, including with async workers. Render-once shadows/lighting must not disappear due to a pipeline miss. `DM_VK_ASYNC_DISCARD=1` explicitly retains the previous skipping tradeoff.
- Prewarm admission is bounded (two concurrent jobs by default, one on Turnip), checks available memory for every worker, and pauses admission while a demanded pipeline is being waited for. Admitted work can finish; demanded compilation never needs a speculative slot. Async workers publish their identity so a waiting draw can raise priority. This is not Lichii's full IR scheduler or a global cap on every driver call.
- Pipeline-cache saves have a minimum interval (120 s) and quiet period (10 s); forced lifecycle saves still run. Complete-file replacement closes the temporary file once, preserves the destination on failure and fixes the prior possible double fclose/leaked buffer. Pipeline-change counters use atomic increments.
- A low-priority thread logs RSS, resident PSS, swap PSS, available RAM, image/pending retirement, pool/staging bytes, allocator free space and active/queued compiler work every ten seconds. Renderer accounting is separate from PSS. Rollup failure is labeled unavailable. `DM_VK_MEM_CENSUS_S=0` disables collection; frame recording never waits for rollup reads.

Earlier 540p, preparation layout/recovery, pool-budget/8 GiB Zelda layout and pending-transfer lifetime fixes remain.

## Separate follow-ups

GPU texture feedback and raw-pointer page indexing require shader ABI/chain changes and regenerated libraries. File-backed pools, lazy shader-pool placement and render-target allocation sharing across extents need broader address-use/aliasing and real-GPU validation. They are not enabled in this batch. No shader-chain/table change or cache deletion was needed.

## Validation

- Full native/Java/resources release build passed: `output/build/014-native-improvements-build.log`. `com.swap2native`, 0.1.4-beta/code 5, DEBUG false/NX_PLAY true.
- JVM command-policy tests passed all diagnostic keys, opt-in, normal frontend requests and mixed requests. Existing memory-migration JVM tests still pass.
- NDK production allocator test passed on ARM64 Android: sole/shared adoption, unchanged handle/bytes, accounting/final release, redundant empty-block release and retention of one useful block.
- NDK admission test passed six workers: bounded entry, pressure/demand pause, admitted completion and resume.
- NDK production pipeline-wait test passed: the native draw waits for publication and demand priority clears afterward. No actual GPU compilation in this unit test.
- NDK cache-file test passed complete replacement and 128 failed renames without partial publication, destination damage or leftover temporary files.
- Production texture-lifetime/cache regression still passes on ARM64 Android, including pending copies and same-frame allocation bursts.
- Three profile/default tests passed under WSL. Five-game regression attempted for Zelda, DK, MKW, Pokopia and Mario Kart 8 Deluxe; unavailable due to macOS `libproc.dylib`. No five-game gameplay pass claimed.

These checks establish policy behavior, not a fixed FPS gain or resolution of all Zelda memory/rendering issues. Cold-cache waits can increase visible first-use stalls while preserving previously skipped effects.

## Signing and installation

- APK swap2-native-0.1.4-beta.apk, 41,266,702 bytes, SHA-256 `b99e7a8b6459c3cd4d1bf5d2c3707b1a82613e0cd608211f4016cab726edbeb1`.
- Original release certificate `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` verified. Installed with `adb install -r`, public package `com.swap2native`, version code 5. First-install time remains 2026-11-08 14:11:23; saves directory remains 2,128 KiB.
- Prepared Zelda main/SDK binaries and the shader library retain their sizes/dates (`output/analysis/zelda-pack-after-014.txt`, unchanged against the previous snapshot). Installed alias 8 GiB and memory enforcement remain active.
- On-device public-command test: a nonexistent driver ZIP request is rejected before installation, log `Diagnostic launcher command ignored: ADB control is off`. Evidence `output/analysis/014-launcher-command-test.txt`. Frontend game launch remains functional.
- Zelda log `output/analysis/zelda-memory-20261109/0400486024DEC000-20261109-232508.log` confirms sole-allocation adoption (9 MiB), preservation of pending native draws and ten-second memory census with separate resident/swap PSS. This is startup/diagnostic verification, not a long gameplay stability or FPS comparison.
- No commit or GitHub publication requested.
