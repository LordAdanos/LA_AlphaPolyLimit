#!/usr/bin/env python3
"""ASC check for LA_AlphaPolyLimit: finds faces that make the engine double-free a zCVertex.

zCModelPrototype::ReadVertexList (0x0058BEA0) adds every *MESH_VERTEX with zCMesh::AddVertex(zVEC3&) 0x005688F0 (no
position merge), faces reference them by index. A face with a repeated index (A==B, B==C or A==C) is a polygon that holds
one zCVertex twice. zCMesh::RemoveDegeneratePolys (called by zCProgMeshBuilder::BuildProgMeshProto, also from
ReadSoftSkinVertList 0x0058D417) deletes it with DeletePolyAndVerts(ulong) 0x005736E0, which frees a vertex that no other
polygon uses once per OCCURRENCE -> double free (see plugins\\LA_AlphaPolyLimit\\doc\\ANALYSE.md).

The emulation follows the engine order: polygons in file order, degenerate = repeated index or zero area (exact cross
product 0; the engine's CalcNormal also rejects near-collinear faces, so this is a lower bound), sticky 'shared' flag.
Read-only; prints one line per file with candidates and a total.  Usage: python asc_degen_scan.py <dir with *.ASC> ...
"""
import os
import re
import sys

VERT = re.compile(r"\*MESH_VERTEX\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)")
FACE = re.compile(r"\*MESH_FACE\s+(\d+):\s+A:\s+(\d+)\s+B:\s+(\d+)\s+C:\s+(\d+)")


def meshes(path):
    """yields (name, verts, faces) per *GEOMOBJECT"""
    name, verts, faces = None, {}, []
    with open(path, encoding="latin-1") as f:
        for line in f:
            s = line.strip()
            if s.startswith("*GEOMOBJECT"):
                if name is not None:
                    yield name, verts, faces
                name, verts, faces = "?", {}, []
            elif s.startswith("*NODE_NAME") and name == "?":
                name = s.split(None, 1)[1].strip('"') if " " in s else "?"
            elif s.startswith("*MESH_VERTEX "):
                m = VERT.match(s)
                if m:
                    verts[int(m.group(1))] = (float(m.group(2)), float(m.group(3)), float(m.group(4)))
            elif s.startswith("*MESH_FACE "):
                m = FACE.match(s)
                if m:
                    faces.append((int(m.group(2)), int(m.group(3)), int(m.group(4))))
    if name is not None:
        yield name, verts, faces


def zero_area(verts, f):
    try:
        a, b, c = (verts[i] for i in f)
    except KeyError:
        return True
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return n == (0.0, 0.0, 0.0)


def emulate(verts, faces):
    """returns (degenerate, repeated-index faces, double frees) like RemoveDegeneratePolys/DeletePolyAndVerts"""
    polys = [list(f) for f in faces]
    degen = [len(set(f)) < 3 or zero_area(verts, f) for f in faces]
    use = {}
    for p in polys:
        for v in set(p):
            use[v] = use.get(v, 0) + 1
    freed, harmful, ndeg, nrep = set(), 0, 0, 0
    i = 0
    while i < len(polys):
        if not degen[i]:
            i += 1
            continue
        ndeg += 1
        p = polys[i]
        if len(set(p)) < 3:
            nrep += 1
        shared = False
        for v in p:
            if shared:
                continue
            if use.get(v, 0) > 1:  # another polygon holds it
                shared = True
                continue
            if v in freed:
                harmful += 1
            else:
                freed.add(v)
        for v in set(p):
            use[v] -= 1
        del polys[i], degen[i]
    return ndeg, nrep, harmful


def main(dirs):
    total_files = total_harm = files_harm = 0
    for d in dirs:
        for dirpath, _, names in os.walk(d):
            for n in sorted(names):
                if not n.upper().endswith(".ASC"):
                    continue
                total_files += 1
                path = os.path.join(dirpath, n)
                rep = []
                harm_file = 0
                for name, verts, faces in meshes(path):
                    ndeg, nrep, harm = emulate(verts, faces)
                    if nrep or harm:
                        rep.append(f"{name}: {len(faces)} faces, {ndeg} degenerate, {nrep} with a repeated index, {harm} double free(s)")
                    harm_file += harm
                if harm_file:
                    files_harm += 1
                    total_harm += harm_file
                if rep:
                    print(f"{n}: " + "; ".join(rep[:6]) + (f" (+{len(rep) - 6} meshes)" if len(rep) > 6 else ""))
    print(f"RESULT asc: {total_files} ASC files, {files_harm} with engine double frees ({total_harm} in total)")


if __name__ == "__main__":
    main(sys.argv[1:] or ["."])
