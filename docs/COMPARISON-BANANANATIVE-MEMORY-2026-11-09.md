# BananaNative memory comparison: Zelda LOW_MEMORY

## Scope and outcome

Compared Swap2's installed 0.1.3-beta and local source with pinned BananaNative v0.2.5 and v0.2.6. The latter was released on 2026-11-09, commit `c017e7b12b`. Public [release notes](https://git.eden-emu.dev/moceure/BananaNative/releases/tag/v0.2.6) report memory/session fixes, but still classify Zelda as experimental. This is a source comparison with Swap2 device evidence, not a same-phone BananaNative gameplay benchmark.

The strongest missing semantic is the application-pool mapping limit: Swap2's installed Zelda pack explicitly contains `MEM_MAP_LIMIT=0`; BananaNative 0.2.5 explicitly enables it for Zelda, and 0.2.6 enables it by default. Texture-cache changes alone have not resolved Swap2's termination.

Subsequently implemented at the user's request: priorities 1-3 below. The comparison table describes the pre-fix state; [implementation, tests and installation status](BUILD-0.1.3-beta-MEMORY-POLICY-2026-11-09.md) are recorded separately. Gameplay success is not implied by the source changes.

## Device evidence after the additional cache fix

- Original release signature retained; main package `com.swap2native`, version 0.1.3-beta/code 4. Updated APK SHA-256 `b0a8d026028e8ac68dd2a0285c2165cb0d5eb84730582bfbbda8fb7913abde87`.
- Run `output/analysis/zelda-memory-20261109/0400486024DEC000-20261109-174221.log`, same existing save, Qualcomm Adreno 842.8. The 720p scene overlay and FSR are active in this run; unlike the earlier baseline, their activation is explicitly logged.
- Pressure pruning now reclaims completed images immediately: 308 MiB and later 420 MiB batches, zero pending retirement. Production ARM64 ownership tests pass. This confirms the narrow retirement-latency fix, not overall memory stability.
- At frame 2145, ctl stats report 4,834 MB images: 2,513 MB GPU-written and 2,321 MB from storage. Detailed source buckets account for 1,640 MB tracked, 225 MB filled once and 10 MB cleared; shared-allocation accounting makes these buckets differ from the aggregate. Do not sum inconsistent samples/categories as if all were independently resident.
- Allocator: 41 blocks (2,624 MiB), 2,216.5 MiB used, 407.5 MiB free within blocks; 1,968 MiB dedicated. Zero-sharing saves 0 MiB in this sample, so the earlier Mario Kart zero-target saving is not demonstrated for this Zelda scene.
- Sampled total PSS around 7.5 GiB includes roughly 5.2 GiB swap PSS. Major faults and multi-second frames confirm paging. Android again recorded `:game LOW_MEMORY` at 17:45:31.873. Exit PSS can be stale/zero and is not the peak.
- Last periodic dirty-tracker diagnostics show zero mprotect failures. Maps were below 65,530; this is not a demonstrated map-count exhaustion failure.

## Confirmed differences and implementation order

| Priority | Mechanism | BananaNative | Swap2 and relevance |
|---|---|---|---|
| 1 | Horizon application-pool accounting | v0.2.5 Zelda profile explicitly `mem_map_limit on`; v0.2.6 `game.py` defaults it on. `MapPhysicalMemory` refuses over-budget commitment with ResultLimitReached; GetInfo includes mapped pages in used memory. | Installed Zelda runtime profile says `MEM_MAP_LIMIT 0`; local default is off. Existing host already implements the correct guarded paths. Heap is declared 6.20 GiB, mapped alias pages reach 3.00 GiB, application pool 9.00 GiB, plus reported used/module bytes. These are logical committed/accounted guest quantities, not a direct physical-PSS sum. The disabled limit lets the SDK retain cached mapped blocks beyond its budget. Strong candidate, not yet a measured fix. |
| 2 | Zelda alias address-space layout | Both pinned references use `mem_alias 0x200000000` (8 GiB), `mem_reserve 0x468000000` (17.625 GiB). The profile comment attributes the larger alias region to an OptimizeChangeBanc freeze in Kokiri Forest. | Installed/local profile has alias 4.5 GiB, reserve 14 GiB; log reaches +4.25 GiB of alias address extent. Both report the same 9 GiB application pool. Virtual extent differs from mapped physical pages. Compare allocator/disassembly and address exhaustion before carrying over the layout; enlarging a reservation alone is not a RAM fix. |
| 3 | Pruning during allocation bursts | `vka_tex_get` invokes pruning before new images, not only during prepare. A dry scan suppresses repeated unproductive scans for that frame. | Swap2 only scans once per frame at prepare. A measured frame uploaded over 1,000 new textures; the cache can grow substantially before the next prepare. Port allocation-boundary checks with a cheap byte-growth gate, preserve current-frame/fence/holder protection, and test bursts before copying broader GPU eviction. |
| 4 | GPU-written image eviction | Both references permit eviction after 120 unused frames, after CPU-backed candidates. Their memory reference explicitly leaves loss of GPU-written content open. | Swap2 retains live GPU-only content unconditionally. This can retain more RAM (2,513 MB GPU-written in the sample), but blindly copying the upstream policy can destroy the only valid copy of a render result. Implement preservation or prove overwritten/dead storage before eviction; age alone is not proof. Also retain Swap2's actual-owner reference handling rather than upstream `refs=1`. |
| 5 | Compiler concurrency | v0.2.6 serializes prewarm builds on Turnip, tracks long builds and avoids overlapping builds of the same heavy shader. Frame-path/asynchronous builds are not globally serialized. | Local implementation lacks these guards. Useful for Turnip: upstream measured compiler allocation/map spikes. Current failing run uses Qualcomm and late long frames report zero new pipelines, so this is not established as its principal cause. |
| 6 | Dirty-state correctness | v0.2.6 page state follows successful mprotect; partial/failing protection is recovered and untrackable pages stay dirty. | Local tracker can publish protected state after a failed call. Worth porting with failure/race tests; this run reports zero such failures, so it does not explain this captured termination. |
| 7 | Partial texture uploads | v0.2.6 uploads changed bands and handles WriteTexels subregions, with base-level-aware offsets. | Local uploads can do more copying/deswizzling. Relevant to recording time and temporary bandwidth; preserve compressed-block, layer/mip and sparse/base-level semantics. Not a proven fix for the whole live working set. |
| 8 | Soft texture option | v0.2.6 can omit the finest mip of eligible large file-backed compressed textures, with copy-triggered restoration. Enabled for Mario Kart World, not explicitly for Zelda's reference profile. | Not in Swap2's Vulkan path. This changes texture quality and shader-visible size/LOD behavior; do not silently enable it for Zelda as a correctness fix. Android's implementation does not free the corresponding placed guest-pool pages. |

## Concrete first change and validation

1. Port the general default for application-pool accounting after auditing explicit opt-outs. Ensure the installed runtime profile is regenerated: changing `game.py` alone does not change `game_profile.tsv` already on the phone. Preserve the shader library and code outputs when only regenerating compatible profile metadata. Do not shrink the verified heap/application pool.
2. Before gameplay, inspect the actual installed runtime profile for `MEM_MAP_LIMIT=1`; verify the same main build, driver, 720p scene overlay, FSR and save. Do not claim the new setting is active solely from repository source.
3. Exercise MapPhysicalMemory at/below/above the pool budget, duplicate and overlapping mapping, unmap/recommit, heap plus module/BSS accounting and concurrent accounting. Confirm failed commitment does not change the bitmap or totals; GetInfo's used/available values must agree. Test the SDK's release-and-retry sequence rather than adding host retries.
4. Observe alias mapped bytes and `MapPhysicalMemory ... refused` followed by guest UnmapPhysicalMemory/retry. If the SDK hangs instead, capture that sequence and its wait stacks; upstream explicitly requests reports of that outcome.
5. Repeat the same title-to-house/outdoor route for at least ten minutes, cold and warm pipeline cache, then a second run. Record PSS/swap PSS, MemAvailable, mapping totals, image classes, allocation bytes and major faults. A functioning cache or surviving opening scene alone is insufficient.
6. Address allocation-burst cache checks separately, testing a many-image frame, held/in-flight views, aliases, completed retirement, bounded reupload counts and GPU-only preservation. Compare to the first accounting-only run to isolate effects.
7. Regress DK, Mario Kart World, Pokopia and one additional affected profile. macOS-only regression tooling cannot validate Android gameplay on this Windows host; record portable-test and device results separately.

## Sources and reproducibility

- [Pinned v0.2.5 Zelda profile](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.5/dm/games/0400486024DEC000/game.tsv).
- [Pinned v0.2.6 profile defaults](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.6/dm/games/game.py).
- [Pinned v0.2.6 memory mechanisms and caveats](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.6/docs/reference/memory.md).
- [Pinned v0.2.6 texture implementation](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.6/dm/gfx/vk_android/vka_tex.c).
- [Pinned v0.2.6 pipeline compiler](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.6/dm/gfx/vk_android/vka_pso.c).
- [Pinned v0.2.6 dirty tracking](https://git.eden-emu.dev/moceure/BananaNative/src/tag/v0.2.6/dm/gfx/nvn/nvn_dirty.c).

Selected downloaded source files, hashes and local diffs are under `output/analysis/banananative-v0.2.6/` (`sources.json`). Swap2's one-host runtime-profile architecture differs from upstream per-profile constants: do not overwrite its host/runtime interfaces or translator ABI wholesale.
