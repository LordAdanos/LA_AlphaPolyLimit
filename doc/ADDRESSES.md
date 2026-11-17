# Addresses

Gothic 2 NotR 2.6 fix, Steam `Gothic2.exe`: 9,038,140 bytes, ImageBase `0x00400000`.
Source: static analysis (disassembly) of this exe, checked by running the engine
code in `test\harness.cpp`. Analysis: `ANALYSE.md`.

## Exe check

Two FNV-1a fingerprints, one per patch group (`kAlphaRanges`, `kDegenRanges` in `src/la_alphapolylimit.cpp`). A group whose
fingerprint differs is not patched (the other one still is). Before writing, the original bytes of the site are compared.

| Group | Fingerprint | Ranges |
|---|---|---|
| alpha | `0x33BDA70F` | `0x005D6FD0`–`0x005D70BD` GetFreeAlphaWorldPoly; `0x005D70C0`–`0x005D70D1` ResetFreeList; `0x005D6F10`–`0x005D6F19` WorldPoly ctor (vtable); `0x005D7140`–`0x005D72FB` WorldPoly::Draw; `0x005D7A00`–`0x005D7C90` zCRenderManager::Flush body (first 0x10 bytes left out: renderer replacements may detour the entry); `0x00650990`–`0x00650A5B` zCRnd_D3D::AddAlphaSortObject; `0x00650A60`–`0x00650AD9` zCRnd_D3D::RenderAlphaSortList bucket walk |
| degenerate | `0x7CA0FECB` | `0x00573650`–`0x005736DE` DeletePolyAndVerts(zCPolygon*); `0x005736E0`–`0x0057385F` DeletePolyAndVerts(ulong); `0x00573860`–`0x00573910` RemoveDegeneratePolys loop; `0x005BA420`–`0x005BA44C` zCPolygon::VertPartOfPoly; `0x00566680`–`0x005666BB` zCMemPoolBase::Free; `0x005664E0`–`0x0056666A` zCMemPoolBase::Alloc; `0x005C3FA5`–`0x005C3FBD` BuildProgMeshProto call of RemoveDegeneratePolys |

Alpha group: the 8 bytes `0x005D6FF1`–`0x005D6FF4` (pool size, imm32 of `cmp ebx,0x1000`) and
`0x005D70AD`–`0x005D70B0` (pool base, disp32 of `lea eax,[edx*4+0x9868B0]`) are hashed as 0. SystemPack
(Union's `System\Union.patch`, block `#engine [G2A]` → `#patch [ALPHA_VOB and ALPHA_WORLD_POLY]`) rewrites exactly these:
a rewrite of the compare at `0x005D6FEF` to compare against 0x2600 (9728 slots) and `HEX @0x005D70AD = 2x00000000` (its own memory block, a
different address each start), plus ctor/dtor pushes `0x005D6EEA/0x005D6EF2/0x005D6F61/0x005D6F6B` (outside the ranges).
Hashing them would make the alpha check fail next to SystemPack (`0xAE446F4D`). Instead, the plugin validates them
(`AlphaPoolConsistent`): count 0x1000..0x100000, and count/base identical to the ctor (`0x005D6EEB`, `0x005D6EF2`) and
dtor (`0x005D6F62`, `0x005D6F6B`) pushes, first slot readable. No other write of `Union.patch` (G2A) touches any range
of either group (checked by address+length).

## Patched

| Address | Original bytes | Instructions | New | Group |
|---|---|---|---|---|
| `0x005D7B10` | `E8 BB F4 FF FF 8B F8` | `call 0x005D6FD0` (GetFreeAlphaWorldPoly); `mov edi,eax` | `jmp StubAlpha`, `nop nop` | alpha |
| `0x00573766` | `8B 0B 8B 2C 81` | `mov ecx,[ebx]`; `mov ebp,[ecx+eax*4]` | `jmp StubDegen` | degenerate |

Continuations: `StubAlpha` → `0x005D7B17` (slot in `edi`) or `0x005D7BAD` (loop tail, pool full).
`StubDegen` → `0x0057376B` (first occurrence) or `0x005737FC` (loop tail, repeated vertex).

## Engine data used

| Address | What |
|---|---|
| `0x009868B0` | `zCRndAlphaSortObject_WorldPoly` array, 0x1000 × 0x14 (vtable, next, z, polygon, key) |
| `0x00832BC8` | vtable of `zCRndAlphaSortObject_WorldPoly` (written by the ctor `0x005D6F10`) |
| `0x0099AA70` | `s_currentAlphaWorldPoly` (set 0 by Flush `0x005D7A0C`; read by Draw `0x005D72AD`) |
| `0x0099AA80` | "too many alphaWorldPolys already reported" flag |
| `0x008D4304` | `zCVertex::s_MemPool` (`zCMemPool<zCVertex>`: `+0xC` free head, `+0x10` free count) |
| `0x008A2558` | `zCMemPoolBase::s_disablePools` (harness only; game: `zDieter_StartUp` `0x006306A9`, parameter `ZNOMEMPOOL`) |

## Related, not patched

| Address | What |
|---|---|
| `0x00528D00` | `zCRndAlphaSortObject_Vob::GetFreeAlphaVob`, 256 slots `0x008D4A40`, counter `0x008D7EA0`, same wrap; inlined in `zCBspTree::RenderVobList` (`0x0052D293`) |
