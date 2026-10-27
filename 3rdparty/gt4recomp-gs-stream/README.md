# GT4Recomp GS stream protocol (vendored, MIT)

These files are copied unchanged from the GT4Recomp main repository (https://github.com/moesuito/gt4-staticrecomp),
licensed MIT (`LICENSE.MIT`). They are the normative protocol header, the reference shared-memory transport used
between the game process (producer) and `gt4-gs.exe` (consumer), and the GS settings allow-list. Specification:
`docs/GS-STREAM-PROTOCOL.md` and `docs/GS-SETTINGS.md` in the main repository.

| File | Role | Copied from |
|---|---|---|
| `gs_stream_protocol.h` | record layouts, control block, `.gscap` v2 file header (protocol 1.3) | `native/gs_stream/` of the macOS port (github.com/dgadacha/gt4-mac, commit `86fb754`; adds `WindowPayload::layer_id`) |
| `gs_stream_ipc.hpp`, `gs_stream_ipc.cpp` | named mapping, SPSC rings, wake-up events, peer-process waits | `native/gs_stream/`, commit `e5ce53c`; the header from the macOS port, commit `86fb754` (POSIX declarations only) |
| `gs_stream_ipc_posix.cpp` | the same transport on macOS and Linux (POSIX shared memory, address waits, pid peers) | `native/gs_stream/` of the macOS port (github.com/dgadacha/gt4-mac, commit `86fb754`) |
| `gs_stream_file.hpp`, `gs_stream_file.cpp` | `.gscap` v2 reader and writer | `native/gs_stream/`, commit `e5ce53c` |
| `gs-settings-allowlist.json` | INI keys and values `gt4-gs.exe` accepts (1.0.0 and later) | `docs/launcher/` of the macOS port, commit `e6cd745` (adds the `[GT4GS]` keys `HDR`, `HDRPeak`, `MetalFX`, `Bloom`, `FilmLook`, `SMAA` and `AO`) |

The transport files of `e5ce53c` are the consumer side used here; since then the main repository changed only
producer-side wake batching and a counter in them (TASK-041), which the consumer does not use.

`SHA256SUMS` holds the SHA-256 of each copied file in its LF form, which equals `git show <commit>:<path>` in the main
repository (for the allow-list, whose blob there has CRLF line endings, the same text with LF); `.gitattributes`
keeps LF on checkout on every platform. CMake (`pcsx2/CMakeLists.txt`) recomputes them at configure time and stops
with an error when a file differs, so an edit here is never silent. `pcsx2-gt4gs/CMakeLists.txt` turns the allow-list
into a C++ table at configure time. To update: copy the files from the main repository (LF), regenerate `SHA256SUMS`
(`sha256sum <files> > SHA256SUMS`) and record the new commit above.

The transport uses C++ exceptions. PCSX2 is built without exceptions, so these files and the small adapter
`pcsx2-gt4gs/StreamTransport.cpp` are compiled into a separate static library with exceptions enabled; the adapter
catches every exception and exposes a no-throw interface to the GPL code.
