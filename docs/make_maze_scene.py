# -*- coding: utf-8 -*-
# Generates assets/scenes/LightMaze.glb: the SPARSE-CARRIER stress scene
# for spatial-reuse candidate selection. A diffuse main room receives light
# ONLY through a two-bend chicane from a hidden emitter alcove:
#
#   - the emitter has no direct line to the door plane (the alcove baffle
#     blocks every straight emitter->door segment: crossing z at the baffle
#     plane is <= 0.4*z_e + 0.6*z_door < 0.35 for all emitter/door points),
#   - camera-side NEE from any main-room point to the emitter is occluded
#     for the same reason, and
#   - every camera-visible surface is lit at >= 2 bounces (far corner 3+).
#
# Per-pixel initial candidates therefore almost never carry contribution;
# what signal exists arrives via light subpaths that escaped the chicane
# (t=1 landings + BDPT connections) - a few CARRIER pixels per region,
# frame-varying. This is the regime where candidate SELECTION, the cell
# search, and contribution-aware pool choice can actually matter, unlike
# Cornell-class scenes whose 256-entry block runs are contribution-dense.
#
# Layout (world units, Y up; wall slabs are closed boxes, thickness 0.08,
# so there are no thin-wall light/shadow leaks):
#   main room : interior x [-2.0, 2.0], y [0, 2.2], z [-2.0, 2.0]
#   alcove    : interior x [-3.4, -2.08], y [0, 2.2], z [-1.0, 1.0]
#   door      : shared wall x [-2.08, -2.0], opening z [-0.95, -0.15],
#               y [0, 1.8] (lintel above). No-direct-line check: a straight
#               emitter->door segment crosses the baffle plane at
#               z = 0.399 z_e + 0.601 z_d <= 0.399*0.85 - 0.601*0.15 = 0.249
#               < 0.35 (baffle top) for every emitter/door point pair.
#   baffle    : x [-2.62, -2.54], z [-1.0, 0.35], full height -> gap at
#               z (0.35, 1.0) on the OPPOSITE z side of the door
#   emitter   : quad on the alcove far wall, x = -3.395, z [0.25, 0.85],
#               y [0.7, 1.5], facing +x, warm white, strength tuned so the
#               main-room mean sits ~1e-2 (see header of the test log)
#   clutter   : axis-aligned box near the dark corner + a 30-degree-rotated
#               box mid-room (exercises the normal-octant cluster term)
#
# Suggested camera (dark-end framing, door out of frame):
#   ROYALGL_CAM="1.45,1.25,1.5,-1.2,0.7,-1.0"
import json
import math
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "scenes", "LightMaze.glb")

T = 0.08          # wall slab thickness
H = 2.2           # interior height
EMIT_STRENGTH = 4000.0


class Prim:
    def __init__(self, material):
        self.material = material
        self.pos = []
        self.nrm = []
        self.idx = []

    def vertex(self, p, n):
        self.pos.append(tuple(p))
        self.nrm.append(tuple(n))
        return len(self.pos) - 1

    def quad(self, p0, p1, p2, p3, n):
        i0 = self.vertex(p0, n)
        i1 = self.vertex(p1, n)
        i2 = self.vertex(p2, n)
        i3 = self.vertex(p3, n)
        self.idx += [i0, i1, i2, i0, i2, i3]

    def box(self, lo, hi):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1))
        self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (0, 0, -1))
        self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (1, 0, 0))
        self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (-1, 0, 0))
        self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), (0, 1, 0))
        self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0))

    # Y-rotated box: 8 corners, 6 quads, side normals rotated with it.
    def rot_box2(self, center, half, angle_deg):
        cx, cy, cz = center
        hx, hy, hz = half
        c = math.cos(math.radians(angle_deg))
        s = math.sin(math.radians(angle_deg))

        def pt(dx, dy, dz):
            rx = dx * hx * c - dz * hz * s
            rz = dx * hx * s + dz * hz * c
            return (cx + rx, cy + dy * hy, cz + rz)

        def rn(dx, dz):
            return (dx * c - dz * s, 0.0, dx * s + dz * c)

        # +x/-x/+z/-z side faces
        self.quad(pt(1, -1, -1), pt(1, -1, 1), pt(1, 1, 1), pt(1, 1, -1), rn(1, 0))
        self.quad(pt(-1, -1, 1), pt(-1, -1, -1), pt(-1, 1, -1), pt(-1, 1, 1), rn(-1, 0))
        self.quad(pt(1, -1, 1), pt(-1, -1, 1), pt(-1, 1, 1), pt(1, 1, 1), rn(0, 1))
        self.quad(pt(-1, -1, -1), pt(1, -1, -1), pt(1, 1, -1), pt(-1, 1, -1), rn(0, -1))
        self.quad(pt(-1, 1, 1), pt(-1, 1, -1), pt(1, 1, -1), pt(1, 1, 1), (0, 1, 0))
        self.quad(pt(-1, -1, -1), pt(-1, -1, 1), pt(1, -1, 1), pt(1, -1, -1), (0, -1, 0))


def build_prims():
    walls = Prim(0)      # main-room shell
    corridor = Prim(1)   # alcove + baffle + shared wall (different tint/run)
    floors = Prim(2)     # floor slab
    ceil_ = Prim(3)      # ceiling slab
    box_a = Prim(4)      # dark-corner clutter box
    box_b = Prim(5)      # rotated mid-room box
    emit = Prim(6)       # emitter quad

    # one floor + one ceiling slab under/over BOTH rooms
    floors.box((-3.48, -T, -2.08), (2.08, 0.0, 2.08))
    ceil_.box((-3.48, H, -2.08), (2.08, H + T, 2.08))

    # main room shell: +x (behind camera), +z, -z
    walls.box((2.0, 0.0, -2.08), (2.08, H, 2.08))
    walls.box((-2.08, 0.0, 2.0), (2.08, H, 2.08))
    walls.box((-2.08, 0.0, -2.08), (2.08, H, -2.0))

    # shared wall x [-2.08, -2.0] with door opening z [-0.95, -0.15], y [0, 1.8]
    corridor.box((-2.08, 0.0, -2.08), (-2.0, H, -0.95))  # left of door
    corridor.box((-2.08, 0.0, -0.15), (-2.0, H, 2.08))   # right of door
    corridor.box((-2.08, 1.8, -0.95), (-2.0, H, -0.15))  # lintel

    # alcove shell: far wall, two z side walls
    corridor.box((-3.48, 0.0, -1.08), (-3.4, H, 1.08))
    corridor.box((-3.48, 0.0, -1.08), (-2.08, H, -1.0))
    corridor.box((-3.48, 0.0, 1.0), (-2.08, H, 1.08))

    # baffle: full height, gap at z (0.35, 1.0)
    corridor.box((-2.62, 0.0, -1.0), (-2.54, H, 0.35))

    # clutter
    box_a.box((-1.2, 0.0, -1.3), (-0.4, 0.7, -0.6))
    box_b.rot_box2((0.9, 0.5, -0.9), (0.3, 0.5, 0.3), 30.0)

    # emitter quad on the alcove far wall, facing +x
    emit.quad((-3.395, 0.7, 0.25), (-3.395, 0.7, 0.85),
              (-3.395, 1.5, 0.85), (-3.395, 1.5, 0.25), (1.0, 0.0, 0.0))

    return [walls, corridor, floors, ceil_, box_a, box_b, emit]


MATERIALS = [
    {"name": "room walls", "pbrMetallicRoughness": {
        "baseColorFactor": [0.62, 0.62, 0.62, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "corridor", "pbrMetallicRoughness": {
        "baseColorFactor": [0.72, 0.74, 0.70, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "floor", "pbrMetallicRoughness": {
        "baseColorFactor": [0.45, 0.45, 0.45, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "ceiling", "pbrMetallicRoughness": {
        "baseColorFactor": [0.70, 0.70, 0.70, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "box A", "pbrMetallicRoughness": {
        "baseColorFactor": [0.35, 0.42, 0.60, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "box B", "pbrMetallicRoughness": {
        "baseColorFactor": [0.60, 0.36, 0.30, 1.0], "metallicFactor": 0.0, "roughnessFactor": 1.0}},
    {"name": "emitter",
     "pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                               "metallicFactor": 0.0, "roughnessFactor": 1.0},
     "emissiveFactor": [1.0, 0.95, 0.85],
     "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": EMIT_STRENGTH}}},
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
        "asset": {"version": "2.0", "generator": "make_maze_scene.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "LightMaze"}],
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
