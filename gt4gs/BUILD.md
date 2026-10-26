# Building gt4-gs.exe (Windows x64)

Releases are built and published from the owner's PC with `gt4gs/release-local.ps1` (owner decision 2026-10-16,
see "Release flow" below). The CI workflow `.github/workflows/gt4gs-windows.yml` is a manual fallback
(`workflow_dispatch` only) and runs the recipe below. Anyone can rebuild a release from its tag with this file; a
build from the same tag should produce the same file list; bit-identical output is a goal, not a promise.

## Release flow (local)

1. Commit, tag `gt4gs-v<version>` and push the branch and the tag.
2. Check the tag out (clean tree) and run, from the repository root:
   `powershell -File gt4gs\release-local.ps1 -Tag gt4gs-v<version> -Build <empty build dir> -Deps <deps dir> -OutDir <empty dir>`
   (`-Release` for a full release instead of a prerelease, `-Draft` for a draft release, `-NoPublish` to stop before publishing). For a tag made
   before the script existed, run a copy of the script from outside the tree (`git show gt4gs/window:gt4gs/release-local.ps1`).
3. The script refuses a dirty tree or a HEAD that is not the tag, builds `pcsx2-gt4gs` with the local recipe below,
   packages the CI zip layout (`gt4-gs.exe`, DLLs, `resources/`, `COPYING.GPLv3`, `NOTICE`, `README.md`, `SOURCE.txt`
   with tag, commit, compiler, CMake and Ninja versions and the dependency script hash), makes the source archive with
   `git archive` of the tag (GPLv3 6(d)), writes `SHA256SUMS` and runs `gh release create` with the three assets.
4. GT4Recomp pins the published zip: download it back, check it against `SHA256SUMS`, extract and pin it.

## Tool versions

- Runner image: `windows-2025` (GitHub-hosted). The exact image version is in the job log of each run.
- Visual Studio Build Tools with the MSVC x64 tools (MSVC, `vcvars64.bat`), located with `vswhere -latest`. The runner's installed version is in the job log (the first run used VS 18, MSVC 2026, Enterprise).
- CMake and Ninja from the runner image. The versions used are printed by the step "Tool versions".
- Git for Windows (provides `bash.exe` and `patch.exe` used by the dependency script), 7-Zip.
- Dependency versions are pinned inside `build-dependencies-gt4gs.bat` with SHA-256 checks for every archive.
  The script is upstream's `build-dependencies.bat` with the Qt block and the Qt downloads removed (Qt is not
  needed when `ENABLE_QT_UI=OFF`). FFmpeg is built with `BUILD_FFMPEG=1`, as in upstream.

## Recipe

From the repository root, in a `cmd` shell:

```
.github\workflows\scripts\windows\build-dependencies-gt4gs.bat
call "<Visual Studio install>\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF -DENABLE_GT4GS=ON -DGT4GS_ONLY=ON -DENABLE_TESTS=OFF ^
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DDISABLE_ADVANCE_SIMD=ON ^
  -DCMAKE_PREFIX_PATH=%cd%\deps
cmake --build build --target pcsx2-gt4gs --config Release
```

Dependencies are built with `DEBUG=0` and `BUILD_FFMPEG=1`. The dependency directory `deps` is cached in CI
keyed by the hash of the dependency script and `common/shaderc-changes.patch`. A cold dependency build takes
roughly 40 to 90 minutes; a cached run skips it.

Package (same script CI uses):

```
powershell -File .github\workflows\scripts\windows\package-gt4gs.ps1 -Version <ver> -Build build -Deps deps -OutDir dist
```

This writes `dist\gt4-gs-<ver>-windows-x64.zip` and `dist\SHA256SUMS`.

The target `pcsx2-gt4gs` (option `ENABLE_GT4GS`, default OFF) builds `gt4-gs.exe`. Configure fails
when a file in `3rdparty/gt4recomp-gs-stream/` differs from its `SHA256SUMS` entry.

`GT4GS_ONLY` (default OFF; on for every release since 0.4.0) builds the JIT-free core: see "No JIT" below. It
requires `ENABLE_GT4GS=ON`, `ENABLE_QT_UI=OFF`, `ENABLE_GSRUNNER=OFF` and `ENABLE_TESTS=OFF` (configure stops
otherwise), because a core without recompilers cannot run games.

## Local build (owner machine, task 036)

Used for iteration on `gt4gs/live`; the CI recipe above stays the reference for releases.

- Dependencies: the same script into a scratch root (`DEBUG=0`, `BUILD_FFMPEG=0`). PCSX2 v2.8.2 compiles against
  its bundled FFmpeg headers on Windows and loads FFmpeg at run time (video capture only), so FFmpeg is not
  needed to build or run the GS; a package built this way has no FFmpeg DLLs. Time on a Ryzen 7 5700X: about
  15 minutes (shaderc dominates); 44 MB installed.
- The script must have CRLF line endings when run by `cmd` (with LF endings `call :label` jumps go wrong and
  downloads are skipped). Git for Windows checks it out with CRLF when `core.autocrlf=true`.
- `vswhere` needs `-products *` to find a Build Tools-only install (fixed in the script on `gt4gs/live`).
- Configure and build: Visual Studio 2022 Build Tools (MSVC 14.44), CMake 4.3, Ninja 1.13, `Release`,
  `-DDISABLE_ADVANCE_SIMD=ON`, without `CMAKE_INTERPROCEDURAL_OPTIMIZATION` (faster). Cold build 751 steps in
  about 10 minutes; `gt4-gs.exe` about 9.9 MB.
- Package with `package-gt4gs.ps1` as above. Local and CI packages are not bit-identical (different MSVC and
  LTO settings); the GT4Recomp launcher pins the exact package it uses.

## Local build (task 037/038, release 0.3.0)

Same recipe as task 036 into a fresh build directory (GT4Recomp `private/slice194-gt4gs-build`, dependencies of
`private/slice193-gt4gs-deps` reused): cold build 751 steps in about 6 minutes, incremental rebuilds after the
GS stream changes 3 to 10 steps. Package `gt4-gs-0.3.0-windows-x64.zip` with `package-gt4gs.ps1 -Version 0.3.0`.
Release 0.3.1: fresh build directory `private/slice195-gt4gs-build` for iteration; the published 0.3.1 was built
by `release-local.ps1` from the tag checkout into `private/slice195-gt4gs-release-build` (first local release).
Release 0.3.2 (task 047): iteration build `private/slice196-gt4gs-build`; the published 0.3.2 is built by
`release-local.ps1` from the tag checkout into `private/slice196-gt4gs-release-build`.
Release 0.4.0 (task 042, JIT-free): iteration build `private/slice197-gt4gs-build` (`GT4GS_ONLY=ON`, 712 steps cold);
the published 0.4.0 is built by `release-local.ps1` from the tag checkout into `private/slice197-gt4gs-release-build`.
The local dependency build has `BUILD_FFMPEG=0`, so local release zips have no FFmpeg or x264 DLLs (gt4-gs.exe does
not import them; PCSX2 loads FFmpeg only for video capture).

## Renderers in this build

- Vulkan, D3D11 and D3D12 are compiled in on Windows. OpenGL is compiled in too (`USE_OPENGL` default ON)
  and is not offered by the GT4Recomp launcher.
- Since 0.4.0 (`GT4GS_ONLY=ON`) there is no recompiler and no software JIT. The software rasterizer (the software
  renderer and the HW renderer's CPU sprite and CPU CLUT render) runs the C functions of `GSDrawScanline.cpp`.

## No JIT (task 042, release 0.4.0)

`GT4GS_ONLY=ON` defines `GT4GS_NO_JIT` for `common` and the PCSX2 core and:

- does not compile the EE and IOP recompilers, microVU (VU0/VU1), the VIF unpack recompilers (`pcsx2/x86/*`), the
  x86 emitter (`common/emitter/*`) or the GS software JIT code generators (`GS*CodeGenerator.all.cpp`); the xbyak
  include path is removed, so any remaining xbyak use fails to compile. `pcsx2/GT4GSNoJit.cpp` defines the two
  recompiler entry points still named by the core (fastmem backpatch: stops the process; microVU save state block:
  empty, as on ARM64);
- selects the interpreters (like the ARM64 build: `REC_VU1`/`THREAD_VU1` false, interpreter opcode tables, VIF
  unpack interpreter loops) and writes the recompiler settings off;
- reserves no recompiler code area (`Memory.cpp`), and `HostSys` refuses any executable page protection
  (`pxFailRel`);
- uses the C rasterizer (`ENABLE_JIT_RASTERIZER` undefined, no code buffers);
- enables Windows Arbitrary Code Guard (`ProcessDynamicCodePolicy`, `ProhibitDynamicCode`) in `gt4-gs.exe` before
  the window, the VM and GS threads or the graphics driver exist, checks it with a probe allocation that must fail (error 1655), and
  at the end of a stream and at exit walks the address space and logs the executable regions that are not part of a
  loaded image (expected: 0). `[GT4GS] ProhibitDynamicCode = false` switches the OS enforcement off (for a graphics
  driver that needs dynamic code in our process; the binary stays JIT-free).

`gt4gs/check-nojit.ps1 -Path <package>` is the static check (code generator markers, writable and executable
sections, NX flag, imports); `release-local.ps1` refuses a package that fails it.

## Release 1.0.0 (tasks 060, 061, 062): the first stable release

Same recipe and options as 0.4.0 (`GT4GS_ONLY=ON`, Arbitrary Code Guard on). New at configure time:
`pcsx2-gt4gs/CMakeLists.txt` reads `3rdparty/gt4recomp-gs-stream/gs-settings-allowlist.json` (checked with the other
vendored files against `SHA256SUMS`) with CMake's `string(JSON)` and writes `gt4gs_settings_allowlist.inc` into the
build folder; `Main.cpp` includes it. `pdh.lib` (Windows performance counters, for the overlay's GPU usage) is linked by
`pcsx2/GSStreamOverlay.cpp`. Released with `release-local.ps1 -Release` (a normal release, not a prerelease).

## Release 1.0.1 (task 044 follow-up): the data root only

Same recipe, options and dependency script as 1.0.0 (the script hash is unchanged, so the same cached dependency
folder can be passed with `-Deps`). The change is in `pcsx2-gt4gs/Main.cpp`: `VMManager::Internal::LoadStartupSettings()`
no longer runs in `InitializeConfig`. It runs in `main()` after `[GT4GS] DataRoot` is applied (stream modes), or right
after the arguments for the GSRunner dump replay. Released with `release-local.ps1 -Release`.

## Release 1.1.0: GT4 crop, Stretch, protocol 1.3

Same recipe, options and dependency script as 1.0.1 (`GT4GS_ONLY=ON`; the same cached dependency folder works with
`-Deps`). Code changes: `pcsx2-gt4gs/Main.cpp`, `pcsx2/GSStreamReplayer.{h,cpp}`, `pcsx2/GS/Renderers/Common/GSRenderer.cpp`
(see NOTICE). New consumer test, run against a built `gt4-gs.exe` and any `.gscap` v2 capture of at least 1,300 frames:

```
python gt4gs\tests\test_display_aspect.py --gt4gs <build>\pcsx2-gt4gs\gt4-gs.exe --gscap <capture.gscap> --work <empty dir>
```

(`gt4-gs.exe` needs the DLLs and `resources/` of a package next to it; copying the new exe into an extracted 1.0.1
package works because the dependencies are the same.) Released as a draft with `release-local.ps1` (published by the
owner after the in-game test).

## Verifying a release

1. Check the zip against `SHA256SUMS` (`sha256sum -c SHA256SUMS` on Linux or Git Bash).
2. Read `SOURCE.txt` in the zip: it names the tag and commit. Check out that commit and build it with the
   recipe above.
3. The source archive attached to the same release is `git archive` of the tag.

## Upstream workflows

The upstream PCSX2 workflows in `.github/workflows/` were changed to `workflow_dispatch` only, so that the
fork does not spend Actions minutes on PCSX2's own Linux, macOS, Qt, flatpak and cron jobs. No upstream file
was deleted. `windows_build_qt.yml`, `linux_build_qt.yml`, `linux_build_flatpak.yml` and `macos_build.yml`
are reusable workflows and remain callable.

## Release 1.2.0 (TASK-088): custom textures, crop 2 rows, black window

Same recipe, options and dependency script as 1.1.0 (`GT4GS_ONLY=ON`; the same cached dependency folder works with
`-Deps`; the dependency script hash is unchanged). Code changes: `pcsx2-gt4gs/Main.cpp` (version string, the crop of
2 bottom rows, the Dump* keys refused and forced off, the black window background), `pcsx2/GS/Renderers/HW/
GSTextureReplacements.{h,cpp}` and `GSTextureCache.cpp` (replacement folder without serial, texture dumping compiled
out of the lookup; see README "Texture replacements"). Local build directory for the test build:
`private/slice241-gt4gs-build` (GT4Recomp checkout), dependencies `private/slice193-gt4gs-deps/root/deps`.

The release is published from the owner's PC with `release-local.ps1` after the owner's in-game test. The 1.2.0 test
package is not tagged yet, so the local script (which needs the tag checked out) was not run for it; its package layout
and checks follow the script (zip with `gt4-gs.exe`, DLLs, `resources/`, `COPYING.GPLv3`, `NOTICE`, `README.md`,
`SOURCE.txt`; `gt4gs/check-nojit.ps1` on the staged package).

Replay check for the texture folder (headless, no window): `gt4-gs.exe --replay-gscap <capture> --config <ini>` with
`[Folders] Textures` pointing at a scratch folder that holds `replacements\<tiny png>`; the log names the replacement
found (`Found ... replacement`) without any serial folder.

## Release 1.2.1 (TASK-093): texture folder is the search root

Same recipe, options and dependency script as 1.2.0 (`GT4GS_ONLY=ON`, `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` as in the
CI recipe; the dependency script hash is unchanged, so the same cached dependency folder works with `-DCMAKE_PREFIX_PATH`).
Branch `gt4gs/textures-root-1.2.1` (worktree of `gt4gs/textures-1.2.0`).

Change (`pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp`, `pcsx2-gt4gs/Main.cpp`):

- `ReloadReplacementMap()` searches `[Folders] Textures` itself, recursively (`FILESYSTEM_FIND_RECURSIVE`, unbounded
  depth; `FileSystem::RecursiveFindFiles` guards symbolic-link loops). The old `replacements` folder is one more
  subfolder. The serial-folder check and the case-mismatch OSD warning are removed: the search does not depend on folder
  names in between. The console line `gt4-gs textures: N replacement(s) in <Textures>` names the root.
- Dumps stay out of the search root: `DumpTexture()` returns at once and `GetDumpFilename()` returns an empty name while
  `kTextureDumpingAllowed` is false, so no `Textures\dumps` folder is created. Nothing else gt4-gs writes goes under
  `Textures` (cache files are under `[Folders] Cache`). Re-enabling dumping needs a dump folder outside `Textures` first.
- Version string `gt4gs 1.2.1`.

Test: `gt4gs/tests/test_textures_1_2.py` (updated for the root; run it against the built exe and a `.gscap` capture):
three synthetic packs at depth 0, depth 3 and under `Textures\SCUS-97328\replacements` are all counted; nothing but
the packs is under `Textures` afterwards; an empty folder gives identical frames.

```
python gt4gs\tests\test_textures_1_2.py --gt4gs <build>\pcsx2-gt4gs\gt4-gs.exe --gscap <capture.gscap> --work <new empty dir>
```

The package is `gt4-gs-1.2.1-windows-x64.zip`, built with `package-gt4gs.ps1 -Version 1.2.1` (not tagged, not published:
the owner's in-game test comes first).

## macOS arm64 (branch gt4gs/macos)

`gt4-gs` for the macOS port of GT4Recomp (github.com/dgadacha/gt4-mac, from commit `86fb754`; its
`docs/BUILD-MACOS.md` covers the game side). Based on `gt4gs-v1.2.1`, built and tested on an M3 Pro (macOS 26.6,
Xcode 26.2). Not released: build it from the branch.

```
xcodebuild -downloadComponent MetalToolchain
brew install cmake ninja sdl3 zstd lz4 libpng jpeg-turbo webp freetype harfbuzz
.github/workflows/scripts/macos/build-dependencies-gt4gs-arm64.sh ../gt4gs-deps
cmake -S . -B ../gt4gs-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_PREFIX_PATH="$PWD/../gt4gs-deps;$(brew --prefix)" -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF \
  -DENABLE_GT4GS=ON -DGT4GS_ONLY=ON -DUSE_VULKAN=OFF -DENABLE_TESTS=OFF -DDISABLE_ADVANCE_SIMD=ON
cmake --build ../gt4gs-build --target pcsx2-gt4gs
```

The executable is `pcsx2-gt4gs/gt4-gs` in the build folder, with `resources/` beside it: `Metal23.metallib` (the Metal
shaders, compiled at build time; the Metal Toolchain is an optional Xcode component) and the rest of `bin/resources`.
The dependency script builds PlutoVG, PlutoSVG and RapidYAML (the versions and SHA-256 of
`build-dependencies-universal.sh`); Homebrew provides the others.

Changes for macOS:

- Transport: the vendored `gs_stream_ipc_posix.cpp` (POSIX shared memory, waits on the waiter flags with
  `os_sync_wait_on_address`, which needs macOS 14.4 or later, peers watched by pid); `PosixCompat.h` gives
  `gs_stream_file.cpp` its `fopen_s`.
- JIT-free build on arm64 (`GT4GS_ONLY`, `GT4GS_NO_JIT`): no vixl code generators, as on x86-64; `getpid` on POSIX.
- Live mode has no window: `pcsx2-gt4gs/LiveViewMac.mm` gives the Metal device an off-screen view whose layer is the
  root of a remote Core Animation context. CONTROL attach_window (no window handle on macOS) sizes it in pixels, and
  WINDOW restored sends the context id (`WindowPayload::layer_id`), which the game shows in its own window with a
  `CALayerHost`. Resize follows CONTROL resize; the cursor policy is the game window's. No Dock icon, no App Nap.
- CONTROL reload_config: the `[EmuCore/GS]` keys of the `--config` file are read again (allow-list and fixed keys as at
  start) and applied with `VMManager::ApplySettings()`; the game's Resolution row uses it for the internal resolution.
- Defaults on macOS: Metal renderer; data root `~/Library/Application Support/GT4Recomp/gt4-gs` without `[GT4GS]
  DataRoot`.
- Replay without a window (`--replay-gscap`): `GSDeviceMTL::BeginPresent` submits each frame and keeps the GPU at most
  two frames behind (before, the frame's work stayed in one command buffer and the upload buffers doubled until the
  Metal driver could not allocate them), and the replayer prints the GS thread frame times of every 600 vsyncs.

Checked: a 6,000-frame capture of the macOS game (menus, movie, Arcade race on the Nordschleife) replays at 1x, 3x, 4x
and 6x (race frames: about 2 ms per frame at 1x, 7.4 ms at 6x); in live mode the picture shows in the game window,
follows window and fullscreen changes, and 6x holds 60 FPS in a 1920x1080 window. Known: the 480i menus leave a black
band at the bottom of the picture.
