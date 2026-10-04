# SPDX-License-Identifier: GPL-3.0-or-later
# (runs inside Blender and uses its Python API, see docs/LICENSING.md)
"""Neon Requiem — procedural city kit, modeled in Blender (run through `dcc_run_script`).

sky.ARGS[0] is a JSON job: {"textures": true, "buildings": [spec...], "signs": [spec...],
"props": [name...], "skyline": [spec...]}. Every model is written to SKY_OUT as its own .glb
(Y-up, meters); textures as PNGs; `anchors.json` (and sky.result) lists, per building, where
the engine should hang signs, lamps and beacons.

Building local frame (Blender): x along the street facade (centered), the street facade at
y = 0 facing -y, the block extending to +y, z up. Exported glTF: the facade faces +Z.
"""
import json
import math
import os
import random

import bmesh
import bpy
import numpy as np
from mathutils import Vector

from skywalker_dcc import blender as B

JOB = json.loads(sky.ARGS[0]) if sky.ARGS else {}  # noqa: F821  (sky is injected by the bootstrap)
OUT = sky.OUT  # noqa: F821
ATLAS_N = 8          # window atlas: 8 x 8 cells
ATLAS_PX = 2048
DETAIL = 1.0         # per building: < 1 drops small clutter on faraway blocks


# =============================================================================================
# Geometry builder
# =============================================================================================
class Geo:
    def __init__(self):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.col = self.bm.loops.layers.color.new("Col")
        self.mats = []
        self.tint = (1.0, 1.0, 1.0)
        self.grime = False

    def mi(self, name):
        if name not in self.mats:
            self.mats.append(name)
        return self.mats.index(name)

    def poly(self, pts, normal, mat, uvs=None, col=(1, 1, 1)):
        pts = [Vector(p) for p in pts]
        n = (pts[1] - pts[0]).cross(pts[2] - pts[0])
        if len(pts) == 4 and n.length < 1e-9:
            n = (pts[2] - pts[0]).cross(pts[3] - pts[0])
        order = list(range(len(pts)))
        if n.dot(Vector(normal)) < 0:
            order.reverse()
        verts = [self.bm.verts.new(pts[i]) for i in order]
        try:
            f = self.bm.faces.new(verts)
        except ValueError:
            return None
        f.material_index = self.mi(mat)
        if uvs is None:
            # planar projection (meters / 2), adequate for triplanar materials anyway
            nv = Vector(normal)
            ax = max(range(3), key=lambda k: abs(nv[k]))
            a, b = [(1, 2), (0, 2), (0, 1)][ax]
            uvs_o = [(pts[i][a] * 0.5, pts[i][b] * 0.5) for i in order]
        else:
            uvs_o = [uvs[i] for i in order]
        tint = self.tint if mat.startswith("wall_") or mat == "trim" else (1.0, 1.0, 1.0)
        for loop, uv in zip(f.loops, uvs_o):
            loop[self.uv].uv = uv
            g = 1.0
            if self.grime:
                z = loop.vert.co.z
                g = 0.62 + 0.38 * min(1.0, max(0.0, (z - 0.3) / 7.0))          # street grime
                g *= 1.0 - 0.12 * max(0.0, math.sin(loop.vert.co.x * 1.7 + loop.vert.co.y * 1.3) * math.sin(z * 0.21))
            loop[self.col] = (col[0] * tint[0] * g, col[1] * tint[1] * g, col[2] * tint[2] * g, 1.0)
        return f

    def box(self, x0, y0, z0, x1, y1, z1, mat, col=(1, 1, 1), skip=()):
        x0, x1 = min(x0, x1), max(x0, x1)
        y0, y1 = min(y0, y1), max(y0, y1)
        z0, z1 = min(z0, z1), max(z0, z1)
        F = {
            "-x": ([(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)], (-1, 0, 0)),
            "+x": ([(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)], (1, 0, 0)),
            "-y": ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)], (0, -1, 0)),
            "+y": ([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)], (0, 1, 0)),
            "-z": ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)], (0, 0, -1)),
            "+z": ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], (0, 0, 1)),
        }
        for k, (pts, n) in F.items():
            if k not in skip:
                self.poly(pts, n, mat, col=col)

    def cyl(self, cx, cy, z0, z1, r, mat, segs=12, col=(1, 1, 1), caps=True, r1=None):
        r1 = r if r1 is None else r1
        ring0 = [(cx + math.cos(2 * math.pi * i / segs) * r, cy + math.sin(2 * math.pi * i / segs) * r, z0) for i in range(segs)]
        ring1 = [(cx + math.cos(2 * math.pi * i / segs) * r1, cy + math.sin(2 * math.pi * i / segs) * r1, z1) for i in range(segs)]
        for i in range(segs):
            j = (i + 1) % segs
            a = 2 * math.pi * (i + 0.5) / segs
            self.poly([ring0[i], ring0[j], ring1[j], ring1[i]], (math.cos(a), math.sin(a), 0), mat, col=col)
        if caps:
            self.poly(ring1, (0, 0, 1), mat, col=col)
            self.poly(ring0, (0, 0, -1), mat, col=col)

    def tube(self, p0, p1, r, mat, segs=8, col=(1, 1, 1)):
        """A capped cylinder between two points (pipes, rails, cables)."""
        p0, p1 = Vector(p0), Vector(p1)
        d = p1 - p0
        if d.length < 1e-6:
            return
        d.normalize()
        a = Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((1, 0, 0))
        u = d.cross(a).normalized()
        v = d.cross(u).normalized()
        r0 = [p0 + (u * math.cos(2 * math.pi * i / segs) + v * math.sin(2 * math.pi * i / segs)) * r for i in range(segs)]
        r1 = [p1 + (u * math.cos(2 * math.pi * i / segs) + v * math.sin(2 * math.pi * i / segs)) * r for i in range(segs)]
        for i in range(segs):
            j = (i + 1) % segs
            a2 = 2 * math.pi * (i + 0.5) / segs
            n = u * math.cos(a2) + v * math.sin(a2)
            self.poly([r0[i], r0[j], r1[j], r1[i]], n, mat, col=col)
        self.poly(r1, d, mat, col=col)
        self.poly(r0, -d, mat, col=col)

    def to_object(self, name, smooth=False):
        me = bpy.data.meshes.new(name)
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=0.0005)
        self.bm.to_mesh(me)
        self.bm.free()
        ob = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(ob)
        for m in self.mats:
            me.materials.append(material(m))
        for p in me.polygons:
            p.use_smooth = smooth
        return ob


class Frame:
    """A facade plane: P(u, v, w) = origin + U*u + Z*v + N*w (N points out of the building)."""

    def __init__(self, origin, U, N):
        self.o, self.U, self.N, self.V = Vector(origin), Vector(U), Vector(N), Vector((0, 0, 1))

    def p(self, u, v, w=0.0):
        return tuple(self.o + self.U * u + self.V * v + self.N * w)

    def quad(self, g, u0, v0, u1, v1, w, mat, normal=None, uvs=None, col=(1, 1, 1)):
        n = self.N if normal is None else normal
        g.poly([self.p(u0, v0, w), self.p(u1, v0, w), self.p(u1, v1, w), self.p(u0, v1, w)], tuple(n), mat, uvs=uvs, col=col)

    def box(self, g, u0, v0, w0, u1, v1, w1, mat, col=(1, 1, 1), skip_back=True):
        """Box in frame space; faces get outward normals. skip_back drops the face against the wall."""
        u0, u1 = min(u0, u1), max(u0, u1)
        v0, v1 = min(v0, v1), max(v0, v1)
        w0, w1 = min(w0, w1), max(w0, w1)
        P = self.p
        U, V, N = self.U, self.V, self.N
        g.poly([P(u0, v0, w1), P(u1, v0, w1), P(u1, v1, w1), P(u0, v1, w1)], tuple(N), mat, col=col)
        if not skip_back:
            g.poly([P(u0, v0, w0), P(u1, v0, w0), P(u1, v1, w0), P(u0, v1, w0)], tuple(-N), mat, col=col)
        g.poly([P(u0, v1, w0), P(u1, v1, w0), P(u1, v1, w1), P(u0, v1, w1)], tuple(V), mat, col=col)
        g.poly([P(u0, v0, w0), P(u1, v0, w0), P(u1, v0, w1), P(u0, v0, w1)], tuple(-V), mat, col=col)
        g.poly([P(u0, v0, w0), P(u0, v1, w0), P(u0, v1, w1), P(u0, v0, w1)], tuple(-U), mat, col=col)
        g.poly([P(u1, v0, w0), P(u1, v1, w0), P(u1, v1, w1), P(u1, v0, w1)], tuple(U), mat, col=col)

    def bar(self, g, u0, v0, w0, u1, v1, w1, mat):
        """A vertical bar: four side faces only (balusters, posts)."""
        P = self.p
        U, N = self.U, self.N
        g.poly([P(u0, v0, w1), P(u1, v0, w1), P(u1, v1, w1), P(u0, v1, w1)], tuple(N), mat)
        g.poly([P(u0, v0, w0), P(u1, v0, w0), P(u1, v1, w0), P(u0, v1, w0)], tuple(-N), mat)
        g.poly([P(u0, v0, w0), P(u0, v1, w0), P(u0, v1, w1), P(u0, v0, w1)], tuple(-U), mat)
        g.poly([P(u1, v0, w0), P(u1, v1, w0), P(u1, v1, w1), P(u1, v0, w1)], tuple(U), mat)

    def hole(self, g, u0, v0, u1, v1, depth, mat_reveal, col=(1, 1, 1)):
        """The four reveal faces of a rectangular opening `depth` deep (normals point into the hole)."""
        P = self.p
        U, V = self.U, self.V
        g.poly([P(u0, v0, 0), P(u0, v1, 0), P(u0, v1, -depth), P(u0, v0, -depth)], tuple(U), mat_reveal, col=col)
        g.poly([P(u1, v0, 0), P(u1, v1, 0), P(u1, v1, -depth), P(u1, v0, -depth)], tuple(-U), mat_reveal, col=col)
        g.poly([P(u0, v0, 0), P(u1, v0, 0), P(u1, v0, -depth), P(u0, v0, -depth)], tuple(V), mat_reveal, col=col)
        g.poly([P(u0, v1, 0), P(u1, v1, 0), P(u1, v1, -depth), P(u0, v1, -depth)], tuple(-V), mat_reveal, col=col)


# =============================================================================================
# Materials (glTF PBR; the engine later swaps walls to photoscanned triplanar materials)
# =============================================================================================
MAT_DEFS = {
    # walls (overridden in the engine with Poly Haven scans; colors are fallbacks)
    "wall_concrete": dict(color=(0.42, 0.42, 0.43), roughness=0.85),
    "wall_plaster": dict(color=(0.55, 0.52, 0.48), roughness=0.9),
    "wall_tile": dict(color=(0.6, 0.62, 0.64), roughness=0.5),
    "wall_brick": dict(color=(0.42, 0.22, 0.16), roughness=0.85),
    "wall_panel": dict(color=(0.25, 0.27, 0.3), roughness=0.45, metallic=0.6),
    "trim": dict(color=(0.3, 0.3, 0.31), roughness=0.75),
    "metal_dark": dict(color=(0.06, 0.065, 0.07), roughness=0.45, metallic=0.9),
    "metal_rust": dict(color=(0.25, 0.14, 0.08), roughness=0.75, metallic=0.6),
    "metal_galv": dict(color=(0.5, 0.52, 0.54), roughness=0.4, metallic=0.9),
    "ac_body": dict(color=(0.62, 0.62, 0.6), roughness=0.55, metallic=0.2),
    "ac_grille": dict(color=(0.03, 0.03, 0.035), roughness=0.6, metallic=0.5),
    "shutter": dict(color=(0.4, 0.42, 0.44), roughness=0.5, metallic=0.8),
    "fascia": dict(color=(0.04, 0.04, 0.045), roughness=0.35, metallic=0.3),
    "awning_red": dict(color=(0.45, 0.04, 0.05), roughness=0.7),
    "awning_teal": dict(color=(0.02, 0.25, 0.28), roughness=0.7),
    "awning_black": dict(color=(0.03, 0.03, 0.035), roughness=0.6),
    "glass_dark": dict(color=(0.02, 0.025, 0.03), roughness=0.05, metallic=0.0),
    "rubber": dict(color=(0.02, 0.02, 0.02), roughness=0.8),
    "concrete_raw": dict(color=(0.4, 0.4, 0.4), roughness=0.9),
    "tank_wood": dict(color=(0.3, 0.2, 0.12), roughness=0.85),
    "windows": dict(color=(0.5, 0.5, 0.5), roughness=0.08),           # the engine binds the shared atlas
    "skyline_windows": dict(color=(0.5, 0.5, 0.5), roughness=0.3),
    "led_white": dict(color=(0, 0, 0), emission=(1.0, 0.95, 0.9), strength=6.0),
    "led_cyan": dict(color=(0, 0, 0), emission=(0.1, 0.85, 1.0), strength=6.0),
    "led_magenta": dict(color=(0, 0, 0), emission=(1.0, 0.1, 0.6), strength=6.0),
    "led_amber": dict(color=(0, 0, 0), emission=(1.0, 0.55, 0.12), strength=6.0),
    "led_red": dict(color=(0, 0, 0), emission=(1.0, 0.05, 0.04), strength=8.0),
    "train_body": dict(color=(0.55, 0.57, 0.6), roughness=0.3, metallic=0.85),
    "train_stripe": dict(color=(0.6, 0.05, 0.1), roughness=0.35, metallic=0.3),
    "skin_hull": dict(color=(0.08, 0.085, 0.095), roughness=0.3, metallic=0.8),
    "canopy_clear": dict(color=(0.75, 0.85, 0.95), roughness=0.08, alpha=0.35),
}


def material(name):
    m = bpy.data.materials.get(name)
    if m:
        return m
    d = MAT_DEFS.get(name)
    if d is None:
        # neon / sign colors are encoded in the name: neon_<hex>_<strength>
        if name.startswith("neon_") or name.startswith("glow_"):
            parts = name.split("_")
            hx = parts[1]
            rgb = tuple(int(hx[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
            strength = float(parts[2]) if len(parts) > 2 else 6.0
            d = dict(color=(0, 0, 0), emission=rgb, strength=strength)
        else:
            d = dict(color=(0.5, 0.5, 0.5), roughness=0.7)
    m = B.make_material(name, color=d.get("color", (0.5, 0.5, 0.5)), roughness=d.get("roughness", 0.5),
                        metallic=d.get("metallic", 0.0), alpha=d.get("alpha", 1.0))
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    if "emission" in d:
        bsdf.inputs["Emission"].default_value = tuple(d["emission"]) + (1.0,)
        bsdf.inputs["Emission Strength"].default_value = d.get("strength", 5.0)
    if "image" in d:
        img = bpy.data.images.load(os.path.join(OUT, d["image"]), check_existing=True)
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = img
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission"])
        bsdf.inputs["Emission Strength"].default_value = d.get("emissive", 1.0)
    return m


# =============================================================================================
# Textures (numpy -> PNG)
# =============================================================================================
def save_png(name, arr):
    h, w = arr.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=True)
    rgba = np.ones((h, w, 4), np.float32)
    rgba[..., :arr.shape[2]] = arr
    img.pixels.foreach_set(rgba.ravel())
    img.filepath_raw = os.path.join(OUT, name)
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


CELL_KINDS = []  # filled by window_atlas(): kind per cell index


def window_atlas():
    """8x8 cells of window interiors: dark glass, warm homes, cool offices, TV glow, neon rooms, shops."""
    rng = np.random.default_rng(7)
    S = ATLAS_PX // ATLAS_N
    img = np.zeros((ATLAS_PX, ATLAS_PX, 3), np.float32)  # linear; row 0 = bottom (Blender convention)
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) / S  # yy: 0 bottom .. 1 top
    kinds = (["dark"] * 26 + ["warm"] * 14 + ["cool"] * 8 + ["tv"] * 4 + ["neon"] * 4 + ["shop"] * 8)
    CELL_KINDS.clear()
    CELL_KINDS.extend(kinds)
    for k, kind in enumerate(kinds):
        cx, cy = k % ATLAS_N, k // ATLAS_N
        c = np.zeros((S, S, 3), np.float32)
        if kind == "dark":
            base = np.array([0.006, 0.007, 0.01]) * rng.uniform(0.6, 1.5)
            c[:] = base
            c *= (0.7 + 0.6 * yy)[..., None]
            if rng.random() < 0.4:  # faint blinds catching street light
                c *= (1.0 + 0.35 * (np.sin(yy * 70) > 0.6))[..., None]
            if rng.random() < 0.25:  # a faraway glow deep in the room
                gx, gy = rng.uniform(0.2, 0.8), rng.uniform(0.3, 0.7)
                d = np.exp(-(((xx - gx) ** 2 + (yy - gy) ** 2) / 0.02))
                col = np.array(rng.choice([[0.6, 0.35, 0.15], [0.15, 0.3, 0.6], [0.5, 0.1, 0.35]]))
                c += d[..., None] * col * 0.06
        else:
            if kind == "warm":
                col = np.array(rng.choice([[1.0, 0.62, 0.3], [1.0, 0.72, 0.42], [0.95, 0.55, 0.25], [1.0, 0.8, 0.55]]))
                bright = rng.uniform(0.35, 0.8)
            elif kind == "cool":
                col = np.array(rng.choice([[0.7, 0.88, 1.0], [0.8, 0.95, 0.92], [0.6, 0.8, 1.0]]))
                bright = rng.uniform(0.35, 0.75)
            elif kind == "tv":
                col = np.array([0.25, 0.4, 1.0])
                bright = 0.05
            elif kind == "neon":
                col = np.array(rng.choice([[1.0, 0.2, 0.55], [0.75, 0.25, 1.0], [0.2, 0.9, 0.85], [1.0, 0.3, 0.2]]))
                bright = 0.45
            else:  # shop
                col = np.array(rng.choice([[1.0, 0.95, 0.88], [0.92, 0.97, 1.0], [1.0, 0.85, 0.7]]))
                bright = rng.uniform(0.4, 0.55)
            # room: ceiling brighter, floor darker, a lamp hotspot
            room = 0.35 + 0.65 * yy ** 1.3
            lx, ly = rng.uniform(0.25, 0.75), rng.uniform(0.6, 0.95)
            room = room + 0.8 * np.exp(-(((xx - lx) ** 2) / 0.05 + ((yy - ly) ** 2) / 0.03))
            c[:] = col * bright
            c *= room[..., None]
            if kind == "tv":
                tx = rng.uniform(0.2, 0.8)
                flick = np.exp(-(((xx - tx) ** 2) / 0.04 + ((yy - 0.3) ** 2) / 0.05))
                c += flick[..., None] * np.array([0.3, 0.5, 1.0]) * 0.8
            if kind == "neon":
                tx = rng.uniform(0.2, 0.8)
                c += (np.exp(-((xx - tx) ** 2) / 0.0006) * (yy > 0.25) * (yy < 0.85))[..., None] * col * 3.0
            if kind in ("warm", "cool") and rng.random() < 0.55:  # blinds
                slats = (np.sin(yy * rng.uniform(55, 80)) > rng.uniform(-0.2, 0.5)).astype(np.float32)
                drop = rng.uniform(0.3, 1.0)  # how far the blinds come down
                mask = yy > (1 - drop)
                c *= np.where(mask, 0.35 + 0.65 * slats, 1.0)[..., None]
            if kind in ("warm", "neon") and rng.random() < 0.5:  # curtains at the sides
                cw = rng.uniform(0.12, 0.3)
                curtain = (xx < cw) | (xx > 1 - cw)
                ccol = np.array(rng.choice([[0.5, 0.15, 0.1], [0.25, 0.2, 0.4], [0.6, 0.45, 0.25]]))
                folds = 0.75 + 0.25 * np.sin(xx * 90)
                c = np.where(curtain[..., None], (ccol * bright * 0.9 * folds[..., None]) * room[..., None], c)
            if kind == "shop":  # shelves with colorful goods
                for row in range(4):
                    y0 = 0.12 + row * 0.2
                    shelf = (yy > y0) & (yy < y0 + 0.015)
                    c[shelf] = 0.08
                    goods = (yy > y0 + 0.015) & (yy < y0 + 0.11)
                    hues = rng.random((24, 3)) * 0.5 + 0.25
                    idx = np.clip((xx * 24).astype(int), 0, 23)
                    gcol = hues[idx] * bright * 0.8
                    gap = (np.sin(xx * 24 * math.pi * 2) > -0.6)
                    c = np.where((goods & gap)[..., None], gcol, c)
            # silhouettes: furniture / a plant / a person
            if kind in ("warm", "cool", "neon", "tv"):
                for _ in range(rng.integers(0, 3)):
                    fx0 = rng.uniform(0.0, 0.7)
                    fw = rng.uniform(0.15, 0.4)
                    fh = rng.uniform(0.15, 0.35)
                    m = (xx > fx0) & (xx < fx0 + fw) & (yy < fh)
                    c[m] *= 0.15
                if rng.random() < 0.3:
                    px = rng.uniform(0.2, 0.8)
                    body = ((xx - px) ** 2 / 0.006 + (yy - 0.35) ** 2 / 0.06) < 1
                    head = ((xx - px) ** 2 + (yy - 0.66) ** 2) < 0.0035
                    c[body | head] *= 0.08
                if rng.random() < 0.3:
                    px = rng.uniform(0.1, 0.9)
                    leaves = np.zeros_like(xx, bool)
                    for _ in range(9):
                        bx, by = px + rng.uniform(-0.08, 0.08), rng.uniform(0.18, 0.42)
                        leaves |= ((xx - bx) ** 2 + (yy - by) ** 2) < rng.uniform(0.001, 0.003)
                    c[leaves] *= 0.1
        # frame + mullion (dark)
        frame = (xx < 0.035) | (xx > 0.965) | (yy < 0.035) | (yy > 0.965)
        if rng.random() < 0.6:
            frame |= np.abs(xx - 0.5) < 0.014
        if rng.random() < 0.4:
            frame |= np.abs(yy - 0.62) < 0.012
        c[frame] = 0.012
        # grime / rain streaks on the glass
        streak = 1 - 0.18 * (np.sin(xx * rng.uniform(150, 260) + rng.uniform(0, 6)) > 0.93) * yy
        c *= streak[..., None]
        img[cy * S:(cy + 1) * S, cx * S:(cx + 1) * S] = c
    save_png("window_atlas.png", srgb(img))
    return kinds


def skyline_atlas():
    """Tiny dense window grids for faraway towers (8x8 cells, each a slab of many floors)."""
    rng = np.random.default_rng(11)
    P = 1024
    S = P // ATLAS_N
    img = np.zeros((P, P, 3), np.float32)
    for k in range(ATLAS_N * ATLAS_N):
        cx, cy = k % ATLAS_N, k // ATLAS_N
        rows, cols = rng.integers(10, 18), rng.integers(6, 14)
        lit_p = rng.uniform(0.15, 0.55)
        warm = rng.random() < 0.6
        c = np.full((S, S, 3), 0.008, np.float32)
        ch, cw = S / rows, S / cols
        for r in range(rows):
            for q in range(cols):
                y0, x0 = int(r * ch + ch * 0.25), int(q * cw + cw * 0.18)
                y1, x1 = int(r * ch + ch * 0.85), int(q * cw + cw * 0.82)
                if rng.random() < lit_p:
                    col = np.array([1.0, 0.7, 0.4] if (warm and rng.random() < 0.8) else [0.6, 0.85, 1.0])
                    c[y0:y1, x0:x1] = col * rng.uniform(0.25, 1.0)
                else:
                    c[y0:y1, x0:x1] = 0.02
        img[cy * S:(cy + 1) * S, cx * S:(cx + 1) * S] = c
    save_png("skyline_atlas.png", srgb(img))


def cell_uv(k, inset=0.04):
    cx, cy = k % ATLAS_N, k // ATLAS_N
    a = 1.0 / ATLAS_N
    u0, v0 = cx * a + inset * a, cy * a + inset * a
    u1, v1 = (cx + 1) * a - inset * a, (cy + 1) * a - inset * a
    return [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]


def pick_cell(rng, lit=0.38, kinds=("warm", "cool", "tv", "neon")):
    if rng.random() < lit:
        choices = [i for i, k in enumerate(CELL_KINDS) if k in kinds]
    else:
        choices = [i for i, k in enumerate(CELL_KINDS) if k == "dark"]
    return rng.choice(choices)


# =============================================================================================
# Buildings
# =============================================================================================
def window_quad(g, F, u0, v0, u1, v1, w, cell):
    """Glass with an atlas cell (UV order matches Frame.quad's corner order)."""
    F.quad(g, u0, v0, u1, v1, w, "windows", uvs=cell_uv(cell))


def ac_unit(g, F, u, v, rng, out=0.0):
    w, h, d = 0.85, 0.6, 0.38
    F.box(g, u - w / 2, v, out, u + w / 2, v + h, out + d, "ac_body")
    F.quad(g, u - w / 2 + 0.06, v + 0.07, u + w / 2 - 0.3, v + h - 0.07, out + d + 0.005, "ac_grille")
    # brackets
    F.box(g, u - w / 2 + 0.05, v - 0.08, out, u - w / 2 + 0.1, v, out + d, "metal_rust")
    F.box(g, u + w / 2 - 0.1, v - 0.08, out, u + w / 2 - 0.05, v, out + d, "metal_rust")


def railing(g, F, u0, u1, v, w0, w1, mat="metal_dark", height=1.05, spacing=0.14):
    """A balcony railing around the front and sides of a slab (u0..u1, out to w1)."""
    t = 0.03
    F.box(g, u0, v + height - 0.04, w1 - 0.05, u1, v + height, w1, mat)              # top rail front
    F.box(g, u0, v + height - 0.04, w0, u0 + 0.05, v + height, w1, mat)              # side rails
    F.box(g, u1 - 0.05, v + height - 0.04, w0, u1, v + height, w1, mat)
    F.box(g, u0, v + 0.1, w1 - 0.04, u1, v + 0.13, w1 - 0.01, mat)
    n = max(2, int((u1 - u0) / spacing))
    for i in range(n + 1):
        u = u0 + (u1 - u0) * i / n
        F.bar(g, u - t / 2, v, w1 - 0.04, u + t / 2, v + height, w1 - 0.01, mat)
    m = max(2, int((w1 - w0) / spacing))
    for i in range(1, m):
        w = w0 + (w1 - w0) * i / m
        F.bar(g, u0, v, w - t / 2, u0 + t, v + height, w + t / 2, mat)
        F.bar(g, u1 - t, v, w - t / 2, u1, v + height, w + t / 2, mat)


def fire_escape(g, F, u0, u1, floors, ground_h, fh, rng):
    """Zig-zag steel fire escape over a bay range: landings, railings, stairs, drop ladder."""
    depth = 1.25
    for f in range(floors):
        v = ground_h + f * fh - 0.05
        F.box(g, u0, v - 0.06, 0.0, u1, v, depth, "metal_rust")
        railing(g, F, u0, u1, v, 0.0, depth, mat="metal_rust", height=0.95, spacing=0.12)
        # brackets under the landing
        for u in (u0 + 0.2, u1 - 0.2):
            g.tube(F.p(u, v - 0.06, depth - 0.1), F.p(u, v - 0.9, 0.0), 0.03, "metal_rust", segs=6)
        if f < floors - 1:
            # stair flight from this landing up to the next, alternating direction
            a, b = (u0 + 0.3, u1 - 0.6) if f % 2 == 0 else (u1 - 0.3, u0 + 0.6)
            steps = 12
            for s in range(steps):
                t = s / steps
                uu = a + (b - a) * t
                vv = v + fh * t
                F.box(g, uu - 0.12, vv, 0.25, uu + 0.12, vv + 0.04, depth - 0.15, "metal_rust")
            g.tube(F.p(a, v + 0.9, depth - 0.15), F.p(b, v + fh + 0.9, depth - 0.15), 0.02, "metal_rust", segs=5)
            g.tube(F.p(a, v, depth - 0.15), F.p(b, v + fh, depth - 0.15), 0.035, "metal_rust", segs=5)
    # drop ladder from the first landing
    lu = (u0 + u1) / 2
    v = ground_h - 0.1
    for side in (-0.22, 0.22):
        F.box(g, lu + side - 0.02, v - 2.6, depth - 0.12, lu + side + 0.02, v, depth - 0.08, "metal_rust")
    for s in range(9):
        F.box(g, lu - 0.22, v - 2.5 + s * 0.3, depth - 0.11, lu + 0.22, v - 2.5 + s * 0.3 + 0.025, depth - 0.09, "metal_rust")


def pipe_run(g, F, u, v0, v1, r=0.07, w=0.18, mat="metal_galv"):
    g.tube(F.p(u, v0, w), F.p(u, v1, w), r, mat, segs=8)
    v = v0 + 1.0
    while v < v1:
        F.box(g, u - r - 0.03, v, 0.0, u + r + 0.03, v + 0.06, w + r + 0.02, "metal_dark")
        v += 2.6


def storefront(g, F, u0, u1, h, rng, anchors, side_name):
    """Ground floor shop: recessed glass, door, fascia band for a lightbox sign, shutter/awning."""
    rec = 0.55
    fas = 1.15  # fascia height
    g_top = h - fas - 0.25
    pier = 0.35
    F.box(g, u0, 0, 0, u0 + pier, h, 0.12, "trim")
    F.box(g, u1 - pier, 0, 0, u1, h, 0.12, "trim")
    a0, a1 = u0 + pier, u1 - pier
    # fascia (sign band) and its soffit
    F.box(g, a0, g_top, 0.0, a1, h, 0.2, "fascia")
    F.quad(g, a0, g_top, a1, g_top, 0.0, "fascia")
    # recessed opening
    F.hole(g, a0, 0.0, a1, g_top, rec, "trim")
    F.quad(g, a0, g_top - 0.001, a1, g_top - 0.001, 0, "trim", normal=(0, 0, -1))
    F.quad(g, a0, 0.02, a1, g_top, -rec, "trim")  # back wall (fallback behind glass)
    kind = rng.random()
    cell = rng.choice([i for i, k in enumerate(CELL_KINDS) if k in ("shop",)])
    if kind < 0.18:  # closed: roller shutter down
        F.quad(g, a0, 0.0, a1, g_top, -0.1, "shutter")
        F.box(g, a0, g_top - 0.35, -0.12, a1, g_top, 0.05, "shutter")
    else:
        # glass with mullions, door
        nseg = max(2, int((a1 - a0) / 1.6))
        for i in range(nseg):
            s0 = a0 + (a1 - a0) * i / nseg
            s1 = a0 + (a1 - a0) * (i + 1) / nseg
            window_quad(g, F, s0, 0.35, s1, g_top - 0.1, -rec + 0.12, cell if i != nseg // 2 else cell)
            F.box(g, s1 - 0.03, 0.0, -rec + 0.08, s1 + 0.03, g_top, -rec + 0.16, "metal_dark")
        F.box(g, a0, 0.0, -rec + 0.08, a1, 0.35, -rec + 0.16, "metal_dark")
        F.box(g, a0, g_top - 0.1, -rec + 0.08, a1, g_top, -rec + 0.16, "metal_dark")
        if kind < 0.45:  # half-open shutter
            drop = rng.uniform(0.3, 0.9)
            F.box(g, a0, g_top - (g_top - 0.3) * drop, -0.14, a1, g_top, -0.06, "shutter")
        F.box(g, a0, g_top - 0.35, -0.12, a1, g_top, 0.05, "shutter")  # shutter housing
    # awning
    if rng.random() < 0.5:
        mat = rng.choice(["awning_red", "awning_teal", "awning_black"])
        ah = g_top - 0.05
        out = rng.uniform(1.1, 1.6)
        drop = 0.55
        g.poly([F.p(a0 - 0.1, ah, 0.2), F.p(a1 + 0.1, ah, 0.2), F.p(a1 + 0.1, ah - drop, out), F.p(a0 - 0.1, ah - drop, out)],
               tuple(F.N * 0.5 + F.V), mat)
        g.poly([F.p(a0 - 0.1, ah, 0.2), F.p(a1 + 0.1, ah, 0.2), F.p(a1 + 0.1, ah - drop, out), F.p(a0 - 0.1, ah - drop, out)],
               tuple(-(F.N * 0.5 + F.V)), mat)
        F.box(g, a0 - 0.1, ah - drop - 0.25, out - 0.02, a1 + 0.1, ah - drop, out + 0.02, mat)
    anchors.append({"kind": "shop_sign", "side": side_name, "pos": list(F.p((a0 + a1) / 2, g_top + 0.08 + (h - g_top) / 2 - 0.04, 0.2)),
                    "width": a1 - a0, "height": h - g_top - 0.15, "normal": list(F.N), "u": list(F.U)})
    anchors.append({"kind": "shop_light", "side": side_name, "pos": list(F.p((a0 + a1) / 2, g_top * 0.6, -0.3)), "normal": list(F.N)})


def facade(g, F, width, height, ground_h, fh, style, rng, anchors, side_name, primary=True, base_v=0.0, lit=0.36):
    """One face of a building mass, from v=base_v (bottom) to height. Windows in bays."""
    wall = {"tenement": "wall_plaster", "megablock": "wall_concrete", "glass": "wall_panel",
            "brick": "wall_brick", "tile": "wall_tile"}[style]
    bay = {"tenement": 2.8, "megablock": 3.0, "glass": 3.2, "brick": 2.6, "tile": 2.7}[style] * rng.uniform(0.95, 1.08)
    nb = max(1, int(round(width / bay)))
    bay = width / nb
    v_start = base_v + (ground_h if base_v == 0.0 else 0.0)
    det = DETAIL if primary else DETAIL * 0.35
    if base_v == 0.0 and primary:
        # ground floor: shops in groups of bays
        u = 0.0
        while u < width - 0.5:
            span = min(width - u, bay * rng.choice([1, 2, 2, 3]))
            if width - (u + span) < bay * 0.9:
                span = width - u
            storefront(g, F, u - width / 2, u + span - width / 2, ground_h, rng, anchors, side_name)
            u += span
        # cornice over the shops
        F.box(g, -width / 2, ground_h - 0.05, 0, width / 2, ground_h + 0.3, 0.35, "trim")
    elif base_v == 0.0:
        F.quad(g, -width / 2, 0, width / 2, ground_h, 0, wall)
        if rng.random() < 0.5:
            # service door + a lamp
            du = rng.uniform(-width / 2 + 1.5, width / 2 - 1.5)
            F.quad(g, du - 0.55, 0.0, du + 0.55, 2.3, 0.02, "shutter")
            anchors.append({"kind": "door_light", "side": side_name, "pos": list(F.p(du, 2.7, 0.35)), "normal": list(F.N)})
    floors = max(0, int((height - v_start) // fh))
    top_v = v_start + floors * fh
    # bays x floors
    fe_bays = set()
    if primary and DETAIL >= 0.9 and style in ("tenement", "brick") and nb >= 3 and rng.random() < 0.75 and floors >= 3:
        b0 = rng.integers(0, nb - 1)
        fe_bays = {b0, b0 + 1}
    balcony_bays = set()
    if style in ("tenement", "megablock", "tile"):
        for b in range(nb):
            if b not in fe_bays and rng.random() < (0.3 if style != "megablock" else 0.45) * det:
                balcony_bays.add(b)
    for f in range(floors):
        v0 = v_start + f * fh
        v1 = v0 + fh
        # floor band
        if style in ("tenement", "brick", "tile"):
            F.box(g, -width / 2, v0 - 0.06, 0, width / 2, v0 + 0.1, 0.08, "trim")
        for b in range(nb):
            u0 = -width / 2 + b * bay
            u1 = u0 + bay
            if style == "glass":
                wu0, wu1 = u0 + 0.12, u1 - 0.12
                wv0, wv1 = v0 + 0.85, v1 - 0.25
                rd = 0.12
            elif style == "megablock":
                wu0, wu1 = u0 + bay * 0.22, u1 - bay * 0.22
                wv0, wv1 = v0 + 0.9, v1 - 0.55
                rd = 0.28
            else:
                ww = min(1.35, bay * 0.5)
                wu0, wu1 = (u0 + u1) / 2 - ww / 2, (u0 + u1) / 2 + ww / 2
                wv0, wv1 = v0 + 0.85, v1 - 0.5
                rd = 0.22
            # wall around the opening
            F.quad(g, u0, v0, wu0, v1, 0, wall)
            F.quad(g, wu1, v0, u1, v1, 0, wall)
            F.quad(g, wu0, v0, wu1, wv0, 0, "wall_panel" if style == "glass" else wall)
            F.quad(g, wu0, wv1, wu1, v1, 0, "wall_panel" if style == "glass" else wall)
            F.hole(g, wu0, wv0, wu1, wv1, rd, "trim")
            cell = pick_cell(rng, lit=lit if style != "glass" else lit * 0.9,
                             kinds=("cool", "warm") if style == "glass" else ("warm", "cool", "tv", "neon", "warm"))
            window_quad(g, F, wu0, wv0, wu1, wv1, -rd, cell)
            if style != "glass":
                F.box(g, wu0 - 0.08, wv0 - 0.1, 0, wu1 + 0.08, wv0, 0.12, "trim")  # sill
                F.box(g, wu0 - 0.05, wv1, 0, wu1 + 0.05, wv1 + 0.12, 0.06, "trim")  # lintel
            if style == "megablock":
                # protruding capsule frame around each cell
                out = 0.25 + 0.5 * ((b * 7 + f * 3) % 5) / 4
                F.box(g, u0 + 0.05, v0 + 0.05, 0, u0 + 0.2, v1 - 0.05, out, "trim")
                F.box(g, u1 - 0.2, v0 + 0.05, 0, u1 - 0.05, v1 - 0.05, out, "trim")
                F.box(g, u0 + 0.05, v1 - 0.2, 0, u1 - 0.05, v1 - 0.05, out, "trim")
            if b in balcony_bays and f % (1 if style == "megablock" else 1) == 0 and b not in fe_bays:
                d = 1.1 if style != "megablock" else 0.9
                F.box(g, u0 + 0.15, v0 - 0.12, 0, u1 - 0.15, v0 + 0.05, d, "concrete_raw")
                railing(g, F, u0 + 0.15, u1 - 0.15, v0 + 0.05, 0.0, d, height=1.0)
            elif b not in fe_bays and rng.random() < (0.32 if style != "glass" else 0.0) * det:
                ac_unit(g, F, (wu0 + wu1) / 2 + rng.uniform(-0.2, 0.2), wv0 - 0.75, rng)
        if style == "glass":
            # vertical fins and slab edges
            F.box(g, -width / 2, v0 - 0.05, 0, width / 2, v0 + 0.12, 0.15, "metal_dark")
    if style == "glass":
        for b in range(nb + 1):
            u = -width / 2 + b * bay
            F.box(g, u - 0.06, v_start, 0, u + 0.06, top_v, 0.45, "metal_dark")
    # leftover strip up to the roof line
    if top_v < height:
        F.quad(g, -width / 2, top_v, width / 2, height, 0, wall)
    # downpipes between some bays
    for b in range(1, nb):
        if rng.random() < 0.25 and (b not in fe_bays) and (b - 1 not in fe_bays):
            pipe_run(g, F, -width / 2 + b * bay, base_v + 0.2, height - 0.2, r=rng.choice([0.05, 0.08, 0.11]))
    if fe_bays and floors >= 2:
        b0 = min(fe_bays)
        fire_escape(g, F, -width / 2 + b0 * bay + 0.2, -width / 2 + (b0 + 2) * bay - 0.2, floors, v_start, fh, rng)
    # blade-sign and facade-sign anchors on street faces
    if primary:
        for b in range(nb):
            if rng.random() < 0.45:
                fv = v_start + rng.uniform(0.5, min(6.0, max(1.0, floors - 1))) * fh
                anchors.append({"kind": "blade", "side": side_name, "pos": list(F.p(-width / 2 + b * bay, fv, 0.0)),
                                "normal": list(F.N), "u": list(F.U)})
        if floors > 6:
            anchors.append({"kind": "billboard", "side": side_name,
                            "pos": list(F.p(rng.uniform(-width / 4, width / 4), v_start + rng.uniform(3, min(floors - 2, 12)) * fh, 0.0)),
                            "normal": list(F.N), "u": list(F.U), "width": width})
    return top_v


def roof(g, x0, y0, x1, y1, z, rng, anchors, big=False):
    """Parapet and rooftop clutter on a flat roof rectangle."""
    g.poly([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], (0, 0, 1), "concrete_raw")
    t, ph = 0.3, 1.0
    g.box(x0, y0, z, x1, y0 + t, z + ph, "trim")
    g.box(x0, y1 - t, z, x1, y1, z + ph, "trim")
    g.box(x0, y0, z, x0 + t, y1, z + ph, "trim")
    g.box(x1 - t, y0, z, x1, y1, z + ph, "trim")
    w, d = x1 - x0, y1 - y0
    # HVAC boxes
    for _ in range(rng.integers(1, 4)):
        bw, bd, bh = rng.uniform(1.5, 3.5), rng.uniform(1.2, 2.5), rng.uniform(0.8, 1.8)
        cx, cy = rng.uniform(x0 + 2, max(x0 + 2.1, x1 - 2 - bw)), rng.uniform(y0 + 2, max(y0 + 2.1, y1 - 2 - bd))
        g.box(cx, cy, z, cx + bw, cy + bd, z + bh, "ac_body")
        g.cyl(cx + bw / 2, cy + bd / 2, z + bh, z + bh + 0.25, min(bw, bd) * 0.3, "ac_grille", segs=14)
    # water tank on legs
    if rng.random() < 0.7 and w > 7 and d > 7:
        tx, ty = rng.uniform(x0 + 3, x1 - 3), rng.uniform(y0 + d * 0.5, y1 - 3)
        r = rng.uniform(1.2, 1.8)
        lh = rng.uniform(2.0, 3.0)
        for a in range(4):
            ang = math.pi / 4 + a * math.pi / 2
            g.tube((tx + math.cos(ang) * r * 0.8, ty + math.sin(ang) * r * 0.8, z), (tx + math.cos(ang) * r * 0.8, ty + math.sin(ang) * r * 0.8, z + lh), 0.08, "metal_rust", segs=6)
        g.cyl(tx, ty, z + lh, z + lh + 3.0, r, "tank_wood", segs=16, caps=False)
        g.cyl(tx, ty, z + lh + 3.0, z + lh + 4.0, r * 1.02, "metal_dark", segs=16, r1=0.08)
        g.cyl(tx, ty, z + lh - 0.1, z + lh, r * 1.02, "metal_dark", segs=16)
    # antenna mast with a beacon on top
    if rng.random() < (0.9 if big else 0.5):
        mx, my = rng.uniform(x0 + 2, x1 - 2), rng.uniform(y0 + d * 0.4, y1 - 2)
        mh = rng.uniform(5, 14) if big else rng.uniform(3, 7)
        g.tube((mx, my, z), (mx, my, z + mh), 0.09, "metal_galv", segs=6)
        for k in range(3):
            hz = z + mh * (0.4 + 0.2 * k)
            g.tube((mx - 0.6, my, hz), (mx + 0.6, my, hz), 0.025, "metal_galv", segs=4)
        anchors.append({"kind": "beacon", "pos": [mx, my, z + mh + 0.15]})
    if big:
        anchors.append({"kind": "roof_sign", "pos": [(x0 + x1) / 2, y0 + 1.0, z + ph], "width": w * 0.8})


def building(spec):
    rng = np.random.default_rng(spec.get("seed", 1))
    W, D, H = spec["width"], spec["depth"], spec["height"]
    gh, fh = spec.get("ground", 5.0), spec.get("floor", 3.3)
    style = spec.get("style", "tenement")
    side_style = spec.get("side_style", style)
    lit = spec.get("lit", 0.36)
    anchors = []
    global DETAIL
    DETAIL = spec.get("detail", 1.0)
    g = Geo()
    g.grime = True
    t = float(rng.uniform(0.8, 1.05))
    g.tint = (t, t * float(rng.uniform(0.96, 1.02)), t * float(rng.uniform(0.96, 1.04)))
    masses = [(0.0, spec.get("setback_at", H), 0.0)]
    if spec.get("setback_at") and spec["setback_at"] < H - 6:
        masses.append((spec["setback_at"], H, spec.get("setback", 3.0)))
    for mi_, (zb, zt, sb) in enumerate(masses):
        x0, x1 = -W / 2 + sb, W / 2 - sb
        y0, y1 = sb, D - sb
        w, d = x1 - x0, y1 - y0
        front = Frame((0, y0, zb), (1, 0, 0), (0, -1, 0))
        right = Frame((x1, (y0 + y1) / 2, zb), (0, 1, 0), (1, 0, 0))
        left = Frame((x0, (y0 + y1) / 2, zb), (0, -1, 0), (-1, 0, 0))
        back = Frame((0, y1, zb), (-1, 0, 0), (0, 1, 0))
        hh = zt - zb
        if mi_ == 0:
            facade(g, front, w, hh, gh, fh, style, rng, anchors, "front", primary=True, lit=lit)
        else:
            facade(g, front, w, hh, 0.001, fh, style, rng, anchors, "front", primary=False, base_v=0.0, lit=lit)
        for F, nm in ((right, "right"), (left, "left")):
            facade(g, F, d, hh, gh if mi_ == 0 else 0.001, fh, side_style, rng, anchors, nm, primary=False, lit=lit * 0.8)
        facade(g, back, w, hh, gh if mi_ == 0 else 0.001, fh, side_style, rng, anchors, "back", primary=False, lit=lit * 0.6)
        roof(g, x0, y0, x1, y1, zt, rng, anchors, big=(mi_ == len(masses) - 1 and H > 60))
        # LED accents: a glowing parapet edge and, on towers, vertical light bands on the street corners
        if H > 45 and rng.random() < (0.85 if H > 80 else 0.45):
            led = ["led_cyan", "led_magenta", "led_amber", "led_white", "led_red"][int(rng.integers(0, 5))]
            g.box(x0, y0 - 0.08, zt + 1.0, x1, y0 + 0.04, zt + 1.16, led)
            if mi_ == len(masses) - 1 and H > 80 and rng.random() < 0.6:
                for cx in (x0, x1):
                    g.box(cx - 0.09, y0 - 0.09, zb + (gh if mi_ == 0 else 0.0) + 3, cx + 0.09, y0 + 0.09, zt + 1.0, led)
    ob = g.to_object(spec["name"])
    return ob, anchors


# =============================================================================================
# Street kit, skybridges, monorail, skyline, signs
# =============================================================================================
def skybridge(spec):
    """Enclosed glass walkway spanning the street along local x (length L), centered on origin."""
    g = Geo()
    L, w, h = spec["length"], spec.get("width", 4.0), spec.get("height", 3.4)
    x0, x1 = -L / 2, L / 2
    y0, y1 = -w / 2, w / 2
    # deck + roof
    g.box(x0, y0, -0.6, x1, y1, 0.0, "concrete_raw")
    g.box(x0, y0, h, x1, y1, h + 0.5, "wall_panel")
    # truss under the deck
    n = int(L / 3)
    for i in range(n):
        a = x0 + L * i / n
        b = x0 + L * (i + 1) / n
        for yy in (y0 + 0.1, y1 - 0.1):
            g.tube((a, yy, -0.6), (b, yy, -1.8), 0.07, "metal_dark", segs=6)
            g.tube((a, yy, -1.8), (b, yy, -0.6), 0.07, "metal_dark", segs=6)
    for yy in (y0 + 0.1, y1 - 0.1):
        g.box(x0, yy - 0.12, -1.9, x1, yy + 0.12, -1.7, "metal_dark")
    # glazing with mullions; interior seen through as lit corridor
    for side, yy, nrm in (("front", y0, (0, -1, 0)), ("back", y1, (0, 1, 0))):
        g.poly([(x0, yy, 0.0), (x1, yy, 0.0), (x1, yy, h), (x0, yy, h)], nrm, "glass_dark")
        m = int(L / 1.6)
        for i in range(m + 1):
            x = x0 + L * i / m
            g.box(x - 0.05, yy - 0.06, 0, x + 0.05, yy + 0.06, h, "metal_dark")
        g.box(x0, yy - 0.07, 1.0, x1, yy + 0.07, 1.08, "metal_dark")
    # interior: floor, ceiling light strip
    g.poly([(x0, y0 + 0.1, 0.01), (x1, y0 + 0.1, 0.01), (x1, y1 - 0.1, 0.01), (x0, y1 - 0.1, 0.01)], (0, 0, 1), "fascia")
    g.box(x0, -0.25, h - 0.08, x1, 0.25, h - 0.02, "led_white")
    # LED edge strips outside (a signature accent)
    col = spec.get("accent", "led_cyan")
    g.box(x0, y0 - 0.08, -0.62, x1, y0 - 0.02, -0.56, col)
    g.box(x0, y1 + 0.02, -0.62, x1, y1 + 0.08, -0.56, col)
    return g.to_object(spec["name"])


def monorail_track(spec):
    """Concrete beam along local x with T-pier columns."""
    g = Geo()
    L, h = spec["length"], spec["height"]
    x0, x1 = -L / 2, L / 2
    g.box(x0, -0.55, h - 1.6, x1, 0.55, h, "concrete_raw")       # beam
    g.box(x0, -0.75, h - 1.75, x1, 0.75, h - 1.6, "trim")          # lip
    g.box(x0, -0.62, h, x1, -0.55, h + 0.15, "metal_dark")         # power rails
    g.box(x0, 0.55, h, x1, 0.62, h + 0.15, "metal_dark")
    for x in spec.get("piers", []):
        g.box(x - 0.8, -0.8, 0.0, x + 0.8, 0.8, h - 2.6, "concrete_raw")
        g.box(x - 1.0, -2.0, h - 2.6, x + 1.0, 2.0, h - 1.75, "concrete_raw")
    # underside service lights
    n = int(L / 6)
    for i in range(n):
        x = x0 + (i + 0.5) * L / n
        g.box(x - 0.4, -0.1, h - 1.82, x + 0.4, 0.1, h - 1.76, "led_amber")
    return g.to_object(spec["name"])


def train_car(spec):
    """Monorail car: rounded body along x, ribbon windows (lit interior), stripe, bogie skirt."""
    g = Geo()
    L, w, h = spec.get("length", 14.0), 2.8, 3.0
    x0, x1 = -L / 2, L / 2
    nose = spec.get("nose", 0)  # -1 front at x0, +1 front at x1
    segs = 10
    prof = []  # rounded-rectangle cross-section in (y, z)
    for i in range(segs + 1):
        a = math.pi * i / segs
        prof.append((math.cos(a) * w / 2, 0.6 + (h - 0.6) * 0.5 + math.sin(a) * (h - 0.6) * 0.5))
    prof = [(w / 2, 0.6)] + prof + [(-w / 2, 0.6)]
    xa = x0 + (1.6 if nose < 0 else 0.0)
    xb = x1 - (1.6 if nose > 0 else 0.0)
    for i in range(len(prof) - 1):
        (ya, za), (yb, zb) = prof[i], prof[i + 1]
        nrm = Vector((0, (ya + yb) / 2, (za + zb) / 2 - 1.8)).normalized()
        mat = "train_body"
        g.poly([(xa, ya, za), (xb, ya, za), (xb, yb, zb), (xa, yb, zb)], tuple(nrm), mat)
    # ends (nose tapers)
    for end, xe, d in ((-1, xa, -1), (1, xb, 1)):
        tip = xe + d * (1.6 if nose == end else 0.0)
        cz = 1.7
        for i in range(len(prof) - 1):
            (ya, za), (yb, zb) = prof[i], prof[i + 1]
            if nose == end:
                g.poly([(xe, ya, za), (xe, yb, zb), (tip, yb * 0.35, cz + (zb - cz) * 0.5), (tip, ya * 0.35, cz + (za - cz) * 0.5)],
                       (d, 0, 0), "train_body")
            else:
                g.poly([(xe, ya, za), (xe, yb, zb), (xe, 0, cz)], (d, 0, 0), "train_body")
        if nose == end:
            g.poly([(tip, p[0] * 0.35, cz + (p[1] - cz) * 0.5) for p in prof[:-1]][::-1], (d, 0, 0), "glass_dark")
            g.box(tip - 0.05 * d - 0.04, -0.5, 1.0, tip - 0.05 * d + 0.04, 0.5, 1.12, "led_white")
    # windows: lit strip on both sides
    for yy, n in ((w / 2 + 0.01, (0, 1, 0)), (-w / 2 - 0.01, (0, -1, 0))):
        nw = int((xb - xa) / 1.4)
        for i in range(nw):
            a = xa + 0.4 + i * (xb - xa - 0.8) / nw
            b = a + (xb - xa - 0.8) / nw - 0.2
            g.poly([(a, yy, 1.6), (b, yy, 1.6), (b, yy, 2.5), (a, yy, 2.5)], n, "glow_ffe6c8_2.2")
        g.poly([(xa, yy, 1.2), (xb, yy, 1.2), (xb, yy, 1.35), (xa, yy, 1.35)], n, spec.get("stripe", "led_magenta"))
    # skirt
    g.box(xa + 0.5, -w / 2 + 0.1, 0.0, xb - 0.5, w / 2 - 0.1, 0.6, "metal_dark")
    return g.to_object(spec["name"])


def skyline_tower(spec):
    """Faraway megatower: stacked setbacks, skyline window atlas, crown lights and a logo panel."""
    rng = np.random.default_rng(spec.get("seed", 3))
    g = Geo()
    W, D, H = spec["width"], spec["depth"], spec["height"]
    tiers = spec.get("tiers", 3)
    z = 0.0
    anchors = []
    for t in range(tiers):
        f = 1.0 - 0.18 * t
        w, d = W * f, D * f
        th = H * ([0.55, 0.3, 0.15] + [0.1] * 5)[t] if tiers > 1 else H
        x0, x1, y0, y1 = -w / 2, w / 2, -d / 2, d / 2
        for (pts, nrm, length) in (
            ([(x0, y0), (x1, y0)], (0, -1, 0), w), ([(x1, y0), (x1, y1)], (1, 0, 0), d),
            ([(x1, y1), (x0, y1)], (0, 1, 0), w), ([(x0, y1), (x0, y0)], (-1, 0, 0), d)):
            (ax, ay), (bx, by) = pts
            # tile atlas cells up the face: each cell covers ~30 m x ~45 m
            nu = max(1, int(length / 30))
            nv = max(1, int(th / 45))
            for i in range(nu):
                for j in range(nv):
                    ta, tb = i / nu, (i + 1) / nu
                    pa = (ax + (bx - ax) * ta, ay + (by - ay) * ta)
                    pb = (ax + (bx - ax) * tb, ay + (by - ay) * tb)
                    za, zb = z + th * j / nv, z + th * (j + 1) / nv
                    k = int(rng.integers(0, ATLAS_N * ATLAS_N))
                    g.poly([(pa[0], pa[1], za), (pb[0], pb[1], za), (pb[0], pb[1], zb), (pa[0], pa[1], zb)], nrm,
                           "skyline_windows", uvs=cell_uv(k, inset=0.0))
        g.poly([(x0, y0, z + th), (x1, y0, z + th), (x1, y1, z + th), (x0, y1, z + th)], (0, 0, 1), "skin_hull")
        # crown light band
        g.box(x0 - 0.3, y0 - 0.3, z + th - 1.2, x1 + 0.3, y1 + 0.3, z + th - 0.6, spec.get("crown", "led_cyan"))
        z += th
    mh = H * 0.12
    g.tube((0, 0, z), (0, 0, z + mh), 1.2, "metal_dark", segs=8)
    anchors.append({"kind": "beacon", "pos": [0, 0, z + mh + 1]})
    anchors.append({"kind": "tower_top", "pos": [0, 0, z]})
    ob = g.to_object(spec["name"])
    return ob, anchors


# ----- signs -----------------------------------------------------------------------------------
def _text_obj(text, size, extrude=0.0, bevel=0.0, font=None):
    cu = bpy.data.curves.new("txt", "FONT")
    cu.body = text
    cu.size = size
    cu.align_x = "CENTER"
    cu.align_y = "CENTER"
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = 2
    if font:
        cu.font = font
    ob = bpy.data.objects.new("txt", cu)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def _neon_outline(text, size, tube=0.025):
    """Letters as glowing tubes: the text's outline converted to a beveled curve."""
    ob = _text_obj(text, size)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.convert(target="CURVE")
    ob = bpy.context.view_layer.objects.active
    ob.data.dimensions = "3D"
    ob.data.fill_mode = "FULL"
    ob.data.bevel_depth = tube
    ob.data.bevel_resolution = 1
    ob.data.resolution_u = 4
    bpy.ops.object.convert(target="MESH")
    ob = bpy.context.view_layer.objects.active
    ob.select_set(False)
    return ob


def _solid_text(text, size, depth):
    ob = _text_obj(text, size, extrude=depth / 2)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.convert(target="MESH")
    ob = bpy.context.view_layer.objects.active
    ob.select_set(False)
    return ob


def _set_mat(ob, name):
    ob.data.materials.clear()
    ob.data.materials.append(material(name))


def _orient_vertical_text(ob):
    """Blender text lies in XY; stand it up in XZ (facing -y)."""
    ob.rotation_euler = (math.pi / 2, 0, 0)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    ob.select_set(False)


def glyph_strokes(rng, n_strokes=None):
    """An invented script: strokes on a 3x4 grid (no real-language characters)."""
    pts = [(x, y) for x in (0, 0.5, 1) for y in (0, 0.33, 0.66, 1)]
    strokes = []
    n = n_strokes or int(rng.integers(3, 6))
    for _ in range(n):
        a = pts[int(rng.integers(0, len(pts)))]
        b = pts[int(rng.integers(0, len(pts)))]
        if a == b:
            continue
        if a[0] != b[0] and a[1] != b[1] and rng.random() < 0.6:
            b = (b[0], a[1])
        strokes.append((a, b))
    return strokes


def sign(spec):
    """Signs, built facing -y (front) in local XZ, origin at the mounting point on the wall (y=0)."""
    kind = spec["kind"]
    rng = np.random.default_rng(spec.get("seed", 5))
    col = spec.get("color", "ff2a8a")
    strength = spec.get("strength", 7.0)
    neon = f"neon_{col}_{strength}"
    objs = []
    g = Geo()
    if kind == "blade":
        # vertical box sign standing off the wall: frame, glyph column on both faces, chaser bulbs
        h, w, t = spec.get("height", 6.0), spec.get("width", 1.1), 0.35
        out0 = 0.5
        g.box(-t / 2, -out0 - w, -h / 2, t / 2, -out0, h / 2, "fascia")
        g.box(-0.05, -out0, -0.08 + h / 2 - 0.4, 0.05, 0.0, h / 2 - 0.3, "metal_dark")
        g.box(-0.05, -out0, -h / 2 + 0.3, 0.05, 0.0, -h / 2 + 0.4, "metal_dark")
        # rim tube
        for face in (-1, 1):
            x = face * (t / 2 + 0.03)
            corners = [(-out0 - w + 0.08, -h / 2 + 0.08), (-out0 - 0.08, -h / 2 + 0.08), (-out0 - 0.08, h / 2 - 0.08), (-out0 - w + 0.08, h / 2 - 0.08)]
            for i in range(4):
                a, b = corners[i], corners[(i + 1) % 4]
                g.tube((x, a[0], a[1]), (x, b[0], b[1]), 0.022, f"neon_{spec.get('rim', 'ffffff')}_{strength * 0.7:.1f}", segs=6)
            # glyphs
            n = max(2, int((h - 0.6) / (w * 0.9)))
            gs = (w - 0.35)
            for k in range(n):
                cz = h / 2 - 0.45 - k * (h - 0.6) / n - gs / 2
                for (a, b) in glyph_strokes(np.random.default_rng(spec.get("seed", 5) * 31 + k)):
                    pa = (x * 1.02, -out0 - w / 2 + (a[0] - 0.5) * gs, cz + (a[1] - 0.5) * gs * 1.2)
                    pb = (x * 1.02, -out0 - w / 2 + (b[0] - 0.5) * gs, cz + (b[1] - 0.5) * gs * 1.2)
                    g.tube(pa, pb, 0.03, neon, segs=6)
        objs.append(g.to_object(spec["name"]))
    elif kind == "lightbox":
        # shop fascia lightbox: glowing panel + dark extruded letters (or reverse)
        w, h = spec["width"], spec["height"]
        g.box(-w / 2, -0.22, -h / 2, w / 2, 0.0, h / 2, "fascia")
        panel = f"glow_{spec.get('panel', 'f2f2f2')}_{spec.get('panel_strength', 2.5)}"
        g.poly([(-w / 2 + 0.06, -0.225, -h / 2 + 0.06), (w / 2 - 0.06, -0.225, -h / 2 + 0.06),
                (w / 2 - 0.06, -0.225, h / 2 - 0.06), (-w / 2 + 0.06, -0.225, h / 2 - 0.06)], (0, -1, 0), panel)
        objs.append(g.to_object(spec["name"]))
        txt = _solid_text(spec["text"], h * 0.55, 0.05)
        _orient_vertical_text(txt)
        dims = txt.dimensions
        s = min(1.0, (w * 0.86) / max(0.01, dims.x))
        txt.scale = (s, 1, s)
        txt.location = (0, -0.26, 0)
        _set_mat(txt, f"glow_{spec.get('text_color', '101014')}_{spec.get('text_strength', 0.0)}" if spec.get('text_strength') else "fascia")
        objs.append(txt)
    elif kind == "neon_text":
        # tube lettering on a dark raceway, standing off the wall
        txt = _neon_outline(spec["text"], spec.get("size", 1.0), tube=spec.get("tube", 0.028))
        _orient_vertical_text(txt)
        dims = txt.dimensions
        maxw = spec.get("max_width", 99)
        if dims.x > maxw:
            s = maxw / dims.x
            txt.scale = (s, s, s)
            bpy.context.view_layer.objects.active = txt
            txt.select_set(True)
            bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
            txt.select_set(False)
            dims = txt.dimensions
        txt.location = (0, -0.18, 0)
        _set_mat(txt, neon)
        objs.append(txt)
        w, h = dims.x + 0.5, dims.z + 0.4
        g.box(-w / 2, -0.12, -h / 2, w / 2, 0.0, h / 2, "fascia")
        # standoffs
        for x in (-w / 2 + 0.3, w / 2 - 0.3):
            g.box(x - 0.03, -0.18, -h / 2 + 0.05, x + 0.03, -0.12, -h / 2 + 0.1, "metal_dark")
        objs.append(g.to_object(spec["name"]))
    elif kind == "billboard":
        # big framed screen (the engine drives its emissive animation) + catwalk + floodlights
        w, h = spec["width"], spec["height"]
        g.box(-w / 2 - 0.3, -0.6, -h / 2 - 0.3, w / 2 + 0.3, 0.0, h / 2 + 0.3, "metal_dark")
        g.box(-w / 2 - 0.3, -1.6, -h / 2 - 0.5, w / 2 + 0.3, -0.4, -h / 2 - 0.4, "metal_dark")  # catwalk
        railing_g = Frame((0, -1.6, -h / 2 - 0.4), (1, 0, 0), (0, -1, 0))
        railing(g, railing_g, -w / 2 - 0.3, w / 2 + 0.3, 0.0, -1.2, 0.0, height=0.9, spacing=0.4)
        for x in (-w / 3, 0, w / 3):
            g.tube((x, -1.5, -h / 2 - 0.4), (x, -2.2, -h / 2 + 0.2), 0.04, "metal_dark", segs=5)
            g.box(x - 0.25, -2.4, -h / 2 + 0.1, x + 0.25, -2.0, -h / 2 + 0.35, "metal_dark")
            g.box(x - 0.2, -2.0, -h / 2 + 0.36, x + 0.2, -1.95, -h / 2 + 0.4, "led_white")
        objs.append(g.to_object(spec["name"]))
    elif kind == "roof_text":
        # big rooftop letters on a steel lattice
        txt = _solid_text(spec["text"], spec.get("size", 3.0), 0.4)
        _orient_vertical_text(txt)
        dims = txt.dimensions
        txt.location = (0, -0.4, dims.z / 2 + 1.6)
        _set_mat(txt, neon)
        objs.append(txt)
        w = dims.x + 1.0
        for i in range(int(w / 2.5) + 1):
            x = -w / 2 + i * 2.5
            g.tube((x, 0.2, 0), (x, 0.2, dims.z + 1.8), 0.08, "metal_dark", segs=6)
            g.tube((x, 0.2, 0), (x, 1.6, 0), 0.06, "metal_dark", segs=5)
            g.tube((x, 1.6, 0), (x, 0.2, dims.z + 1.2), 0.06, "metal_dark", segs=5)
        g.tube((-w / 2, 0.2, 1.4), (w / 2, 0.2, 1.4), 0.07, "metal_dark", segs=6)
        g.tube((-w / 2, 0.2, dims.z + 1.8), (w / 2, 0.2, dims.z + 1.8), 0.07, "metal_dark", segs=6)
        objs.append(g.to_object(spec["name"]))
    return objs


# ----- props ------------------------------------------------------------------------------
def prop(name, spec):
    g = Geo()
    if name == "street_light":
        # tall modern lamp: tapered pole, long arm, LED bar head
        h = 8.5
        g.cyl(0, 0, 0, 0.6, 0.2, "metal_dark", segs=10)
        g.cyl(0, 0, 0.6, h, 0.11, "metal_dark", segs=10, r1=0.07)
        g.tube((0, 0, h - 0.2), (0, -3.2, h + 0.25), 0.06, "metal_dark", segs=8)
        g.box(-0.18, -3.6, h + 0.12, 0.18, -2.4, h + 0.32, "metal_dark")
        g.box(-0.13, -3.55, h + 0.1, 0.13, -2.45, h + 0.12, "led_white")
        g.box(-0.04, -0.2, 2.5, 0.04, -0.12, 3.6, "led_cyan")  # pole accent strip
    elif name == "bollard":
        g.cyl(0, 0, 0, 0.95, 0.11, "metal_dark", segs=12)
        g.cyl(0, 0, 0.78, 0.84, 0.115, "led_amber", segs=12, caps=False)
    elif name == "noodle_stall":
        # vendor stall: counter, steel back shelf, canopy, hanging lantern hooks, menu board
        W, D = 3.2, 1.6
        g.box(-W / 2, 0, 0, W / 2, 0.75, 1.0, "metal_galv")              # counter base
        g.box(-W / 2 - 0.05, -0.25, 1.0, W / 2 + 0.05, 0.8, 1.06, "metal_dark")  # counter top (overhang to the front)
        g.box(-W / 2, D - 0.1, 0, W / 2, D, 2.3, "metal_galv")            # back panel
        for x in (-W / 2 + 0.05, W / 2 - 0.05):
            for y in (0.05, D - 0.05):
                g.box(x - 0.04, y - 0.04, 0, x + 0.04, y + 0.04, 2.55, "metal_dark")
        # canopy slanting to the front
        g.poly([(-W / 2 - 0.3, -0.8, 2.35), (W / 2 + 0.3, -0.8, 2.35), (W / 2 + 0.3, D + 0.1, 2.75), (-W / 2 - 0.3, D + 0.1, 2.75)],
               (0, -0.2, 1), "awning_red")
        g.poly([(-W / 2 - 0.3, -0.8, 2.35), (W / 2 + 0.3, -0.8, 2.35), (W / 2 + 0.3, D + 0.1, 2.75), (-W / 2 - 0.3, D + 0.1, 2.75)],
               (0, 0.2, -1), "awning_red")
        g.box(-W / 2 - 0.3, -0.82, 2.0, W / 2 + 0.3, -0.78, 2.37, "awning_red")  # valance
        # pots on a burner
        for x in (-0.8, 0.0):
            g.cyl(x, 0.45, 1.06, 1.12, 0.22, "metal_dark", segs=12)
            g.cyl(x, 0.45, 1.12, 1.45, 0.2, "metal_galv", segs=16)
        # menu board (glowing)
        g.box(-1.3, D - 0.14, 1.5, 0.6, D - 0.1, 2.15, "glow_ffd9a0_0.7")
        # shelf with bottles
        g.box(0.8, D - 0.45, 1.5, 1.5, D - 0.1, 1.53, "metal_dark")
        for i in range(5):
            g.cyl(0.88 + i * 0.13, D - 0.28, 1.53, 1.75, 0.04, "glass_dark", segs=8)
    elif name == "drone":
        # quad drone: body, arms, rotors, lights
        g.box(-0.25, -0.18, -0.08, 0.25, 0.18, 0.08, "skin_hull")
        for a in range(4):
            ang = math.pi / 4 + a * math.pi / 2
            x, y = math.cos(ang) * 0.45, math.sin(ang) * 0.45
            g.tube((0, 0, 0), (x, y, 0.02), 0.025, "metal_dark", segs=5)
            g.cyl(x, y, 0.03, 0.05, 0.2, "rubber", segs=12)
        g.box(-0.08, -0.2, -0.04, 0.08, -0.18, 0.02, "led_red")
        g.cyl(0, 0, -0.14, -0.08, 0.07, "glass_dark", segs=10)
    elif name == "cable_set":
        # catenaries between facade anchor points across the street (engine-space pairs given in spec)
        for k, (a, b, sag, r) in enumerate(spec["cables"]):
            a, b = Vector(a), Vector(b)
            n = 16
            prev = None
            for i in range(n + 1):
                t = i / n
                p = a.lerp(b, t)
                p.z -= sag * 4 * t * (1 - t)
                if prev is not None:
                    g.tube(prev, p, r, "rubber", segs=5)
                prev = p
    elif name == "steam_grate":
        g.box(-0.6, -0.4, 0.0, 0.6, 0.4, 0.03, "metal_dark")
        for i in range(9):
            x = -0.5 + i * 0.125
            g.box(x - 0.02, -0.35, 0.03, x + 0.02, 0.35, 0.045, "metal_galv")
    elif name == "umbrella":
        # clear canopy with a glowing shaft (origin at the hand grip)
        ribs = 10
        R, top, edge = 0.55, 1.05, 0.78
        for i in range(ribs):
            a0, a1 = 2 * math.pi * i / ribs, 2 * math.pi * (i + 1) / ribs
            p0 = (math.cos(a0) * R, math.sin(a0) * R, edge)
            p1 = (math.cos(a1) * R, math.sin(a1) * R, edge)
            am = (a0 + a1) / 2
            g.poly([p0, p1, (0, 0, top)], (math.cos(am) * 0.5, math.sin(am) * 0.5, 1), spec.get("canopy", "canopy_clear"))
            g.poly([p0, p1, (0, 0, top)], (-math.cos(am) * 0.5, -math.sin(am) * 0.5, -1), spec.get("canopy", "canopy_clear"))
            g.tube(p0, (0, 0, top), 0.006, "metal_dark", segs=4)
        g.tube((0, 0, 0.0), (0, 0, top + 0.06), 0.012, spec.get("shaft", "led_cyan"), segs=6)
        g.cyl(0, 0, -0.12, 0.0, 0.018, "rubber", segs=8)
    elif name == "holo_ring":
        R, n = spec.get("radius", 3.0), 48
        pts = [(math.cos(2 * math.pi * i / n) * R, math.sin(2 * math.pi * i / n) * R, 0.0) for i in range(n)]
        for i in range(n):
            g.tube(pts[i], pts[(i + 1) % n], spec.get("thickness", 0.05), spec.get("mat", "led_cyan"), segs=6)
    elif name == "paper_lantern":
        segs, rings = 12, 7
        for j in range(rings):
            z0, z1 = -0.25 + 0.5 * j / rings, -0.25 + 0.5 * (j + 1) / rings
            r0 = 0.2 * math.sqrt(max(0.0, 1 - (z0 / 0.27) ** 2)) + 0.03
            r1 = 0.2 * math.sqrt(max(0.0, 1 - (z1 / 0.27) ** 2)) + 0.03
            g.cyl(0, 0, z0, z1, r0, spec.get("paper", "glow_ff3a24_3.0"), segs=segs, caps=False, r1=r1)
        g.cyl(0, 0, 0.25, 0.3, 0.07, "metal_dark", segs=10)
        g.cyl(0, 0, -0.3, -0.25, 0.07, "metal_dark", segs=10)
        g.tube((0, 0, 0.3), (0, 0, 0.7), 0.005, "rubber", segs=4)
    elif name == "hover_car":
        # sleek two-seat hovercar: low wedge body, canopy, light bars (front at -y)
        L, Wd = 4.6, 1.9
        g.box(-Wd / 2, -L / 2 + 0.6, 0.25, Wd / 2, L / 2, 0.75, "skin_hull")
        g.poly([(-Wd / 2, -L / 2, 0.35), (Wd / 2, -L / 2, 0.35), (Wd / 2, -L / 2 + 0.6, 0.75), (-Wd / 2, -L / 2 + 0.6, 0.75)], (0, -1, 1), "skin_hull")
        g.poly([(-Wd / 2, -L / 2, 0.25), (Wd / 2, -L / 2, 0.25), (Wd / 2, -L / 2, 0.35), (-Wd / 2, -L / 2, 0.35)], (0, -1, 0), "skin_hull")
        g.poly([(-Wd / 2, -L / 2, 0.25), (-Wd / 2, -L / 2 + 0.6, 0.25), (-Wd / 2, -L / 2 + 0.6, 0.75), (-Wd / 2, -L / 2, 0.35)], (-1, 0, 0), "skin_hull")
        g.poly([(Wd / 2, -L / 2, 0.25), (Wd / 2, -L / 2 + 0.6, 0.25), (Wd / 2, -L / 2 + 0.6, 0.75), (Wd / 2, -L / 2, 0.35)], (1, 0, 0), "skin_hull")
        g.poly([(-Wd / 2, -L / 2, 0.25), (Wd / 2, -L / 2, 0.25), (Wd / 2, -L / 2 + 0.6, 0.25), (-Wd / 2, -L / 2 + 0.6, 0.25)], (0, 0, -1), "skin_hull")
        # canopy
        g.poly([(-0.75, -0.6, 0.75), (0.75, -0.6, 0.75), (0.6, 0.2, 1.25), (-0.6, 0.2, 1.25)], (0, -0.6, 1), "glass_dark")
        g.poly([(-0.6, 0.2, 1.25), (0.6, 0.2, 1.25), (0.75, 1.3, 0.75), (-0.75, 1.3, 0.75)], (0, 0.6, 1), "glass_dark")
        g.poly([(-0.75, -0.6, 0.75), (-0.6, 0.2, 1.25), (-0.75, 1.3, 0.75)], (-1, 0, 0.3), "glass_dark")
        g.poly([(0.75, -0.6, 0.75), (0.6, 0.2, 1.25), (0.75, 1.3, 0.75)], (1, 0, 0.3), "glass_dark")
        g.box(-Wd / 2 + 0.1, -L / 2 - 0.01, 0.42, Wd / 2 - 0.1, -L / 2 + 0.02, 0.48, "led_white")
        g.box(-Wd / 2 + 0.1, L / 2, 0.55, Wd / 2 - 0.1, L / 2 + 0.03, 0.65, "led_red")
        g.box(-Wd / 2 + 0.2, -L / 2 + 0.8, 0.22, Wd / 2 - 0.2, L / 2 - 0.4, 0.25, spec.get("under", "led_cyan"))
    return g.to_object(name)


# =============================================================================================
# Animated billboard frames (rendered with Cycles: real typography from Blender's font)
# =============================================================================================
def _emit_mat(name, rgb, strength=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    em.inputs["Color"].default_value = tuple(rgb) + (1.0,)
    em.inputs["Strength"].default_value = strength
    nt.links.new(em.outputs[0], out.inputs[0])
    return m


def _fit(ob, max_w, max_h):
    bpy.context.view_layer.update()
    d = ob.dimensions
    s = min(max_w / max(d.x, 1e-3), max_h / max(d.y, 1e-3))
    ob.scale = (s, s, s)


def ad_frames(spec):
    """Renders `frames` PNGs of one ad: dark field with sliding light bars, a headline that
    slides in (with a chromatic glitch near the end), a tagline and a rotating emblem."""
    clear()
    for m in list(bpy.data.materials):
        bpy.data.materials.remove(m)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.samples = 8
    sc.cycles.use_denoising = False
    sc.render.resolution_x, sc.render.resolution_y = spec.get("res", (768, 384))
    sc.view_settings.view_transform = "Standard"
    sc.world = sc.world or bpy.data.worlds.new("w")
    sc.world.use_nodes = True
    sc.world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
    aspect = sc.render.resolution_x / sc.render.resolution_y
    W, H = (2.0 * aspect, 2.0) if aspect >= 1 else (2.0, 2.0 / aspect)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = max(W, H)
    cam.location = (0, 0, 10)
    sc.collection.objects.link(cam)
    sc.camera = cam
    n = spec.get("frames", 12)
    c0, c1 = [tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)) for h in spec["colors"]]
    ink = tuple(int(spec.get("ink", "ffffff")[i:i + 2], 16) / 255 for i in (0, 2, 4))
    vertical = aspect < 1

    def plane(name, w, h, loc, mat, rot=0.0):
        bpy.ops.mesh.primitive_plane_add(size=1, location=loc)
        p = bpy.context.active_object
        p.name = name
        p.scale = (w, h, 1)
        p.rotation_euler = (0, 0, rot)
        p.data.materials.append(mat)
        return p

    plane("bg", W, H, (0, 0, 0), _emit_mat("bg", tuple(c * 0.10 for c in c0), 1.0))
    glow = plane("glow", W * 0.9, H * 0.9, (0, 0, 0.01), _emit_mat("glow", tuple(c * 0.25 for c in c0), 1.0))
    bars = [plane(f"bar{i}", 0.22 + 0.1 * (i % 3), max(W, H) * 2.2, (0, 0, 0.02 + i * 0.001),
                  _emit_mat(f"barm{i}", c1 if i % 2 else c0, 0.55 + 0.25 * (i % 2)), rot=0.5) for i in range(4)]
    for i in range(int(H / 0.035)):  # scanlines
        plane(f"scan{i}", W, 0.008, (0, -H / 2 + i * 0.035, 0.5), _emit_mat("scan", (0, 0, 0), 0.0))
    head = _text_obj(spec["headline"], 0.5)
    tag = _text_obj(spec.get("tagline", ""), 0.5)
    ghost_a = _text_obj(spec["headline"], 0.5)
    ghost_b = _text_obj(spec["headline"], 0.5)
    head.data.materials.append(_emit_mat("ink", ink, 1.2))
    tag.data.materials.append(_emit_mat("ink2", c1, 1.0))
    ghost_a.data.materials.append(_emit_mat("ga", c0, 1.0))
    ghost_b.data.materials.append(_emit_mat("gb", c1, 1.0))
    for ob in (head, ghost_a, ghost_b):
        if vertical:
            ob.rotation_euler = (0, 0, math.pi / 2)
            _fit(ob, H * 0.62, W * 0.42)
        else:
            _fit(ob, W * 0.56, H * 0.36)
    if vertical:
        tag.rotation_euler = (0, 0, math.pi / 2)
        _fit(tag, H * 0.5, W * 0.12)
    else:
        _fit(tag, W * 0.42, H * 0.1)
    emblem = []
    for i in range(spec.get("dots", 12)):
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.045, segments=12, ring_count=6)
        d = bpy.context.active_object
        d.data.materials.append(_emit_mat(f"dot{i}", ink if i % 3 == 0 else c1, 1.3))
        emblem.append(d)
    for f in range(n):
        t = f / n
        slide = (1 - min(1.0, t * 3.0)) ** 2
        for i, bar in enumerate(bars):
            off = ((t * (1.6 + 0.4 * i) + i * 0.29) % 1.0) * (W + 2.0) - (W / 2 + 1.0)
            bar.location = (off, 0, 0.02 + i * 0.001)
        glow.scale = (W * (0.85 + 0.1 * math.sin(t * 6.283)), H * 0.9, 1)
        if vertical:
            hx, hy = -W * 0.14 - slide * 3, H * 0.08
            head.location = (hx, hy, 1)
            tag.location = (W * 0.3, H * 0.08, 1)
            ex, ey, rad = 0.0, -H / 2 + 0.55, 0.38
        else:
            hx, hy = -W * 0.12 - slide * 4, H * 0.1
            head.location = (hx, hy, 1)
            tag.location = (-W * 0.12, -H * 0.3, 1)
            ex, ey, rad = W / 2 - 0.62, 0.0, 0.45
        glitch = f == n - 2 or f == n // 2
        ghost_a.location = (hx - 0.03, hy + 0.01, 0.99) if glitch else (0, 0, -5)
        ghost_b.location = (hx + 0.03, hy - 0.01, 0.98) if glitch else (0, 0, -5)
        for i, d in enumerate(emblem):
            a = 2 * math.pi * (i / len(emblem) + t)
            if spec.get("emblem", "ring") == "helix":
                d.location = (ex + math.cos(a) * rad * 0.5, ey + (i / len(emblem) - 0.5) * rad * 2.2, 1 + math.sin(a))
                d.scale = ((1.2 + 0.6 * math.sin(a)),) * 3
            else:
                d.location = (ex + math.cos(a) * rad, ey + math.sin(a) * rad, 1)
                d.scale = ((1.0 + 0.6 * (i == int(t * len(emblem)))),) * 3
        sc.render.filepath = os.path.join(OUT, f"ad_{spec['name']}_{f:02d}.png")
        bpy.ops.render.render(write_still=True)


# =============================================================================================
# Main
# =============================================================================================
def export(objs, fname):
    path = sky.out_path(fname)  # noqa: F821
    B.export_glb(path, objects=objs)
    return path


def clear():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for me in list(bpy.data.meshes):
        bpy.data.meshes.remove(me)
    for cu in list(bpy.data.curves):
        bpy.data.curves.remove(cu)


B.reset_scene()
os.makedirs(OUT, exist_ok=True)
all_anchors = {}
if JOB.get("textures", True) or not os.path.exists(os.path.join(OUT, "window_atlas.png")):
    window_atlas()
    skyline_atlas()
else:
    # rebuild the cell table deterministically without rewriting the PNG
    _save = save_png
    save_png = lambda *a, **k: None  # noqa: E731
    window_atlas()
    save_png = _save
for spec in JOB.get("buildings", []):
    clear()
    ob, anchors = building(spec)
    export([ob], spec["name"] + ".glb")
    all_anchors[spec["name"]] = anchors
for spec in JOB.get("skyline", []):
    clear()
    ob, anchors = skyline_tower(spec)
    export([ob], spec["name"] + ".glb")
    all_anchors[spec["name"]] = anchors
for spec in JOB.get("skybridges", []):
    clear()
    export([skybridge(spec)], spec["name"] + ".glb")
for spec in JOB.get("monorail", []):
    clear()
    export([monorail_track(spec) if spec.get("kind") == "track" else train_car(spec)], spec["name"] + ".glb")
for spec in JOB.get("signs", []):
    clear()
    export(sign(spec), spec["name"] + ".glb")
for spec in JOB.get("props", []):
    clear()
    export([prop(spec["kind"], spec)], spec["name"] + ".glb")
for spec in JOB.get("ads", []):
    ad_frames(spec)
with open(os.path.join(OUT, "anchors.json"), "w") as f:
    json.dump(all_anchors, f)
sky.result(models=len(all_anchors))  # noqa: F821
