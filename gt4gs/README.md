# gt4recomp-gs

GPL-3.0+ renderer process for GT4Recomp (`gt4-gs.exe`), based on PCSX2's GS.

**Not affiliated with PCSX2.** This repository is a standalone public repository that starts from the
full PCSX2 history at tag `v2.8.2` (commit `fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3`). It is not a GitHub
fork relationship. All PCSX2 authors, copyright notices and license texts are kept. The PCSX2 project
(https://pcsx2.net, https://github.com/PCSX2/pcsx2) made the emulator and its GS renderers; this repository
credits them and is licensed under GPL-3.0-or-later like upstream (see `COPYING.GPLv3` and `gt4gs/NOTICE`).

## What this is

GT4Recomp is a static recompilation of Gran Turismo 4 (MIT code). Its release executable does not contain
GPL code. The graphics work runs in a separate process, `gt4-gs.exe`, built from this repository. The two
processes share a generic GS stream (GIF packets, privileged register writes, vsync, readback requests)
over shared memory. No game code or guest memory crosses the boundary. The design is recorded in
decision 0070 of the GT4Recomp repository (https://github.com/moesuito/gt4-staticrecomp,
`docs/decisions/0070-pcsx2-gs-separate-process.md`).

## Status

- Branch `gt4gs/main`, based on PCSX2 `v2.8.2`.
- Current binary: the stock `pcsx2-gsrunner` built from this tree, renamed `gt4-gs.exe` in the package.
  No GT4Recomp protocol code is in this repository yet (TASK-035 and TASK-036).
- Changes from upstream so far are limited to `gt4gs/` (documentation and notices), the trimmed dependency
  script `.github/workflows/scripts/windows/build-dependencies-gt4gs.bat`, the packaging script, the
  `gt4gs-windows.yml` workflow, and upstream workflows set to manual dispatch (see `gt4gs/BUILD.md`).

## Build

See [BUILD.md](BUILD.md). Release downloads are on the GitHub Releases page of this repository, each with
`SHA256SUMS` and a source archive of the same tag.

## Licensing

GPL-3.0-or-later. Full text in `COPYING.GPLv3`. Notices and third-party licenses are in `gt4gs/NOTICE`,
which is also shipped inside each release zip. Game data, keys and BIOS files are never part of this
repository or its releases.

## Credits

PCSX2 and its contributors. See `README.md` (upstream) and the git history.
