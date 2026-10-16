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

- Branch `gt4gs/main`: PCSX2 `v2.8.2` plus documentation, packaging and CI (task 033).
- Branch `gt4gs/live` (task 036): `gt4-gs.exe` is built by the new frontend `pcsx2-gt4gs/` (from
  `pcsx2-gsrunner`) and consumes the GT4Recomp GS stream live. Prerelease `gt4gs-v0.2.0-live`.
- Branch `gt4gs/window` (tasks 037 and 038): readback modes and timing, own data and cache folders, deferred swap
  chain acquire, window hardening. Prerelease `gt4gs-v0.3.0`.

## gt4-gs.exe

```
gt4-gs.exe --ipc <session> --parent <pid> [--config <gt4-gs.ini>]   live: started by the game
gt4-gs.exe --replay-gscap <file.gscap> [-dumpdir <dir>] [--config <ini>]   offline replay of a stream file
gt4-gs.exe [GSRunner options] <dump.gs>                             GSRunner's GS dump replay (unchanged)
```

- **Live mode.** The game (MIT, separate process) creates a shared-memory session and starts `gt4-gs.exe`
  in a Job Object. `gt4-gs.exe` opens the session, checks magic, protocol major version, struct sizes, region
  layout, frames-in-flight limit, serial and that the producer pid equals `--parent`, then boots the GS
  (no BIOS, no disc, no input, no audio, no memory cards) and posts READY. The serial from the handshake
  (`SCUS-97328`) selects the GameDB entry, so the GT4 GS hardware fixes apply. Blending accuracy defaults to
  High, the renderer to Vulkan, the aspect ratio to 16:9 and deinterlacing to Automatic; `--config` overrides
  them.
- **Records** (`pcsx2/GSStreamReplayer.cpp`, a CPU provider like the GS dump replayer): GIF chunks go to the
  GIF unit path named in the record (PATH1/2/3) as completed GS packets; PRIV writes go to the 8 KiB
  privileged block (CSR RESET resets the block, IMR, CSR and the GS; CSR is otherwise not stored; IMR keeps
  its mask bits); VSYNC sets the CSR FIELD bit to the field of the previous VSYNC (PCSX2's EE swaps the field
  after posting the vsync), posts the vsync (the GS thread presents) and bumps `vsyncs_done` from the GS thread
  once processed; READ_FIFO reads the GS FIFO into the response area (see Readbacks); RESET hard-resets the GS or soft-resets
  GIF paths; CONTROL handles shutdown, attach_window, resize and snapshot (reload_config is answered
  "unsupported"). STATS are posted every 500 vsyncs, with a console line that adds PCSX2's GS-thread and GPU
  times. The session always starts with a clean GS hard reset, never a freeze state.
- **Readbacks (0.3.0).** `[EmuCore/GS] HWDownloadMode` selects how READ_FIFO is answered: `0` Enabled
  (accurate, default: waits for the GS thread and the GPU), `3` Unsynchronized (answered on the CPU thread from
  the GS local memory copy, without waiting; GPU-drawn pixels are stale, so effects that read the picture back
  may change), `4` Disabled (zeros). Unsynchronized needs the transfer registers that PCSX2 captures on the EE
  side; a GIF tag scanner per path captures them from the stream. Every 500 vsyncs a console line reports the
  readback service times (count, mean, max, histogram) and the time the GS thread waited for a swap chain image.
- **Folders (0.3.0).** The data root is `[GT4GS] DataRoot` (default `%LOCALAPPDATA%\GT4Recomp\gt4-gs`), never
  the PCSX2 user folder, and the `[Folders]` keys of the config (`Cache`, `Textures`, ...) are applied before the
  GS opens, so the shader and pipeline caches persist where the launcher puts them. 0.2.0 ignored them and used
  `Documents\PCSX2`. SPU2 uses the Null backend (0.2.0 opened a WASAPI stream).
- **Presentation (0.3.0).** In live mode the swap chain image for the next frame is acquired when that frame is
  presented, not right after the previous present (`[GT4GS] DeferSwapChainAcquire`, default true in live mode):
  with FIFO and two images the early acquire blocks the GS thread until the next display refresh, and a readback
  queued behind it waits as long. `[GT4GS] SpinUs` sets the transport spin before a wait sleeps (default 30).
- **Window.** A hidden, non-activating popup is created at start so the Vulkan device has a surface. On
  CONTROL attach_window the window thread (which only pumps messages) makes it a `WS_CHILD` of the game
  window covering its client area and detaches its input queue from the game's window thread (0.3.0), so a
  stalled GS window thread cannot hold up the game's input. It answers `WM_MOUSEACTIVATE` with `MA_NOACTIVATE`
  (bringing the game window to the foreground when needed) and gives focus back to the parent. Resize follows
  CONTROL resize; a zero size (minimized game window) is ignored. `[GT4GS] DebugStallWindowMs` and
  `DebugStallAfterMs` stall the window thread once, for tests only.
- **Headless.** `[GT4GS] Surfaceless = true` runs without a window; `DumpDir` and `DumpEvery` write frame
  PNGs named like GSRunner's `-dumpdir` (`live_frameNNNNN.png`).
- **Protocol.** The MIT protocol header and transport are vendored unchanged in
  `3rdparty/gt4recomp-gs-stream/` and checked against `SHA256SUMS` at configure time. They are compiled into
  a separate static library with C++ exceptions (PCSX2 builds without them); `pcsx2-gt4gs/StreamTransport.cpp`
  exposes them without exceptions.

Changes to PCSX2 files are small and listed in `gt4gs/NOTICE` (VMManager boot hook, GSDumpReplayer hooks,
console output to redirected handles, CMake lists).

## Build

See [BUILD.md](BUILD.md). Release downloads are on the GitHub Releases page of this repository, each with
`SHA256SUMS` and a source archive of the same tag.

## Licensing

GPL-3.0-or-later. Full text in `COPYING.GPLv3`. Notices and third-party licenses are in `gt4gs/NOTICE`,
which is also shipped inside each release zip. Game data, keys and BIOS files are never part of this
repository or its releases.

## Credits

PCSX2 and its contributors. See `README.md` (upstream) and the git history.
