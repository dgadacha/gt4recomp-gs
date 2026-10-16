# GT4Recomp GS stream protocol (vendored, MIT)

These files are copied unchanged from the GT4Recomp main repository
(https://github.com/moesuito/gt4-staticrecomp, directory `native/gs_stream/`, commit `e5ce53c`, protocol 1.1), licensed MIT
(`LICENSE.MIT`). They are the normative protocol header and the reference shared-memory transport used between the
game process (producer) and `gt4-gs.exe` (consumer). Specification: `docs/GS-STREAM-PROTOCOL.md` in the main
repository.

| File | Role |
|---|---|
| `gs_stream_protocol.h` | record layouts, control block, `.gscap` v2 file header |
| `gs_stream_ipc.hpp`, `gs_stream_ipc.cpp` | named mapping, SPSC rings, wake-up events, peer-process waits |
| `gs_stream_file.hpp`, `gs_stream_file.cpp` | `.gscap` v2 reader and writer |

`SHA256SUMS` holds the SHA-256 of each copied file in its LF form, which equals `git show e5ce53c:native/gs_stream/<file>` in
the main repository; `.gitattributes` keeps LF on checkout on every platform. CMake (`pcsx2/CMakeLists.txt`) recomputes them at configure
time and stops with an error when a file differs, so an edit here is never silent. To update: copy the files from
the main repository, regenerate `SHA256SUMS` (`sha256sum <files> > SHA256SUMS`) and record the new commit above.

The transport uses C++ exceptions. PCSX2 is built without exceptions, so these files and the small adapter
`pcsx2-gt4gs/StreamTransport.cpp` are compiled into a separate static library with exceptions enabled; the adapter
catches every exception and exposes a no-throw interface to the GPL code.
