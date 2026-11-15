# NUP support (branch android-nup)

Play and prepare a game straight from one `.nup` file (github.com/a55uka/nup, SPEC.md at commit 92089de3, format 1.0-draft),
with no unpacking. The `.nup` is only ever read: it is the user's dump and is never modified. The rewritten code goes into the
pack as it does for a folder.

## The format in one paragraph

Superblock (0x80) at 0, then the TOC (packages, dirs, files, children, string pool; SHA-256 in the superblock), then the
blob region. A package is `base` (kind 0), `update` (1, one per version) or `addon` (2, own title ID); each has an
`exefs_dir` and a `romfs_dir` (0xFFFFFFFF = none). Each file is one extent, 16-byte aligned, codec 0 (raw) or 1 (one zstd
frame for the whole file, content size in the frame header). Equal files may share one extent. An update package holds a
full RomFS (as `updates/<version>/romfs` does in a folder dump): no layering is needed.

## Naming a package: `<file>.nup:<selector>`

Everywhere a path to a RomFS (or an ExeFS) is given, a `.nup` package is named as the file's path, a colon, and:

| selector | package |
|---|---|
| `base` | the base package |
| `update/<decimal version>` | the update of that NCM version, e.g. `update/458752` |
| `addon/<16 hex digits>` | the add-on of that title ID (lower case), e.g. `addon/040074a01bf13001` |

The path is split at the last `.nup:`. Without a selector (`game.nup`), the RomFS is the highest-version update's, or the
base's when there is none. A selector naming no package is an error.

A package stands where a package folder of a folder dump would (`base/`, `updates/<v>/`, `dlc/<tid>/<n>/`), and a path inside
it may follow the selector, as inside that folder: `game.nup:update/458752/romfs`, `game.nup:update/458752/exefs/main`,
`game.nup:addon/040074a01bf13001/romfs`. The selector's own form is fixed (`base`, `update/<digits>`, `addon/<16 hex>`), so
the rest after it is unambiguous. Tools take these wherever they take the matching folder path. The host's `DM_ROMFS` takes
`<file>.nup:<selector>/romfs` (also `<file>.nup:<selector>` and plain `<file>.nup`, both meaning that package's RomFS), and
`pack.tsv` writes the full `/romfs` form.

## Host (dm/host/hle_fs.c, dm/host/nup_image.h)

- `DM_ROMFS` may be `<file>.nup` or `<file>.nup:<selector>`. The mount reads the TOC, checks it, and maps the chosen
  package's `romfs_dir` subtree onto the same in-memory index a RomFS image (DMROMFS1) uses, so lookups, listings and
  sizes are shared code.
- Reads: a raw file is read with pread at its extent. A zstd file is decompressed whole into memory when the game opens
  it, held until it is closed, then kept in a least-recently-used cache of `DM_NUP_CACHE_MB` (default 128). No cache on
  disk: in Donkey Kong Bananza the compressed files are 5.6 GiB that zstd shrinks by only 13 %, so a disk cache would give
  back what compression saved; the largest is 53 MiB and only 8 are over 16 MiB (one decompresses in tens of ms).
- zstd: `DM_NUP_ZSTD` (android/host/CMakeLists.txt, the unit tests) links dm/third_party/zstd. The Mac and iOS hosts are
  built without it: raw files read, a compressed file fails to open with a red line.
- Tests: `make -C dm/host/tests GAME=<id> run` (t_fs "NUP container": a .nup written in C from the spec). Against a real
  dump: `build/t_nup_dump <file>.nup:<selector>/romfs <that package's RomFS folder>` reads every file through hle_fs.c and
  compares it (DKB update 458752: 77,342 files, 8.55 GiB, all equal; base and DLC too).
- `DM_AOC_DIR/<index>.nup` (a file or a symbolic link to the `.nup`): add-on content `<index>` is the addon package whose
  title ID's low 12 bits equal `<index>` (pack_android.py's index is `tid - app - 0x1000`, the same number). Checked
  before `<index>.img` and `<index>/`.

## Tools (Python: dm/tools/nup.py and its users)

- `dm/tools/nup.py`: reader. Checks a `.nup` per SPEC §6.1 (TOC SHA-256 before any TOC value is used, bounds, no overlapping
  extents but shared ones, sorted acyclic directories; unknown codec, kind, flags refused), lists packages, resolves selectors,
  and gives walk / list / stat / read and seekable streams. zstd: in memory up to 64 MiB, streamed above that, a temp file in
  `$DM_TEMP` only when a caller needs a descriptor for mmap. CLI: `packages`, `ls`, `cat`, `verify`.
- `dm/tools/romfs.py` (`Path`, `open`, `os`, `mmap` for paths inside images) is the access layer: a path with `.nup:` goes to
  a NUP view, so `shader_split.py`, `romfs_image.py` and `pack_dump.py` (scan, pack_version, pack_dlc) take `.nup` paths with
  no code of their own. `<file>.nup:` with an empty selector is the container root (folders `base`, `update/<v>`,
  `addon/<tid>`); a path naming a missing package is not found.
- zstd module: `compression.zstd` (Python 3.14: bananative-env on the Mac, the phone's CPython 3.14.6), else
  `backports.zstd`; imported at the first compressed file, so a raw-only `.nup` needs none.
- The rewriter reads plain files: `pack_android.py` copies a NUP module into the pack's `.nup-src/<module>/` for
  `patch_nso.py` and deletes it after (`rewriter_src`). `nso.py` and the other `REWRITER` files are unchanged, so the rewrite
  key is the same for a folder and a `.nup` of it, and packs made before this branch keep their modules.
- `pack.tsv` written for a `.nup` game: `romfs` = `<name>.nup:update/<version>/romfs` (or `:base/romfs`), and each `aoc`
  line's path = `<name>.nup:addon/<tid>/romfs`, relative to the folder that holds the `.nup`. The pack goes to
  `<title ID>-work/android-pack` next to the `.nup`, as next to a game folder.
- A `.nup` has no `.listing` files (SPEC §6.3), so its file count differs from a folder dump's (DKB update: 77,342 against
  80,273); the shader split's output is the same.
- `dm/tools/nup_pack.py`: a small clean-room writer, for test fixtures (a folder dump → `.nup`), not for users.
- Tests: `python -m unittest dm/tools/tests/test_nup.py` (bananative-env). On DKB, `pack_android.py` from the folder and from
  the `.nup` (`--in-process`, the phone's path) give byte-identical modules, tables and keys; `shader_split.py` gives the
  same package and tables (22,852 shaders).

## App (android/app)

- The extension is case-insensitive (`.nup`, `.NUP`, `.Nup`); the existing readers accept it without renaming the dump.
- Add game asks "a folder" or "a .nup file". The file picker selection is resolved with `PathUtil.documentToPath(Context, Uri)`:
  local document IDs first, then MediaStore IDs when a local path is available. Previously MainActivity used the context-free overload,
  so the MediaStore lookup could not run for some Recent/Downloads selections. Cloud-only files and providers without a readable local
  path still require downloading locally and browsing to the file through Internal storage. The app does not copy the dump automatically.
  A picked folder holding `.nup` files (or dump subfolders) adds
  all of them (`PackFind.gamesIn`; a title without a profile in this APK is named in the summary). adb: `--es game <file.nup>`, or
  `--es add_game <file.nup or folder>` for Add game's own path. A `.nup`'s pack is looked for beside it and in `Work/<title ID>/<any name>`
  and `<title ID>-work/<any name>` of its folder and up to three folders above it (`PackFind`; on the phone a `.nup` is in
  `Switch2/Games`, its work folder `Switch2/Work/<title ID>`: [README.md](README.md#the-switch2-folder)).
  If the picker's ID maps to a path where no file exists, the app reads the file through the picker's permission
  (`PickedNup`). It does not report an access problem in that case.
- The library's own search (`LibraryScan`, `MainActivity.libraryScan`) runs once All files access is on. It runs at the launcher's
  start and on every return to it, off the main thread and after the Switch2 folder's move (`Switch2Migration`). It creates
  `Switch2/Games` when it is missing, then looks in the Switch2 folder (its `Games/`, one level of subfolders there, and the root of
  the older layout) and in the library folder last picked. Each game with no card yet goes through Add game's checks with no dialog.
  A toast names the games it added and the ones it could not add; **Refresh** (also shown in the empty library) checks everything
  again and its summary gives each reason. The search does not check these again: a game it refused, until the file's size or
  time changes; a game the user removed from the list (Add game and Refresh still add it). A file changed in the last 30 seconds,
  or a `.nup` shorter than its header says (`Nup.Incomplete`: still being copied, or cut at 4 GB on a FAT32 card, which must be
  exFAT), waits for the next look.
- Add game, the search and Refresh run Play's own dump check (`DumpCheck.identify`) when a game has no usable pack: another
  version or build, the base alone where the profile needs an update, or an update-only `.nup` without RomFS. Such a game is
  refused with that sentence: one game gets a dialog, several get a line each in the summary. Before this, the card appeared
  and only Play refused it.
- Base and update in two `.nup` files of one title in the same folder: if the update `.nup` holds the whole game (its own ExeFS
  and RomFS), it is added and the base-only `.nup` is noted as not needed. Picking the base `.nup` alone adds the update `.nup`
  beside it instead. **Open:** combining a base `.nup` with an update `.nup` whose RomFS needs the base. The host, the tools and
  Prepare take one file per game today, so such an update `.nup` is refused with the reason ("make one `.nup` of the game with
  that update installed").
- `Nup.java` reads the superblock and TOC (SHA-256 checked, cached by path, size and mtime), `Zstd.java` is a pure-Java zstd
  frame decoder (RFC 8878, no dictionaries) for main.npdm, NSO build IDs and the rewrite key check: the launcher process
  loads no native code.
- GameActivity: `DM_ROMFS` = the game's own `.nup` + pack.tsv's selector + `/romfs` (only the selector is taken, so a
  renamed `.nup` still plays; a pack made from a folder falls back to `update/<pack version>` or `base`). `DM_AOC_DIR/<index>.nup`
  links to the game's own `.nup` when it holds that add-on, else to the `.nup` the aoc row names, else the game's own.
- Prepare on the phone (PackService) passes `<N>` (the `.nup`) to pack_android.py and `<N>:update/<v>/romfs`,
  `<N>:update/<v>/exefs/main` (`<N>:base/…` for version 0) to shader_split.py. It needs a version with its own `exefs/main`, an `exefs/main.npdm` and a RomFS (its own or the base's): a base and an update, a base alone when the profile's build is the base game, or an update alone that carries the whole ExeFS and RomFS (a "merged" dump: `DumpLayout`, `Pack.isDump`); DumpCheck checks the TOC hash,
  the profile's version, main / main.npdm / RomFS and each module's build ID.
- Readiness: a "Game files" row per game (a `.nup` must open and pass its TOC hash).
- The replayable [Getting started guide](../docs/SWAP2-NATIVE-TUTORIAL.md) explains local storage, file versus folder selection,
  supported game builds and preparation. NUP import errors retain their actual diagnostic and offer Choose again or the guide.
- Tests: `NupJvmTest` (javac + java, no Gradle runner yet; see its header): 293 checks on a synthetic `.nup`, 30 on the
  real DKB one. `LibraryScanJvmTest` (`dm/tools/tests/test_android_library_scan.py`) covers the library's search, the
  incomplete-copy test and Add game's dump check on `.nup` files made by `nup_pack.py`.

## Test data

Tested with a `040074A01BF12000` dump (base, update 458752, add-on `040074a01bf13001`) as an official `.nup` and as a folder,
and a `0400C3F00006E000` 1.8.0 dump (base only) as a `.nup` written by `dm/tools/nup_pack.py` (2026-11-05, on the phone: both
reach their level from the `.nup` with no Prepare). Dump folders and test `.nup` files belong on the dump drive, not in the
repository; a `.nup` is a full-size copy of a dump, so mind the free space before writing one.
