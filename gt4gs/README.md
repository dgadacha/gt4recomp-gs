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
  chain acquire, window hardening. Prerelease `gt4gs-v0.3.0`; `gt4gs-v0.3.1` fixes a click on the picture leaving
  the game window inactive.
- Branch `gt4gs/deinterlace` (task 047): 480i deinterlacing. `gt4gs-v0.3.2` presents each frame with the field of
  its own VSYNC record (protocol 1.1, the display field); with the game's slice196, which also sends SYNCV, PCSX2's
  Automatic deinterlacing weaves the 480i menus in the right field order.
- Branch `gt4gs/osd` (tasks 060, 061, 062): **`gt4gs-v1.0.0`, the first stable release** (not a prerelease). GS stream
  protocol 1.2: the cursor hides over the picture after 3 s without movement with the game's policy (CONTROL cursor),
  a plain "Compiling shaders�" notice shows in the bottom-left corner while shaders or pipelines are really compiled,
  and an optional performance overlay (top right: FPS, frame time, CPU and GPU usage of the game, resolutions, the
  per-part frame cost and the headroom) draws the game's OSD_STATS numbers. Settings outside the allow-list of the
  game's settings table are ignored with a warning line instead of being applied. Same JIT-free build as 0.4.0.
- Branch `gt4gs/datadir` (task 044 follow-up): **`gt4gs-v1.0.1`, stable.** No writes under `Documents\PCSX2`: 1.0.0 still
  created the PCSX2 user folders and rewrote `Documents\PCSX2\logs\emulog.txt` on every session, before `[GT4GS] DataRoot`
  was applied. From 1.0.1 the PCSX2 folders and log are set up only under the data root (stream modes), or only for the
  GSRunner dump replay. Otherwise the same as 1.0.0 (same JIT-free build, same protocol 1.2).
- Branch `gt4gs/nojit` (task 042): `gt4-gs.exe` contains and runs no JIT. `gt4gs-v0.4.0` is built with
  `GT4GS_ONLY=ON`: no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software JIT (the software rasterizer uses
  PCSX2's C functions), no executable memory mapping, and Windows Arbitrary Code Guard on (see "No JIT" in
  BUILD.md). The HW renderers (Vulkan, D3D11, D3D12) are unchanged; replayed frames are byte-identical to 0.3.2.

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
  its mask bits); VSYNC sets the CSR FIELD bit to the record's field, which is the display field of the frame
  that ends there (protocol 1.1; until 0.3.1 the previous record's field was used, which wove the fields in the
  wrong order once the stream marked the output as interlaced), posts the vsync (the GS thread presents) and bumps `vsyncs_done` from the GS thread
  once processed; READ_FIFO reads the GS FIFO into the response area (see Readbacks); RESET hard-resets the GS or soft-resets
  GIF paths; CONTROL handles shutdown, attach_window, resize, snapshot and (protocol 1.2) cursor (reload_config is
  answered "unsupported"); OSD_STATS (protocol 1.2) feeds the performance overlay. STATS are posted every 500 vsyncs, with a console line that adds PCSX2's GS-thread and GPU
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
  `Documents\PCSX2` (1.0.1 no longer writes there either). SPU2 uses the Null backend (0.2.0 opened a WASAPI stream).
- **Presentation (0.3.0).** In live mode the swap chain image for the next frame is acquired when that frame is
  presented, not right after the previous present (`[GT4GS] DeferSwapChainAcquire`, default true in live mode):
  with FIFO and two images the early acquire blocks the GS thread until the next display refresh, and a readback
  queued behind it waits as long. `[GT4GS] SpinUs` sets the transport spin before a wait sleeps (default 30).
- **Window.** A hidden, non-activating popup is created at start so the Vulkan device has a surface. On
  CONTROL attach_window the window thread (which only pumps messages) makes it a `WS_CHILD` of the game
  window covering its client area and detaches its input queue from the game's window thread (0.3.0), so a
  stalled GS window thread cannot hold up the game's input. With detached queues a click on the picture makes the GS
  window thread the foreground thread; since 0.3.1 every click posts a message that gives the foreground back to the
  game window (this thread received the click, so Windows allows it), `WM_MOUSEACTIVATE` returns `MA_NOACTIVATE`, and
  the child no longer answers `HTTRANSPARENT` (across threads that only discarded the click). Focus goes back to the
  parent. Resize follows
  CONTROL resize; a zero size (minimized game window) is ignored. `[GT4GS] DebugStallWindowMs` and
  `DebugStallAfterMs` stall the window thread once, for tests only.
- **Headless.** `[GT4GS] Surfaceless = true` runs without a window; `DumpDir` and `DumpEvery` write frame
  PNGs named like GSRunner's `-dumpdir` (`live_frameNNNNN.png`).
- **Protocol.** The MIT protocol header and transport are vendored unchanged in
  `3rdparty/gt4recomp-gs-stream/` and checked against `SHA256SUMS` at configure time. They are compiled into
  a separate static library with C++ exceptions (PCSX2 builds without them); `pcsx2-gt4gs/StreamTransport.cpp`
  exposes them without exceptions.

- **Settings (1.0.0).** Every key of `--config` is checked against the allow-list of the game repository's settings
  table (`docs/launcher/gs-settings-allowlist.json`, vendored by hash in `3rdparty/gt4recomp-gs-stream/` and compiled
  into a table at configure time). An unknown key, or a value outside its rule (bool and enum values, int and float
  ranges), is ignored with one line `gt4-gs: ignoring [section] key = value in <file> (<why>)`; the session always
  starts. `[EmuCore]` keys of the list are applied too (`EnableGameFixes`).
- **Cursor (1.0.0, task 060).** After CONTROL attach_window a protocol 1.2 game sends CONTROL cursor (auto-hide flag,
  delay). The window thread then samples the cursor every 100 ms and on mouse moves: over the picture (this window is
  the one under the cursor) and without movement for the delay it hides the cursor (`SetCursor(nullptr)`, and
  `WM_SETCURSOR` keeps it hidden); any movement shows it again; elsewhere it is never touched. The game window applies
  the same rule to its own client area, so both processes agree. Without the record nothing changes.
- **Overlay (1.0.0, tasks 061 and 062).** `pcsx2/GSStreamOverlay.cpp`, drawn from the ImGui OSD pass with PCSX2's OSD
  font and margins. `[GT4GS] ShaderCompileNotice` (default true): "Compiling shaders�" in the bottom-left corner,
  plain text, while a shader was compiled (shaderc) or a pipeline was created that the driver did not take from its
  pipeline cache (VK_EXT_pipeline_creation_feedback cache-hit bit; without the extension every pipeline creation
  counts), until 0.5 s after the last one. `[GT4GS] PerfOverlay` (default false) with `PerfOverlayFPS`,
  `PerfOverlayFrameTime`, `PerfOverlayCPU`, `PerfOverlayGPU`, `PerfOverlayResolution`, `PerfOverlayBreakdown` (default
  true each): top-right lines. FPS, frame time and the breakdown (EE, VU1 worker, GS stream write, readbacks) come from
  the game's OSD_STATS records (READY sets `kReadyFlagOsdStats` only while the overlay is on); the GS thread time from
  PCSX2's PerformanceMetrics; CPU usage = GetProcessTimes of the game process and gt4-gs; GPU usage = PDH
  `\GPU Engine(*)\Utilization Percentage` of the two pids, summed per engine, busiest engine; both sampled every 0.5 s
  on a below-normal thread. The session log ends with a `gt4-gs overlay:` line (frames drawn, mean draw time, records,
  compiles and cache hits).
- **No JIT (0.4.0).** The log starts with `GT4GS no-JIT: ... Arbitrary Code Guard on ...; probe RWX allocation
  refused, error 1655` and ends with `GT4GS no-JIT audit (...): N regions, 0 executable non-image regions`.
  `gt4gs/check-nojit.ps1` checks a package statically.

Changes to PCSX2 files are small and listed in `gt4gs/NOTICE` (VMManager boot hook, GSDumpReplayer hooks,
console output to redirected handles, CMake lists, the `GT4GS_ONLY` switches of 0.4.0).

## Build

See [BUILD.md](BUILD.md). Release downloads are on the GitHub Releases page of this repository, each with
`SHA256SUMS` and a source archive of the same tag. Since 0.3.1 releases are built and published from the owner's PC
with `gt4gs/release-local.ps1`; the GitHub Actions workflow is a manual fallback only. Anyone can rebuild a release
from its tag with BUILD.md.

## Licensing

GPL-3.0-or-later. Full text in `COPYING.GPLv3`. Notices and third-party licenses are in `gt4gs/NOTICE`,
which is also shipped inside each release zip. Game data, keys and BIOS files are never part of this
repository or its releases.

## Credits

PCSX2 and its contributors. See `README.md` (upstream) and the git history.
