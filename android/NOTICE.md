# Swap2-Native: license notice for the Android build

Swap2-Native (the Android launcher in `android/app` and the host it builds, `libnxhost.so`) is a fork of
[Banana Native](https://github.com/moceure/BananaNative) v0.1.1 by moceure (commit `d154da1`), whose Android port is based on
[nxnative](https://github.com/seedds/nxnative) by seedds. Both are MIT-licensed ([LICENSE](../LICENSE)); the repository's overview of what is MIT and what is
GPL is in [NOTICE.md](../NOTICE.md).

**The Android build that includes LSFG is GPL-3.0-or-later.** By default the host links Lossless Scaling frame generation, a C port of
[Eden](https://git.eden-emu.dev/eden-emu/eden)'s implementation (itself based on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)), in
[`dm/gfx/vk_android/lsfg`](../dm/gfx/vk_android/lsfg/LICENSE.md), licensed GPL-3.0-or-later. An APK or `libnxhost.so` built with it is therefore distributed as a
whole under the GNU General Public License, version 3 or later ([COPYING](../dm/gfx/vk_android/lsfg/COPYING)); whoever passes such an APK on must offer its
complete source (this repository at the commit in the build tag). The MIT files keep their MIT license on their own; the rest of nxnative (the Mac and iPhone
builds) stays MIT. `./gradlew apk -PnxLsfg=false` builds an APK without LSFG; it is still GPL-3.0-or-later as a whole when it carries the on-device packer, whose translator is GPL too (below).

**The shader translator is GPL-3.0-or-later as a whole.** It translates Switch 2 task and mesh programs with
[`InstEmitAmpereMesh.cs`](../dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Instructions/InstEmitAmpereMesh.cs), ported from
[switch2mac](https://github.com/azmasm/switch2mac)'s `nx2_lower` (from [Eglantine](https://github.com/djihads80/eglantine)), GPL-3.0-or-later
([COPYING.GPL-3.0](../dm/third_party/ryujinx/COPYING.GPL-3.0)). The on-phone translator `libdmsh_xlate.so` and the Mac `dmsh_xlate_bin` are built with it, so
they are combined works under the GPL (see [dm/third_party/ryujinx/README.md](../dm/third_party/ryujinx/README.md)); the shader libraries they produce are the
games' own shaders translated, not part of this code. The other translator files keep their MIT license. Every APK that carries the on-device packer (`libdmsh_xlate.so`) is
therefore a GPL-3.0-or-later work whatever `nxLsfg` says, and the Mac and iPhone builds stay MIT only as apps: the committed translator `dm/gfx/tools/dmsh_xlate_bin` they run to
build shader libraries is the same GPL combined work.

**AMD FidelityFX Super Resolution 1** (MIT, Copyright (c) 2021 Advanced Micro Devices, Inc.): the headers `ffx_a.h` and `ffx_fsr1.h` in
[`dm/third_party/fidelityfx-fsr1`](../dm/third_party/fidelityfx-fsr1/README.md) ([LICENSE.txt](../dm/third_party/fidelityfx-fsr1/LICENSE.txt)), unmodified, are
compiled into the present's FSR 1 upscale (`dm/gfx/vk_android/fsr/`, SPIR-V in `vka_present_fsr.h` and `vka_ndsr_spv.h`). The other third-party code the
APK carries (libadrenotools, zstd, Ryujinx's shader library) is listed with its license in [dm/third_party/README.md](../dm/third_party/README.md).

The Vulkan replay's stand-in for NVN2's DLSS (`dm/gfx/vk_android/vka_dlss.c`, ported from Banana Native, MIT) and the temporal path in agl's NDSR slot
(`DM_VK_NDSR=gsr2`, `vka_ndsr.c`) run Qualcomm's [Snapdragon Game Super Resolution 2](https://github.com/SnapdragonStudios/snapdragon-gsr) (GSR2) 3-pass shaders,
adapted to compute passes in [`dm/gfx/vk_android/gsr2`](../dm/gfx/vk_android/gsr2/NOTICE.md) and compiled into the host (`gsr2/vka_gsr2_spv.h`):
Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved, under the BSD-3-Clause license, whose full text, conditions and disclaimer are in
[`dm/gfx/vk_android/gsr2/LICENSE`](../dm/gfx/vk_android/gsr2/LICENSE) and apply to the shaders in source and binary form. Its FSR 1 mode uses AMD's FidelityFX FSR 1
headers (MIT, [dm/third_party/fidelityfx-fsr1](../dm/third_party/fidelityfx-fsr1/LICENSE.txt)), as the present's FSR 1 does. Its FSR 3.1 mode (`vka_fsr3.c`) runs AMD FidelityFX
Super Resolution 3.1's upscaler passes from the FidelityFX SDK 1.1.4, compiled to SPIR-V (`dm/gfx/vk_android/fsr3/*.h`): Copyright (c) Advanced Micro Devices, Inc., under the MIT
license in [dm/third_party/fidelityfx-fsr3](../dm/third_party/fidelityfx-fsr3/LICENSE.txt).

Some fixes follow other projects' findings without copying their code: [kartnative](https://github.com/pahorema/kartnative) by pahorema (MIT) for
GPU-written sparse textures (`dm/gfx/vk_android/vka_tex.c`) and the per-primitive header of mesh programs (`dm/gfx/tools/shader_split.py`), and
[switch2mac](https://github.com/azmasm/switch2mac) by azmasm (MIT outside `nx2_lower`) for the SPIR-V mesh entry points marked in the translator. Thanks to them.

Code adapted from later [Banana Native](https://github.com/moceure/BananaNative) releases (MIT, Copyright (c) 2026 seedds), each marked with the commit at the place:
`dm/host/lse_unaligned.h`, its test `dm/host/tests/t_lse_unaligned.c` and the unaligned-atomic branch of `on_crash` in `dm/host/crash.c` (v0.3.40, commit `e6b6f0d0`).
`vka_spv_gs_fill` in `dm/gfx/vk_android/vka_spvpatch.c` and its use in `vka_shader.c` (`gs_fill`): a geometry shader that emits nothing emits a degenerate primitive (commit `0749171c`).
`android/host/adreno_binder_compat.c` (`libbinder_compat_hook.so`) and its test `dm/tools/tests/test_android_binder_compat.py`: the API 36 Binder annotation Qualcomm's 891.x drivers import, for Android 15 and older (commit `f71720e0`).
Sampler min / max reduction in `vka_tex.c` `sampler_of` (commit `02d081a2`). The drawn depth copy: `dm/gfx/vk_android/vka_t2tpath.h`, `vka_depthcopy.h`, `vka_depthcopy_spv.h` (its GLSL rebuilt here) and the `copy_t2t` hook in `vka_exec.c` (commit `a4a64a03`). The measured timestamp tick: `dm/gfx/vk_android/vka_tstick.h`, its test `dm/host/tests/t_android_tstick.c` (unchanged) and `tick_sample` in `vka_exec.c` (commit `5e4212d1`). The custom-driver check: `dm/gfx/vk_android/vka_driverpath.h`, its test `dm/host/tests/t_android_driverpath.c` (unchanged), `vka_driver_mapped` in `vka_driver.c` and the advertised placed-map test maps in `vka_dev.c` (v0.3.43, `af1ed596`). The guest remap probe `dm/gfx/vk_android/vka_remap_probe.h` (unchanged) and its use in `vka_dev.c` (v0.3.44, `f03e6671`).

`Lossless.dll` belongs to Lossless Scaling (proprietary, sold separately). It is never in this repository or in an APK: the user supplies their own copy, which the
launcher finds on the phone or lets the user pick, and copies into the app's private files.

Custom GPU drivers are never in this repository or in an APK either. The launcher's **Download…** (`DriverFetch.java`) fetches a driver zip only when the user
taps it, from the public GitHub release pages of [MrPurple666/purple-turnip](https://github.com/MrPurple666/purple-turnip),
[K11MCH1/AdrenoToolsDrivers](https://github.com/K11MCH1/AdrenoToolsDrivers), [whitebelyash/freedreno_turnip-CI](https://github.com/whitebelyash/freedreno_turnip-CI),
[Weab-chan/freedreno_turnip-CI](https://github.com/Weab-chan/freedreno_turnip-CI) and [crueter/GameHub-8Elite-Drivers](https://github.com/crueter/GameHub-8Elite-Drivers);
thanks to their authors. Each download keeps its own licence: Mesa's Turnip driver is MIT-licensed, Qualcomm's Adreno driver binaries are proprietary.

## Interface art and sounds

The launcher's art and the touch-control artwork, under [`assets/artwork/swap2-native`](../assets/artwork/swap2-native) and `android/app/src/main/res`
(icons, backgrounds, drawables), are the project's own and are released with the project. The interface sounds in `android/app/src/main/res/raw` (`s2_*.wav`)
were generated with ElevenLabs Sound Effects on a paid plan that grants commercial use, and are released with the project likewise.

The Zelda-themed touch artwork (for example `s2_zelda_controls.png`) depicts Nintendo trademarks. They are used only to identify the game the layout is for;
Swap2-Native is not affiliated with, endorsed or sponsored by Nintendo. Nintendo, The Legend of Zelda, Mario Kart, Donkey Kong and Nintendo Switch are
trademarks of Nintendo.

## Adapted runtime fixes in 0.1.5-beta

Queue dependency scheduling, texture-object ownership and file-backed pools were adapted from BananaNative v0.2.8 (moceure, MIT), revision `7ea6a966e4be51d5e2ac0be08543ff44c036e447`. File-backed pool helpers originate in AnkaNative v0.2.2, Copyright (c) 2026 EddizEge, MIT. Their notices are preserved in the source.

**bcdec 0.985** (Sergii "iOrange" Kudlai, MIT or the Unlicense) decodes BC1-BC7 texture blocks on the CPU on GPUs without BC textures (ARM Mali, Samsung Xclipse). See `dm/third_party/bcdec/LICENSE`. The hardware-buffer guest memory backing follows the route DroidPlay Core (enjoyerofslop, MIT) uses for its raw global memory. Recording the pipeline build a crash was in, at any length, keyed by driver and host build (`dm/gfx/vk_android/vka_deadly.h`, `vka_pso.c` `deadly_*`), follows DroidPlay Core commit `1ae945d2`; the idea only, written here without its code. From DroidPlay Core (enjoyerofslop, MIT), each marked at the place: `vka_spv_shuffle_xor` in `dm/gfx/vk_android/vka_spvpatch.c` (shuffles as ShuffleXor on Qualcomm's V@07xx drivers, commit `8115593e`, adapted to this replay's module path); `dm/gfx/vk_android/vka_memreport.h` (`DM_VK_MEMREPORT`, from `gfx_vk_memreport.inc` of commit `151f1557`, the full report only); the texture pool readback copied out before its scan in `vka_tex.c` follows commit `c1abc57d` (the idea only).

From the erickmendes22 Swap2-Native fork (MIT per file): the per-draw GPU timestamps of `dm/gfx/vk_android/vka_gprof.c` (`DM_VK_GPUPROF_DRAWS`) follow its `vka_gprof.c` (branch `ocarina-v11-ios-perf-from-v101`, commits `2b098ec5`, `4ed24d0e`), rewritten for every game and pass.

**dav1d 1.5.4** (VideoLAN and dav1d contributors, BSD 2-Clause) is included for AV1 software decoding and midstream hardware recovery. See `dm/third_party/dav1d/COPYING` and `THANKS.md`; this Android build uses its C implementation.
