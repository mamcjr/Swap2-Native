# Graphics settings imported from BananaNative 0.2.5

Source reviewed: the locally archived `output/analysis/banananative-v0.2.5/source/banananative`, especially Android `GameSettings.java`, `GameUpscaler.java`, `Prefs.java`, `GameActivity.java`, and the Vulkan implementation. Release: https://git.eden-emu.dev/moceure/BananaNative/releases/tag/v0.2.5 . This comparison targets 0.2.5, not the newer 0.2.6 release.

## Implemented in Swap2

Global defaults are in **Settings > Performance > Graphics**. Each title can override them in **Game settings > Graphics**, or follow the defaults again. Restore game defaults removes the new overrides, without changing saves, mods, shader caches or touch layouts.

| Setting | Choices and behavior |
|---|---|
| Upscaler | Nearest neighbor, bilinear, FSR 1, native. Controls final enlargement to the screen and compatible NDSR network replacements. Live in the in-game panel. |
| FSR output sharpness | Off, softer, soft, default, maximum. Shown only for FSR. Affects RCAS at final enlargement to the screen; also live. |
| Picture format | Fit, preserving proportions, or stretch to screen. Applied at next start. The touch picture rectangle follows the same choice. |
| Background pipelines | On/off, default on as in BananaNative 0.2.5. Two background compilation threads when enabled. Applied at next start; missing pipelines can temporarily omit a draw. |
| GPU timing | Game-profile default, per segment, per submission. Applied at next start. Reports GPU timestamps to the game, not just to the overlay. |
| Resolution, FPS cap, GPU driver | Existing Swap2 controls retained, together with its scene-resolution overlay and frame generation. |

Portuguese and English strings use the existing Swap2 settings components, icons, themes and navigation. The old explicit per-game NDSR choice is preserved until changed through the new selector; an unset new global mode follows the earlier profile default. Legacy output-FSR sharpness is preserved. An explicit new global upscaler becomes the default for titles without an override.

`GameGraphics.env` runs before the player's environment overrides. Manual environment lines still win at startup. The panel's scaling rows display saved preferences; manual environment overrides may differ until the user chooses a scaling mode there. Changing the panel's upscaler intentionally sends that choice to the renderer through the existing live-control file.

### Renderer integration

- Keep named `DM_VK_NDSR=net/fsr/bilinear/nearest`. The local renderer historically interprets numeric `1` as FSR, while BananaNative 0.2.5 uses `1` for bilinear and `2` for FSR. Copying its numeric UI mapping would silently select the wrong mode. `GraphicsMode` and its JVM test protect the local contract.
- Nearest-neighbor NDSR uses an explicit texel fetch in the existing generic network-interface replacement. Retains packed output stores and linear/sqrt input handling. No game-key allowlist was added.
- Nearest filtering is also applied to both rotated presentation and ordinary blits (`DM_VK_PRESENT_NEAREST`).
- Sharpness Off uses RCAS strength zero, retaining the pass that places/rotates the final image. This import does **not** add BananaNative's separate internal NDSR RCAS pass. The UI explicitly calls this **output sharpness**.
- `DM_VK_STRETCH` fills the presentation extent; `TouchControls` uses the full normalized picture rectangle for the saved stretch choice. Picture placement remains relevant in Fit mode.
- The new NDSR shader was rebuilt with the NDK shader compiler and validated with SPIRV-Tools. These host-side built-in shaders do not change `DMSH_CHAIN` or translated game-library ABI; no game library rebuild is required solely for these settings.

## Not imported: renderer/host work still required

These controls are present in BananaNative 0.2.5 but deliberately **not exposed as working controls** in Swap2 yet:

| Option | Missing prerequisite and required validation |
|---|---|
| Dynamic resolution, 30–80% minimum or Off | BananaNative `probe.c` DRS controller and its verified per-build profile hooks. Integrate the controller, SDK/build gates and measured GPU timing before exposing the range. Validate actual render sizes and scene transitions. The current scene-resolution overlay is a different mechanism. |
| DK's 60/45/40/30 FPS decoupled pacing | Imported setting alone is insufficient: needs the measured-rate controller in `hle_fpslock.c`, profile flags/probes and native/JNI FPS-limit path. Retain current working caps until speed/audio/timing are tested. |
| Instanced draws | No equivalent `DM_VK_INSTANCE` consumer in the current renderer. Requires draw-run batching, shader variants, descriptor/state handling and cache-key integration. Compare rendering and repeated driver starts before enabling; upstream defaults off. |
| Classic render passes | No equivalent `DM_VK_CLASSIC_RP` consumer. Requires imageless framebuffer feature negotiation, render-pass/framebuffer lifetime, compatibility keys and command encoding. Validate Vulkan errors, depth/MSAA behavior and pipeline-cache reloads; upstream defaults off. |
| FSR sharpening inside NDSR | Requires the additional intermediate image and RCAS compute pass with proper format handling and synchronization. Output sharpening is already available independently. |

Host/rendering owner can implement these separately. Do not copy profile addresses without verifying NSO build IDs, replace Swap2's render files wholesale, or add a UI switch whose environment key has no reader.

## Verification

Final test APK: swap2-native-0.1.2-beta-graphics.apk, `com.swap2native.test`, version `0.1.2-beta`, code 3, 43,624,378 bytes. SHA-256: `e1a0ba0b0f696b14f2f99a61ea191531ab1d2dc253c4c6253d287f732bc900ff`. Signature verification passed with the existing Android test certificate. Not installed or published in this task.

Subsequent user-requested release update: swap2-native-0.1.2-beta.apk, package `com.swap2native`, version `0.1.2-beta`, code 3, 41,233,586 bytes. SHA-256 `497001b685b0248bda5b17c20abcc2a302bcf977e896b2459432a0f6e9e2764a`. Built with `nxLive=true` (not debuggable) and signed with the original release key, certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1`, matching the installed main application. Installed successfully with `adb install -r` on POCO serial `98d66058`, without uninstalling the main package. Native packer stage was reconstructed byte-for-byte from the verified test APK's native libraries/Python archive after transient WSL share access interrupted the first release build. The temporary build certificate was replaced by the original release signature before installation and independently verified. No GitHub publication.

- `GraphicsModeJvmTest`: named mode mapping, final FSR selection, nearest presentation filtering, sharpness Off, invalid mode fallback: passed.
- `test_ndsr_standin.Header`: generated SPIR-V matches GLSL and both shader variants validate: passed.
- `test_ndsr_standin.Lavapipe`: passed on software Vulkan, including new nearest checks for both color encodings and float/B10G11R11/RGBA8 stores, plus existing EASU/bilinear/mapping checks.
- Full native-preparer Android APK compilation is recorded in `output/build/012-beta-graphics-gradle.log`.
- Five-game gameplay regression harness remains unavailable on this Windows/Linux workstation because it imports macOS `libproc.dylib` (see the previous [build report](BUILD-BANANANATIVE-v0.2.5-2026-11-09.md)). No device gameplay, FPS improvement or driver stability result is claimed for these settings.

## Device checks

### Unreleased: 540p resolution

Added 960x540 after the existing resolution entries, preserving their order and saved values. Available as a global default and per-game selection for profiles with `window_fn`/`res_levels`, or an explicit 960x540 graphics-scale mapping. The existing host accepts the dimension through `DM_WINDOW_SIZE`; `res_levels` treats it as a cap, so the actual render size can be lower or the nearest supported level.

Mario Kart World's profile currently has only verified 1080p/720p graphics scales. Its selector omits 540p; inheriting a global 540p choice resolves to its existing 720p mapping and displays that effective inherited size. No unverified scale or build-specific binary patch was introduced. This source change is not part of the already published 0.1.2-beta APK.

1. Open global Performance and a game's Graphics page in both themes and languages. Verify selection labels, inheritance, and restoring only one game's defaults.
2. In DK/Zelda, compare all four scaling modes; verify the nearest mode really stays pixelated and native restores the network. Check ordinary and rotated presentation.
3. At a window size smaller than the panel, compare output FSR sharpness Off/default/max. Internal NDSR-only enlargement does not use this sharpness setting.
4. Restart after choosing Stretch/Fit, and verify picture coverage, touch coordinates and saved control placement.
5. Restart after changing GPU timing or background pipelines. Confirm startup environment/logs and cache-cold drawing; compare stability with background pipelines off.
