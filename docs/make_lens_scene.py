# -*- coding: utf-8 -*-
# Generates assets/scenes/LensFog.glb: a biconvex glass lens under a small,
# snooted emitter. In a homogeneous scattering medium the lens focuses the
# light into a visible converging cone (volumetric caustic) with a bright
# spot where the cone crosses the floor. Materials use
# KHR_materials_transmission / KHR_materials_ior (mapped to the renderer's
# delta dielectric) and KHR_materials_emissive_strength.
#
# Geometry (world units, Y up):
#   lens    : biconvex, center (0, 1.5, 0), sphere radius R=0.7, aperture
#             a=0.5, half thickness h=0.24 -> f ~ 0.7 (n=1.5). The emitter
#             sits 2.0 above the lens center, so the focus lands ~1.08 below
#             it (y ~ 0.42), a visible beam waist in the fog above the floor.
#   emitter : 0.12 x 0.12 quad at y=3.5 facing down, inside a dark open box
#             (snoot) that limits spill so the refracted cone dominates.
#   floor   : 8 x 8 gray quad at y=0; back wall at z=-2.5, near black.
import json
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "scenes", "LensFog.glb")

LENS_C = (0.0, 1.5, 0.0)
LENS_R = 0.7
LENS_A = 0.5
LENS_H = 0.24
RINGS, SEGS = 24, 64

LIGHT_Y = 3.5
LIGHT_HALF = 0.06
SNOOT_HALF = 0.13
SNOOT_Y0, SNOOT_Y1 = 3.10, 3.54


class Prim:
    def __init__(self, material):
        self.material = material
        self.pos = []
        self.nrm = []
        self.idx = []

    def vertex(self, p, n):
        self.pos.append(p)
        self.nrm.append(n)
        return len(self.pos) - 1

    def tri(self, a, b, c):
        self.idx += [a, b, c]

    def quad(self, p0, p1, p2, p3, n):
        i0 = self.vertex(p0, n)
        i1 = self.vertex(p1, n)
        i2 = self.vertex(p2, n)
        i3 = self.vertex(p3, n)
        self.tri(i0, i1, i2)
        self.tri(i0, i2, i3)


def lens_cap(prim, top):
    cx, cy, cz = LENS_C
    sag = LENS_R - math.sqrt(LENS_R * LENS_R - LENS_A * LENS_A)
    sphere_cy = cy + (LENS_H - LENS_R) * (1 if top else -1)
    rings = []
    for i in range(RINGS + 1):
        r = LENS_A * i / RINGS
        dy = math.sqrt(LENS_R * LENS_R - r * r)
        y = sphere_cy + dy * (1 if top else -1)
        ring = []
        for j in range(SEGS):
            th = 2.0 * math.pi * j / SEGS
            p = (cx + r * math.cos(th), y, cz + r * math.sin(th))
            n = ((p[0] - cx) / LENS_R, (p[1] - sphere_cy) / LENS_R, (p[2] - cz) / LENS_R)
            ring.append(prim.vertex(p, n))
        rings.append(ring)
    for i in range(RINGS):
        for j in range(SEGS):
            j2 = (j + 1) % SEGS
            a, b = rings[i][j], rings[i][j2]
            c, d = rings[i + 1][j], rings[i + 1][j2]
            if i == 0:
                prim.tri(a, d, c) if top else prim.tri(a, c, d)
            else:
                if top:
                    prim.tri(a, b, d)
                    prim.tri(a, d, c)
                else:
                    prim.tri(a, d, b)
                    prim.tri(a, c, d)
    return cy + (LENS_H - sag) * (1 if top else -1)  # rim y


def build_prims():
    glass = Prim(0)
    y_rim_top = lens_cap(glass, True)
    y_rim_bot = lens_cap(glass, False)
    for j in range(SEGS):  # rim band, radial normals
        th0 = 2.0 * math.pi * j / SEGS
        th1 = 2.0 * math.pi * ((j + 1) % SEGS) / SEGS
        for th_a, th_b in [(th0, th1)]:
            ca, sa = math.cos(th_a), math.sin(th_a)
            cb, sb = math.cos(th_b), math.sin(th_b)
            pa_t = (LENS_A * ca, y_rim_top, LENS_A * sa)
            pb_t = (LENS_A * cb, y_rim_top, LENS_A * sb)
            pa_b = (LENS_A * ca, y_rim_bot, LENS_A * sa)
            pb_b = (LENS_A * cb, y_rim_bot, LENS_A * sb)
            ia = glass.vertex(pa_t, (ca, 0.0, sa))
            ib = glass.vertex(pb_t, (cb, 0.0, sb))
            ic = glass.vertex(pa_b, (ca, 0.0, sa))
            id_ = glass.vertex(pb_b, (cb, 0.0, sb))
            glass.tri(ia, ib, id_)
            glass.tri(ia, id_, ic)

    light = Prim(1)
    lh = LIGHT_HALF
    light.quad((-lh, LIGHT_Y, -lh), (lh, LIGHT_Y, -lh), (lh, LIGHT_Y, lh), (-lh, LIGHT_Y, lh),
               (0.0, -1.0, 0.0))

    snoot = Prim(2)
    s = SNOOT_HALF
    y0, y1 = SNOOT_Y0, SNOOT_Y1
    snoot.quad((-s, y0, -s), (-s, y1, -s), (-s, y1, s), (-s, y0, s), (1.0, 0.0, 0.0))
    snoot.quad((s, y0, s), (s, y1, s), (s, y1, -s), (s, y0, -s), (-1.0, 0.0, 0.0))
    snoot.quad((-s, y0, -s), (s, y0, -s), (s, y1, -s), (-s, y1, -s), (0.0, 0.0, 1.0))
    snoot.quad((s, y0, s), (-s, y0, s), (-s, y1, s), (s, y1, s), (0.0, 0.0, -1.0))
    snoot.quad((-s, y1, -s), (s, y1, -s), (s, y1, s), (-s, y1, s), (0.0, -1.0, 0.0))  # top cap

    floor = Prim(3)
    floor.quad((-4.0, 0.0, -4.0), (4.0, 0.0, -4.0), (4.0, 0.0, 4.0), (-4.0, 0.0, 4.0),
               (0.0, 1.0, 0.0))

    wall = Prim(4)
    wall.quad((-4.0, 0.0, -2.5), (4.0, 0.0, -2.5), (4.0, 4.5, -2.5), (-4.0, 4.5, -2.5),
              (0.0, 0.0, 1.0))
    return [glass, light, snoot, floor, wall]


MATERIALS = [
    {"name": "lens glass",
     "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                               "metallicFactor": 0.0, "roughnessFactor": 0.0},
     "extensions": {"KHR_materials_transmission": {"transmissionFactor": 1.0},
                     "KHR_materials_ior": {"ior": 1.5}}},
    {"name": "emitter",
     "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                               "metallicFactor": 0.0, "roughnessFactor": 1.0},
     "emissiveFactor": [1.0, 0.95, 0.85],
     "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2600.0}}},
    {"name": "snoot", "pbrMetallicRoughness": {"baseColorFactor": [0.04, 0.04, 0.04, 1.0],
                                                 "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "floor", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1.0],
                                                 "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "back wall", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1.0],
                                                    "metallicFactor": 0.0, "roughnessFactor": 1.0}},
]


def main():
    prims = build_prims()
    blob = b""
    buffer_views = []
    accessors = []
    gltf_prims = []
    for prim in prims:
        pos_bytes = b"".join(struct.pack("<3f", *p) for p in prim.pos)
        nrm_bytes = b"".join(struct.pack("<3f", *n) for n in prim.nrm)
        idx_bytes = b"".join(struct.pack("<I", i) for i in prim.idx)
        entries = []
        for data, target in [(pos_bytes, 34962), (nrm_bytes, 34962), (idx_bytes, 34963)]:
            buffer_views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data),
                                  "target": target})
            blob += data + b"\0" * (-len(data) % 4)
            entries.append(len(buffer_views) - 1)
        mins = [min(p[i] for p in prim.pos) for i in range(3)]
        maxs = [max(p[i] for p in prim.pos) for i in range(3)]
        accessors.append({"bufferView": entries[0], "componentType": 5126, "count": len(prim.pos),
                           "type": "VEC3", "min": mins, "max": maxs})
        accessors.append({"bufferView": entries[1], "componentType": 5126, "count": len(prim.nrm),
                           "type": "VEC3"})
        accessors.append({"bufferView": entries[2], "componentType": 5125, "count": len(prim.idx),
                           "type": "SCALAR"})
        gltf_prims.append({"attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
                            "indices": len(accessors) - 1, "material": prim.material})

    gltf = {
        "asset": {"version": "2.0", "generator": "make_lens_scene.py"},
        "extensionsUsed": ["KHR_materials_transmission", "KHR_materials_ior",
                            "KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "LensFog"}],
        "meshes": [{"primitives": gltf_prims}],
        "materials": MATERIALS,
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(OUT, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<I4s", len(js), b"JSON"))
        f.write(js)
        f.write(struct.pack("<I4s", len(blob), b"BIN\0"))
        f.write(blob)
    tris = sum(len(p.idx) // 3 for p in prims)
    print("wrote", OUT, "-", tris, "triangles")


if __name__ == "__main__":
    main()
