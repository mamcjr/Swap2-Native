# Swap2-Native 0.1.6-beta: task/mesh release

Status: release signed with the original application certificate and installed over `com.swap2native` on 2026-11-11. No uninstall or data-clear command was used. The package retains its first installation date, 2026-11-08; the launcher still lists the four registered games. Live Zelda startup/opening/closing acceptance is in progress.

## Included changes

- Task/mesh execution through compute, task payload distribution, bounded generated geometry and a pulling vertex stage on devices without native mesh shading. The implementation includes helper-function invocation-state handling and subgroup-plan initialization for standalone library checks.
- Conservative local-load forwarding prevents the reproduced Turnip compiler failure without requiring an Android optimizer dependency. Zelda's full library contains 42,847 shaders and 53,881 modules, including the 508 previously missing keys and 520 total task/mesh entries. Vulkan chain `73e7b3fa3bae`, ABI `623c0a2f1e8a`, inputs `1c3c0cb61d66`.
- Optional bounded textual `VK_EXT_device_fault` diagnostics and immediate loss handling before further fallback timeline signals; draw/dispatch/upload guards avoid continuing normal GPU work after loss.
- Capture/replay preservation of GPU placement (`CAP_PLAC`), independent of CPU pool registration. This fixes the separate replay-only missing-address-map defect that had made a foliage task hang.
- Preparation snapshot/job-identity correction for imported completed shader libraries. Pokopia's existing imported library and previous Zelda library versions were retained.

## Verification

The selected mesh shader that previously crashed Turnip compiles after the local-temporary transformation. Eleven targeted SPIR-V/unit cases passed; every module in the full Zelda build was validated. Comparison against `f26049265575` shows only the 520 task/mesh entries changed; all 42,327 ordinary entries and all non-compute variants are identical. The profile manifest was published from that completed build, without a false equivalence/restamp row.

The final host passed 160 GPU fixture cases on Qualcomm and 160 on Turnip, with zero pipeline failures. Native Android tests passed for shader-library compatibility, SPIR-V structural rewriting and the capture format (86 checks). Five launcher flow tests passed. The Android structural test has no local `spirv-val` executable; generated shader validation ran offline. The Mac regression harness is unavailable on this Windows/Android machine; previous documentation and stale other-game-manifest failures are not reported as clean full-suite results.

The full production-library replay completed without SPIR-V overrides or omitted mesh draws after restoring two ranges absent from the legacy capture's placement metadata. It recorded 2,786 conventional draws, zero skipped, and 154 ordinary dispatches. The user confirmed the closed door in the full replay image. This is visual confirmation, not a passing strict pixel comparison against an older capture that lacked the geometry. Bright-window artifacts and purple-glow investigation remain outside this correction.

## Artifact and installation

APK: `android/app/out/swap2-native-0.1.6-beta.apk`, normal package `com.swap2native`, version code 7. The packaged host matches the stripped build and carries the current chain/ABI; the embedded preparer contains the exact current `vk_android_mesh.py`.

Certificate SHA-256: `9d6f1e98632da3def8699c908acf8a1387257a35ef58c46c25d7388f34c7c0f1`, verified before `adb install -r --no-incremental`, which returned `Success`. The signing password was entered only in the local PowerShell window and not retained. This release has not been published to GitHub.

Post-install launcher check: four registered games remain; first installation time is unchanged. Zelda reports changed mods and Play starts the existing code-rewrite flow before launching. The new shader library is already present; the imported Pokopia library was not replaced or removed. Live game validation waits for that code step to complete.

See the [door investigation](ANALISE-ZELDA-DOOR-2026-11-10.md) for diagnostic history, exact artifacts and pending live acceptance. The BananaNative candidate matrix distinguishes implemented diagnostics from scheduler, texture-feedback and UI work that is still separate.
