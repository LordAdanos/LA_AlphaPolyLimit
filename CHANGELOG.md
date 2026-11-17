# Changelog

## v1.2 – 2026-11-16

First public release.

- **AlphaPoolFix** (default 1, `AlphaWorldPolys=16384`, up to 262144): the only call of `GetFreeAlphaWorldPoly`
  (`zCRenderManager::Flush`, `0x005D7B10`) gets slots from the plugin's own allocator. A full pool skips the polygon for
  that frame and never wraps (the engine's wrap closes a cycle in the depth buckets and hangs `RenderAlphaSortList`).
- **DegenerateFix** (default 1): `zCMesh::DeletePolyAndVerts(ulong)` (`0x00573766`) skips a vertex that already occurred
  in the same polygon, so a degenerate polygon no longer frees a vertex twice into `zCVertex::s_MemPool` (crash in
  `zCMemPoolBase::Alloc` during `-zautoconvertdata`).
- **SystemPack:** its `Union.patch` (`ALPHA_WORLD_POLY`) rewrites the pool size and base of `GetFreeAlphaWorldPoly`. These
  bytes are left out of the code fingerprint and validated instead; the pool base and count are read from the code, so
  the alpha fix stays active next to SystemPack.
- Code check per group (Steam `Gothic2.exe`, 9,038,140 bytes): alpha `0x33BDA70F`, degenerate `0x7CA0FECB`.

Tests (no game start, `test\run_tests.ps1`): `patchcheck` 18/18 (incl. SystemPack emulation), `degen` 9/9, `alpha`
11/11 (engine code hangs after 5814 insertions, the plugin never wraps), `files` (all vob 3DS files of a large mod world:
0 pool errors with the plugin). In-game test: see `doc\INGAME_TEST.md`.

| File | Bytes | SHA-256 |
|---|---|---|
| `release\LA_AlphaPolyLimit.dll` | 129024 | `753B6B1AC84ED83E5A14AD0334AE8E4F38791282BD4B671F61100CA3C41E01E4` |
| `release\LA_AlphaPolyLimit.ini` | 967 | `004F7F81B20CD7A92F99774AE7ABEFEFD096019BAE7BBEE089DD51EFC6E34B79` |
