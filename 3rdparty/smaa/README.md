# SMAA lookup textures (vendored, MIT)

`AreaTex.h` and `SearchTex.h` are copied unchanged from the reference implementation of SMAA, Enhanced Subpixel
Morphological Antialiasing, by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez
(https://github.com/iryoku/smaa, `Textures/`, branch `master` on 2026-10-27), licensed MIT (`LICENSE.txt`, the notice
of the reference `SMAA.hlsl`).

| File | Content | SHA-256 |
|---|---|---|
| `AreaTex.h` | the precomputed areas, R8G8, 160x560 (`areaTexBytes`) | `1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2` |
| `SearchTex.h` | the search lengths, R8, 64x16 (`searchTexBytes`) | `1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a` |

GT4Recomp fork (macOS port): `GSDeviceMTL` uploads them as Metal textures for SMAA 1x (`[GT4GS] SMAA`), whose shaders
are a Metal port of the reference `SMAA.hlsl` in `pcsx2/GS/Renderers/Metal/gt4smaa.metal`.
