# SPDX-License-Identifier: GPL-3.0-or-later
# (runs inside Blender and uses its Python API, see docs/LICENSING.md)
"""Meridian Accord — map miniatures modeled in Blender (run through `dcc_run_script`).

sky.ARGS[0] is a JSON job:
  {"towns": [{name, kind: town|city|capital, seed}], "block": true, "flag": true, "trees": true,
   "arrows": [{name, points: [[x, y, z] engine coords], width, head, thickness}]}
Every model is written to SKY_OUT as its own .glb (meters; origin at the bottom center for
miniatures; arrows keep world coordinates so they drop in at the origin).
"""
import json
import math
import random

import bmesh
import bpy
from mathutils import Vector

from skywalker_dcc import blender as B

JOB = json.loads(sky.ARGS[0]) if sky.ARGS else {}  # noqa: F821  (sky is injected by the bootstrap)


MATS = {}


def mat(name, color, roughness=0.8, metallic=0.0, vcol=True):
    """A material spec; faces tint it with their color (one Blender material per distinct tint, so
    the variation survives glTF without vertex colors). Colors are sRGB 0..255."""
    return (name, tuple(color), roughness, metallic)


def _material(spec, col):
    name, base, rough, metal = spec
    c = tuple(min(1.0, base[i] / 255.0 * col[i]) for i in range(3))
    q = tuple(round(v * 40) / 40 for v in c)
    key = f"{name}_{int(q[0] * 40):02d}{int(q[1] * 40):02d}{int(q[2] * 40):02d}"
    if key not in MATS:
        MATS[key] = B.make_material(key, color=q, roughness=rough, metallic=metal)
    return MATS[key]


class Geo:
    """One mesh object built face by face, with a per-face material (spec x tint)."""

    def __init__(self, name):
        self.name = name
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.mats = []

    def _mi(self, m, col):
        bm = _material(m, col)
        if bm not in self.mats:
            self.mats.append(bm)
        return self.mats.index(bm)

    def face(self, pts, m, col=(1, 1, 1), uvs=None):
        vs = [self.bm.verts.new(Vector(p)) for p in pts]
        try:
            f = self.bm.faces.new(vs)
        except ValueError:
            return None
        f.material_index = self._mi(m, col)
        for i, lp in enumerate(f.loops):
            if uvs:
                lp[self.uv].uv = uvs[i]
            else:
                co = lp.vert.co
                lp[self.uv].uv = (co.x * 0.5 + co.z * 0.3, co.y * 0.5 + co.z * 0.3)
        return f

    def box(self, x0, y0, z0, x1, y1, z1, m, col=(1, 1, 1), rot=0.0, cx=0.0, cy=0.0, bottom=False):
        c, s = math.cos(rot), math.sin(rot)

        def P(x, y, z):
            return (cx + (x * c - y * s), cy + (x * s + y * c), z)
        v = [P(x0, y0, z0), P(x1, y0, z0), P(x1, y1, z0), P(x0, y1, z0), P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)]
        quads = [(4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        if bottom:
            quads.append((3, 2, 1, 0))
        for q in quads:
            self.face([v[i] for i in q], m, col)

    def gable(self, x0, y0, x1, y1, z, h, m, col, rot=0.0, cx=0.0, cy=0.0, over=0.25, wall=None, wcol=None):
        """A pitched roof along x (ridge at y = mid), eaves overhang `over`; gable ends filled with `wall`."""
        c, s = math.cos(rot), math.sin(rot)

        def P(x, y, zz):
            return (cx + (x * c - y * s), cy + (x * s + y * c), zz)
        ym = 0.5 * (y0 + y1)
        a0, a1, b0, b1 = x0 - over, x1 + over, y0 - over, y1 + over
        zr = z + h
        ze = z - over * h / max(ym - y0, 1e-3)
        self.face([P(a0, b0, ze), P(a1, b0, ze), P(a1, ym, zr), P(a0, ym, zr)], m, col)
        self.face([P(a1, b1, ze), P(a0, b1, ze), P(a0, ym, zr), P(a1, ym, zr)], m, col)
        # underside of the eaves (dark) so the roof reads as a thin slab from below
        self.face([P(a0, ym, zr - 0.08), P(a1, ym, zr - 0.08), P(a1, b0, ze - 0.08), P(a0, b0, ze - 0.08)], m, tuple(v * 0.35 for v in col))
        self.face([P(a1, ym, zr - 0.08), P(a0, ym, zr - 0.08), P(a0, b1, ze - 0.08), P(a1, b1, ze - 0.08)], m, tuple(v * 0.35 for v in col))
        if wall:
            self.face([P(x0, y1, z), P(x0, y0, z), P(x0, ym, zr - 0.05)], wall, wcol)
            self.face([P(x1, y0, z), P(x1, y1, z), P(x1, ym, zr - 0.05)], wall, wcol)

    def hip(self, x0, y0, x1, y1, z, h, m, col, rot=0.0, cx=0.0, cy=0.0, over=0.2):
        c, s = math.cos(rot), math.sin(rot)

        def P(x, y, zz):
            return (cx + (x * c - y * s), cy + (x * s + y * c), zz)
        a0, a1, b0, b1 = x0 - over, x1 + over, y0 - over, y1 + over
        inset = min(x1 - x0, y1 - y0) * 0.5
        r0, r1 = (a0 + inset, a1 - inset) if (x1 - x0) > (y1 - y0) else ((a0 + a1) / 2, (a0 + a1) / 2)
        ym = 0.5 * (b0 + b1)
        zr = z + h
        if (x1 - x0) <= (y1 - y0):
            xm = 0.5 * (a0 + a1)
            q0, q1 = b0 + inset, b1 - inset
            self.face([P(a0, b0, z), P(a1, b0, z), P(xm, q0, zr)], m, col)
            self.face([P(a1, b1, z), P(a0, b1, z), P(xm, q1, zr)], m, col)
            self.face([P(a1, b0, z), P(a1, b1, z), P(xm, q1, zr), P(xm, q0, zr)], m, col)
            self.face([P(a0, b1, z), P(a0, b0, z), P(xm, q0, zr), P(xm, q1, zr)], m, col)
        else:
            self.face([P(a0, b0, z), P(a1, b0, z), P(r1, ym, zr), P(r0, ym, zr)], m, col)
            self.face([P(a1, b1, z), P(a0, b1, z), P(r0, ym, zr), P(r1, ym, zr)], m, col)
            self.face([P(a1, b0, z), P(a1, b1, z), P(r1, ym, zr)], m, col)
            self.face([P(a0, b1, z), P(a0, b0, z), P(r0, ym, zr)], m, col)

    def cyl(self, cx, cy, z0, z1, r, m, col, n=16, cap=True):
        ring0 = [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
        for i in range(n):
            a, b = ring0[i], ring0[(i + 1) % n]
            self.face([(a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)], m, col)
        if cap:
            self.face([(p[0], p[1], z1) for p in ring0], m, col)

    def cone(self, cx, cy, z0, z1, r, m, col, n=12, r_top=0.0):
        for i in range(n):
            a0 = 2 * math.pi * i / n
            a1 = 2 * math.pi * (i + 1) / n
            p0 = (cx + r * math.cos(a0), cy + r * math.sin(a0), z0)
            p1 = (cx + r * math.cos(a1), cy + r * math.sin(a1), z0)
            if r_top > 0:
                self.face([p0, p1, (cx + r_top * math.cos(a1), cy + r_top * math.sin(a1), z1),
                           (cx + r_top * math.cos(a0), cy + r_top * math.sin(a0), z1)], m, col)
            else:
                self.face([p0, p1, (cx, cy, z1)], m, col)
        self.face([(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n), z0) for i in reversed(range(n))],
                  m, col)

    def dome(self, cx, cy, z0, r, m, col, n=20, rings=7, squash=1.0):
        pts = []
        for j in range(rings + 1):
            t = (math.pi / 2) * j / rings
            pts.append([(cx + r * math.cos(t) * math.cos(2 * math.pi * i / n), cy + r * math.cos(t) * math.sin(2 * math.pi * i / n),
                         z0 + r * squash * math.sin(t)) for i in range(n)])
        for j in range(rings):
            for i in range(n):
                a, b = pts[j][i], pts[j][(i + 1) % n]
                c2, d = pts[j + 1][(i + 1) % n], pts[j + 1][i]
                if j == rings - 1:
                    self.face([a, b, (cx, cy, z0 + r * squash)], m, col)
                else:
                    self.face([a, b, c2, d], m, col)

    def finish(self, smooth=False):
        me = bpy.data.meshes.new(self.name)
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=0.002)
        bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces)
        self.bm.to_mesh(me)
        self.bm.free()
        ob = bpy.data.objects.new(self.name, me)
        bpy.context.scene.collection.objects.link(ob)
        for m in self.mats:
            ob.data.materials.append(m)
        if smooth:
            for p in me.polygons:
                p.use_smooth = True
        return ob


def export(objs, name):
    # Only these objects live in the scene while exporting (everything else was removed), so the
    # exporter can take the whole scene.
    bpy.context.view_layer.update()
    B.export_glb(sky.out_path(f"{name}.glb"))  # noqa: F821
    for o in objs:
        bpy.data.objects.remove(o, do_unlink=True)


# ============================================================================================
# Towns: plaster houses with tile and slate roofs, a church, and for capitals a domed palace
# ============================================================================================
WALLS = [(236, 226, 204), (226, 208, 172), (214, 196, 168), (240, 234, 222), (206, 184, 150), (222, 214, 196), (198, 176, 140)]
TILES = [(150, 70, 48), (166, 84, 56), (132, 60, 44), (176, 100, 66), (120, 66, 52)]
SLATE = [(84, 88, 96), (98, 100, 108), (74, 78, 88)]


def town(spec):
    rng = random.Random(spec.get("seed", 1))
    kind = spec["kind"]
    R = {"town": 13.0, "city": 21.0, "capital": 30.0}[kind]
    g = Geo(spec["name"])
    plaster = mat("plaster", (255, 255, 255), 0.85)
    roof = mat("roof_tile", (255, 255, 255), 0.7)
    stone = mat("stone", (200, 192, 176), 0.8)
    copper = mat("copper", (255, 255, 255), 0.45, 0.3)
    gold = mat("gilt", (230, 190, 100), 0.3, 1.0, vcol=False)
    buried = -4.0
    occupied = []

    def free(x, y, r):
        return all((x - a) ** 2 + (y - b) ** 2 > (r + c) ** 2 for a, b, c in occupied)

    # Landmark at the center.
    if kind == "capital":
        # Palace: a long block around a court, a drum and a dome, corner pavilions.
        w, d, h = 18.0, 12.0, 6.0
        g.box(-w / 2, -d / 2, buried, w / 2, -d / 2 + 3.2, h, stone, (1, 1, 1))
        g.box(-w / 2, d / 2 - 3.2, buried, w / 2, d / 2, h, stone, (1, 1, 1))
        g.box(-w / 2, -d / 2, buried, -w / 2 + 3.2, d / 2, h, stone, (1, 1, 1))
        g.box(w / 2 - 3.2, -d / 2, buried, w / 2, d / 2, h, stone, (1, 1, 1))
        for (x0, y0, x1, y1) in ((-w / 2, -d / 2, w / 2, -d / 2 + 3.2), (-w / 2, d / 2 - 3.2, w / 2, d / 2)):
            g.gable(x0, y0, x1, y1, h, 1.8, roof, (0.33, 0.35, 0.38), over=0.3)
        for (x0, y0, x1, y1) in ((-w / 2, -d / 2, -w / 2 + 3.2, d / 2), (w / 2 - 3.2, -d / 2, w / 2, d / 2)):
            g.gable(-d / 2, x0, d / 2, x1, h, 1.8, roof, (0.33, 0.35, 0.38), rot=math.pi / 2, over=0.3)
        for sx in (-1, 1):
            for sy in (-1, 1):
                g.box(sx * w / 2 - 2.3, sy * d / 2 - 2.3, buried, sx * w / 2 + 2.3, sy * d / 2 + 2.3, h + 1.6, stone, (0.96, 0.95, 0.92))
                g.hip(sx * w / 2 - 2.3, sy * d / 2 - 2.3, sx * w / 2 + 2.3, sy * d / 2 + 2.3, h + 1.6, 2.4, copper, (0.42, 0.66, 0.58))
        g.cyl(0, 0, h - 1, h + 4.5, 4.4, stone, (1, 1, 1), n=24)
        for i in range(16):  # drum columns
            a = 2 * math.pi * i / 16
            g.box(-0.3, -0.3, h + 0.2, 0.3, 0.3, h + 4.2, stone, (0.85, 0.83, 0.8), cx=4.65 * math.cos(a), cy=4.65 * math.sin(a))
        g.dome(0, 0, h + 4.5, 4.9, copper, (0.45, 0.70, 0.62), squash=1.15)
        g.cyl(0, 0, h + 4.5 + 5.6, h + 4.5 + 7.2, 0.9, stone, (1, 1, 1), n=10)
        g.cone(0, 0, h + 4.5 + 7.2, h + 4.5 + 9.6, 0.95, gold, (1, 1, 1), n=10)
        occupied.append((0, 0, 12.0))
        # A tall clock tower.
        tx, ty = 15.0, -9.0
        g.box(-1.8, -1.8, buried, 1.8, 1.8, 17.0, stone, (0.92, 0.9, 0.86), cx=tx, cy=ty)
        g.box(-2.1, -2.1, 17.0, 2.1, 2.1, 18.0, stone, (0.8, 0.78, 0.74), cx=tx, cy=ty)
        g.cone(tx, ty, 18.0, 24.5, 2.0, roof, (0.30, 0.33, 0.36), n=4)
        occupied.append((tx, ty, 3.5))
    else:
        # Church: nave with a gable roof and a west tower with a spire.
        L, W, H = (11.0, 5.0, 5.5) if kind == "city" else (8.0, 4.0, 4.2)
        rot = rng.uniform(0, math.pi)
        g.box(-L / 2, -W / 2, buried, L / 2, W / 2, H, stone, (0.95, 0.93, 0.88), rot=rot)
        g.gable(-L / 2, -W / 2, L / 2, W / 2, H, W * 0.75, roof, (0.30, 0.33, 0.37), rot=rot, over=0.25, wall=stone, wcol=(0.95, 0.93, 0.88))
        tw = W * 0.55
        tx, ty = -L / 2 - tw * 0.5, 0.0
        c, s = math.cos(rot), math.sin(rot)
        px, py = tx * c, tx * s
        th = H * (2.3 if kind == "city" else 2.0)
        g.box(-tw / 2, -tw / 2, buried, tw / 2, tw / 2, th, stone, (0.92, 0.9, 0.85), rot=rot, cx=px, cy=py)
        g.cone(px, py, th, th + tw * 2.6, tw * 0.62, roof, (0.30, 0.33, 0.37), n=8)
        occupied.append((0, 0, L * 0.62))
        if kind == "city":
            # a market hall and a city gate tower
            g.box(-3, -2, buried, 3, 2, 4.0, stone, (0.9, 0.86, 0.78), cx=-9, cy=8)
            g.hip(-3, -2, 3, 2, 4.0, 2.2, roof, (0.6, 0.3, 0.2), cx=-9, cy=8)
            occupied.append((-9, 8, 4.2))

    # Houses in rings with radial streets.
    n_rings = {"town": 3, "city": 5, "capital": 6}[kind]
    streets = rng.randint(3, 5)
    st_off = rng.uniform(0, math.pi)
    for ring in range(n_rings):
        r = (0.32 + 0.68 * (ring + 0.5) / n_rings) * R
        n = int(2 * math.pi * r / rng.uniform(5.2, 6.4))
        for i in range(n):
            a = 2 * math.pi * (i + rng.uniform(-0.25, 0.25)) / n
            # leave radial streets open
            if min(abs(((a - st_off) * streets / (2 * math.pi)) % 1.0 - 0.5) for _ in (0,)) > 0.42:
                continue
            rr = r + rng.uniform(-1.0, 1.0)
            x, y = rr * math.cos(a), rr * math.sin(a)
            fall = 1.0 - 0.55 * (rr / R)
            w = rng.uniform(3.2, 5.2)
            d = rng.uniform(2.6, 3.6)
            if not free(x, y, max(w, d) * 0.5):
                continue
            h = rng.uniform(2.6, 4.4) * (0.75 + 0.6 * fall) * (1.25 if kind == "capital" else 1.0)
            rot = a + math.pi / 2 + rng.uniform(-0.15, 0.15)
            wc = [v / 255 for v in rng.choice(WALLS)]
            if kind == "capital" and rng.random() < 0.35:
                rc = [v / 255 for v in rng.choice(SLATE)]
            else:
                rc = [v / 255 for v in rng.choice(TILES)]
            g.box(-w / 2, -d / 2, buried, w / 2, d / 2, h, plaster, wc, rot=rot, cx=x, cy=y)
            if rng.random() < 0.8:
                g.gable(-w / 2, -d / 2, w / 2, d / 2, h, d * rng.uniform(0.5, 0.7), roof, rc, rot=rot, cx=x, cy=y, wall=plaster, wcol=wc)
            else:
                g.hip(-w / 2, -d / 2, w / 2, d / 2, h, d * 0.5, roof, rc, rot=rot, cx=x, cy=y)
            occupied.append((x, y, max(w, d) * 0.5))
    # A few trees in the gaps (round crowns) give the miniature its model-railway charm.
    leaf = mat("leaf", (255, 255, 255), 0.9)
    for _ in range(int(R * 0.9)):
        a = rng.uniform(0, 2 * math.pi)
        rr = rng.uniform(0.3, 1.05) * R
        x, y = rr * math.cos(a), rr * math.sin(a)
        if not free(x, y, 1.1):
            continue
        cr = rng.uniform(1.0, 1.6)
        gc = rng.choice([(0.25, 0.38, 0.17), (0.30, 0.42, 0.2), (0.22, 0.33, 0.16)])
        g.cyl(x, y, buried, 1.2, 0.18, leaf, (0.3, 0.22, 0.15), n=5, cap=False)
        g.dome(x, y, 0.8, cr, leaf, gc, n=8, rings=3, squash=1.6)
        occupied.append((x, y, cr))
    return g.finish()


# ============================================================================================
# Unit block, flag, trees
# ============================================================================================
def block():
    bpy.ops.mesh.primitive_cube_add(size=1.0)
    ob = bpy.context.active_object
    ob.name = "unit_block"
    ob.scale = (15.0, 6.0, 11.0)     # x = width, y = depth (glTF z), z = height
    ob.location = (0, 0, 5.5)
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=True)
    m = ob.modifiers.new("bevel", "BEVEL")
    m.width = 0.7
    m.segments = 3
    m.limit_method = "ANGLE"
    ob.data.materials.append(_material(mat("block_wood", (196, 160, 112), 0.55), (1, 1, 1)))
    bpy.ops.object.shade_smooth()
    return ob


def flag():
    pole = Geo("flag_pole")
    brass = mat("brass", (214, 176, 96), 0.3, 1.0, vcol=False)
    wood = mat("pole_wood", (70, 52, 38), 0.6, vcol=False)
    pole.cyl(0, 0, -2.0, 30.0, 0.32, wood, (1, 1, 1), n=10)
    pole.dome(0, 0, 30.0, 0.65, brass, (1, 1, 1), n=10, rings=4, squash=1.4)
    pole.cyl(0, 0, -2.0, 0.6, 1.4, brass, (0.6, 0.6, 0.6), n=12)
    p = pole.finish(smooth=True)
    export([p], "flag_pole")
    # The cloth: 14 x 9.3 m, rippling more toward the fly end; UV (0,0) = bottom left.
    cloth = Geo("flag_cloth")
    cm = mat("flag_cloth", (255, 255, 255), 0.85, vcol=False)
    nx, nz = 28, 14
    W, H, z0 = 14.0, 9.3, 20.0

    def P(i, j):
        u, v = i / nx, j / nz
        x = 0.35 + u * W
        y = math.sin(u * 7.0 + v * 0.8) * 0.75 * u ** 0.8 + math.sin(u * 13.0) * 0.18 * u
        z = z0 + v * H - u * u * 0.9
        return (x, y, z)
    for i in range(nx):
        for j in range(nz):
            cloth.face([P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)], cm, (1, 1, 1),
                       uvs=[(i / nx, j / nz), ((i + 1) / nx, j / nz), ((i + 1) / nx, (j + 1) / nz), (i / nx, (j + 1) / nz)])
    c = cloth.finish(smooth=True)
    export([c], "flag_cloth")


def trees():
    g = Geo("tree_conifer")
    leaf = mat("conifer", (60, 92, 58), 0.9, vcol=False)
    bark = mat("bark", (70, 52, 40), 0.9, vcol=False)
    g.cyl(0, 0, -0.2, 0.25, 0.05, bark, (1, 1, 1), n=5, cap=False)
    g.cone(0, 0, 0.18, 0.75, 0.32, leaf, (1, 1, 1), n=7)
    g.cone(0, 0, 0.48, 1.0, 0.24, leaf, (1, 1, 1), n=7)
    export([g.finish()], "tree_conifer")
    g = Geo("tree_broadleaf")
    leaf = mat("broadleaf", (78, 104, 52), 0.9, vcol=False)
    g.cyl(0, 0, -0.2, 0.35, 0.06, bark, (1, 1, 1), n=5, cap=False)
    g.dome(0, 0, 0.25, 0.42, leaf, (1, 1, 1), n=8, rings=3, squash=1.5)
    g.cone(0, 0, 0.25, 0.26, 0.42, leaf, (1, 1, 1), n=8)
    export([g.finish()], "tree_broadleaf")


# ============================================================================================
# Arrows: a swept, beveled ribbon along a path with a broad arrowhead (world coordinates)
# ============================================================================================
def catmull(pts, n_per):
    out = []
    P = [pts[0]] + pts + [pts[-1]]
    for k in range(len(pts) - 1):
        p0, p1, p2, p3 = (Vector(P[k + i]) for i in range(4))
        for s in range(n_per):
            t = s / n_per
            out.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    out.append(Vector(pts[-1]))
    return out


def arrow(spec):
    # engine (X, Y, Z) -> Blender (X, -Z, Y)
    pts = [(p[0], -p[2], p[1]) for p in spec["points"]]
    path = catmull(pts, 14)
    # resample by arc length
    L = [0.0]
    for a, b in zip(path[:-1], path[1:]):
        L.append(L[-1] + (b - a).length)
    total = L[-1]
    head = min(spec.get("head", 60.0), total * 0.4)
    W = spec.get("width", 22.0)
    T = spec.get("thickness", 2.2)
    n = 90
    samples = []
    j = 0
    for i in range(n + 1):
        s = total * i / n
        while j < len(L) - 2 and L[j + 1] < s:
            j += 1
        t = (s - L[j]) / max(L[j + 1] - L[j], 1e-6)
        samples.append((s, path[j].lerp(path[j + 1], t)))
    g = Geo(spec["name"])
    m = mat("arrow", (255, 255, 255), 0.35, vcol=True)
    body_end = total - head
    rows = []
    for k, (s, p) in enumerate(samples):
        a = samples[max(k - 1, 0)][1]
        b = samples[min(k + 1, n)][1]
        d = Vector((b.x - a.x, b.y - a.y, 0)).normalized()
        side = Vector((-d.y, d.x, 0))
        if s <= body_end:
            u = s / max(body_end, 1e-3)
            w = W * (0.55 + 0.45 * u)                 # tapered tail
        else:
            u = (s - body_end) / head
            w = W * 2.3 * (1 - u) + 0.01              # arrowhead
        rows.append((s, p, side, w))
    # insert the head's shoulder: a hard step from the shaft to the head base
    prev = None
    for (s, p, side, w) in rows:
        top_l = p + side * w * 0.5 + Vector((0, 0, T * 0.5))
        top_r = p - side * w * 0.5 + Vector((0, 0, T * 0.5))
        bot_l = p + side * w * 0.5 - Vector((0, 0, T * 0.5))
        bot_r = p - side * w * 0.5 - Vector((0, 0, T * 0.5))
        cur = (top_l, top_r, bot_l, bot_r, s)
        if prev:
            pl, pr, pbl, pbr, ps = prev
            # fade the tail (vertex color alpha-ish via darkening) and brighten toward the head
            g.face([pl, top_l, top_r, pr], m)
            g.face([pl, pbl, bot_l, top_l], m)
            g.face([pr, top_r, bot_r, pbr], m)
            g.face([pbl, pbr, bot_r, bot_l], m)
        prev = cur
    # the head base (shoulders): connect shaft width to head width at body_end
    for k in range(1, len(rows)):
        if rows[k - 1][0] <= body_end < rows[k][0] or (rows[k - 1][0] < body_end and rows[k][0] >= body_end):
            s, p, side, _ = rows[k]
            ws, wh = W, W * 2.3
            for sg in (1, -1):
                a = p + side * sg * ws * 0.5
                b = p + side * sg * wh * 0.5
                g.face([a + Vector((0, 0, T / 2)), b + Vector((0, 0, T / 2)), b - Vector((0, 0, T / 2)), a - Vector((0, 0, T / 2))], m)
            break
    # tail cap
    s, p, side, w = rows[0]
    g.face([p + side * w / 2 + Vector((0, 0, T / 2)), p - side * w / 2 + Vector((0, 0, T / 2)),
            p - side * w / 2 - Vector((0, 0, T / 2)), p + side * w / 2 - Vector((0, 0, T / 2))], m)
    ob = g.finish()
    for p in ob.data.polygons:
        p.use_smooth = False
    return ob


B.reset_scene()
made = []
for spec in JOB.get("towns", []):
    export([town(spec)], spec["name"])
    made.append(spec["name"])
if JOB.get("block"):
    export([block()], "unit_block")
    made.append("unit_block")
if JOB.get("flag"):
    flag()
    made += ["flag_pole", "flag_cloth"]
if JOB.get("trees"):
    trees()
    made += ["tree_conifer", "tree_broadleaf"]
for spec in JOB.get("arrows", []):
    export([arrow(spec)], spec["name"])
    made.append(spec["name"])
sky.result(models=made)  # noqa: F821
