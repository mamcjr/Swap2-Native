# Hyrule Warriors: Age of Imprisonment (`04003F40232DE000`)

A Switch 2 game: update v196608, SDK 20.5.15 built for the Switch 2 (`Ounce`). Koei Tecmo's KTGL engine on NVN2, modules `main`, `subsdk0` (NintendoSDK_movie: VP9 movies) and `sdk`. **Plays on the Mac** from a new save through the first-run settings, the opening movies and cutscenes into the first field, where Zelda walks; the frame time's median is 16.6 ms there, with long frames every few frames on a 16 GB Mac ([Performance](#performance)). Battles have not been reached. No IPA has been built. **On Android: in bring-up; the first boot reaches the opening at 60 fps on a POCO F7 Ultra** ([Android](#android)). The profile is upstream Banana Native's (eden/android 7daac0fb, 2026-11-16), for the same build as our dump; the Mac findings below are upstream's.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_reserve 0x270000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | Donkey Kong Bananza's 9 GB layout, as Pokopia's; not measured here. The NPDM declares no system resource |
| `pad_layout full` | `game.tsv` | the camera is on the right stick |

The rewritten modules take 62.5 MB, so the default `mem_used` (64 MB) covers them. The rewriter needs no cut for this build.

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 47 GB: the base game and update v196608, whose RomFS (46 GB, 2,174 files) is whole; packed from the update's RomFS alone, without the base's ([android/NUP.md](../../../android/NUP.md): the pack reads `<file>.nup:update/196608/romfs` in place). It carries no NACP or icon; its name record says "HWAoI", so the app takes the profile's name.
- RomFS: KTGL's RDB under `b6e28c40` (`root.rdb`, `system.rdb`, 1,922 `.fdata` files, `data/*.file` and per-language `data/*.file`), and 129 `.bin` archives under `ebc8c781`.

## Shaders

- 101,860 in [`shader_map.tsv`](shader_map.tsv) (101,644 from the RDB, 161 GLSLC outputs in `main`, 55 others), split in about 90 s ([KTGL on NVN2](../../../docs/reference/shaders.md#ktgl-on-nvn2-switch-2-glslc-outputs-and-ld_--li_-linkdata)). The runs here used a partial Mac library of the 15,344 keys asked from boot to the first field ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)): 11,451 of them at boot, as KTGL makes its programs up front. A full Mac build was estimated at 13 hours. [`shader_seen.tsv`](shader_seen.tsv) lists the 15,449 keys of the playtests' library (the first-run settings, the opening, the first field and the areas a 12-minute playtest reached), which [Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders) builds alone: 11,451 of them took about 30 minutes here from an empty cache, the next 3,900 another 29.
- Resolved: vertex shader `cc679c7b4d3e703c` (Pokopia's too) and pixel shader `059a0a910ff87a26` stopped at an indexed read of c1 with the bank in the address register ([LDC with the bank in its address register](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is), chain 316fb82e2f22). With it every shader of the playtests' list translates.
- Resolved: pixel shaders `02e49c189b769074` and `49241cf469c76830` stopped at the barrier moves around their warp shuffles ([Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles), chain 95ba69b5403e).
- The opening movie's frame is drawn by pixel shader `3aafef4169955790` from `main`; while it was missing from the library the movie stayed black under its "+ Skip" prompt. The movie needs no `tex_flush_upload`.

## Performance

- The first field, from boot: frame time p50 16.6 ms, p95 about 18 ms, p99 100–114 ms on an M6 Mac with 16 GB. Each frame takes 3–6 ms of GPU time; the long frames wait for Metal's submission queue.
- Resolved: about 10 fps (p50 100 ms) in the field. Each dedicated c1 constant block of the partial library was a Metal buffer of its own, and the residency set's 16,118 objects made every commit slow (Dedicated c1 constants as slices of a bank).
- Current (open): the Metal driver wires and unwires memory for each submission, 2.8 s of every 4 s in a Metal System Trace. The game's own GPU memory pool is a constant 4,655 MB (`main+0x19eb098`), with a second pool of 350 MB, and its GPU allocations total 7 GB; the 16 GB Mac had 9.7 GB wired system-wide and about 160 MB free. With fewer commits per frame (`DM_NVN_CUT=0`) p99 was 52–69 ms. A Mac with more memory has not been tried.

## Saves

- The save is one file, `DATA.BIN` (8 MB). A new save creates it at boot with its defaults (operation 0 of the game's file request routine, `main+0x1325be0`); every boot reads its header and three areas (operations 1, 6, 7 and 8).
- The game writes it at its autosave points (operation 2; four of its nine write requests come from the state machine of the save manager, a sequence the game creates only when it saves, `main+0x15463d4`). The opening and the first field reach none, so until then every boot starts at the first-run settings again. Checked: in a 12-minute playtest the game asked for no write and called no file or SDK function to save.
- The autosave icon in the lower right corner after confirming the first-run settings is not a write: the game makes no request and no SDK call while it shows.
- A save written at an autosave point and loaded again has not been seen yet.

## Android

Current, 2026-11-17 (Swap2-Native, POCO F7 Ultra, Adreno 830 target):

- The dump is the user's NUP, `Hyrule_Warriors_Age_of_Imprisonment.nup` (47.5 GB: `base` v0 and `update/196608`), on the PC and on the phone (`/sdcard/Switch2/Games/`). Its update's modules are upstream's build: `main` 659cb288…, `subsdk0` 2f1ad415…, `sdk` 820cd7d0… (read from the NUP, 2026-11-17). The NPDM declares no system resource (META +0x14 = 0).
- The host already answers what upstream's Mac bring-up needed (GameChat, the language selection limit, the graphics frame length: `hle_misc.c`, `hle_oe.c`) and the translator has its two Ampere fixes (BMOV around warp shuffles, LDC `.IS`).
- Shader library (chain `e3d2789e93f6`, built on the PC in WSL, 2026-11-17): the split of the NUP's update RomFS gives exactly upstream's `shader_map.tsv`, `shader_facts.tsv` and `shader_pairs.tsv`; 101,844 of 101,860 translate and every module passes `spirv-val` (146,602 modules, 5.3 GB library, 3 h with 12 at a time). **Open:** 16 pixel shaders fail with `BRX with no target found in the c1 jump table`, Pokopia's open case ([its note](../0400A3A01EE4C000/README.md#found-on-this-game)); what draws with them does not draw. No `vk_delta.tsv` yet, so a Prepare on the phone, which needs every shader (`DMSH_VK_REQUIRE_ALL`), would stop at them: until they translate or the delta is published, the pack is made on the PC (`pack_android.py`, code rewritten in 82 s, no rewriter cut needed) and copied to `Switch2/Work/04003F40232DE000/`.
- First boot (2026-11-17, POCO F7 Ultra, Qualcomm 891.14, the test app, a new save): **reaches the first-run settings and the opening's cutscenes at 60 fps**, picture correct as far as seen (the forest, a Korok, Zelda). Heap 8.94 GB in the 9 GB layout; GPU 2.8-3.0 ms a frame (p50), frame time p50 16.6 ms, p95 19-30 ms; 54 pipelines by frame 11,400, 734 ms making them. Process PSS 6.0 GB at 300 s (1.7 GB of it graphics), still rising slowly. The game's 4,655 MB pool is placed by parts of 16 MiB ([Large pools placed whole for one buffer in them](../../../docs/reference/memory.md#large-pools-placed-whole-for-one-buffer-in-them-android)).
- Red lines of that run (open): a linear texture across two parts of the large pool, whose draws are skipped (as Pokopia's); 1D texture buffers (`target 10`) of shader `04ba2c6077fb933c` read as unbound (`texture buffers are not images yet`).
- **Open, current (2026-11-17, the user's 25-minute session on 891.14):** the game stops for 30-60 s at a time while Qualcomm's compiler makes some of KTGL's compute pipelines on the replay thread, which every frame waits for: `0b22caa9738d37f5` 59.4 s, `6bef3d23a8ddeafd` 50.0 s, `4fb03a033366f0b2` 37.0 s, `37dfdedb0dc0e80b` 35.0 s, `19391de31e77a42b` 31.8 s (about 3.5 minutes of stalls in a row after a Continue). `19391de31e77a42b` is the compute shader of Pokopia's open stop ([its note](../0400A3A01EE4C000/README.md#android): 2.0 MiB of SPIR-V, 26,391 `OpVariable`), and the cause is the same for all five: the translator's render scale helper, called once per texel fetch coordinate (2,926-5,224 calls), and a Function variable for every intermediate (24,014-49,754 in one function) ([the entry](../../../docs/reference/shaders.md#android-render-scale-helpers-inlined-at-every-texel-fetch)). Fixed in the library build, `DMSH_VK_CHAIN` `0d54d1f5ecf6` (`0b22caa9738d37f5`: 4,806 variables and 86,940 instructions instead of 49,754 and 235,667; `spirv-opt -O` on the PC 2.8 s instead of 21.0 s). On the phone (2026-11-17, 891.14, the rewritten modules through `DM_VK_SPV_OVERRIDE`, made by the prewarm threads) the five compile in 2.5-4.2 s instead of 32-59 s; what they draw was not on screen in that run. Each is remembered in the pipeline cache's `.heavy` list after its first compile, so a later run should find it in the driver's cache (not checked). Before the stalls the field ran at 30 fps (frame time p50 33 ms, about 280 draws a frame) with 3.8 GB of the process swapped out and 240-600 MB free on the 16 GB phone. Logs `output/hwaoi/pause1*.log` (kept local).
- Pausing during those stalls froze the screen and Android reported "not responding" ([the entry](../../../docs/reference/rendering.md#not-responding-on-pause-the-suspend-waited-for-the-gpu-on-the-launchers-ui-thread-android)); fixed in the host, not yet run on the phone.
- Open: battles, a longer session's memory, Turnip, the 16 shaders, measured `readiness.tsv` rows (its memory rows are from upstream's Mac figures).

## Driving it on the Mac

Run headless: `DM_WINDOW` set to anything, `0` too, opens a window, and a covered window takes the focus away (`oe focus state → 2`), which slows the display link to about once a second and holds `DM_CTL` screenshots. Shots of the 3840x2160 swap chain hold the 1920x1080 picture in its top-left quarter. From a new save: about 40 s to the settings screen, `A` (Next), `A` (Yes), `A` at the autosave notice, then the movies and cutscenes, `A` every 25 s or so; a tap of `Plus` does not skip. Zelda is in the field about 8 minutes after boot, and `LUp` walks. Presses during fades are lost. The work folder has `boot.sh`, `ctl.sh`, `see.sh`, `shaders.sh` and `to_field.sh`.

## Open

- The long frames ([Performance](#performance)).
- Battles, the picture against a Switch 2, an IPA (a full iOS shader build first).

## Found on this game

- [GameChat at boot](../../../docs/reference/services.md#gamechat-at-boot): boot aborts.
- No Game language row in an app packed from a NUP or a dump: its NUP has no NACP. With `DM_LANG` it boots in German, Dutch and Simplified Chinese; Russian and Portuguese, which it lacks, show English.
- [Whether the language selection is limited](../../../docs/reference/services.md#whether-the-language-selection-is-limited).
- [The target frame rate (graphics frame length)](../../../docs/reference/services.md#the-target-frame-rate-graphics-frame-length).
- Dedicated c1 constants as slices of a bank: about 10 fps in the first field.
- [Barrier moves around warp shuffles](../../../docs/reference/shaders.md#barrier-moves-around-warp-shuffles): two pixel shaders left out of the build.
- [LDC with the bank in its address register (`.IS`)](../../../docs/reference/shaders.md#ldc-with-the-bank-in-its-address-register-is): two shaders left out of the build.
- [Android render scale helpers inlined at every texel fetch](../../../docs/reference/shaders.md#android-render-scale-helpers-inlined-at-every-texel-fetch): stops of 32-59 s while Qualcomm's compiler made five compute pipelines; 2.5-4.2 s each with the rewritten modules on the phone.
- ["Not responding" on Pause: the suspend waited for the GPU on the launcher's UI thread](../../../docs/reference/rendering.md#not-responding-on-pause-the-suspend-waited-for-the-gpu-on-the-launchers-ui-thread-android): Pause during its compute pipeline compiles on the phone.
- [Non-indexed indirect quad draws skipped on the Vulkan replay (Android)](../../../docs/reference/rendering.md#non-indexed-indirect-quad-draws-skipped-on-the-vulkan-replay-android): found on this game in Banana Native v0.3.51 (Koei Tecmo's KTGL engine); ported here, not yet run on Android here.
