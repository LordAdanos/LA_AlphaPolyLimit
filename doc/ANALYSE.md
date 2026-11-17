# LA_AlphaPolyLimit – analysis

Gothic 2 NotR 2.6 fix, Steam `Gothic2.exe` (9,038,140 bytes). All engine facts below come from a static analysis
(disassembly) of this exe and from running the exe's own code in the harness
(`test\harness.cpp`). This document gives addresses and descriptions in our own words; it contains no code from the exe.

## 1. Alpha world polygon pool (observed in game: endless hang)

### 1.1 Engine

- `zCRndAlphaSortObject_WorldPoly::GetFreeAlphaWorldPoly` `0x005D6FD0` (cdecl, no arguments): reads the counter at
  `0x0099AA70`; below 4096 it hands out slot `counter` (address = `0x009868B0` + 20 * index) and increments the counter.
  At 4096 or more it logs "D: RNDMAN: too many alphaWorldPolys!" once and **wraps**: the counter restarts at slot 0
  (compare immediate at `0x005D6FF1`, hand-out path at `0x005D709C`).
  Pool = static array `0x009868B0`, 0x1000 entries × 0x14 bytes (static constructor `0x005D6EE0`:
  `_vector_constructor_iterator(0x9868B0, 0x14, 0x1000, ctor 0x005D6F10)`; the ctor writes vtable `0x00832BC8`).
  Layout used by the engine: `+0` vtable, `+4` next (bucket list), `+8` z, `+0xC` polygon, `+0x10` key.
- Only caller (cross-references and the call at `0x005D7B10`): `zCRenderManager::Flush` `0x005D79F0`. Flush sets the
  counter to 0 at its start (`0x005D7A0C`), then for every material bucket whose material has `+0x74 != 1`
  (alphaFunc not NONE) and for every polygon of it: calls the allocator (`0x005D7B10`), stores the polygon (`+0xC`),
  the key (`+0x10`) and the mean vertex depth (`+8`) in the slot, and hands it to the renderer's
  `AddAlphaSortObject` through the vtable (`0x005D7BA7`, `zCRnd_D3D` `0x00650990`). The loop tail is at `0x005D7BAD`.
- `zCRnd_D3D::AddAlphaSortObject` `0x00650990`: bucket = clamp(int(f(z·scale)), 0, 255) (scale `+0x82E28`), heads at
  `+0x82A28`; sorted insert through `obj+4` (`0x00650A31`..`0x00650A50`: walk while next->z > z, then link).
- `zCRnd_D3D::RenderAlphaSortList` `0x00650A60`: for each of the 256 buckets
  `while (node) { node->vtbl[1](index); node = node->next; }` (`0x00650AA7`..`0x00650AB9`), no step limit.
- `zCRndAlphaSortObject_WorldPoly::Draw` `0x005D7140` batches polygons and flushes when its own draw count
  `0x0099AA7C` reaches the counter `0x0099AA70` – it never touches the array, only the counter.

### 1.2 Why it hangs

After the wrap, slot 0, 1, 2 … are handed out again while they are still linked in a bucket. `AddAlphaSortObject`
overwrites `next` of a linked node; inserting it in front of its old position closes a cycle (B→X→C, X re-inserted
before B: X→B→X). From then on the walk in `RenderAlphaSortList` – and also the sorted-insert walk of the next
`AddAlphaSortObject` into that bucket – never ends.

**Reproduced in the harness with the engine's own code** (`test_alpha.log`): engine allocator + engine
`AddAlphaSortObject` on a stand-in renderer, 6000 polygons in one frame → after 5814 insertions the engine code does
not return any more (thread killed after 10 s). The game shows exactly this: one zERR "too many alphaWorldPolys",
then 100 % CPU on the game thread, no frames.

### 1.3 Fix

The 7 bytes at `0x005D7B10` (`call GetFreeAlphaWorldPoly; mov edi,eax`) become `jmp StubAlpha` + 2 `nop`.
`StubAlpha` calls `AlphaGetFree()`:

- pool of `AlphaWorldPolys` slots (default 16384; 4096 = the engine array itself). A larger pool is allocated on the
  first call (the exe's static constructors have run by then), every slot gets the vtable `0x00832BC8` (checked against
  the constructed engine array first), rest 0 – the same state the engine's constructor leaves.
- counter: the engine's own `0x0099AA70` (Flush and `Draw` keep working unchanged).
- full pool → returns 0 → the stub jumps to the loop tail `0x005D7BAD` (stack depth, `ebx`/`esi`/`[esp+0x18]` as in
  the engine loop): this polygon is not drawn this frame, the counter stays at the pool size, so `Draw`'s final flush
  (count == counter) still happens. No wrap → no cycle.
- Log: first overflow and each new worst case (polygons skipped in one frame), summary at exit (peak per frame).

Cost of a larger pool: 20 bytes per slot (16384 → 320 KB). The engine's sorted insert is linear per bucket; with
16384 polygons spread over 256 buckets that is cheap, all in one bucket it would be ~1.3·10⁸ compares per frame (a
slow frame, not a hang). Vanilla-like content never comes near that.

### 1.4 Renderer replacements

Renderer replacements that wrap `ddraw.dll` generally leave the engine's own alpha list alone. A renderer replacement that hooks
`zCRenderManager::Flush` only at its **entry** (first 5 bytes at `0x005D79F0`) and does not touch the allocator, the array,
the counter or the sort list. The hang therefore also happens with such a renderer.

Interaction: our patch is in the Flush **body** (`0x005D7B10`), a renderer's detour is in the first 5 bytes
(`0x005D79F0`); they do not overlap, in either load order. The code check of LA_AlphaPolyLimit leaves out
`0x005D79F0`-`0x005D7A00` for that reason.

### 1.5 SystemPack

Without special handling, a game with SystemPack logs `code check alpha 0xAE446F4D (DIFFERS)`.
Cause: not the renderer (it only detours the Flush entry, which is outside the check) but **SystemPack inside Union**:
`System\Union.patch`, `#engine [G2A]`, unconditional `#patch [ALPHA_VOB and ALPHA_WORLD_POLY]` enlarges the world
poly pool to 0x2600 = 9728 slots in its own memory block (`cmp` imm at `0x005D6FF1`, `lea` disp at `0x005D70AD`, ctor/dtor
pushes) and the vob pool to 0xC00. The wrap logic stays – SystemPack only moves the limit. A scan of every G2A
`@0x…` write (address + length) against both range lists finds exactly these two inside them.

The plugin leaves the two 4-byte fields out of the fingerprint and validates them against the constructor/destructor pushes
instead; it reads the engine's current pool (base, count) from the code. `AlphaWorldPolys` ≤ engine count → the
engine's (SystemPack's) array with its count, no wrap; larger → own pool. Harness: SystemPack's writes emulated
byte for byte → same fingerprint, group active, engine still wraps at 0x2600, LA_AlphaPolyLimit does not; an inconsistent
relocation is refused.

### 1.6 Not covered: the vob pool

`zCRndAlphaSortObject_Vob::GetFreeAlphaVob` `0x00528D00` has the same wrap with only 256 entries
(`0x008D4A40`, counter `0x008D7EA0`, "D: RNDMAN: too few alphaVOBs"). the exe has no call to the function; the flag
`0x008D7EA4` is read/written in `zCBspTree::RenderVobList` `0x0052D293`/`0x0052D2F5`, i.e. the allocator is inlined
there. Not patched (needs its own analysis); it can hang the same way when more than 256
transparent/fading vobs are visible in one frame.

## 2. zCVertex pool corruption during -zautoconvertdata (observed in game)

### 2.1 Crash evidence

`Gothic2.RPT` of two crashes:

| Crash | EIP | Stack | Registers |
|---|---|---|---|
| 1 | `0x00566649` `zCMemPoolBase::Alloc` | `AddVertexSmart` ← `TPoly3ds`/`zCFile3DS::Load3DS` ← `zCProgMeshProto::Load` ← `zCVob::SetVisual` | EBX `0x008D4304` (zCVertex pool), EAX `7EFA5A48`, ECX `C23BE8B0` = −46.97723f, EDX `449B0002` = 1240.0002f |
| 2 | `0x00566649` | `zCModelPrototype::ReadVertexList` ← `ReadMesh` ← `Load3DSMAXAsc` ← `LoadModelASC` | EBX `0x008D4304`, ECX `4109DFD9` = 8.617f, EDX `42F48BAA` = 122.27f |

`zCMemPoolBase::Alloc` `0x005664E0` pops the head of a doubly linked free list (`+0xC` head, node `+0`
next, `+4` prev): `*(node->prev) = node->next; if (node->next) node->next->prev = node->prev; node->next = node->prev = 0`.
The fault is the write `node->next->prev` with `next` = a float: the free-list head is a vertex **in use** (x at `+0`,
y at `+4`).

### 2.2 Cause: double free in `zCMesh::DeletePolyAndVerts(ulong)` `0x005736E0`

Behaviour of the engine function (described in our own words): it takes the polygon at
index `i` and walks its vertices (loop head at `0x00573766`, vertex index in `eax`, polygon in `ebx`). For each vertex
it checks whether any *other* polygon of the mesh uses it; the resulting "shared" flag is sticky and never reset
inside the polygon. If the vertex is not shared, it is removed from the mesh's vertex list (swap with the last entry,
if found) and returned to the `zCVertex` memory pool (`Free`, call at `0x005737F7`) - also when it was **not** found in
the list because an earlier iteration already removed it. After the vertex loop the polygon itself is deleted and removed
from the polygon list.

A polygon that holds the same vertex twice (corner positions merged by `AddVertexSmart`'s merge cache, eps 0.01) is
degenerate; if no other polygon uses that vertex, it is freed once per occurrence. Second `Free(A)` with A = head:
`A->next = A`, `A->prev = A` → the next `Alloc` returns A but the head stays A → the vertex written into A turns its
`next`/`prev` into floats → the following `Alloc` writes to `*(y)` and crashes at `x+4` (exactly the register pattern
above), or silently corrupts memory if those addresses are mapped.

Callers: `zCMesh::RemoveDegeneratePolys` `0x00573860` (`CalcNormal()==0` → `DeletePolyAndVerts(i)`) ←
`zCProgMeshBuilder::BuildProgMeshProto` `0x005C3E40` at `0x005C3FB8` unless flag `0x10` ← `zCProgMeshProto::Load`
`0x005C7987` (flags 4/5: 3DS → MRM) and `zCModelPrototype::ReadSoftSkinVertList` `0x0058D417` (flags 0x2A/0x2B: ASC →
MDM). Both run only when a visual is converted, i.e. with `-zautoconvertdata` and no up-to-date compiled file.
`zCPolygon::CalcNormal` `0x005B8180` returns 0 when an edge has length 0 (repeated vertex) or the edges are collinear.

### 2.3 Proof

1. **Synthetic, engine code** (`test_degen_orig.log`): mesh with two good triangles and isolated `(A,A,B)`, `(C,D,C)`;
   engine `RemoveDegeneratePolys` → vertex free list broken ("free node … wrong prev link"). Patched: list sound, every
   vertex freed exactly once (free count +4), second mesh with a shared repeated vertex and a `(v,v,v)` triangle ok.
2. **Real files, engine code** (`test_files_*`): each vob 3DS built with the engine's merge cache, `AddVertexSmart`
   and `AddPoly` (like the vob import), then the engine's `RemoveDegeneratePolys`.
   - In the load order of a large mod world up to the first crash (1196 files): **2 files double-free** (two castle
     meshes with 2 and 4 repeated vertices). With the original code the first one breaks the pool exactly as predicted.
   - All 3691 vob 3DS files of that world: 4 files (two more tree meshes with 2 each).
   - Patched: 0 pool errors in 1196 and 3691 files; skipped repeated vertices = predicted (14,075 / 15,620).
3. **The crash registers match**: vertex #31 of the second castle mesh is (−46.97723, 1240.0002, −94.90) – the ECX/EDX
   values of the first crash. That run had just converted the first castle mesh → pool head = a live slot → the next
   imported file was the second castle mesh, whose 32nd vertex landed in that slot → crash. A second run converted the
   second castle mesh (its 4 double frees) and crashed at the next vertex allocation, in the ASC reader of a fire model.
   Runs that did not reach those files, or had both MRMs cached, did not crash.
4. ASC files: `test\asc_degen_scan.py` (ASC faces index the vertex list directly, `ReadVertexList` uses
   `AddVertex(zVEC3&)` `0x005688F0` without merging): 115 ASC files of that world, 0 faces with a repeated index → no
   double free from those ASC models.

Side finding: after the exe's static initializers `zCMemPoolBase::s_disablePools` (`0x008A2558`) is **1**; the game
sets it in `zDieter_StartUp` (`0x006306A9`: `DisablePools(zoptions->Parm("ZNOMEMPOOL"))`), i.e. 0 in a normal start.
A harness that does not reset it tests `operator new/delete` instead of the pools. The LA_AlphaPolyLimit harness sets 0. A harness built on the loader (`src\la_exe_image.h`) that does not reset the flag
runs with pools disabled and sees no pool traffic; set it to 0 for tests that involve the zCVertex pool.

### 2.4 Fix

5 bytes at `0x00573766` (the two instructions that load the polygon's vertex array and fetch vertex k; `eax` = vertex index k,
`ebx` = polygon) are replaced by `jmp StubDegen`. The stub re-executes both instructions, then compares `ebp` with `vertex[0..k-1]`; if it occurred
before, it jumps to the loop tail `0x005737FC` (no second "shared" scan, no second removal, no second `Free`),
otherwise back to `0x0057376B`. `edx` is free at this point (reloaded before every use), flags are dead. For the first
occurrence the engine logic is unchanged, so results are identical except that the vertex is freed once. The bug of the
sticky `shared` flag (later vertices of the same polygon are never freed = a small leak) is left as is.

Content side (optional): the two castle meshes have degenerate triangles with merged corners (e.g. a corner at
(1.822, 1133, −174.8)); cleaning them in the export removes the trigger even without the plugin.
