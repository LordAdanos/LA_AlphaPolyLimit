// LA_AlphaPolyLimit v1.2 - Union plugin for Gothic 2 NotR 2.6 fix
// License: MIT (see LICENSE). Own code; structure like LA_FastImport (single source, 32-bit DLL with static
// runtime, loaded from System\Autorun by Union, code check against the Steam Gothic2.exe before patching, ini, log).
//
// Two engine faults that stopped our game runs (analysis: doc\ANALYSE.md, addresses: doc\ADDRESSES.md):
//
//  1. Alpha world polygon pool (zCRndAlphaSortObject_WorldPoly::GetFreeAlphaWorldPoly 0x005D6FD0).
//     A static array of 4096 entries (0x009868B0, 0x14 bytes each). On overflow the engine reports
//     "D: RNDMAN: too many alphaWorldPolys!" once and WRAPS to slot 0 while the earlier slots are still linked in the
//     renderer's depth buckets (zCRnd_D3D::AddAlphaSortObject 0x00650990). Re-inserting a linked node can close a
//     cycle; zCRnd_D3D::RenderAlphaSortList 0x00650A60 then walks it forever (observed in game: 100 % CPU, no frames).
//     Fix: the only call site (zCRenderManager::Flush 0x005D7B10) gets its slot from our allocator: a larger pool
//     (AlphaWorldPolys, default 16384) and, when even that is full, NO slot -> the polygon is skipped for this frame
//     (never a wrap, so never a cycle).
//
//  2. zCVertex pool double free (zCMesh::DeletePolyAndVerts(ulong) 0x005736E0, called by RemoveDegeneratePolys
//     0x00573860 from zCProgMeshBuilder::BuildProgMeshProto 0x005C3E40 = every MRM/MDM build of -zautoconvertdata).
//     For each vertex of a degenerate polygon that no other polygon uses, the engine removes it from the vertex list
//     and frees it. A degenerate polygon that holds the SAME vertex twice (two corners at one position, merged by
//     AddVertexSmart, or an ASC face with a repeated index) frees that vertex twice. The pool's free list then
//     points at a vertex that is in use; the next zCVertex allocation crashes in zCMemPoolBase::Alloc 0x00566649
//     (observed in game) or silently corrupts memory.
//     Fix: at the top of the per-vertex loop (0x00573766) a vertex that already occurred earlier in the same
//     polygon is skipped (it was handled at its first occurrence).

#define LAAP_VERSION "v1.2"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

namespace laap {

constexpr uint32_t kImageBase = 0x00400000;

// ---- engine addresses (Steam Gothic2.exe 2.6 fix, 9,038,140 bytes) -----------------------------
// alpha world polygons
constexpr uint32_t kAlphaArray = 0x009868B0;     // static zCRndAlphaSortObject_WorldPoly[0x1000]
constexpr int kAlphaVanillaCount = 0x1000;
constexpr uint32_t kAlphaEntrySize = 0x14;       // vtable, next, z, polygon, key
constexpr uint32_t kAlphaVtable = 0x00832BC8;    // set by the constructor 0x005D6F10
constexpr uint32_t kAlphaCountImm = 0x005D6FF1;  // GetFreeAlphaWorldPoly: imm32 of cmp ebx,0x1000 (SystemPack: 0x2600)
constexpr uint32_t kAlphaBaseDisp = 0x005D70AD;  // GetFreeAlphaWorldPoly: disp32 of lea eax,[edx*4+0x9868B0] (SystemPack: own block)
constexpr uint32_t kAlphaCurrent = 0x0099AA70;   // s_currentAlphaWorldPoly (Flush and ResetFreeList set it to 0)
constexpr uint32_t kAlphaCallSite = 0x005D7B10;  // Flush: call GetFreeAlphaWorldPoly; mov edi,eax
constexpr uint32_t kAlphaBackOk = 0x005D7B17;    //   continue with the slot in edi
constexpr uint32_t kAlphaBackSkip = 0x005D7BAD;  //   loop tail: next polygon of the material bucket
// degenerate polygons
constexpr uint32_t kDegenSite = 0x00573766;      // DeletePolyAndVerts(ulong): mov ecx,[ebx]; mov ebp,[ecx+eax*4]
constexpr uint32_t kDegenBackOk = 0x0057376B;    //   continue with vertex ebp
constexpr uint32_t kDegenBackSkip = 0x005737FC;  //   loop tail: next vertex of the polygon

// ---- settings ----------------------------------------------------------------------------------
int g_enabled = 1;
int g_alphaFix = 1;
int g_alphaWanted = 16384;  // AlphaWorldPolys
int g_degenFix = 1;
int g_diag = 1;
int g_diagMaxLines = 200;

HMODULE g_module = nullptr;

// ---- log ---------------------------------------------------------------------------------------
HANDLE g_logFile = INVALID_HANDLE_VALUE;
int g_logLines = 0;
int g_logDropped = 0;

bool OpenLog() {
    if (g_logFile != INVALID_HANDLE_VALUE) return true;
    wchar_t path[MAX_PATH];
    if (!g_module || !GetModuleFileNameW(g_module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"LA_AlphaPolyLimit.log");
    g_logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_logFile != INVALID_HANDLE_VALUE;
}

void CloseLog() {
    if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
    g_logFile = INVALID_HANDLE_VALUE;
}

// summary lines (force) are written even after DiagMaxLines
void Log(const char* text, bool force = false) {
    if (!g_diag) return;
    if (!force && g_logLines >= g_diagMaxLines) {
        ++g_logDropped;
        return;
    }
    ++g_logLines;
    if (!g_module) {  // test harness: stdout
        printf("  [log] %s\n", text);
        return;
    }
    if (!OpenLog()) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[600];
    const int n = sprintf_s(line, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
    DWORD written = 0;
    if (n > 0) WriteFile(g_logFile, line, DWORD(n), &written, nullptr);
}

template <class T> T& Ref(uint32_t address) { return *reinterpret_cast<T*>(address); }

// ---- 1. alpha world polygon pool ---------------------------------------------------------------
uint8_t* g_alphaPool = nullptr;  // lazily set on the first call (after the exe's static constructors)
int g_alphaCap = 0;
int g_alphaPeak = 0;              // most slots used in one frame
long long g_alphaDropped = 0;     // polygons skipped because the pool was full
long long g_alphaDropFrames = 0;  // frames with at least one skipped polygon
int g_alphaFrameDropped = 0;
int g_alphaWorstFrame = 0;

// The engine's pool as GetFreeAlphaWorldPoly uses it right now: count = imm32 of "cmp ebx,imm" (0x005D6FF1), base =
// disp32 of "lea eax,[edx*4+disp]" (0x005D70AD). Vanilla 0x1000 / 0x009868B0; SystemPack (Union.patch, G2A block
// "[ALPHA_VOB and ALPHA_WORLD_POLY]") relocates it into its own memory with 0x2600 slots - and still wraps.
uint8_t* EngineAlphaArray() { return reinterpret_cast<uint8_t*>(Ref<uint32_t>(kAlphaBaseDisp)); }
int EngineAlphaCount() { return Ref<int>(kAlphaCountImm); }

void AlphaInitPool() {
    uint8_t* engineArray = EngineAlphaArray();
    const int engineCount = EngineAlphaCount();
    g_alphaPool = engineArray;
    g_alphaCap = engineCount;
    char line[260];
    if (g_alphaWanted > engineCount) {
        uint32_t vt = 0;
        __try {
            vt = *reinterpret_cast<uint32_t*>(engineArray);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            vt = 0;
        }
        if (vt != kAlphaVtable) {
            sprintf_s(line, "alpha pool: engine array %p not constructed (vtable 0x%08lX, expected 0x%08lX) - keeping %d slots",
                      engineArray, static_cast<unsigned long>(vt), static_cast<unsigned long>(kAlphaVtable), g_alphaCap);
            Log(line);
        } else if (void* mem = VirtualAlloc(nullptr, size_t(g_alphaWanted) * kAlphaEntrySize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
            uint8_t* p = static_cast<uint8_t*>(mem);
            for (int i = 0; i < g_alphaWanted; ++i) *reinterpret_cast<uint32_t*>(p + i * kAlphaEntrySize) = kAlphaVtable;  // rest 0
            g_alphaPool = p;
            g_alphaCap = g_alphaWanted;
        } else {
            sprintf_s(line, "alpha pool: VirtualAlloc of %d slots failed - keeping %d slots", g_alphaWanted, g_alphaCap);
            Log(line);
        }
    }
    sprintf_s(line, "alpha pool: %d slots at %p (engine array %d slots at %p; full pool = polygon skipped, no wrap)", g_alphaCap, g_alphaPool, EngineAlphaCount(), EngineAlphaArray());
    Log(line);
}

void AlphaFrameDone() {
    if (!g_alphaFrameDropped) return;
    ++g_alphaDropFrames;
    if (g_alphaFrameDropped > g_alphaWorstFrame) {
        g_alphaWorstFrame = g_alphaFrameDropped;
        char line[200];
        sprintf_s(line, "alpha pool full: %d alpha world polygons skipped in one frame (pool %d; frames with skips so far %lld)",
                  g_alphaFrameDropped, g_alphaCap, g_alphaDropFrames);
        Log(line);
    }
    g_alphaFrameDropped = 0;
}

// replaces zCRndAlphaSortObject_WorldPoly::GetFreeAlphaWorldPoly at its only call site; nullptr = skip the polygon
void* __cdecl AlphaGetFree() {
    if (!g_alphaPool) AlphaInitPool();
    int& current = Ref<int>(kAlphaCurrent);
    const int index = current;
    if (index == 0) AlphaFrameDone();  // Flush reset the counter: a new frame
    if (index < 0 || index >= g_alphaCap) {
        ++g_alphaDropped;
        ++g_alphaFrameDropped;
        return nullptr;
    }
    current = index + 1;
    if (index + 1 > g_alphaPeak) g_alphaPeak = index + 1;
    return g_alphaPool + size_t(index) * kAlphaEntrySize;
}

uint32_t g_alphaBackOk = kAlphaBackOk;
uint32_t g_alphaBackSkip = kAlphaBackSkip;

// at Flush 0x005D7B10 (replaces "call 0x005D6FD0; mov edi,eax"); ebx/esi/ebp/stack as in the engine loop
__declspec(naked) void StubAlpha() {
    __asm {
        call AlphaGetFree
        test eax, eax
        jz skip
        mov edi, eax
        jmp dword ptr [g_alphaBackOk]
    skip:
        jmp dword ptr [g_alphaBackSkip]
    }
}

// ---- 2. degenerate polygon with a repeated vertex -----------------------------------------------
long long g_degenSkipped = 0;  // repeated vertices skipped (= double frees prevented)
int g_degenLogged = 0;

void __cdecl OnDegenDuplicate(const float* vertex) {
    ++g_degenSkipped;
    if (g_degenLogged >= 20) return;
    ++g_degenLogged;
    char line[200];
    sprintf_s(line, "degenerate polygon holds vertex %p twice (%.2f %.2f %.2f): second free skipped (engine would double-free)",
              static_cast<const void*>(vertex), vertex ? vertex[0] : 0.0f, vertex ? vertex[1] : 0.0f, vertex ? vertex[2] : 0.0f);
    Log(line);
}

uint32_t g_degenBackOk = kDegenBackOk;
uint32_t g_degenBackSkip = kDegenBackSkip;

// at DeletePolyAndVerts 0x00573766: ebx = polygon, eax = vertex index k, [ebx] = vertex pointer array.
// Re-executes "mov ecx,[ebx]; mov ebp,[ecx+eax*4]"; if vertex k already occurs at an index < k it is skipped.
// edx is free here (the engine reloads it before every use), flags are not live.
__declspec(naked) void StubDegen() {
    __asm {
        mov ecx, [ebx]
        mov ebp, [ecx + eax * 4]
        xor edx, edx
    scan:
        cmp edx, eax
        jge unique
        cmp ebp, [ecx + edx * 4]
        je duplicate
        inc edx
        jmp scan
    unique:
        jmp dword ptr [g_degenBackOk]
    duplicate:
        pushad
        push ebp
        call OnDegenDuplicate
        add esp, 4
        popad
        jmp dword ptr [g_degenBackSkip]
    }
}

// ---- code check --------------------------------------------------------------------------------
struct CodeRange {
    uint32_t begin;
    uint32_t end;
};
// group 0 (alpha): everything the alpha fix relies on. The first 0x10 bytes of Flush are left out on purpose:
// renderer replacements (hooking ddraw) may detour zCRenderManager::Flush at its entry.
constexpr CodeRange kAlphaRanges[] = {
    {0x005D6FD0, 0x005D70BD},  // GetFreeAlphaWorldPoly (counter 0x0099AA70, array 0x009868B0, wrap)
    {0x005D70C0, 0x005D70D1},  // ResetFreeList (counter reset)
    {0x005D6F10, 0x005D6F19},  // constructor: vtable 0x00832BC8
    {0x005D7140, 0x005D72FB},  // zCRndAlphaSortObject_WorldPoly::Draw (uses the counter, not the array)
    {0x005D7A00, 0x005D7C90},  // zCRenderManager::Flush body with the call site and the loop tail
    {0x00650990, 0x00650A5B},  // zCRnd_D3D::AddAlphaSortObject (bucket insert, next at +4)
    {0x00650A60, 0x00650AD9},  // zCRnd_D3D::RenderAlphaSortList bucket walk
};
// group 1 (degenerate): DeletePolyAndVerts both overloads, RemoveDegeneratePolys loop, VertPartOfPoly, pool Free/Alloc
constexpr CodeRange kDegenRanges[] = {
    {0x00573650, 0x005736DE},  // DeletePolyAndVerts(zCPolygon*)
    {0x005736E0, 0x0057385F},  // DeletePolyAndVerts(ulong) with the patch site
    {0x00573860, 0x00573910},  // RemoveDegeneratePolys: CalcNormal loop -> DeletePolyAndVerts
    {0x005BA420, 0x005BA44C},  // zCPolygon::VertPartOfPoly
    {0x00566680, 0x005666BB},  // zCMemPoolBase::Free
    {0x005664E0, 0x0056666A},  // zCMemPoolBase::Alloc
    {0x005C3FA5, 0x005C3FBD},  // BuildProgMeshProto: RemoveDegeneratePolys unless flag 0x10
};
#ifndef LAAP_FP_ALPHA
#define LAAP_FP_ALPHA 0x33BDA70Fu  // Steam Gothic2.exe 2.6 fix, 9,038,140 bytes
#endif
#ifndef LAAP_FP_DEGEN
#define LAAP_FP_DEGEN 0x7CA0FECBu
#endif
constexpr uint32_t kExpectedAlpha = LAAP_FP_ALPHA;
constexpr uint32_t kExpectedDegen = LAAP_FP_DEGEN;

// Bytes left out of the fingerprint (hashed as 0): the pool size imm32 and the pool base disp32 of
// GetFreeAlphaWorldPoly. SystemPack's Union.patch (G2A, "[ALPHA_VOB and ALPHA_WORLD_POLY]") rewrites exactly these
// (0x2600 slots in its own memory block, address differs per start). The instructions around them stay checked;
// the values are validated separately (AlphaPoolConsistent).
inline bool Masked(uint32_t va) {
    return (va >= kAlphaCountImm && va < kAlphaCountImm + 4) || (va >= kAlphaBaseDisp && va < kAlphaBaseDisp + 4);
}
template <size_t N> uint32_t Fingerprint(const CodeRange (&ranges)[N]) {
    uint32_t hash = 2166136261u;
    for (const CodeRange& r : ranges)
        for (uint32_t va = r.begin; va < r.end; ++va) hash = (hash ^ (Masked(va) ? 0 : Ref<uint8_t>(va))) * 16777619u;
    return hash;
}

// pool size/base used by GetFreeAlphaWorldPoly must equal what the static constructor 0x005D6EE0 built and the atexit
// destructor 0x005D6F50 destroys (push count 0x005D6EEB / 0x005D6F62, push array 0x005D6EF2 / 0x005D6F6B), and the
// first slot must be readable. Holds for vanilla and for SystemPack's relocation.
bool AlphaPoolConsistent(char* why, size_t whySize) {
    __try {
        const uint32_t count = Ref<uint32_t>(kAlphaCountImm), base = Ref<uint32_t>(kAlphaBaseDisp);
        const bool pushes = Ref<uint8_t>(0x005D6EEA) == 0x68 && Ref<uint8_t>(0x005D6EF1) == 0x68 && Ref<uint8_t>(0x005D6F61) == 0x68 &&
                            Ref<uint8_t>(0x005D6F6A) == 0x68;
        const bool same = Ref<uint32_t>(0x005D6EEB) == count && Ref<uint32_t>(0x005D6F62) == count &&
                          Ref<uint32_t>(0x005D6EF2) == base && Ref<uint32_t>(0x005D6F6B) == base;
        const bool range = count >= 0x1000 && count <= 0x100000 && base >= 0x10000;
        sprintf_s(why, whySize, "engine alpha pool %lu slots at 0x%08lX%s", static_cast<unsigned long>(count), static_cast<unsigned long>(base),
                  count == 0x1000 && base == kAlphaArray ? " (vanilla)" : " (relocated, e.g. SystemPack)");
        if (!pushes || !same || !range) {
            strcat_s(why, whySize, " - INCONSISTENT with ctor/dtor");
            return false;
        }
        volatile uint8_t probe = *reinterpret_cast<const uint8_t*>(base);
        (void)probe;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(why, whySize, "engine alpha pool not readable");
        return false;
    }
}
template <size_t N> uint32_t SafeFingerprint(const CodeRange (&ranges)[N]) {
    __try {
        return Fingerprint(ranges);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint32_t ExeFileSize() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const DWORD size = GetFileSize(file, nullptr);
    CloseHandle(file);
    return size == INVALID_FILE_SIZE ? 0 : size;
}

// ---- ini ---------------------------------------------------------------------------------------
bool IniPath(wchar_t* path, bool besideDll) {
    HMODULE module = besideDll ? g_module : nullptr;
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"LA_AlphaPolyLimit.ini");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

int ReadInt(const wchar_t* ini, const wchar_t* key, int def, int lo, int hi) {
    const int v = int(GetPrivateProfileIntW(L"LA_ALPHAPOLYLIMIT", key, def, ini));
    return (v < lo || v > hi) ? def : v;
}

void LoadConfig() {
    wchar_t ini[MAX_PATH];
    if (!(g_module && IniPath(ini, true)) && !IniPath(ini, false)) return;  // defaults
    g_enabled = ReadInt(ini, L"Enabled", 1, 0, 1);
    g_alphaFix = ReadInt(ini, L"AlphaPoolFix", 1, 0, 1);
    g_alphaWanted = ReadInt(ini, L"AlphaWorldPolys", 16384, kAlphaVanillaCount, 262144);
    g_degenFix = ReadInt(ini, L"DegenerateFix", 1, 0, 1);
    g_diag = ReadInt(ini, L"Diag", 1, 0, 1);
    g_diagMaxLines = ReadInt(ini, L"DiagMaxLines", 200, 1, 100000);
}

// ---- patching ----------------------------------------------------------------------------------
bool Write(uint32_t address, const void* bytes, size_t size) {
    void* p = reinterpret_cast<void*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    return true;
}

struct PatchDef {
    uint32_t address;
    uint8_t size;
    uint8_t original[8];
    const void* target;  // jmp rel32 target, rest nop
    int group;           // 0 alpha, 1 degenerate
};

std::vector<PatchDef> Patches() {
    return {
        // call 0x005D6FD0 (GetFreeAlphaWorldPoly); mov edi,eax
        {kAlphaCallSite, 7, {0xE8, 0xBB, 0xF4, 0xFF, 0xFF, 0x8B, 0xF8}, reinterpret_cast<const void*>(&StubAlpha), 0},
        // mov ecx,[ebx]; mov ebp,[ecx+eax*4]
        {kDegenSite, 5, {0x8B, 0x0B, 0x8B, 0x2C, 0x81}, reinterpret_cast<const void*>(&StubDegen), 1},
    };
}

struct Applied {
    uint32_t address;
    uint8_t size;
    uint8_t saved[8];
};
std::vector<Applied> g_applied;
int g_activeAlpha = 0;
int g_activeDegen = 0;
uint32_t g_lastFpAlpha = 0;
uint32_t g_lastFpDegen = 0;

void Remove() {
    for (auto it = g_applied.rbegin(); it != g_applied.rend(); ++it) Write(it->address, it->saved, it->size);
    g_applied.clear();
    g_activeAlpha = g_activeDegen = 0;
}

bool ApplyGroup(int group) {
    for (const PatchDef& d : Patches()) {
        if (d.group != group) continue;
        if (memcmp(reinterpret_cast<const void*>(d.address), d.original, d.size) != 0) {
            char line[200];
            const uint8_t* b = reinterpret_cast<const uint8_t*>(d.address);
            sprintf_s(line, "bytes at 0x%08lX differ (%02X %02X %02X %02X %02X ...; another patch?) - group %d not patched",
                      static_cast<unsigned long>(d.address), b[0], b[1], b[2], b[3], b[4], group);
            Log(line);
            return false;
        }
    }
    for (const PatchDef& d : Patches()) {
        if (d.group != group) continue;
        Applied a{d.address, d.size, {}};
        memcpy(a.saved, reinterpret_cast<const void*>(d.address), d.size);
        uint8_t bytes[8];
        memset(bytes, 0x90, sizeof bytes);
        bytes[0] = 0xE9;
        const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(d.target)) - int32_t(d.address + 5);
        memcpy(bytes + 1, &rel, 4);
        if (!Write(d.address, bytes, d.size)) {
            Log("writing failed - group not patched");
            return false;
        }
        g_applied.push_back(a);
    }
    return true;
}

// checkModule=false only in the test harness (exe image mapped by hand at 0x400000)
bool Install(bool checkModule = true) {
    if (checkModule) LoadConfig();
    if (g_diag && g_module) OpenLog();
    char line[400];
    if (checkModule && reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        Log("LA_AlphaPolyLimit " LAAP_VERSION " loaded. Exe is not at 0x400000, so it is not Gothic2.exe. No patch.");
        return false;
    }
    g_lastFpAlpha = SafeFingerprint(kAlphaRanges);
    g_lastFpDegen = SafeFingerprint(kDegenRanges);
    char why[200];
    const bool okAlpha = g_lastFpAlpha == kExpectedAlpha && AlphaPoolConsistent(why, sizeof why);
    if (g_lastFpAlpha == kExpectedAlpha) Log(why);
    const bool okDegen = g_lastFpDegen == kExpectedDegen;
    sprintf_s(line, "LA_AlphaPolyLimit " LAAP_VERSION " loaded. Gothic2.exe %lu bytes, code check alpha 0x%08lX (%s), degenerate 0x%08lX (%s)",
              static_cast<unsigned long>(checkModule ? ExeFileSize() : 0), static_cast<unsigned long>(g_lastFpAlpha),
              okAlpha ? "ok" : "DIFFERS", static_cast<unsigned long>(g_lastFpDegen), okDegen ? "ok" : "DIFFERS");
    Log(line);
    if (!g_enabled) {
        Log("LA_AlphaPolyLimit: Enabled=0, no patch.");
        return false;
    }
    if (g_alphaFix && okAlpha && ApplyGroup(0)) g_activeAlpha = 1;
    if (g_degenFix && okDegen && ApplyGroup(1)) g_activeDegen = 1;
    sprintf_s(line, "LA_AlphaPolyLimit " LAAP_VERSION ": AlphaPoolFix %s (AlphaWorldPolys=%d), DegenerateFix %s",
              g_activeAlpha ? "active" : "off", g_alphaWanted, g_activeDegen ? "active" : "off");
    Log(line);
    return g_activeAlpha || g_activeDegen;
}

void LogSummary() {
    if (!g_activeAlpha && !g_activeDegen) return;
    AlphaFrameDone();
    char line[300];
    sprintf_s(line, "LA_AlphaPolyLimit summary: alpha pool %d slots, peak %d per frame, %lld polygons skipped in %lld frames; "
              "degenerate double frees prevented %lld; log lines dropped %d",
              g_alphaCap, g_alphaPeak, g_alphaDropped, g_alphaDropFrames, g_degenSkipped, g_logDropped);
    Log(line, true);
}

}  // namespace laap

#ifndef LAAP_NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        laap::g_module = module;
        DisableThreadLibraryCalls(module);
        laap::Install();
    } else if (reason == DLL_PROCESS_DETACH) {
        laap::LogSummary();
        if (reserved == nullptr) laap::Remove();
        laap::CloseLog();
    }
    return TRUE;
}
#endif
