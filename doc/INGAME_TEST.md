# In-game test LA_AlphaPolyLimit

Needs: a Gothic II installation (Steam 2.6 fix) with Union that you may start, and a GPU. Use a copy of the game
folder, never your only installation.

Install the plugin (`release\LA_AlphaPolyLimit.dll`, `release\LA_AlphaPolyLimit.ini`) into `<Gothic II folder>\System\Autorun\`. With
`Enabled=0` in the ini the plugin loads, checks the code and writes one line to `System\Autorun\LA_AlphaPolyLimit.log`, but
patches nothing (the shipped ini has `Enabled=1`).
Uninstall: delete `LA_AlphaPolyLimit.dll`, `LA_AlphaPolyLimit.ini`, `LA_AlphaPolyLimit.log`.

## 1. What to check in the log

1. `LA_AlphaPolyLimit v1.2 loaded. ... code check alpha 0x33BDA70F (ok), degenerate 0x7CA0FECB (ok)` – otherwise another
   patch changed that code (the group is then off; `bytes at ... differ` names the site).
2. `AlphaPoolFix active (AlphaWorldPolys=16384), DegenerateFix active`.
3. After the first world frame: `alpha pool: 16384 slots at ...`.
4. At exit: `LA_AlphaPolyLimit summary: alpha pool 16384 slots, peak N per frame, ...; degenerate double frees prevented M`.

## 2. Runs worth doing

- **Conversion run** (the vertex-pool crash): delete the compiled MRM files of the affected vob meshes
  from `_work\Data\Meshes\_compiled\` of your game folder (they are rebuilt), start with `-zautoconvertdata`.
  Expected: no crash, log lines `degenerate polygon holds vertex ... twice` (2 + 4 harmful ones among them) and a
  summary with "degenerate double frees prevented" > 0. Without the plugin the same run should crash in
  `zCMemPoolBase::Alloc` (do not repeat unless wanted).
- **Alpha run**: any scene with many transparent polygons. `peak` shows how close the frame got to 4096; with opaque
  materials it stays far below. `alpha pool full: ... skipped` lines (instead of the original hang) appear only if
  more than `AlphaWorldPolys` alpha polygons are visible in one frame.
- Renderer replacements that hook `zCRenderManager::Flush` at its entry: nothing to configure (this plugin patches its body).
