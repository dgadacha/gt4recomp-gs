# Building gt4-gs.exe (Windows x64)

This recipe is what the CI workflow `.github/workflows/gt4gs-windows.yml` runs. A local build from the same
tag should produce the same file list; bit-identical output is a goal, recorded in the release notes.

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
  -DENABLE_QT_UI=OFF -DENABLE_GSRUNNER=OFF -DENABLE_GT4GS=ON -DENABLE_TESTS=OFF ^
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

## Renderers in this build

- Vulkan, D3D11 and D3D12 are compiled in on Windows. OpenGL is compiled in too (`USE_OPENGL` default ON)
  and is not offered by the GT4Recomp launcher.
- The software renderer and the recompilers are still present in this build. Removing them is TASK-042
  (no-JIT hardening) and is required before release 1.0.0.

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
