// LA_AlphaPolyLimit test harness (own code, MIT like the plugin). Gothic is never started: the Steam Gothic2.exe is mapped
// read-only into this 32-bit process (loader ..\src\la_exe_image.h) and the ENGINE'S OWN machine code is
// called, once original and once with the LA_AlphaPolyLimit patches written into the mapped image.
//
// Usage: laap_harness.exe <Gothic2.exe> <test> [args]
//   fingerprint            : code fingerprints of both patch groups
//   patchcheck             : install -> both sites jmp to our stubs -> remove -> original bytes
//   degen orig|patched     : synthetic meshes through zCMesh::RemoveDegeneratePolys 0x00573860 (engine code), vertex
//                            pool free-list check; orig must show the double free, patched must stay clean
//   files <list> orig|patched : real 3DS files (engine AddVertexSmart/AddPoly like the vob import), prediction of the
//                            engine's double frees, engine RemoveDegeneratePolys, pool check, DeleteAll, pool check.
//                            orig stops at the first file whose prediction says "double free" and shows the corruption.
//   alpha                  : GetFreeAlphaWorldPoly wrap (engine) -> AddAlphaSortObject (engine) -> bucket cycle;
//                            the same with LA_AlphaPolyLimit' allocator (4096 and 16384 slots) -> no cycle; stub register test
#define LAAP_NO_DLLMAIN
#include "../src/la_alphapolylimit.cpp"
#include "../src/la_exe_image.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>

namespace {

int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_failed;                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #c);                     \
        }                                                                     \
    } while (0)

// ------------------------------------------------------------------------------------------- engine
using FnCreateMesh = void*(__cdecl*)();
using FnInitCache = void(__cdecl*)(void* mesh);
using FnDeleteCache = void(__cdecl*)();
using FnAllocN = void(__thiscall*)(void* mesh, int n);
using FnAddVertexSmart = void*(__thiscall*)(void* mesh, const float* v);
using FnAddPoly3 = void*(__thiscall*)(void* mesh, void* a, void* b, void* c);
using FnMeshVoid = void(__thiscall*)(void* mesh);
using FnCalcNormal = int(__thiscall*)(void* poly);
const auto E_CreateMesh = reinterpret_cast<FnCreateMesh>(0x00566CD0);         // zCMesh::_CreateNewInstance
const auto E_InitCache = reinterpret_cast<FnInitCache>(0x005680A0);           // zCMesh::S_InitVertexMergeCache
const auto E_DeleteCache = reinterpret_cast<FnDeleteCache>(0x00568320);       // zCMesh::S_DeleteVertexMergeCache
const auto E_AllocVerts = reinterpret_cast<FnAllocN>(0x00568500);
const auto E_AllocPolys = reinterpret_cast<FnAllocN>(0x005685F0);
const auto E_AddVertexSmart = reinterpret_cast<FnAddVertexSmart>(0x00568B80);
const auto E_AddPoly3 = reinterpret_cast<FnAddPoly3>(0x00568EA0);
const auto E_RemoveDegenerate = reinterpret_cast<FnMeshVoid>(0x00573860);     // zCMesh::RemoveDegeneratePolys
const auto E_DeleteAll = reinterpret_cast<FnMeshVoid>(0x00567290);            // zCMesh::DeleteAll
const auto E_CalcNormal = reinterpret_cast<FnCalcNormal>(0x005B8180);         // zCPolygon::CalcNormal
constexpr uint32_t kVertexPool = 0x008D4304;  // zCMemPool<zCVertex>: +0xC free head, +0x10 free count

int NumVerts(void* m) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(m) + 0x38); }
int NumPolys(void* m) { return *reinterpret_cast<int*>(static_cast<uint8_t*>(m) + 0x34); }
void** Polys(void* m) { return *reinterpret_cast<void***>(static_cast<uint8_t*>(m) + 0x44); }
void** PolyVerts(void* p) { return *reinterpret_cast<void***>(p); }
int PolyNum(void* p) { return static_cast<uint8_t*>(p)[0x30]; }

// walks the zCVertex free list (next at +0, prev at +4, head at pool+0xC); "" = consistent
int WalkPool(int* count, int* expected, void** bad) {
    uint8_t* pool = reinterpret_cast<uint8_t*>(kVertexPool);
    *expected = *reinterpret_cast<int*>(pool + 0x10);
    void** link = reinterpret_cast<void**>(pool + 0xC);
    int n = 0;
    *bad = nullptr;
    __try {
        for (void** node = static_cast<void**>(*link); node; node = static_cast<void**>(node[0])) {
            if (node[1] != reinterpret_cast<void*>(link)) {
                *bad = node;
                *count = n;
                return 1;
            }
            link = node;
            if (++n > *expected + 1) {
                *count = n;
                return 2;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *count = n;
        return 3;
    }
    *count = n;
    return n == *expected ? 0 : 4;
}
std::string CheckPool(int* freeCount = nullptr) {
    int count = 0, expected = 0;
    void* bad = nullptr;
    const int r = WalkPool(&count, &expected, &bad);
    if (freeCount) *freeCount = count;
    char b[220] = "";
    if (r == 1)
        sprintf_s(b, "free node %p has a wrong prev link after %d nodes (next %08X prev %08X)", bad, count,
                  static_cast<uint32_t*>(bad)[0], static_cast<uint32_t*>(bad)[1]);
    if (r == 2) sprintf_s(b, "free list longer than the free count %d (cycle)", expected);
    if (r == 3) sprintf_s(b, "access violation while walking the free list after %d nodes", count);
    if (r == 4) sprintf_s(b, "free list has %d nodes, pool counts %d", count, expected);
    return b;
}

// ---------------------------------------------------------------- prediction of RemoveDegeneratePolys
// Emulates zCMesh::RemoveDegeneratePolys + DeletePolyAndVerts(ulong) (decompile in doc\ANALYSE.md) on the polygon
// lists: degenerate = engine zCPolygon::CalcNormal returns 0; per vertex: sticky "shared" flag, vertex used by any
// other polygon -> shared, else freed. harmful = a vertex freed twice; dups = repeated vertex occurrences.
struct Prediction {
    int degenerate = 0, harmful = 0, dups = 0;
    std::vector<std::array<float, 3>> harmfulPos;  // positions of the vertices the engine frees twice
};
Prediction Predict(void* mesh) {
    Prediction p;
    std::vector<std::vector<void*>> polys;
    std::vector<int> degen;
    for (int i = 0; i < NumPolys(mesh); ++i) {
        void* poly = Polys(mesh)[i];
        polys.emplace_back(PolyVerts(poly), PolyVerts(poly) + PolyNum(poly));
        degen.push_back(E_CalcNormal(poly) == 0);
    }
    std::set<void*> freed;
    for (size_t i = 0; i < polys.size();) {
        if (!degen[i]) {
            ++i;
            continue;
        }
        ++p.degenerate;
        bool shared = false;
        const auto& pv = polys[i];
        for (size_t k = 0; k < pv.size(); ++k) {
            void* v = pv[k];
            if (std::find(pv.begin(), pv.begin() + k, v) != pv.begin() + k) ++p.dups;
            if (shared) continue;
            for (size_t o = 0; o < polys.size() && !shared; ++o)
                if (o != i && std::find(polys[o].begin(), polys[o].end(), v) != polys[o].end()) shared = true;
            if (shared) continue;
            if (!freed.insert(v).second) {
                ++p.harmful;
                const float* f = static_cast<const float*>(v);
                p.harmfulPos.push_back({f[0], f[1], f[2]});
            }
        }
        polys.erase(polys.begin() + i);
        degen.erase(degen.begin() + i);
    }
    return p;
}

// ------------------------------------------------------------------------------------------ 3DS read
struct Mesh3ds {
    std::vector<float> pos;
    std::vector<uint32_t> faces;
};
void Parse3dsChunks(const uint8_t* p, const uint8_t* end, Mesh3ds& m, uint32_t& base) {
    while (p + 6 <= end) {
        const uint16_t id = *reinterpret_cast<const uint16_t*>(p);
        const uint32_t len = *reinterpret_cast<const uint32_t*>(p + 2);
        if (len < 6 || p + len > end) return;
        const uint8_t* data = p + 6;
        const uint8_t* next = p + len;
        switch (id) {
            case 0x4D4D: case 0x3D3D: Parse3dsChunks(data, next, m, base); break;
            case 0x4000: {
                const uint8_t* q = data;
                while (q < next && *q) ++q;
                Parse3dsChunks(q + 1, next, m, base);
                break;
            }
            case 0x4100: Parse3dsChunks(data, next, m, base); break;
            case 0x4110: {
                const uint16_t n = *reinterpret_cast<const uint16_t*>(data);
                const float* v = reinterpret_cast<const float*>(data + 2);
                for (int i = 0; i < n; ++i) {
                    m.pos.push_back(v[3 * i + 0]);
                    m.pos.push_back(v[3 * i + 2]);
                    m.pos.push_back(v[3 * i + 1]);
                }
                break;
            }
            case 0x4120: {
                const uint16_t n = *reinterpret_cast<const uint16_t*>(data);
                const uint16_t* f = reinterpret_cast<const uint16_t*>(data + 2);
                for (int i = 0; i < n; ++i)
                    for (int k = 0; k < 3; ++k) m.faces.push_back(base + f[4 * i + k]);
                break;
            }
            default: break;
        }
        if (id == 0x4100) base = uint32_t(m.pos.size() / 3);
        p = next;
    }
}
bool Load3ds(const std::string& path, Mesh3ds& m) {
    std::vector<uint8_t> data;
    if (!laimg::ReadFileAll(path.c_str(), data)) return false;
    uint32_t base = 0;
    Parse3dsChunks(data.data(), data.data() + data.size(), m, base);
    for (uint32_t f : m.faces)
        if (f >= m.pos.size() / 3) return false;
    return true;
}
// what zCFile3DS::Load3DS does with the positions: merge cache + AddVertexSmart + AddPoly (as in LA_FastImport's
// vobload test); UV wrap splits (TPoly3ds::CheckForWrap) are not modelled - they only add vertices
void* BuildMesh(const Mesh3ds& m) {
    void* mesh = E_CreateMesh();
    const int nv = int(m.pos.size() / 3), nf = int(m.faces.size() / 3);
    E_InitCache(mesh);
    E_AllocVerts(mesh, nv);
    std::vector<void*> vp(nv);
    for (int i = 0; i < nv; ++i) vp[i] = E_AddVertexSmart(mesh, &m.pos[3 * i]);
    E_AllocPolys(mesh, nf);
    for (int f = 0; f < nf; ++f) E_AddPoly3(mesh, vp[m.faces[3 * f]], vp[m.faces[3 * f + 1]], vp[m.faces[3 * f + 2]]);
    E_DeleteCache();
    return mesh;
}

// ------------------------------------------------------------------------------------ test: degen
int TestDegen(bool patched) {
    if (patched) CHECK(laap::Install(false) && laap::g_activeDegen);
    printf("degen (%s): synthetic meshes through the engine's RemoveDegeneratePolys\n", patched ? "patched" : "orig");
    // mesh 1: two good triangles + isolated degenerate triangles (A,A,B) and (C,D,C) whose vertices no other polygon uses
    const float pts[][3] = {{0, 0, 0}, {100, 0, 0}, {0, 0, 100}, {100, 0, 100},       // good quad
                            {500, 0, 0}, {600, 0, 0},                                  // A B
                            {900, 0, 0}, {950, 10, 0},                                 // C D
                            {0, 0, 0}};
    void* mesh = E_CreateMesh();
    E_InitCache(mesh);
    void* v[8];
    for (int i = 0; i < 8; ++i) v[i] = E_AddVertexSmart(mesh, pts[i]);
    E_AddPoly3(mesh, v[0], v[1], v[2]);
    E_AddPoly3(mesh, v[1], v[3], v[2]);
    E_AddPoly3(mesh, v[4], v[4], v[5]);  // A A B
    E_AddPoly3(mesh, v[6], v[7], v[6]);  // C D C
    E_DeleteCache();
    int freeBefore = 0;
    CHECK(CheckPool(&freeBefore).empty());
    const Prediction pr = Predict(mesh);
    printf("  mesh: %d verts %d polys; prediction: %d degenerate, %d repeated vertices, %d double frees\n", NumVerts(mesh),
           NumPolys(mesh), pr.degenerate, pr.dups, pr.harmful);
    CHECK(pr.degenerate == 2 && pr.harmful == 2 && pr.dups == 2);
    const long long skippedBefore = laap::g_degenSkipped;
    E_RemoveDegenerate(mesh);
    int freeAfter = 0;
    const std::string pool = CheckPool(&freeAfter);
    printf("  after RemoveDegeneratePolys: %d verts %d polys, pool %s (free %d -> %d), duplicates skipped by plugin %lld\n",
           NumVerts(mesh), NumPolys(mesh), pool.empty() ? "ok" : pool.c_str(), freeBefore, freeAfter,
           laap::g_degenSkipped - skippedBefore);
    CHECK(NumPolys(mesh) == 2 && NumVerts(mesh) == 4);
    if (!patched) {
        CHECK(!pool.empty());  // the engine's double free must show up
        printf("RESULT degen orig: engine double free %s\n", pool.empty() ? "NOT reproduced" : "REPRODUCED");
        printf("RESULT degen: %d checks, %d failed\n", g_checks, g_failed);
        return g_failed ? 1 : 0;
    }
    CHECK(pool.empty());
    CHECK(freeAfter == freeBefore + 4);  // A, B, C, D each freed exactly once
    CHECK(laap::g_degenSkipped - skippedBefore == 2);
    // mesh 2: a degenerate polygon whose repeated vertex is shared with a good triangle (engine keeps it) + 4-gon
    void* m2 = E_CreateMesh();
    E_InitCache(m2);
    for (int i = 0; i < 8; ++i) v[i] = E_AddVertexSmart(m2, pts[i]);
    E_AddPoly3(m2, v[0], v[1], v[2]);
    E_AddPoly3(m2, v[0], v[0], v[4]);  // shares v0 with the good triangle
    E_AddPoly3(m2, v[5], v[5], v[5]);  // one vertex three times
    E_DeleteCache();
    const Prediction p2 = Predict(m2);
    E_RemoveDegenerate(m2);
    const std::string pool2 = CheckPool(&freeAfter);
    printf("  mesh 2: prediction %d degenerate, %d double frees; after: %d verts %d polys, pool %s\n", p2.degenerate,
           p2.harmful, NumVerts(m2), NumPolys(m2), pool2.empty() ? "ok" : pool2.c_str());
    CHECK(pool2.empty() && NumPolys(m2) == 1);
    E_DeleteAll(mesh);
    E_DeleteAll(m2);
    const std::string pool3 = CheckPool();
    CHECK(pool3.empty());
    laap::Remove();
    printf("RESULT degen patched: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

// ------------------------------------------------------------------------------------ test: files
int TestFiles(const char* listPath, bool patched) {
    std::vector<uint8_t> raw;
    if (!laimg::ReadFileAll(listPath, raw)) {
        fprintf(stderr, "cannot read %s\n", listPath);
        return 2;
    }
    std::vector<std::string> files;
    std::string cur;
    for (uint8_t c : raw) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) files.push_back(cur);
            cur.clear();
        } else {
            cur += char(c);
        }
    }
    if (!cur.empty()) files.push_back(cur);
    if (patched) CHECK(laap::Install(false) && laap::g_activeDegen);
    printf("files (%s): %zu 3DS\n", patched ? "patched" : "orig", files.size());
    int unreadable = 0, withDegen = 0, withHarmful = 0, poolErrors = 0;
    long long sumDegen = 0, sumHarmful = 0, sumDups = 0, sumSkipped = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        Mesh3ds m;
        const std::string name = files[i].substr(files[i].find_last_of("\\/") + 1);
        if (!Load3ds(files[i], m) || m.faces.empty()) {
            ++unreadable;
            continue;
        }
        void* mesh = BuildMesh(m);
        const Prediction pr = Predict(mesh);
        sumDegen += pr.degenerate;
        sumHarmful += pr.harmful;
        sumDups += pr.dups;
        if (pr.degenerate) ++withDegen;
        if (pr.harmful) {
            ++withHarmful;
            if (withHarmful <= 40 || !patched)
                printf("  %5zu %-48s verts %6d polys %6d: %d degenerate, %d repeated vertices -> %d double free(s) in the engine\n", i,
                       name.c_str(), NumVerts(mesh), NumPolys(mesh), pr.degenerate, pr.dups, pr.harmful);
            for (const auto& q : pr.harmfulPos) {
                uint32_t b[3];
                memcpy(b, q.data(), 12);
                printf("          vertex freed twice: (%.3f %.3f %.3f) = bits %08X %08X %08X\n", q[0], q[1], q[2], b[0], b[1], b[2]);
            }
        }
        if (!patched && !pr.harmful) {  // orig: only files without predicted double free are run (the pool must stay usable)
            E_RemoveDegenerate(mesh);
            E_DeleteAll(mesh);
            const std::string pool = CheckPool();
            if (!pool.empty()) {
                printf("  %5zu %s: pool error without predicted double free: %s\n", i, name.c_str(), pool.c_str());
                ++poolErrors;
                break;
            }
            continue;
        }
        if (!patched) {  // first file with a predicted double free: run the engine and show the corruption, then stop
            E_RemoveDegenerate(mesh);
            const std::string pool = CheckPool();
            printf("  -> engine RemoveDegeneratePolys (original code) on %s: vertex pool %s\n", name.c_str(),
                   pool.empty() ? "still consistent (prediction NOT confirmed)" : pool.c_str());
            CHECK(!pool.empty());
            printf("RESULT files orig: first predicted file %s %s after %zu files\n", name.c_str(),
                   pool.empty() ? "did NOT corrupt the pool" : "CORRUPTED the zCVertex pool (double free reproduced)", i + 1);
            printf("RESULT files: %d checks, %d failed\n", g_checks, g_failed);
            return g_failed ? 1 : 0;
        }
        const long long before = laap::g_degenSkipped;
        E_RemoveDegenerate(mesh);
        const long long skipped = laap::g_degenSkipped - before;
        sumSkipped += skipped;
        std::string pool = CheckPool();
        if (pool.empty()) {
            E_DeleteAll(mesh);
            pool = CheckPool();
        }
        if (!pool.empty() || skipped != pr.dups) {
            ++poolErrors;
            printf("  %5zu %s: pool %s, skipped %lld vs predicted repeated %d\n", i, name.c_str(), pool.empty() ? "ok" : pool.c_str(),
                   skipped, pr.dups);
            if (!pool.empty()) break;
        }
        if (i % 500 == 0) printf("  ... %zu files\n", i);
        fflush(stdout);
    }
    printf("  totals: %zu files (%d unreadable/empty), %d with degenerate polygons (%lld), %d with engine double frees (%lld), "
           "%lld repeated vertices, plugin skipped %lld\n",
           files.size(), unreadable, withDegen, sumDegen, withHarmful, sumHarmful, sumDups, sumSkipped);
    CHECK(poolErrors == 0);
    if (patched) CHECK(sumSkipped == sumDups);
    printf("RESULT files %s: %d pool errors; %d of %zu files would double-free in the original engine\n",
           patched ? "patched" : "orig", poolErrors, withHarmful, files.size());
    printf("RESULT files: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

// ------------------------------------------------------------------------------------ test: alpha
using FnGetFree = void*(__cdecl*)();
using FnAddAlpha = void(__thiscall*)(void* rnd, void* obj);
const auto E_GetFreeAlpha = reinterpret_cast<FnGetFree>(0x005D6FD0);
const auto E_AddAlphaSortObject = reinterpret_cast<FnAddAlpha>(0x00650990);
constexpr uint32_t kRndBuckets = 0x82A28;  // zCRnd_D3D: 256 bucket heads (+0x82A28 .. +0x82E24)
constexpr uint32_t kRndZScale = 0x82E28;   // float factor z -> bucket

// walks all 256 buckets of the fake renderer; returns reachable nodes, cycles = buckets whose walk exceeds the limit
int WalkBuckets(uint8_t* rnd, int limit, int* cycles) {
    int total = 0;
    *cycles = 0;
    for (int b = 0; b < 256; ++b) {
        int steps = 0;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(rnd + kRndBuckets + 4 * b); node; node = *reinterpret_cast<uint8_t**>(node + 4)) {
            if (++steps > limit) {
                ++*cycles;
                break;
            }
        }
        total += steps;
    }
    return total;
}

// one "frame": counter reset like Flush, n objects from `alloc`, each into the engine's AddAlphaSortObject
int AlphaFrame(uint8_t* rnd, void* (*alloc)(), int n, int* got, int* cycles) {
    memset(rnd + kRndBuckets, 0, 256 * 4);
    laap::Ref<int>(laap::kAlphaCurrent) = 0;
    *got = 0;
    uint32_t seed = 12345;
    for (int i = 0; i < n; ++i) {
        uint8_t* obj = static_cast<uint8_t*>(alloc());
        if (!obj) continue;
        ++*got;
        seed = seed * 1664525u + 1013904223u;
        const float z = 100.0f + float(seed >> 8) / float(1u << 24) * 9000.0f;  // 1 m .. 91 m
        memcpy(obj + 8, &z, 4);
        E_AddAlphaSortObject(rnd, obj);
    }
    return WalkBuckets(rnd, 4 * n + 16, cycles);
}
void* AllocEngine() { return E_GetFreeAlpha(); }

volatile int g_wrapProgress = 0;
uint8_t* g_wrapRnd = nullptr;
int g_wrapGot = 0, g_wrapReach = 0, g_wrapCycles = 0;
void* AllocEngineCounted() {
    ++g_wrapProgress;
    return E_GetFreeAlpha();
}
DWORD WINAPI WrapWorker(void*) {
    g_wrapReach = AlphaFrame(g_wrapRnd, AllocEngineCounted, 6000, &g_wrapGot, &g_wrapCycles);
    return 0;
}
void* AllocPlugin() { return laap::AlphaGetFree(); }

// stub register test: enter StubAlpha like the engine (jmp), continuation targets redirected to our thunks
uint32_t g_rEdi, g_rEbx, g_rEsi, g_rEbp, g_rEsp, g_espIn, g_branch;
uint32_t g_stubAlpha = 0;  // address of laap::StubAlpha (set in TestAlpha)
__declspec(naked) void ContOk() {
    __asm {
        mov g_rEdi, edi
        mov g_rEbx, ebx
        mov g_rEsi, esi
        mov g_rEbp, ebp
        mov g_rEsp, esp
        mov g_branch, 1
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
__declspec(naked) void ContSkip() {
    __asm {
        mov g_rEdi, edi
        mov g_rEbx, ebx
        mov g_rEsi, esi
        mov g_rEbp, ebp
        mov g_rEsp, esp
        mov g_branch, 2
        pop ebp
        pop edi
        pop esi
        pop ebx
        ret
    }
}
__declspec(naked) void EnterStubAlpha() {
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        mov ebx, 0x11111111
        mov esi, 0x22222222
        mov ebp, 0x33333333
        mov edi, 0x44444444
        mov g_espIn, esp
        jmp dword ptr [g_stubAlpha]
    }
}

int TestAlpha() {
    printf("alpha: engine GetFreeAlphaWorldPoly + AddAlphaSortObject on a stand-in renderer\n");
    uint8_t* rnd = static_cast<uint8_t*>(laimg::H_calloc(1, 0x83000));
    const float scale = 255.0f / 10000.0f;
    memcpy(rnd + kRndZScale, &scale, 4);
    CHECK(laap::Ref<uint32_t>(laap::kAlphaArray) == laap::kAlphaVtable);  // static constructors ran: vtable 0x832BC8
    laap::Ref<uint8_t>(0x0099AA80) = 1;  // "already reported" flag: the engine skips its zERR line
    int got = 0, cycles = 0;
    // engine, 4000 polys: no overflow
    int reach = AlphaFrame(rnd, AllocEngine, 4000, &got, &cycles);
    printf("  engine pool, 4000 polygons: %d inserted, %d reachable, %d cyclic buckets\n", got, reach, cycles);
    CHECK(reach == 4000 && cycles == 0);
    // engine, 6000 polys: wrap after 4096
    void* first = nullptr;
    {
        laap::Ref<int>(laap::kAlphaCurrent) = 4096;
        void* wrapped = E_GetFreeAlpha();
        first = reinterpret_cast<void*>(laap::kAlphaArray);
        printf("  engine: counter 4096 -> GetFreeAlphaWorldPoly returns %p (slot 0 = %p): %s\n", wrapped, first,
               wrapped == first ? "WRAP" : "no wrap");
        CHECK(wrapped == first);
    }
    // the wrapped frame runs in a worker thread: with a cycle already AddAlphaSortObject's sorted insert never returns
    g_wrapRnd = rnd;
    g_wrapProgress = 0;
    HANDLE worker = CreateThread(nullptr, 0, WrapWorker, nullptr, 0, nullptr);
    const bool hung = WaitForSingleObject(worker, 10000) == WAIT_TIMEOUT;
    if (hung) {
        TerminateThread(worker, 0);  // test process only: the engine loop has no exit
        printf("  engine pool, 6000 polygons: HANG - the engine's own alpha list code loops forever after %d of 6000 insertions "
               "(cycle in a depth bucket; observed symptom: 100 %% CPU, no frames)\n", g_wrapProgress);
    } else {
        printf("  engine pool, 6000 polygons: %d handed out, %d reachable, %d cyclic buckets\n", g_wrapGot, g_wrapReach, g_wrapCycles);
    }
    CloseHandle(worker);
    CHECK(hung || g_wrapCycles > 0 || g_wrapReach != 6000);
    const bool engineCycle = hung || g_wrapCycles > 0;
    // plugin allocator, vanilla size 4096: overflow -> skipped, lists stay sound
    laap::g_alphaPool = nullptr;
    laap::g_alphaWanted = 4096;
    reach = AlphaFrame(rnd, AllocPlugin, 6000, &got, &cycles);
    printf("  LA_AlphaPolyLimit 4096 slots, 6000 polygons: %d inserted, %d skipped, %d reachable, %d cyclic buckets\n", got, 6000 - got,
           reach, cycles);
    CHECK(got == 4096 && reach == 4096 && cycles == 0 && laap::g_alphaCap == 4096);
    // plugin allocator, 16384 slots: everything fits
    laap::g_alphaPool = nullptr;
    laap::g_alphaWanted = 16384;
    reach = AlphaFrame(rnd, AllocPlugin, 6000, &got, &cycles);
    printf("  LA_AlphaPolyLimit 16384 slots, 6000 polygons: %d inserted, %d reachable, %d cyclic buckets, pool %p\n", got, reach, cycles,
           laap::g_alphaPool);
    CHECK(got == 6000 && reach == 6000 && cycles == 0 && laap::g_alphaCap == 16384);
    CHECK(laap::Ref<uint32_t>(reinterpret_cast<uint32_t>(laap::g_alphaPool) + 0x14 * 9000) == laap::kAlphaVtable);
    reach = AlphaFrame(rnd, AllocPlugin, 20000, &got, &cycles);
    printf("  LA_AlphaPolyLimit 16384 slots, 20000 polygons: %d inserted, %d skipped, %d reachable, %d cyclic buckets\n", got, 20000 - got,
           reach, cycles);
    CHECK(got == 16384 && reach == 16384 && cycles == 0);
    // depth order inside a bucket stays the engine's (descending z along next)
    int unsorted = 0;
    for (int b = 0; b < 256; ++b) {
        float last = 1e30f;
        for (uint8_t* node = *reinterpret_cast<uint8_t**>(rnd + kRndBuckets + 4 * b); node; node = *reinterpret_cast<uint8_t**>(node + 4)) {
            float z;
            memcpy(&z, node + 8, 4);
            if (z > last) ++unsorted;
            last = z;
        }
    }
    printf("  bucket order: %d out-of-order neighbours (engine sorts far to near)\n", unsorted);
    CHECK(unsorted == 0);
    // stub: both branches, callee-saved registers and stack as the engine loop expects
    g_stubAlpha = reinterpret_cast<uint32_t>(&laap::StubAlpha);
    laap::g_alphaBackOk = reinterpret_cast<uint32_t>(&ContOk);
    laap::g_alphaBackSkip = reinterpret_cast<uint32_t>(&ContSkip);
    laap::Ref<int>(laap::kAlphaCurrent) = 5;
    reinterpret_cast<void(__cdecl*)()>(&EnterStubAlpha)();
    printf("  stub, free slot: branch %u edi %08X ebx %08X esi %08X ebp %08X esp %s\n", g_branch, g_rEdi, g_rEbx, g_rEsi, g_rEbp,
           g_rEsp == g_espIn ? "same" : "DIFFERS");
    CHECK(g_branch == 1 && g_rEdi == reinterpret_cast<uint32_t>(laap::g_alphaPool) + 5 * 0x14 && g_rEbx == 0x11111111 &&
          g_rEsi == 0x22222222 && g_rEbp == 0x33333333 && g_rEsp == g_espIn);
    laap::Ref<int>(laap::kAlphaCurrent) = laap::g_alphaCap;
    reinterpret_cast<void(__cdecl*)()>(&EnterStubAlpha)();
    printf("  stub, pool full: branch %u edi %08X (unchanged) esp %s counter %d\n", g_branch, g_rEdi, g_rEsp == g_espIn ? "same" : "DIFFERS",
           laap::Ref<int>(laap::kAlphaCurrent));
    CHECK(g_branch == 2 && g_rEdi == 0x44444444 && g_rEbx == 0x11111111 && g_rEsi == 0x22222222 && g_rEbp == 0x33333333 &&
          g_rEsp == g_espIn && laap::Ref<int>(laap::kAlphaCurrent) == laap::g_alphaCap);
    laap::g_alphaBackOk = laap::kAlphaBackOk;
    laap::g_alphaBackSkip = laap::kAlphaBackSkip;
    printf("RESULT alpha: engine wrap %s; LA_AlphaPolyLimit: no cycle, overflow skipped; %d checks, %d failed\n",
           engineCycle ? "-> CYCLE reproduced" : "-> corrupt lists", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

// ------------------------------------------------------------------------------- patchcheck etc.
int TestPatchcheck() {
    const auto defs = laap::Patches();
    std::vector<std::vector<uint8_t>> before;
    for (const auto& d : defs) {
        before.emplace_back(reinterpret_cast<const uint8_t*>(d.address), reinterpret_cast<const uint8_t*>(d.address) + d.size);
        CHECK(memcmp(reinterpret_cast<const void*>(d.address), d.original, d.size) == 0);
    }
    CHECK(laap::Install(false));
    CHECK(laap::g_activeAlpha == 1 && laap::g_activeDegen == 1);
    for (const auto& d : defs) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(d.address);
        int32_t rel;
        memcpy(&rel, p + 1, 4);
        const uint32_t target = d.address + 5 + rel;
        printf("  0x%08X: %02X -> 0x%08X (stub %p), %d nop\n", d.address, p[0], target, d.target, d.size - 5);
        CHECK(p[0] == 0xE9 && target == reinterpret_cast<uint32_t>(d.target));
        for (int k = 5; k < d.size; ++k) CHECK(p[k] == 0x90);
    }
    laap::Remove();
    for (size_t i = 0; i < defs.size(); ++i) CHECK(memcmp(reinterpret_cast<const void*>(defs[i].address), before[i].data(), defs[i].size) == 0);
    // a foreign patch on a site: group is skipped, the other group still installs
    const uint8_t foreign[5] = {0xE9, 0, 0, 0, 0};
    uint8_t keep[5];
    memcpy(keep, reinterpret_cast<const void*>(laap::kDegenSite), 5);
    laap::Write(laap::kDegenSite, foreign, 5);
    laap::Install(false);
    CHECK(laap::g_activeAlpha == 1 && laap::g_activeDegen == 0);
    laap::Remove();
    laap::Write(laap::kDegenSite, keep, 5);
    // SystemPack (System\Union.patch, G2A "[ALPHA_VOB and ALPHA_WORLD_POLY]"): count 0x2600 and the array moved into its
    // own block - written at 0x005D6EEB/0x005D6EF2 (ctor), 0x005D6F62/0x005D6F6B (dtor), 0x005D6FF1 (cmp), 0x005D70AD (lea).
    // Emulated here exactly like that (block constructed like the ctor does: vtable per slot).
    {
        const uint32_t spCount = 0x2600;
        uint8_t* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, spCount * 0x14, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        for (uint32_t i = 0; i < spCount; ++i) *reinterpret_cast<uint32_t*>(block + i * 0x14) = laap::kAlphaVtable;
        const uint32_t base = reinterpret_cast<uint32_t>(block);
        const uint32_t sites[] = {0x005D6EEB, 0x005D6F62, laap::kAlphaCountImm, 0x005D6EF2, 0x005D6F6B, laap::kAlphaBaseDisp};
        uint32_t saved[6];
        for (int k = 0; k < 6; ++k) saved[k] = laap::Ref<uint32_t>(sites[k]);
        for (int k = 0; k < 6; ++k) {
            const uint32_t v = k < 3 ? spCount : base;
            laap::Write(sites[k], &v, 4);
        }
        const uint32_t fpSp = laap::SafeFingerprint(laap::kAlphaRanges);
        printf("  SystemPack emulation: pool 0x2600 slots at %p, alpha fingerprint 0x%08X (expected 0x%08X)\n", block, fpSp, laap::kExpectedAlpha);
        CHECK(fpSp == laap::kExpectedAlpha);
        CHECK(laap::Install(false) && laap::g_activeAlpha == 1);
        // engine GetFreeAlphaWorldPoly in this state: wraps at 0x2600 into the SystemPack block
        laap::Ref<uint8_t>(0x0099AA80) = 1;
        laap::Ref<int>(laap::kAlphaCurrent) = int(spCount);
        CHECK(E_GetFreeAlpha() == block);
        // ours: AlphaWorldPolys 4096 (< 0x2600) -> SystemPack's block with its 9728 slots, no wrap
        laap::g_alphaPool = nullptr;
        laap::g_alphaWanted = 4096;
        laap::Ref<int>(laap::kAlphaCurrent) = 0;
        CHECK(laap::AlphaGetFree() == block && laap::g_alphaCap == int(spCount));
        laap::Ref<int>(laap::kAlphaCurrent) = int(spCount);
        CHECK(laap::AlphaGetFree() == nullptr);
        // AlphaWorldPolys 16384 (> 0x2600) -> own pool (template check against SystemPack's block)
        laap::g_alphaPool = nullptr;
        laap::g_alphaWanted = 16384;
        laap::Ref<int>(laap::kAlphaCurrent) = 0;
        void* own = laap::AlphaGetFree();
        CHECK(own != nullptr && own != block && laap::g_alphaCap == 16384);
        printf("  with SystemPack: AlphaWorldPolys 4096 -> its %u slots, 16384 -> own pool %p\n", spCount, own);
        // inconsistent relocation (lea changed, ctor not) -> alpha group refused
        laap::Remove();
        const uint32_t bad = base + 0x14;
        laap::Write(laap::kAlphaBaseDisp, &bad, 4);
        laap::Install(false);
        CHECK(laap::g_activeAlpha == 0);
        laap::Remove();
        for (int k = 0; k < 6; ++k) laap::Write(sites[k], &saved[k], 4);
        laap::g_alphaPool = nullptr;
        laap::g_alphaWanted = 16384;
        laap::Ref<int>(laap::kAlphaCurrent) = 0;
    }
    printf("RESULT patchcheck: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (int code; laimg::RelaunchReserved("LAAP_CHILD", code)) return code;
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <Gothic2.exe> fingerprint|patchcheck|degen orig|patched|files <list> orig|patched|alpha\n", argv[0]);
        return 2;
    }
    if (!laimg::MapExe(argv[1])) return 2;
    laimg::InitEngine();
    printf("Gothic2.exe %u bytes\n", laimg::ExeFileSize());
    // zCMemPoolBase::s_disablePools 0x008A2558: in the game zDieter_StartUp 0x00630580 sets it from a command line
    // parameter (call DisablePools 0x006306A9 with zCOption::Parm(...)), i.e. 0 = pools ON in a normal start (the crash
    // stacks of the observed crashes are in the pool's free-list code). The harness has not run zDieter_StartUp -> set it like the game.
    printf("s_disablePools after static init: %d (parameter name \"%s\") -> 0 as in a normal game start\n",
           laap::Ref<int>(0x008A2558), reinterpret_cast<const char*>(0x008AA99C));
    laap::Ref<int>(0x008A2558) = 0;
    const std::string test = argv[2];
    if (test == "fingerprint") {
        printf("fingerprint alpha 0x%08X (expected 0x%08X), degenerate 0x%08X (expected 0x%08X)\n", laap::SafeFingerprint(laap::kAlphaRanges),
               laap::kExpectedAlpha, laap::SafeFingerprint(laap::kDegenRanges), laap::kExpectedDegen);
        const bool ok = laap::SafeFingerprint(laap::kAlphaRanges) == laap::kExpectedAlpha &&
                        laap::SafeFingerprint(laap::kDegenRanges) == laap::kExpectedDegen;
        printf("RESULT fingerprint: %s\n", ok ? "match" : "MISMATCH");
        return ok ? 0 : 1;
    }
    if (test == "patchcheck") return TestPatchcheck();
    if (test == "degen" && argc > 3) return TestDegen(std::string(argv[3]) == "patched");
    if (test == "files" && argc > 4) return TestFiles(argv[3], std::string(argv[4]) == "patched");
    if (test == "alpha") return TestAlpha();
    fprintf(stderr, "unknown test\n");
    return 2;
}
