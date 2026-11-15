# Swap2-Native 0.1.7.1-beta

Status: published 2026-11-13 as a GitHub pre-release, https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.7.1-beta (tag on the repository's `main`, as 0.1.7-beta; the sources are the release's source zip, not commits; notes `output/build/release-notes-0.1.7.1-beta.md`). Installed over the user's `com.swap2native` the same day (adb install -r: versionCode 8 -> 9, first install time and data kept).

A fix release of [0.1.7-beta](BUILD-0.1.7-beta-2026-11-13.md): the same sources plus the change below (`dm/tools/patch_nso.py`, `scripts/pack_android.py`, docs). No other source file changed after 0.1.7-beta's APK was built (checked by modification time against it).

## Artifacts

| File | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.7.1-beta.apk` (com.swap2native, versionCode 9, versionName 0.1.7.1-beta) | `c62d4711fabb8772662f2053bd8dfd892909503cddeea0284285d0129c96da0a` |
| `swap2-native-0.1.7.1-beta-source.zip` (3,849 files; `output/build/package-release-0171-source.py`, checked by `output/build/scan-release-zip.py`: no keys, personal data or game files; the only `.nso` are the fuzzer's two synthetic inputs of 256 and 434 bytes) | `160ec685cfbe1e804742230d3e033d54bd0548371009b29dcc2dfb4b5e145c99` |

Release certificate SHA-256 `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1` (the same as 0.1.6 and 0.1.7). Built with `output/build/build-public-0171.ps1` (the password typed by the user in its terminal, not kept). The APK's `assets/pack/tools.zip` holds `patch_nso.py`, `pack_android.py` and Zelda's `patches.py` byte for byte as in the tree.

## The fix

With 0.1.7-beta, Prepare of Zelda stopped at the rewrite step for a player with the Dynamic-fps exefs mod on (`frame step original bytes mismatch 0x6e6cc4: 4f7dd594`): the mod hooks the instruction the profile's frame-step stub replaces. The rewriter now leaves the frame step out when an exefs mod changed its site ([Frame-step site changed by an exefs mod](reference/rewriter.md#frame-step-site-changed-by-an-exefs-mod)). Checked on the PC with the v131072 main and the user's mod: with the mod named, the rewrite finishes without `FPS_STATE_OFF` and keeps the mod's cave and its `cntfrq` gate; with the same image and no mod named, it still stops; the plain main still gets the frame step. `test_pack_android`, `test_nup`, `test_android_rewritekey`, `test_exefs_patches` passed.

The rewriter's change gives every module a new rewrite key: the first Prepare after installing rewrites main, sdk and subsdk0 (a few minutes); the shaders are kept.
