# LA_AlphaPolyLimit

Union plugin for **Gothic 2: Night of the Raven 2.6 fix** that raises the number of transparent (alpha) world polygons per
frame from 4096 to 16384 (configurable) and skips polygons instead of wrapping the pool when it is full, which removes an
endless hang in the renderer. It also guards the `zCVertex` pool against a double free caused by degenerate polygons
(crash in `zCMemPoolBase::Alloc` during `-zautoconvertdata`).

Version: see `CHANGELOG.md`.

## What it fixes

Details in `doc/ANALYSE.md`, addresses in `doc/ADDRESSES.md`.

1. **Alpha world polygon pool.** `zCRndAlphaSortObject_WorldPoly::GetFreeAlphaWorldPoly` (`0x005D6FD0`) hands out slots
   of a static 4096-entry array and wraps to slot 0 on overflow while the old slots are still linked in the depth buckets
   of `zCRnd_D3D::AddAlphaSortObject`. Re-inserting a linked slot closes a cycle; `RenderAlphaSortList` then loops
   forever (the game logs "RNDMAN: too many alphaWorldPolys!" and hangs at 100 % CPU). The plugin replaces the only call
   (in `zCRenderManager::Flush`, `0x005D7B10`) with its own allocator: `AlphaWorldPolys` slots (default 16384), and when
   it is full the polygon is skipped for that frame. Never a wrap, so no cycle.
2. **zCVertex pool double free.** `zCMesh::DeletePolyAndVerts(ulong)` (`0x005736E0`, reached through
   `RemoveDegeneratePolys` from every MRM/MDM build of `-zautoconvertdata`) frees an unshared vertex of a degenerate
   polygon once per occurrence. A polygon that holds the same vertex twice corrupts the free list of the pool, and the
   next allocation crashes in `zCMemPoolBase::Alloc` (`0x00566649`). The plugin skips repeated occurrences at the loop
   head (`0x00573766`).

Each patch group is guarded by an FNV-1a fingerprint of the engine code it relies on (Steam `Gothic2.exe`, 9,038,140
bytes: alpha `0x33BDA70F`, degenerate `0x7CA0FECB`) and by the original bytes of its patch site. On a mismatch that group
stays off and the reason is written to the log.

## Comparison with SystemPack

- Vanilla: 4096 alpha world polygons per frame. Above that the slot index wraps to 0 while the old slots are still linked
  in the depth sort, which can cause an endless loop / hang ("RNDMAN: too many alphaWorldPolys!").
- SystemPack (`Union.patch`, `ALPHA_WORLD_POLY`): raises the limit to 9728 (0x2600). The pool still wraps, so the hang is
  moved, not removed.
- LA_AlphaPolyLimit: 16384 by default (INI). Above that the polygon is skipped for that frame and the pool never wraps,
  so there is no hang. At worst a few transparent surfaces are missing for one frame.
- It coexists with SystemPack: the code fingerprint masks the bytes SystemPack rewrites, and the pool base and count are
  read from the patched code.

| | Limit per frame | Behaviour above the limit |
|---|---|---|
| Vanilla | 4096 | index wraps to 0, possible endless loop |
| SystemPack | 9728 | index still wraps, hang only moved |
| LA_AlphaPolyLimit | 16384 (INI, up to 262144) | polygon skipped for that frame, no wrap, no hang |

## Who is this for / limitations

- The alpha fix only matters for worlds with very many transparent world polygons (dense foliage, glass, water). Normal
  mods and the vanilla world rarely hit the limit.
- The vertex-pool guard only matters when Gothic converts models in the game (`-zautoconvertdata`) and a mesh contains
  degenerate triangles.
- Only the Steam `Gothic2.exe` 2.6 fix is supported (code fingerprint check); other executables are left unpatched.

## Requirements

- Gothic 2 Night of the Raven 2.6 fix (Steam `Gothic2.exe`, 9,038,140 bytes), 32-bit.
- **Union 1.0m** (plugin host that loads the DLL from `System\Autorun`).
- SystemPack 1.8 (shipped with Union) is recognised: its `Union.patch` enlarges the alpha pool to 9728 slots and moves
  its base; the plugin detects that and stays active (see `doc/ANALYSE.md`).
- Renderer replacements that hook `zCRenderManager::Flush` at its entry are compatible (this plugin patches the body).
- No Visual C++ Redistributable needed: the DLL is built with `/MT` (static CRT).

Tested with: Steam Gothic 2 NotR 2.6 fix + Union 1.0m + SystemPack 1.8, with Ikarus/LeGo, in a large custom world with many vobs, and together with a renderer replacement

## Install / uninstall

Copy `release\LA_AlphaPolyLimit.dll` and `release\LA_AlphaPolyLimit.ini` to `<Gothic II>\System\Autorun\`.
Uninstall: delete both files (and `LA_AlphaPolyLimit.log`).

## INI switches (`LA_AlphaPolyLimit.ini`, section `[LA_ALPHAPOLYLIMIT]`)

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | master switch; 0 = load, check the code, log one line, patch nothing |
| `AlphaPoolFix` | 1 | own alpha world polygon allocator (no wrap) |
| `AlphaWorldPolys` | 16384 | pool size, 4096 (engine size) to 262144; 20 bytes per slot |
| `DegenerateFix` | 1 | skip repeated vertex occurrences in degenerate polygons |
| `Diag` | 1 | write the log |
| `DiagMaxLines` | 200 | maximum number of log lines |

## Log

`LA_AlphaPolyLimit.log` next to the DLL: load line with both code checks, pool overflow records, prevented double
frees, and a summary at exit.

## Build

`build.bat` (Visual Studio 2019 or newer with the C++ x86 tools; the newest installed one is used) produces
`build\LA_AlphaPolyLimit.dll` (32-bit, static CRT) and `build\laap_harness.exe`. `build.bat dll` builds the DLL only.
Copy the DLL to `release\` and record its SHA-256 in `CHANGELOG.md`.

## Tests

`test\run_tests.ps1 -GameDir <Gothic II folder>` runs the engine's own machine code from `Gothic2.exe` (mapped read-only
into a test process via `src\la_exe_image.h`; the game is never started). The hang and the double free are reproduced
with the original code and shown to be prevented with the plugin; it also checks the patch sites, the SystemPack
emulation and all vob 3DS files of the game folder. `test\asc_degen_scan.py` checks ASC models for faces with a repeated
vertex index.

## Support

Please open a GitHub Issue and attach `LA_AlphaPolyLimit.log`.

Developed with AI assistance; tested by the author.

## Files

| File | Content |
|---|---|
| `src/la_alphapolylimit.cpp` | the whole plugin |
| `src/la_exe_image.h` | test-harness loader (maps `Gothic2.exe`) |
| `release/` | ready-to-use DLL and INI (hashes in `CHANGELOG.md`) |
| `doc/ANALYSE.md`, `doc/ADDRESSES.md`, `doc/INGAME_TEST.md` | analysis, patched addresses, in-game test |
| `test/` | harness, test runner, ASC scan |

## License

MIT, Copyright (c) 2026 LordAdanos (`LICENSE`). Own code; no third-party code. SystemPack and Union are only named as
environment.
