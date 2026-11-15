# Swap2-Native 0.1.7-beta

Status: published 2026-11-13 as a GitHub pre-release, https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.7-beta (tag on the repository's `main`, as 0.1.2-0.1.5 were; the sources are the release's source zip, not commits). Installed over the user's `com.swap2native` the same day (adb install -r: versionCode 7 -> 8, first install time and data kept).

## Artifacts

| File | SHA-256 |
|---|---|
| `swap2-native-0.1.7-beta.apk` (com.swap2native, versionCode 8) | `d311832379bb29910863e7f209ad180182077b2bc5420b68fd4befbc9ed6a9b3` |
| `swap2-native-0.1.7-beta-source.zip` (3,846 files) | `31880f84a38ca81dc63b3858fff5c46f8b9f2c2da33af96143833f4d3051b42b` |

Release certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` (the same as 0.1.6-beta). Built with `output/build/build-public-017.ps1` (the password typed by the user in its terminal, not kept); sources packed by `output/build/package-release-017-source.py`, which checks the APK's embedded preparer against the zip byte for byte. The zip was scanned for keys, saves, game files and personal data before upload: none.

## Contents

The release notes are `output/build/release-notes-0.1.7-beta.md`, as published. Zelda's shader library is Vulkan chain `4e12942060ff`: `dm/games/0400486024DEC000/vk_delta.tsv` (written by `vk_android_build.py --publish-delta`) has `from` rows for `73e7b3fa3bae` (0.1.6: 2,633 shaders to translate on the phone), `c5f547257060`, `96979a7c89ff` and `00d11bfeab4a`.

Held back on purpose: the profile's `fps_target 60` is commented out. The frame-step stub (`patches.py`) is in, inert at target 0. With the cap at 60 and a pack whose main was rewritten before the stub, the game would run at twice real time. Turn it on after a phone check of the "game time ... of real time" line.

## Checks before publishing

- Test APKs 15-20 on the POCO F7 Ultra (Adreno 830, Qualcomm 891.14), library `4e12942060ff`: 200 s at Link's house door without a crash, 26-27 fps (20 when hot), glow on Link in 1 of 120 captured frames.
- Frame generation found `Lossless.dll` in `/sdcard/LSFG` and presented 47 fps from 24.
- `t_android_texlife` (built with the NDK, run on the phone) passed with the new serial map.
- Host unit tests `t_android_smap`, `ubq`, `rcix`, `qsched` passed (WSL).

## Known

- The one-frame white flashes on Link's clothes and small spots in the scenery on Qualcomm (`docs/ANALISE-FRAMETIME-2026-11-13.md` "Brilho no Link").
- Turnip V39-patched loses the device about 130 s in.
