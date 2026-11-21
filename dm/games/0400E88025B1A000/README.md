# Star Fox (`0400E88025B1A000`)

A Switch 2 game: update v131072, SDK 21.4.4 built for the Switch 2 (`Ounce`), NVN2 with DLSS. A custom engine (its own prefixes: `vfs`, `vrender`, `vjob`, `vscript`, `vspirv`, `veffects`, `vphysics`; project name `eclipse`), with Wwise 2024.1 for audio, Bullet physics and NPLN for online play. Modules `main`, `sdk`, `subsdk0` (the SDK's movie module), `subsdk3` (`nn::camera`); `subsdk1` (the C library) and `subsdk2` (`NintendoSDK_DLSS`) are shipped but `main` imports nothing from them. **In Swap2-Native: in bring-up** ([Swap2-Native](#swap2-native)); the profile and the facts below are upstream Banana Native (eden/v0.3.42, `14a31c5d`)'s, for the same build.

**Status on Android (current):** playable by the project's rule (listed as supported, no bring-up warning): it boots and reaches its first scenes on both the system driver and Turnip, and a player played it from start to finish with no complaints reported (user, 2026-11-15; phone and driver not recorded). The frame rate and the memory use on a phone are not measured (open). Upstream plays on Mac (M4 Pro / M5 Max): title, prologue cinematic, and tutorial flight.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `mem_alias 0x400000000`, `mem_reserve 0x550000000`, `mem_stack 0x10000000`, `app_pool 0x240000000` | [`game.tsv`](game.tsv) | the Switch 2's 9 GB. The alias region mapped up to +8.26 GB of its 16 GB, 6.78 GB at most at once, through the tutorial's start; 8.5 GB resident, 4 GB of it allocated on the GPU |
| `dlss_standin 1` | `game.tsv` | Android: NVN2 DLSS runs on vka's stand-in upscaler (`vka_dlss.c`, Snapdragon GSR2 3-pass by default) |
| `sys_resource 0x1000000` | `game.tsv` | the NPDM declares 16 MB, so virtual address memory is on; `main` never sets a heap |
| `modules main subsdk0 subsdk3 sdk` | `game.tsv` | `main` imports 17 movie functions from `subsdk0` and 19 `nn::camera` functions from `subsdk3`, nothing from `subsdk1` or `subsdk2` |
| `romfs_marker data.zip` | `game.tsv` | the RomFS has two files, and this is the game |
| `zip_store .zip` | `game.tsv` | the archive's members are LZ4-compressed already |
| `shader_groups /data.zip=shader` | `game.tsv` | every shader is a member of the archive |

There is no game-specific patch, and only v131072's build row: the base game, v0, has not been run.

## Files

- A NUP container of 30 GB: a base package (v0) and an update package (v131072), each with its own ExeFS and RomFS. Neither package carries a NACP or an icon, so the name comes from the profile, the display version is the placeholder `v131072`, and the app has no icon.
- The RomFS is two files: `data.zip` (14.5 GB) and `veclipse_build.cvar` (a JSON line: `system.is_shipping_build: true`). `main` opens `rom:/data.zip` through its own virtual file system (`vfs.zip.c`): LZ4-chained zip members behind a VZIP preamble. Its 114,154 members are named by a 16-digit hash with no folders or extensions; 92,069 are chained, 22,085 (the Wwise `RIFF` sound files and `BKHD` banks) stored plain.
- `main` is 192 MB of code with 1,036 imports, of which the host and the other modules supply all but two (gRPC thread-local initialisers, `_ZTH…`). No shader is in any code module.

## Shaders

2,856 in [`shader_map.tsv`](shader_map.tsv): 1,483 vertex, 1,305 pixel, 68 compute; 2,828 one per archive member, no task or mesh shaders, and the rest from the two NDSR upscaler networks embedded in `VIPR` containers. All of them translate. Each member is the engine's shader binary (`_vrender_shader_binary_t`), read as a single-stage program and paired at draw time (`separable_pairs`: 1,480 cover pairs).

## Memory Footprint & Phone RAM Constraints

Upstream measured 8.5 GB resident memory, 4 GB of which was allocated on the GPU. On Android:
- Phones with 12 GB or 16 GB of physical RAM are strongly recommended.
- `mem_map_limit` is enabled by default in the host to prevent unconstrained physical mappings beyond the application pool.
- Devices with tighter memory budgets will likely need Vulkan RAM saver options such as `tex_soft` or aggressive swap handling once tested on real hardware.

## The Picture

The 3D scene is the output size times the engine's own resolution scale times its dynamic resolution; the DLSS stand-in then takes it to the output. From code and the game's settings file (update v131072, main `a7730d0f…`):

- **Output:** 1920x1080 on the phone, the host's display size. `QueryDisplayBufferMaxResolution` (`main+0xb448874`; 4K in console mode, 1080p handheld) only clamps it.
- **Scale:** the window update (`main+0xb448780`) reads `render.resolution_scale_console` in console mode, `render.resolution_scale_handheld` in handheld mode (`main+0xb449868`, `main+0xb449898`, getter `main+0xb6ac800`), falling back to `render.resolution_scale` (1.0). The settings file (a JSON member of `data.zip`) gives console 0.833 on other platforms and **0.5 on this console**, handheld 0.833. Some levels set 0.833 for both while they run (their `set_resolution_scale` script; it puts the earlier value back when it ends).
- **Dynamic resolution:** `render.dynres_enabled` true, at least `render.dynres_min_scale_x` / `_y` 0.8 (0.75 handheld). The phone run's GPU time (19.5 to 20.5 ms a frame) keeps it at 0.8.
- So the phone's 768x432 (`DLSS network resized to 768x432`, the one case measured) = 1920 x 0.5 x 0.8. Graphics scale 0 / 1 / 2 only sends the engine a "scale is 0" flag nothing reads; operation mode only picks the console or handheld setting and the clamp. Neither is a lever, so the profile has no `graphics_scale` rows.

**The lever: 3D scene (`scene_arg` rows).** The engine takes any setting as a game argument (`-render_resolution_scale_console=0.75`; a flag as `-<name>` / `-no_<name>`, `main+0xb6b0740`). Game settings › 3D scene passes the choice as `DM_GAME_ARGS_ADD`; nothing is made at Prepare. Expected 3D sizes at the 1920x1080 output (the most the game draws, then x0.8 under load):

| Choice | Arguments | 3D at full dynamic resolution | 3D under load |
|---|---|---|---|
| Game default | none | 960x540 | 768x432 (measured) |
| 60% | both scales 0.6 | 1152x648 | 922x518 |
| 75% | both scales 0.75 | 1440x810 | 1152x648 |
| 100% | both scales 1.0 | 1920x1080 | 1536x864 |
| The game's 50%, no dynamic resolution | `-no_render_dynres_enabled` | 960x540 | 960x540 |

The GPU is already past its budget at 768x432, so the higher choices cost frame rate.

**Static check (resolved, from code): the settings file does not overwrite the argument.** The settings are registered when the engine's settings object is made (`main+0xb6ab800`, called at `main+0xb66414c`, runs the registration callbacks such as `main+0xb38aa60`), before the settings files load (`main+0xb664aa8` → `main+0xb669380` → `main+0xb66caa0`). Registration (`main+0xb6abe20`) finds the argument (`main+0xb6b0740`), logs `Command line overriding value of '%s'.` and writes it through the setter `main+0xb6af9c0`, which marks the setting as set (byte +0xd of its entry, `main+0xb6afddc`). The file's values are parsed into a list (`main+0xb6ae3a0`) and applied by `main+0xb6af7e0` with its flag 1, which skips every setting already marked (`main+0xb6af868`..`main+0xb6af878`). Not checked: whether anything else (the online settings fetch, off offline) sets them later.

**Future option, not built:** the window update reads the scale through the getter on its common path (`main+0xb449810` onward, after the planar-reflection check, on every call), and the levels' script changes the scale while they run, so a host write of the stored value would probably take effect without a restart. Today's choices apply at the next start only.

**Phone verification (open):** with a choice set, the log should show `game arguments added: -render_resolution_scale_console=0.75 -render_resolution_scale_handheld=0.75`, the engine's `Command line overriding value of 'render.resolution_scale_console'.` (and `_handheld`), and `DLSS network resized to 1440x810` or `1152x648` for 75%.

## Swap2-Native

Current, 2026-11-17 (POCO F7 Ultra, Adreno 830 target):

- The profile is upstream Banana Native (eden/v0.3.42, `14a31c5d`)'s. The dump is the user's NUP, `Star Fox [0400E88025B1A000].nup` (30 GB: `base` v0 and `update/131072`); its update's modules are upstream's build: `main` a7730d0f…, `subsdk0` 593524e1…, `subsdk3` 973102ab…, `sdk` 41d4de4a… (read 2026-11-17).
- Upstream profile keys this fork does not read are kept in [`game.tsv`](game.tsv) as comments marked "(upstream, not in this fork)": `dlss_standin` (it drives only upstream's launcher rows; the GSR 2 / FSR 1 / FSR 3.1 stand-ins themselves are ported since 2026-11-20, `vka_dlss.c`, chosen by `DM_VK_DLSS`; the `game.tsv` comment still says bilinear) and `scene_arg` (the engine's own resolution settings as game arguments). [`readiness.tsv`](readiness.tsv) holds `status bring-up`.
- Shader tables: this tree's split of the NUP's update RomFS (`data.zip`, read in place, about 3 minutes) gives upstream's `shader_map.tsv` (2,856 shaders), `shader_facts.tsv` and `pso_pairs.tsv` exactly, after porting upstream's readers of the engine's zip (`VZIP`, members as chained 64 KB LZ4 chunks) and its shader wrapper (`viper_shader`) into `shader_split.py`. Its `shader_pairs.tsv` has 3 pairs where upstream's has 1,483 (upstream pairs separable programs at draw time, `separable_pairs`, not ported); the profile keeps upstream's.
- Shader library (current: chain `45cedbd878f7`, WSL, 2026-11-18): all 2,856 translate, all 2,926 modules valid for `spirv-val` (74.9 MB). The 2,570 that translated at chain `0d54d1f5ecf6` (2026-11-17) have the same modules; the other 286 (284 pixel, 2 compute) stopped at TXD `0xf6c` (281), TLD4 `0xf63` with a depth reference (2), `0x925` (2) and BMSK (1): resolved by [Ampere texture and ALU forms from Banana Native 0.3.42](../../../docs/reference/shaders.md#ampere-texture-and-alu-forms-from-banana-native-0342). Not yet checked on the phone. The pack (code rewritten in WSL, no rewriter cut; 271.6 MiB) is at `<NUP folder>/0400E88025B1A000-work/android-pack`.
- First boot (2026-11-18, POCO F7 Ultra, Qualcomm 891.14, the test app, a new save; the NUP copied to `Switch2/Games/` and checked by SHA-256): past the bring-up warning, the rewritten modules load, `nnMain` mounts `rom:` from the NUP's update and the engine reports `MountRom succeeded`; then `[RED] sf: the game connects to service bsd:s / bsd:a / bsd:u itself, which the host does not serve`, the engine's `vsocket.nintendo.cpp(148): error: socket: 4` and an `nn::diag` abort (`main+0xb6ba2c4`). **Resolved by the host port below:** SDK 21 inlines `nn::socket::Initialize(const Config &)` into the game, and this tree's host answers neither it nor the rest of upstream's Star Fox host work (`hle_net.c` socket initialization, SDK 21 overloads and poll; `hle_sf.c` camera commands; `hle_oe.c`, `hle_offline.c`, `hle_misc.c`, the Opus decoder manager, `nn::chat`; see [Found on this game](#found-on-this-game)). Log `output/4games/sf2.*` (kept local).
- Host port (2026-11-18, from upstream eden/v0.3.42): `hle_net.c` (`nn::socket::internal::Initialize(int)`, which `main` imports and the inlined `Initialize(const Config &)` calls: the `bsd:*` connection; the SDK 21 overloads; the C library's `poll`; Poll of a descriptor the host never made answers nothing ready at the timeout instead of failing), `hle_sf.c` (camera context and `camera_answer`, HIPC Close, the `caps:*` sessions; it replaces `hle_misc.c`'s zeroed-output camera session, so Mario Kart World's camera now gets upstream's answers too: checked on the phone, it boots past them, see its note), `hle_oe.c`, `hle_offline.c` (`GetCurrentIpConfig`), `hle_misc.c` (purchased-item notification, `nn::chat`), `hle_audio.c` (SDK 21 `OutputStream`; the device calls stay `hle_audren.c`'s), `hle_codec.c` (the hardware Opus decoder manager and its log lines), `hle_new_event` (`hle_thread.c`), `dm_rel_s` (`dm.h`). Phone result below.
- Second boot (2026-11-18, same phone and driver, the test app with that port, chain `45cedbd878f7` library on the phone, the test save): no abort in 300 s. The title screen ("Nintendo presents STAR FOX", A Start) at 60 fps at 30 s; past it the opening cutscene (the Arwing in the clouds) at 30 fps, then the first mission's "Customize controls" question at 60 fps at 300 s. Memory: 6.3 GB PSS, 1.1 GB graphics. Host lines: the Opus decoder manager decodes (manager interface), `socket: Poll of 2 descriptors with one the host never made … nothing ready at the timeout` once (the HTTP thread upstream describes). Open, current: the picture is too bright in the cutscene, matching `[RED] vk: DLSS stand-in: input and output differ in sRGB (NVN formats 0x38 → 0x29): a blit` and `rectangles past the textures (input 768x432, output 1920x1080 of 768x432): clamped` (see [The DLSS stand-in ignored sRGB textures](#found-on-this-game)); one `[RED] vk: storage buffer at 0x3f7fffd336340000 is not in placed guest RAM: it is not bound`. Log `output/4games/sf3.*` (kept local).
- The copy over USB broke off twice (13 minutes into a 30 GB `adb push`); `output/4games/push_resume.sh` resumes it in 1 GiB pieces through `adb exec-in`.
- Current (2026-11-20, not yet run): NVN2's DLSS runs on upstream's stand-ins (ported from Banana Native eden/v0.3.45: GSR2 3-pass by default, `DM_VK_DLSS` 4 / 3 / 2 / 1 / 0). The too-bright cutscene of 2026-11-18 was the bilinear blit's: the RGBA8 sRGB colour (NVN `0x38`, stored as UNORM) blitted raw into the RGBA16F output (`0x29`); every mode now reads the colour decoded ([The DLSS stand-in ignored sRGB textures](../../../docs/reference/rendering.md#the-dlss-stand-in-ignored-srgb-textures-android)). The clamp line came from `nvnDlssNetworkResize`, whose 768x432 is the network's output size now ([The DLSS network resized](../../../docs/reference/rendering.md#the-dlss-network-resized)): the evaluation is 768x432 to 768x432. Open: the phone check (the cutscene's brightness in every mode, and the log lines `DLSS stand-in: colour NVN format 0x38 ...` and no `rectangles past the textures`).

## Found on this game

- [Shared fonts depend on the phone's fonts (one Roboto for six types)](https://git.eden-emu.dev/moceure/BananaNative/src/tag/eden/v0.3.42/docs/reference/services.md#shared-fonts-depend-on-the-phones-fonts-one-roboto-for-six-types): issue #223; the APK's Noto Sans is now `standard.ttf`, per-type order in the host. Phone check open.
- SDK 21 socket initialization and overloads, and socket poll with nothing to wait on.
- SDK 21's `OutputStream` on a virtual output device, and SDK 21's hardware Opus decoder manager (`InitializeHardwareOpusDecoderManager`, etc. in `hle_codec.c`).
- `nn::chat`: `GetChattingUid`, VirtualCamera family, `GetRealtimeAudioLevels`, `ShowLocalCameraConfiguration`, `GetCameraImageProcessingModeForVirtualCamera`.
- `nn::oe`: `BeginPresentPhotographedContents`, `EndPresentPhotographedContents`, `ExitApplicationAndGoBackToInStoreDemoMenu`.
- `nn::nifm`: `GetCurrentIpConfig` (SDK 21 overload).
- `nn::applet`: `BeginUsingCameraResource`, `EndUsingCameraResource`.
- `nn::camera::sdk`: `DestroyCameraContext`, camera commands 3, 4, 21, 29 in `camera_answer`.
- [The DLSS network resized](../../../docs/reference/rendering.md#the-dlss-network-resized): `nvnDlssNetworkResize` in `hle_nvn.c` stores the network's new output size (768x432 here).
- [NVN2's DLSS on a stand-in upscaler](../../../docs/reference/rendering.md#nvn2s-dlss-on-a-stand-in-upscaler-android): GSR2 3-pass by default instead of the bilinear blit (2026-11-20, phone check open).
- The 3D scene from the engine's own settings as game arguments (`DM_GAME_ARGS_ADD`, profile `scene_arg`): [Scene resolution from the engine's own settings](https://git.eden-emu.dev/moceure/BananaNative/src/tag/eden/v0.3.42/docs/reference/frame-rate.md#scene-resolution-from-the-engines-own-settings-passed-as-game-arguments-android).
- [The DLSS stand-in ignored sRGB textures](../../../docs/reference/rendering.md#the-dlss-stand-in-ignored-srgb-textures-android) (upstream's entry of that name, eden/v0.3.42, found it too bright with the Upscaler on FSR): here the bilinear blit's too-bright cutscene of 2026-11-18. DLSS flags 0x4a (LDR colour, auto exposure), colour image RGBA8 (VkFormat 37, which an sRGB texture also has); fixed by the textures' sRGB, pending the phone check of the new `DLSS stand-in: colour NVN format` line (open until it names the formats).
- `shader_split.py`: VZIP preamble and LZ4-chained zip members, `viper_shader` single-stage NVN2 program wrapper.
- [Ampere texture and ALU forms from Banana Native 0.3.42](../../../docs/reference/shaders.md#ampere-texture-and-alu-forms-from-banana-native-0342): 286 shaders (TXD `0xf6c`, TLD4 `0xf63` with a depth reference, `0x925`, BMSK).

## Open

- Played start to finish by one player (no phone or driver recorded) and to the first scenes on both drivers by us; frame rate, memory use and online modes are open.
- High memory footprint: 8.5 GB resident, 4 GB GPU; evaluate phone memory pressure and `tex_soft` applicability when run on hardware.
- The 3D scene choices on a phone: whether the engine takes the arguments (see [The Picture](#the-picture), phone verification) and what each costs in frame rate.
- `nn::socket::EventFd` returns stub error; HTTP thread polls without wake-up (harmless offline).
- Open (2026-11-16), current: #238 (0.3.15, Odin 3): voice lines in cutscenes cut off after about 0.5 s. The stream and the Opus manager showed no error in that log (no underruns, no `[RED]` audio line), and no line says whether the game stopped writing or the host stopped taking. The log now has the [OutputStream write lines and the Opus window lines](https://git.eden-emu.dev/moceure/BananaNative/src/tag/eden/v0.3.42/docs/reference/audio.md#a-sound-report-with-no-line-for-the-symptom); a lead only: Wwise plays a voice's first half second from memory and streams the rest from the file, so a stop at 0.5 s may be the file read, not the audio.
