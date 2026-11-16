# Swap2-Native 0.1.3-beta: application-pool, alias and allocation-burst fixes

Historical revision: the shared APK path now contains the later [pending-copy lifetime correction](BUILD-0.1.3-beta-TRANSFER-LIFETIME-2026-11-09.md); its current hash and installation result are recorded there.

## Changes

Requested after the [BananaNative comparison](COMPARISON-BANANANATIVE-MEMORY-2026-11-09.md):

1. General `mem_map_limit` default on, preserving explicit off. Existing Horizon budget/error/used-memory semantics remain in the production dispatcher. The measured Ace Attorney Investigations profile exception retains its old behavior explicitly; its budget discrepancy remains open.
2. Zelda's registered update uses reserve `0x468000000`, alias `0x200000000`: 17.625 GiB reserved, 8 GiB alias, 9.125 GiB heap region. Application pool stays 9 GiB. These are address-space values, not immediate physical allocation.
3. Image cache pressure is also checked before new images. Same-frame scans are gated by 16 MiB growth or one second, with the post-prune size as baseline. Two growth bursts in a frame are both handled. Ordinary unused-image, holder and GPU-fence protections remain; live GPU-only content is not discarded.

Built-in policy assets migrate existing pack memory metadata at launch, gated by registered main build IDs. Only `MEM_MAP_LIMIT` and an exact recognized legacy reserve/alias tuple can change; unrelated runtime rows, custom layouts and unknown builds remain intact. A one-time original metadata backup is kept beside the runtime profile; shaders and rewritten module outputs are not rebuilt by this migration.

## Checks

- Profile-default/explicit-opt-out Python suite: three tests passed under WSL. Initial Windows test execution failed on its temporary-directory/subprocess environment; the Linux run passed.
- JVM memory-policy test passed: known/unknown build gating, exact legacy/custom layout, unrelated row preservation, idempotence, explicit opt-out and duplicate/missing-row rejection.
- NDK production image ownership/cache test passed on ARM64 Android, including two same-frame allocation bursts, holds, writeback, GPU completion and GPU-only preservation.
- NDK production memory SVC `--pool-probe` passed on the actual 4 KiB Android page size: exact budget, duplicate maps, failed-map/heap rollback, release/retry and concurrent operations.
- Full production memory SVC suite passed on Android in its 16 KiB test mode. Unchanged default fixture fails its 16 KiB-specific ownership assertion on a 4 KiB phone; no broad 4 KiB full-suite pass is claimed.
- Full native/Java/resources release build passed, log `output/build/013-memory-policy-build.log`. Embedded Zelda policy has `MEM_MAP_LIMIT=1` and expected old/new layout values; APK ZIP CRC check passed.
- Five-game macOS regression remains unavailable on this Windows/Linux host (`libproc.dylib` dependency, documented in the preceding build report). These checks are not a five-game gameplay pass.

## Signing and device outcome

- APK: swap2-native-0.1.3-beta.apk, 41,250,318 bytes.
- SHA-256 `c7615fcfc48cab01b434556974d6e0946dc40d4997622718d94b27018d9cbe16`.
- Original release certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` verified. Installed with `adb install -r` on POCO serial 98d66058, package `com.swap2native`, version 0.1.3-beta/code 4. First-install time remains 2026-11-08 14:11:23; data path unchanged.
- Zelda was launched through the normal launcher path. Its actual installed runtime profile now has `MEM_MAP_LIMIT=1`, `MEM_ALIAS=8589934592`, `MEM_RESERVE=18924699648`, `APP_POOL=9663676416`. The game log confirms an 8 GiB alias region. No manual metadata edit was used: the APK's launch-time migration ran.
- Main/SDK rewritten files and the 1,256,989,808-byte shader library retain their exact sizes/dates (`output/analysis/zelda-pack-before-policy.txt` versus `zelda-pack-after-policy.txt`, no differences). Existing saves directory remains 2,128 KiB before/after installation. No uninstall or data clearing.
- Initial run log: `output/analysis/zelda-memory-20261109/0400486024DEC000-20261109-183554.log`. An early sample reports total PSS 4,139,744 KiB including swap PSS 1,380,870 KiB; this is not a peak, matched comparison or proof of a ten-minute gameplay pass.
- No Git commit or GitHub publication requested.

The session subsequently ended in a native buffer-to-image copy failure. [Follow-up crash analysis and lifetime correction](BUILD-0.1.3-beta-TRANSFER-LIFETIME-2026-11-09.md) records the distinction from the earlier LOW_MEMORY exits. Initial startup and memory-policy migration were verified; sustained gameplay stability was not established.
