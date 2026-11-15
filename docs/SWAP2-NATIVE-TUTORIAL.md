# Getting started and NUP import assistance

Implemented 2026-11-08 in `LauncherTutorial.java`, integrated by `MainActivity`. This is native launcher UI, with English and Portuguese resources. No online service, generated media, game code or shader changes are involved.

## Entry points and flow

- An ordinary cold launcher start with an empty saved library introduces the guide once, after the opening finishes (or immediately when the opening is disabled). The `tutorial/introduced_v1` preference records that it was offered. Existing libraries and direct frontend/game intents are not interrupted.
- The empty library offers Add game and Getting started. Add game also includes a guide button. Settings → About → Getting started reopens it at any time.
- Four steps cover local game files, the file/folder picker, preparation and recognition failures. Previous/Next, Later, Android Back and the final Add game action keep the guide optional. Text scrolls within its dialog on short windows and with larger fonts.
- Choosing the NUP import path shows a short explanation before the system picker: select the file itself, finish the download, do not extract it, and browse Internal storage if Recent/Downloads cannot resolve it. Switch2/Games is a suggestion, not a required path.
- The guide never starts preparation, changes permissions, moves files or launches a game automatically. Final Add game enters the existing permission and import flow.

## Recognition correction

The existing context-aware resolver could query MediaStore for a local path, but MainActivity called its context-free overload. MainActivity now supplies its context so that resolution is available. Details and supported NUP forms are maintained in [android/NUP.md](../android/NUP.md#app-androidapp).

Removed MainActivity's obsolete `Nup.wrongCase` calls: the current reader already accepts mixed-case extensions and no longer exposes that helper. Do not ask users to rename valid uppercase NUPs. Invalid formats must still be rejected: renaming ZIP/NSP/XCI files is not conversion.

For a direct NUP selection, unresolved paths, wrong file types, unreadable files and identification/profile failures now offer Choose again, Getting started and Cancel. Preserve the original identification/profile error beneath the explanation. The file remains unchanged; all normal title/build/integrity checks still apply. Batch folder-import summaries keep their existing behavior.

## Validation

- Android resource processing and debug Java compilation passed with the installed SDK and `-PnxPackEngine=fake`, offline. These tasks validate launcher resources/Java; they do not produce or install an APK and do not exercise the native preparation engine.
- Two frontend tests passed with explicit UTF-8 for the Windows JDK. This includes the existing document-ID/path resolution cases.
- Seven existing NUP reader/integrity tests passed (package/codecs, folder equivalence, streams, rewriter source copies, raw-only reads and damaged-container/frame rejection), separately from the platform-dependent tests.
- The initial full frontend/NUP run did not pass on Windows: the Java source encoding default affected accented names, uppercase/lowercase fixture copies collided on the case-insensitive filesystem, virtual selector path assertions expected Unix separators, and one packer test required `cp`. The UTF-8 frontend retry passed. This change does not modify those test harnesses or the host/packer to hide platform failures.

Device smoke check, later on 2026-11-08: built the complete native-preparation test APK (`android/app/out/swap2-native-test.apk`, SHA-256 `a48fe3c1f0c59eb34ed67109073e294eb5d44cb342abeec561ddc38134854aaf`) and installed it as an update on the POCO F7 Ultra (`com.swap2native.test`). The public app was left unchanged. The launcher opened and Settings → About → Getting started displayed step 1 in Portuguese with visible navigation buttons. [Installed screen](../output/analysis/tutorial-installed-20261108.png). Full import and tutorial-flow acceptance remains pending:

1. Fresh empty library: opening then guide once; Later/Back returns to usable library; reopen from empty state and About. Existing populated libraries and frontend launch intents show no unsolicited guide.
2. English/Portuguese, both themes, landscape/portrait, large fonts, touch and gamepad: all four pages and dialog actions remain accessible. Rotation during the guide must not add a second dialog.
3. Pick the same valid local NUP through Internal storage, Recent and Downloads, including a provider backed by a MediaStore ID. Verify successful addition and the unchanged read-only file. A provider without a local path must give recovery guidance rather than imply every Recent/Downloads item is unsupported.
4. Select `.NUP`/`.Nup`, a canceled picker, a cloud-only item, an unreadable file, a ZIP renamed to NUP, a truncated/corrupted NUP, and an unsupported title/build. The correct validation remains in force; failure offers retry/help without starting preparation.
5. Add one NUP and a folder containing multiple games; verify existing cards, preparation status, saves and files remain intact. Start preparation explicitly and confirm normal pause/resume behavior.
