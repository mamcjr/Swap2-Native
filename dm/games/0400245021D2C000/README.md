# Yoshi and the Mysterious Book (`0400245021D2C000`)

A Switch 2 game: update v131072 (Ver. 1.0.2 on its title screen), SDK 20.5.15 built for the Switch 2 (`Ounce`). Unreal Engine 5.4.4 (`SDK MW+EpicGames+UnrealEngine-5_4_4` in `main`; project `PJ045`, platform `Sage`) with its deferred renderer on NVN2. Modules `main`, `subsdk2` (NintendoSDK_movie) and `sdk`; `sdk` and the movie module are the builds Hyrule Warriors: Age of Imprisonment runs. **Plays on the Mac** at 60 fps from a new save: the opening movie, Mr. E's dialogue, Chapter 1's page, the first habitat, walking, jumping and the flower friend, ending an exploration, a save, and a reload of that save. No IPA has been built. **In Swap2-Native: in bring-up** ([Swap2-Native](#swap2-native)); the profile and the facts below are upstream Banana Native (eden/v0.3.42, `14a31c5d`)'s, for the same build.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x118000000`, `mem_reserve 0x140000000` | [`game.tsv`](game.tsv) | Engage's 5 GB block. Unreal's engine region takes 4.06 GB of it (the rewriter's engine-wide gate) and the allocator is the host's (the engine-wide cut). See [Memory](#memory) |
| Node area cut 1 GB → 62 MB (`sdk+0x14df18` / `+0x14dfa4`) | [`dm/tools/sdk_patches.py`](../../../dm/tools/sdk_patches.py) SDK-wide rule, no build ID | without it the engine region did not fit and `nninitStartup` called `exit(1)` |
| `mem_stack 0x10000000`, `app_pool 0x240000000` | `game.tsv` | as Donkey Kong Bananza, the Switch 2's 9 GB |
| `sys_resource 0x1400000` | `game.tsv` | the NPDM declares 20 MB, so virtual address memory is on |
| `depth_mode zero` | `game.tsv` | the game sets near = 0, so pipeline bases made offline use `main0_z` |
| `pad_layout full` | `game.tsv` | a 3D field and both triggers in use |

`subsdk0` (NVIDIA's DLSS library, 41 MB) and `subsdk1` (CUDA) are not loaded: `main` takes nothing from them that the host does not supply. There is no `tex_dyn_slots`: the translator traces every read's handle in the 1,519 shaders of the playtests.

## Files

- One [NUP container](../../../docs/development.md#the-packing-script-in-detail) of 41 GB: the base game and update v131072, whose RomFS (20 GB, 147 files) is whole. No NACP; the name comes from the profile.
- RomFS: one IoStore container, `PJ045/Content/Paks/PJ045-Sage.utoc` / `.ucas` (19 GB, TOC version 6, Oodle, 84,412 chunks), `PJ045-Sage.pak` (102 MB, 4,723 files: configs and the global shader cache), and loose movies under `PJ045/Content/Movies` (`.mp4` for the opening, the title and the ending, `.webm` for the staff roll).

## Shaders

- 133,154 in [`shader_map.tsv`](shader_map.tsv) (45,614 vertex, 60,840 pixel, 21,654 compute, 5,041 mesh, 5 geometry), split in 76 s: Unreal Engine 5.4 shaders for the Switch 2.
- The runs here used a partial Mac library of the keys the playtests asked for, listed in [`shader_seen.tsv`](shader_seen.tsv) (1,519, of which 395 come from [PR 1261](https://github.com/seedds/nxnative/pull/1261)'s runs through part of the second level) ([Building selected Mac shaders](../../../docs/development.md#building-selected-mac-shaders)), which takes about a minute to build. A full build is not estimated yet; Pokopia's similar map came to about 6 hours on an M4 Pro. All of the listed shaders translate. With no stamp the host logs a red line at each start and every c1 takes its own 64 KiB.
- Resolved: 3 pixel shaders of the playtests lost 5 texture reads, handles built under a guard, 130 reads in 78 shaders of the whole map. `6f91c450371a880c` (`MM_Masked_SingleLayerWater`) and `c132ee1a17f37080` (`M_HoneyPoolB`) each lost a gather through a register (0x364) of the depth behind the water, c3 +0x18 | +0x68. Both draw on Chapter 1's page, and with the gather's result 0 the pool's water surface there is darker and has no foam. `cbd80cb96ad38a61`, a global lighting pass that draws at the title, lost 3 reads, with no difference in the title's picture. `03be149072c3a673` (`MI_SingleLayerWater_PansySanA`), which draws in the first habitat, read c3 +0x18 with no sampler word; that change was not compared in a picture. Across the map, 189 reads in 152 shaders had that form.
- Resolved: the uniform vote 0x886 writes UP, not P: VOTEU writes the uniform predicate. The handles above do not depend on it, and which pictures it reaches is still unknown.
- Open: SURED.MAX.U64 (33,550 uses in the map, Nanite's visibility buffer) is refused at translation, so those shaders are left out. The first chapter does not ask for them. `SageEngine.ini` sets `r.Nanite.*`, so another level might.

## Frame rate

- Current: 60 fps, the game's own: `rhi.syncinterval=1` in `DefaultDeviceProfiles.ini`, so no `fps_target`. In the first habitat on an M6 Mac with 16 GB, frame time p50 16.7 ms, p95 17.1 ms, CPU about 47% of one core and 1.5 W, with an occasional long frame of 65–79 ms. Long frames were not traced.

## Memory

- Current: `nninitStartup` reserves a 62 GB region, and the rewriter's engine-wide gate gives it 4.06 GB. Unreal's FMallocBinned3 is replaced by the host's allocator through the engine-wide cut (`main+0x11aa0`).
- The alias region is mapped up to +3.51 GB, +3.51 GB and +3.75 GB after one, two and three entries into the first habitat, of 4.38 GB; about 3 GB is mapped at a time. 5.6 GB resident, about 3 GB of Metal allocations.
- Open: later chapters are not measured, and the room left is about 0.6 GB. If they run out, a Mac can take Splatoon Raiders' 16 GB alias region; an iPhone cannot.

## Saves

- The save is one file, `Slot00`. The game creates it empty when New Game is chosen, and writes it (308 KB) when an exploration ends. So until then a reboot shows New Game again. The title then lists the slot, and Start Game continues at Chapter 1's page with the discovery recorded.

## Driving it on the Mac

Run headless, `DM_CTL` at a control file. The work folder has `boot.sh <secs> <log>`, `ctl.sh` (control lines, `snap NAME`, `wait S`), `to_level.sh`, `keys.sh <logs>` (adds the keys the logs ask for and builds them), `loop.sh` and `run/save_ref` (Chapter 1's page, after one exploration).

From an empty save: at 70 s `A` (title), 8 s later `A` (New Game), 10 s later `Plus` then `Plus~2` skips the movie, 15 s later 34 `A` presses 3 s apart through the dialogue. The magnifier is on the left stick: from the centre, `LLeft~0.4` and `LDown~0.25` put it on the flower, then `A`, about 9 more presses into the first habitat. `LRight` walks, `A` jumps. In a habitat: `Plus`, `LDown` four times, `A` (End Exploration), then `A` a few times to the page.

From `save_ref`: at 72 s `A`, 8 s later `A`, 14 s later the book is open at Chapter 1.

## Open

- Current, on chain 1df0f6cd2b57: Chapter 1's page with the pool's water fixed, and the first habitat plays with no new red line.
- Current, on chain 9bb5d8678204: the first habitat's picture and frame times are as on 3dd02e581c0c, the pause menu's blurred background included. The black background seen there earlier did not come back on this route.
- Reported by [PR 1261](https://github.com/seedds/nxnative/pull/1261), not run here: the first level and part of the second work; "Who's there?" has the game's picture behind it on a Switch 2, never black; on the second level the red lines `program has no vertex shader`, `dispatched program has no compute shader`, `image read as float` (format 13) and a geometry shader `502dcf1ce20751aa_b` with no vertex shader.
- The second level on the 5 GB block (see [Memory](#memory)), later chapters, the picture against a Switch 2, listening to the sound (audio opens and starts with no red line), the `.webm` staff roll, Nanite.
- An IPA: the 5 GB block suits iOS, but no iOS shader build or iPhone run has been done.

## Swap2-Native

Current, 2026-11-18:

- The profile is upstream Banana Native (eden/v0.3.42, `14a31c5d`)'s, which ran the game on the Mac only (no Android run upstream). The dump is the user's NUP, `Yoshi and the Mysterious Book [0400245021D2C000].nup` (41 GB: `base` v0 and `update/131072`); its update's modules are upstream's build: `main` 893c30e7…, `subsdk2` 2f1ad415…, `sdk` 820cd7d0… (Hyrule Warriors: Age of Imprisonment's `sdk` and movie module). [`readiness.tsv`](readiness.tsv) holds `status bring-up`.
- The node area cut upstream applies to this SDK by fingerprint is a row gated on this `sdk` build here (`dm/tools/sdk_patches.py` `820cd7d0…`: `sdk+0x14df18`, `sdk+0x14dfa4`, bytes checked); Hyrule Warriors, which ships the same `sdk`, gets it too. The code is rewritten on the PC (WSL, `pack_android.py --no-shaders`, 168.7 MiB) at `<NUP folder>/0400245021D2C000-work/android-pack`.
- **Blocked: the shader library.** The shaders are in Unreal's IoStore container, Oodle-compressed; `shader_split.py` needs them decoded first (`[RED] the RomFS has Unreal paks, but there are no decoded shader code archives in …:update/131072/ue_shaders (the NSP script writes them)`). The NSP script decodes them with the game's own `OodleLZ_Decompress` in the Mac host's tool mode (`dm/gfx/tools/uepak.py`, `dm/host/hle_oodle.c` `DM_OODLE_SERVE`), which needs the Mac host: the Android host is a library inside the app, and no Oodle decoder is in this repository. On a Mac, `scripts/build_ipa_from_nsp.py` with `--platform mac` writes `ue_shaders` (then split and build the Android library on the PC as for the other games). 133,154 shaders upstream: a large library (Hyrule Warriors' 101,860 took 3 h and 5.3 GB).
- Resolved 2026-11-18: the Unreal shader decode ran on the user's Mac (`build_ipa_from_nsp.py --platform mac` on the NUP's update unpacked into a dump folder), which wrote the split package (`pkg`, 13 files, 1.8 GB, 133,154 shaders; kept at `D:\Games\shader`).
- Shader library (vk chain `45cedbd878f7`, WSL, 2026-11-18, about 3 h at 4 at a time beside another build): 97,106 of 133,154 translate (vertex 45,614/45,614, pixel 46,465/60,840, compute 5,022/21,654, geometry 5/5, mesh 0/5,041); all 135,370 modules valid for `spirv-val`; 2,034 MB (WSL `~/yoshi-vklib/v-45cedbd878f7`). **Open:** 36,048 fail: at least 17,069 on Ampere `SURED` (a surface reduction with its handle in a constant bank: Nanite's visibility buffer, upstream's open SURED.MAX.U64; upstream notes the first chapter does not ask for them), 5,041 mesh programs with no mesh layout in the facts, the rest not broken down yet.
- First boot (2026-11-18, POCO F7 Ultra, Qualcomm 891.14, the test app, a new save; the NUP copied to `Switch2/Games/` and checked by SHA-256, the pack at `Switch2/Work/0400245021D2C000/test-pack`): **reaches the title screen at 52-57 fps** ("A Start Game", Ver. 1.0.2), picture correct. `[mem] Unreal's engine region: 62.00 GB asked, 4.06 GB of address space`, no abort; PSS 2.4 GB (1.25 GB graphics), steady over 5 minutes. Red lines: `nvnCommandBufferBindShadingRateImage` unimplemented (variable rate shading; about 20 a second), 9 shaders not in the library, texel buffers (`target 10`) read as unbound (`texture buffers are not images yet`), one storage buffer outside placed guest RAM. Logs `output/4games/yo6.*` (kept local).
- **Open, current (2026-11-18, the user):** the 3D picture is over-exposed (blown highlights, a bluish cast) on Qualcomm 891.14 and on Turnip Gen8 V39 alike, at 60 fps on the first chapter's opening. The engine compute shaders the game asked for and the library lacks (9, all from the global shader cache: its exposure / post passes) fail in the translator: 7 at `SUATOM handle in bank 3` (an image atomic whose handle is read from constant bank 3: fb6ab8fe753be1bc, d01424c4d947d512, b1465a9ea0ad4875, 8e3c96072f4eb41f, 89dd473c5ede4acb, 771534000c75e1f8, 4aa484d221a56fd3), 2 with `Specified argument was out of the range of valid values` (bdc969d0aeb4afc2, 23e73eaf528c9c0b). Handed to the translator session (2026-11-18). Logs `output/4games/yo8.*`.
- **Open, current (2026-11-21, POCO F7 Ultra, Turnip V39, the test app's save):** the opening shows an all-white picture (about 25 s after boot), and Mr. E's dialog ("Who's there? Is someone reading my pages?", about 60 s) a black screen with only the dialog box, as upstream saw on the Mac. Upstream's cause there does not apply as such: Metal's fast math turned the tonemap's film grain (pixel shader `edd037e72278a043`, `s * inf` clamped to -1..1) into NaN, and its fix is a Metal compile flag (nxnative `caccf926`). Here the translated module already clamps with `NMax` / `NMin` (GLSL.std.450, defined for NaN). What is missing here instead: every shader the game asked for and the library lacks is a compute shader the translator refuses at an image atomic or reduction whose handle is in constant bank 3 (`SUATOM` / `SURED handle in bank 3`: 9 at boot, then `cff90fc4457d7b71`, `06cea347d243a300`, `f513d7cacdeb6dc6`, `177d98a46d6bfef8`, `db350b0c50cc14af`, `644c57fa466efd6a` when the opening starts, `218c029749b4e624`, `d3a217e0d1c07c7d` later). Those passes (exposure, post) never run, the likeliest cause of both the white opening and the black dialog. Upstream translates them (`ASurfaceHandle` in `InstEmitAmpereMem.cs`: a surface handle from any bank, or bindless from a register; nxnative `4032a11e` and earlier); porting that is a chain change with a binding kind for an image handle read from a uniform buffer. Logs `output/ports-1121/yoshi.*` (kept local).
  - **Ported, phone-tested 2026-11-21 (chain `c1686c70ebf5`; the hang below resolved the same night):** [Images whose handle is in the game's uniform buffer](../../../docs/reference/shaders.md#images-whose-handle-is-in-the-games-uniform-buffer). The library gains 15 of those 17 shaders (115,168 of 133,154 build, up from 97,106; `23e73eaf528c9c0b` and `bdc969d0aeb4afc2` still missing). The title screen now has its picture (it was white), but every run ends in a GPU hang (device lost, about 30 s after boot, frame ~1200-1400). Bisected with `DM_VK_DISPATCH_SKIP_KEY`: no hang with all 15 skipped, nor with only one of the two culprits running; `218c029749b4e624` and `4aa484d221a56fd3` together hang. Both are UE5 global compute shaders (`cs_global.bin#1122`, `#2017`) that run atomics on one `r32ui` **buffer** image (a storage texel buffer, `IMG_UBO` at `c[3]` word 0, flags `ATOMIC|RW`). The host has no texel buffers on Vulkan yet (`vka_draw.c` "texel buffers are not on Vulkan yet … bound as null"; `vka_tex.c` "texture buffers are not images yet"), so the counter reads 0 on every atomic. The likeliest cause, unconfirmed: a work-queue loop that waits on that counter never ends. Fix to try: storage and uniform texel buffers on the replay (a `VkBufferView` over the buffer texture's pool range). The user also reported red and blue swapped somewhere in Yoshi's picture (open, not located yet). Paused 17:32 at the user's request (Zelda first).
  - **Resolved 2026-11-21 23:50 (POCO F7 Ultra, Turnip V39, library `c1686c70ebf5`):** the GPU hang. [Buffer textures as texel buffers on the Vulkan replay](../../../docs/reference/rendering.md#buffer-textures-as-texel-buffers-on-the-vulkan-replay): the counter is now a `VkBufferView` over the pool. 120 s on the title screen (53-58 fps) and 150 s into the opening (60 fps; the book's pages drawn, where the picture was white) with no device lost. **Open, current:** Mr. E's dialog ("Who's there? Is someone reading my pages?") is still a black screen behind the box; the run logs `[RED] BindVertexAttribState n=17` about 270 times a second (more than 16 vertex attributes: the binds are refused; Banana Native e62de350 records and replays 32), and buffer textures bound to image slots (`target 10`, `has no view of shape 0x81`, 5 binds) still read as unbound. Logs `output/ports-1121/yoshi-tbuf-a.*`, `yoshi-tbuf-b.*` (kept local).
- Not reached yet: past the first chapter's opening.
- Library published (2026-11-20): the profile's [`vk_delta.tsv`](vk_delta.tsv) (version `45cedbd878f7`, 133,154 shaders, 36,048 not built) and a copy of the library in `<game dir>/0400245021D2C000-work/vklib-cache/v-45cedbd878f7`. The colour fix for the bank-3 image handles changes the translator and waits for the next chain version.

## Found on this game

- Unreal Engine 5.4 shaders for the Switch 2: `shaders: 0` from the split.
- [Buffer textures as texel buffers on the Vulkan replay](../../../docs/reference/rendering.md#buffer-textures-as-texel-buffers-on-the-vulkan-replay): an atomic counter bound as null, a GPU hang 30 s after boot (Android).
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area): `nninitStartup` exits. The rule for this `sdk` also matches those of Mario Kart World and Donkey Kong Bananza.
- More than 16 vertex attributes: every bind refused.
- Images and buffer textures with their handle in a uniform buffer: compute passes skipped.
- A guarded load whose destination is its address: the scene's instance data read at index 0.
- Texture and sampler words in either order: the G-buffer bound as dummies, a black picture.
- Buffer textures behind handles in uniform buffers, and rows of finalized descriptors: flat-coloured objects, and a new Metal texture per bind.
- A deferred write under a false guard: the white-out from bloom.
- Ampere forms with a handle in a register, and SURED's other operations.
- [TXD through a uniform register kept no handle](https://git.eden-emu.dev/moceure/BananaNative/src/tag/eden/v0.3.42/docs/reference/shaders.md#txd-through-a-uniform-register-kept-no-handle): reads deleted in 14,079 shaders the playtests have not reached.
- [Layer entry points with the point size passed by reference](https://git.eden-emu.dev/moceure/BananaNative/src/tag/eden/v0.3.42/docs/reference/shaders.md#layer-entry-points-with-the-point-size-passed-by-reference): 38 vertex shaders without them.
- Handles built under a guard the read's guard cannot be tied to: the water on Chapter 1's page.
- VOTEU writes the uniform predicate: 7,560 of its shaders change, no picture does so far; in the keys asked in play only `cbd80cb96ad38a61`, whose uniform registers under the vote are unused.
- Texture views re-made for finalized textures every submission: resolved. Its menus read finalized textures' buffers as 2D rows (114,091 binds to frame 5,100 from an empty save); kept by the memory they view they made 40,308 rows views, kept with the texture 788.
