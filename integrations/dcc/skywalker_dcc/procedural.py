# SPDX-License-Identifier: GPL-3.0-or-later
# (code that runs inside Blender and uses its Python API follows Blender's license, see docs/LICENSING.md)
"""Procedural modeling library (runs inside Blender).

Each recipe builds one mesh object with PBR materials and per-face color variation, sized in
meters, standing on the ground plane with its origin at the bottom center (x/y centered, z=0),
so it can be dropped into a Skywalker scene as-is. Recipes are deterministic for a given
`seed`.

    from skywalker_dcc import procedural
    tower = procedural.generate("tower", height=14, ruin=0.6, seed=3)
    procedural.describe()          # every recipe with its parameters and defaults

Recipes: building, tower, wall, rock, stairs, arch, fence, column, barrel, terrain_chunk.
Combine them in your own script (move/rotate the returned objects) to compose a scene piece.
"""

import inspect
import math
import random

import bmesh
import bpy
from mathutils import Matrix, Vector, noise

from skywalker_dcc import blender as B

# ---------------------------------------------------------------------------
# Building blocks
# ---------------------------------------------------------------------------

_PALETTES = {
    "stone": (0.46, 0.44, 0.40),
    "weathered_stone": (0.38, 0.37, 0.34),
    "brick": (0.52, 0.24, 0.17),
    "plaster": (0.80, 0.74, 0.63),
    "wood": (0.40, 0.27, 0.16),
    "dark_wood": (0.22, 0.14, 0.09),
    "slate": (0.22, 0.25, 0.29),
    "terracotta": (0.62, 0.28, 0.17),
    "moss": (0.20, 0.30, 0.12),
    "iron": (0.12, 0.12, 0.13),
}


def _tint(rgb, rng, amount=0.12):
    f = 1.0 + rng.uniform(-amount, amount)
    return tuple(max(0.0, min(1.0, c * f)) for c in rgb)


def _mix(a, b, t):
    return tuple(a[i] * (1 - t) + b[i] * t for i in range(3))


class Builder:
    """Accumulates geometry in one bmesh with material slots and per-face colors."""

    def __init__(self):
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new("Col")
        self.materials = []

    def mat(self, material):
        if material not in self.materials:
            self.materials.append(material)
        return self.materials.index(material)

    def face(self, verts, mat_index, color):
        try:
            f = self.bm.faces.new(verts)
        except ValueError:  # duplicate face (shared quads between touching solids)
            return None
        f.material_index = mat_index
        for loop in f.loops:
            loop[self.col] = (color[0], color[1], color[2], 1.0)
        return f

    def hexa(self, c, mat, color, rng=None, jitter=0.0):
        """A hexahedron from 8 corners: 4 bottom (counter-clockwise from above), then 4 top."""
        if rng is not None and jitter:
            c = [Vector(p) + Vector((rng.uniform(-jitter, jitter), rng.uniform(-jitter, jitter), rng.uniform(-jitter, jitter))) for p in c]
        v = [self.bm.verts.new(Vector(p)) for p in c]
        mi = self.mat(mat)
        for idx in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
            self.face([v[i] for i in idx], mi, color)

    def box(self, lo, hi, mat, color, rng=None, jitter=0.0):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        self.hexa([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                   (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], mat, color, rng, jitter)

    def gable(self, axis, a0, a1, p_left, p_right, p_ridge, z_eave, z_ridge, mat, color):
        """Triangular prism (gable roof body). `axis` is the ridge direction ('x' or 'y'); a0..a1 is the
        extent along the ridge, p_* positions across it. The underside is closed at z_eave."""
        def pt(a, p, z):
            return (a, p, z) if axis == "x" else (p, a, z)
        pts = [pt(a0, p_left, z_eave), pt(a0, p_right, z_eave), pt(a0, p_ridge, z_ridge),
               pt(a1, p_left, z_eave), pt(a1, p_right, z_eave), pt(a1, p_ridge, z_ridge)]
        v = [self.bm.verts.new(p) for p in pts]
        mi = self.mat(mat)
        self.face([v[0], v[2], v[1]], mi, color)
        self.face([v[3], v[4], v[5]], mi, color)
        self.face([v[0], v[1], v[4], v[3]], mi, color)
        self.face([v[0], v[3], v[5], v[2]], mi, color)
        self.face([v[1], v[2], v[5], v[4]], mi, color)

    def slab(self, points_xy, z0, z1, mat, color):
        """A closed prism from a convex polygon (x, y) extruded from z0 to z1."""
        lo = [self.bm.verts.new((x, y, z0)) for x, y in points_xy]
        hi = [self.bm.verts.new((x, y, z1)) for x, y in points_xy]
        mi = self.mat(mat)
        self.face(lo[::-1], mi, color)
        self.face(hi, mi, color)
        n = len(lo)
        for i in range(n):
            j = (i + 1) % n
            self.face([lo[i], lo[j], hi[j], hi[i]], mi, color)

    def finish(self, name, origin_bottom=True, recalc=True):
        if recalc:  # closed solids only: open surfaces keep the winding they were built with
            bmesh.ops.recalc_face_normals(self.bm, faces=self.bm.faces[:])
        if origin_bottom and self.bm.verts:
            xs = [v.co.x for v in self.bm.verts]
            ys = [v.co.y for v in self.bm.verts]
            zs = [v.co.z for v in self.bm.verts]
            off = Vector(((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2, min(zs)))
            bmesh.ops.translate(self.bm, vec=-off, verts=self.bm.verts[:])
        me = bpy.data.meshes.new(name)
        self.bm.to_mesh(me)
        self.bm.free()
        for m in self.materials:
            me.materials.append(m)
        if len(me.color_attributes):
            me.color_attributes.active_color = me.color_attributes[0]
            me.color_attributes.render_color_index = 0
        obj = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(obj)
        return obj


def _pbr(name, key, roughness=0.85, metallic=0.0, vertex_colors=True, color=None, **kw):
    return B.make_material(name, color or _PALETTES[key], roughness=roughness, metallic=metallic,
                           vertex_colors=vertex_colors, **kw)


def _fbm(p, seed, octaves=4):
    """Deterministic fractal noise in roughly [-1, 1]."""
    return noise.fractal(p + Vector((seed * 13.37, seed * 7.13, seed * 3.71)), 1.0, 2.0, octaves)


def _smoothstep(a, b, x):
    t = max(0.0, min(1.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def _add_blob(b, center, radius, rng, mat, color, subdivisions=1):
    """A small lumpy rock added to an existing Builder (rubble)."""
    g = bmesh.ops.create_icosphere(b.bm, subdivisions=subdivisions, radius=radius)
    mi = b.mat(mat)
    verts = g["verts"]
    for v in verts:
        v.co = Vector((v.co.x * rng.uniform(0.8, 1.25), v.co.y * rng.uniform(0.8, 1.25), v.co.z * rng.uniform(0.5, 0.8)))
        v.co += Vector(center) + Vector((0, 0, radius * 0.4))
        v.co.z = max(v.co.z, 0.0)  # rubble rests on the ground, never under it
    faces = {f for v in verts for f in v.link_faces}
    c = _tint(color, rng, 0.2)
    for f in faces:
        f.material_index = mi
        for loop in f.loops:
            loop[b.col] = (c[0], c[1], c[2], 1.0)


# ---------------------------------------------------------------------------
# Recipes
# ---------------------------------------------------------------------------

def rock(name="Rock", radius=1.0, roughness=0.45, flatten=0.75, detail=4, moss=0.0, seed=0):
    """A boulder: noisy faceted stone sitting flat on the ground. `moss` (0-1) greens the upward-facing faces."""
    rng = random.Random(seed)
    b = Builder()
    stone = _pbr(name + "_stone", "weathered_stone", roughness=0.92)
    mi = b.mat(stone)
    g = bmesh.ops.create_icosphere(b.bm, subdivisions=max(1, min(5, int(detail))), radius=1.0)
    verts = g["verts"]
    freq = 1.4
    for v in verts:
        n = v.co.normalized()
        d = 1.0 + roughness * 0.55 * _fbm(n * freq, seed) + roughness * 0.18 * _fbm(n * freq * 3.1, seed + 11)
        # Sharpen: stones have planes, not spheres.
        d = d * (1.0 + 0.06 * math.copysign(abs(n.x * n.y * n.z) ** 0.33, n.x * n.y * n.z))
        p = n * d * radius
        p.z *= flatten
        v.co = p
    zmin = min(v.co.z for v in verts)
    floor = zmin * 0.72  # a flat-ish bottom so it rests on the ground
    for v in verts:
        v.co.z = max(v.co.z, floor)
    bmesh.ops.recalc_face_normals(b.bm, faces=b.bm.faces[:])
    base = _PALETTES["weathered_stone"]
    for f in b.bm.faces:
        f.material_index = mi
        c = _tint(base, rng, 0.14)
        up = f.normal.z
        if moss > 0 and up > 0.35:
            c = _mix(c, _PALETTES["moss"], min(1.0, moss * (0.4 + up)))
        for loop in f.loops:
            loop[b.col] = (c[0], c[1], c[2], 1.0)
    return b.finish(name)


def stairs(name="Stairs", steps=10, width=1.4, rise=0.18, run=0.30, landing=0.0, stringers=True, style="stone", seed=0):
    """A straight staircase of solid steps going up along +y; origin at the bottom center. `landing` adds a flat top platform (m)."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_" + style, "stone" if style == "stone" else "wood", roughness=0.9)
    base = _PALETTES["stone" if style == "stone" else "wood"]
    for i in range(int(steps)):
        top = rise * (i + 1)
        b.box((-width / 2, i * run, 0), (width / 2, (i + 1) * run, top), mat, _tint(base, rng, 0.08))
    if landing > 0:
        b.box((-width / 2, steps * run, 0), (width / 2, steps * run + landing, rise * steps), mat, _tint(base, rng, 0.08))
    if stringers:
        length = steps * run + max(0.0, landing)
        h = rise * steps
        for sx in (-width / 2 - 0.08, width / 2):
            b.box((sx, 0, 0), (sx + 0.08, length, h + 0.05), mat, _tint(_mix(base, (0, 0, 0), 0.25), rng, 0.05))
    return b.finish(name)


def arch(name="Arch", width=3.0, height=4.0, depth=1.0, thickness=0.55, segments=11, seed=0):
    """A stone archway: two piers topped by a semicircular ring of wedge-shaped voussoirs with a keystone. `width` is the clear opening."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_stone", "stone", roughness=0.9)
    base = _PALETTES["stone"]
    r_in = width / 2
    r_out = r_in + thickness
    spring = max(height - r_out, 0.5)  # height where the curve starts
    # Piers, stacked in courses for a masonry look.
    course = 0.45
    n = max(1, int(round(spring / course)))
    ch = spring / n
    for side in (-1, 1):
        x0 = side * r_in if side > 0 else -r_out
        for c in range(n):
            b.box((x0, -depth / 2, c * ch), (x0 + thickness, depth / 2, (c + 1) * ch - 0.01), mat, _tint(base, rng, 0.1), rng, 0.012)
    segs = max(5, int(segments)) | 1  # odd so a keystone sits on top
    for i in range(segs):
        a0 = math.pi * i / segs
        a1 = math.pi * (i + 1) / segs
        keystone = i == segs // 2
        ro = r_out + (0.12 if keystone else 0.0)
        # angle runs from +x (0) over the top (pi/2) to -x (pi); the solid is the radial quad extruded along y
        def pt(r, a, y):
            return (r * math.cos(a), y, spring + r * math.sin(a))
        ring = [(r_in, a0), (r_in, a1), (ro, a1), (ro, a0)]
        corners = [pt(r, a, -depth / 2) for r, a in ring] + [pt(r, a, depth / 2) for r, a in ring]
        b.hexa(corners, mat, _tint(base, rng, 0.1), rng, 0.01)
    return b.finish(name)


def fence(name="Fence", length=6.0, height=1.1, post_spacing=1.5, style="picket", seed=0):
    """A wooden fence along +x ('picket' or 'rail'); origin at its center line on the ground."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_wood", "wood", roughness=0.88)
    base = _PALETTES["wood"]
    posts = max(2, int(round(length / post_spacing)) + 1)
    step = length / (posts - 1)
    for i in range(posts):
        x = i * step
        h = height + 0.12 + rng.uniform(-0.03, 0.03)
        b.box((x - 0.06, -0.06, 0), (x + 0.06, 0.06, h), mat, _tint(base, rng, 0.1), rng, 0.006)
    rails = (height * 0.28, height * 0.72) if style == "picket" else (height * 0.25, height * 0.55, height * 0.85)
    for z in rails:
        b.box((0, -0.045, z - 0.04), (length, 0.045, z + 0.04), mat, _tint(base, rng, 0.08))
    if style == "picket":
        pitch = 0.15
        count = int(length / pitch)
        for i in range(count):
            x = (i + 0.5) * (length / count)
            h = height * rng.uniform(0.9, 1.0)
            w = 0.045
            # picket body + a tapered pointed cap
            col = _tint(base, rng, 0.12)
            b.box((x - w, 0.045, 0.04), (x + w, 0.075, h - 0.07), mat, col, rng, 0.004)
            n = w * 0.12
            b.hexa([(x - w, 0.045, h - 0.07), (x + w, 0.045, h - 0.07), (x + w, 0.075, h - 0.07), (x - w, 0.075, h - 0.07),
                    (x - n, 0.055, h), (x + n, 0.055, h), (x + n, 0.065, h), (x - n, 0.065, h)], mat, col)
    return b.finish(name)


def column(name="Column", radius=0.35, height=4.0, flutes=12, base=True, capital=True, seed=0):
    """A classical fluted column (lathe profile); origin at the bottom center."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_marble", "stone", roughness=0.55, color=(0.74, 0.72, 0.68))
    flutes = max(6, int(flutes))
    samples = flutes * 2
    # (radius factor, height) profile pieces; the shaft is fluted by modulating the radius.
    base_h = radius * 0.8 if base else 0.0
    cap_h = radius * 0.9 if capital else 0.0
    shaft0, shaft1 = base_h, height - cap_h
    rings = []
    if base:
        rings += [(1.45, 0.0), (1.45, base_h * 0.45), (1.2, base_h * 0.55), (1.1, base_h)]
    rings += [(1.0, shaft0), (0.92, shaft1)]
    if capital:
        rings += [(1.0, shaft1), (1.15, shaft1 + cap_h * 0.4), (1.5, shaft1 + cap_h * 0.7), (1.5, height)]
    mi = b.mat(mat)
    tint = (0.74, 0.72, 0.68)
    verts = []
    for ri, (rf, z) in enumerate(rings):
        row = []
        in_shaft = shaft0 - 1e-6 <= z <= shaft1 + 1e-6 and ri < len(rings)
        for s in range(samples):
            a = 2 * math.pi * s / samples
            flute = 0.035 * radius * (1 if (s % 2 == 0) else 0) if in_shaft and rings[ri][0] <= 1.0 else 0.0
            r = radius * rf - flute
            row.append(b.bm.verts.new((r * math.cos(a), r * math.sin(a), z)))
        verts.append(row)
    for ri in range(len(rings) - 1):
        for s in range(samples):
            s2 = (s + 1) % samples
            quad = [verts[ri][s], verts[ri][s2], verts[ri + 1][s2], verts[ri + 1][s]]
            if len({id(v) for v in quad}) == 4 and abs(rings[ri][1] - rings[ri + 1][1]) + abs(rings[ri][0] - rings[ri + 1][0]) > 1e-6:
                b.face(quad, mi, _tint(tint, rng, 0.03))
    # Caps
    top = [b.bm.verts.new((0, 0, 0)), b.bm.verts.new((0, 0, height))]
    for s in range(samples):
        s2 = (s + 1) % samples
        b.face([top[0], verts[0][s2], verts[0][s]], mi, tint)
        b.face([top[1], verts[-1][s], verts[-1][s2]], mi, tint)
    return b.finish(name)


def barrel(name="Barrel", radius=0.38, height=0.95, staves=16, hoops=3, seed=0):
    """A wooden barrel with iron hoops; origin at the bottom center."""
    rng = random.Random(seed)
    b = Builder()
    wood = _pbr(name + "_wood", "wood", roughness=0.8)
    iron = _pbr(name + "_iron", "iron", roughness=0.45, metallic=0.9)
    mw, mi_ = b.mat(wood), b.mat(iron)
    rows = 9
    bulge = 1.14
    prof = []
    for r in range(rows):
        t = r / (rows - 1)
        prof.append((radius * (1.0 + (bulge - 1.0) * math.sin(math.pi * t)), t * height))
    ring = []
    for rad, z in prof:
        ring.append([b.bm.verts.new((rad * math.cos(2 * math.pi * s / staves), rad * math.sin(2 * math.pi * s / staves), z)) for s in range(staves)])
    base = _PALETTES["wood"]
    for r in range(rows - 1):
        for s in range(staves):
            s2 = (s + 1) % staves
            b.face([ring[r][s], ring[r][s2], ring[r + 1][s2], ring[r + 1][s]], mw, _tint(base, rng, 0.14))
    c0, c1 = b.bm.verts.new((0, 0, 0)), b.bm.verts.new((0, 0, height))
    for s in range(staves):
        s2 = (s + 1) % staves
        b.face([c0, ring[0][s2], ring[0][s]], mw, _tint(base, rng, 0.1))
        b.face([c1, ring[-1][s], ring[-1][s2]], mw, _tint(base, rng, 0.1))
    for i in range(int(hoops)):
        t = 0.12 + 0.76 * (i / max(1, hoops - 1)) if hoops > 1 else 0.5
        z = t * height
        rad = radius * (1.0 + (bulge - 1.0) * math.sin(math.pi * t))
        hh = 0.035
        outer, inner = rad + 0.02, rad - 0.005
        loops = [[b.bm.verts.new((r * math.cos(2 * math.pi * s_ / staves), r * math.sin(2 * math.pi * s_ / staves), zz))
                  for s_ in range(staves)] for r, zz in ((outer, z - hh), (outer, z + hh), (inner, z + hh), (inner, z - hh))]
        for s_ in range(staves):
            s2 = (s_ + 1) % staves
            for k in range(4):
                k2 = (k + 1) % 4
                b.face([loops[k][s_], loops[k][s2], loops[k2][s2], loops[k2][s_]], mi_, _PALETTES["iron"])
    return b.finish(name)


def _masonry_ring(b, rng, mat, color_fn, radius, wall, height, sides, course_h, ruin, seed, door, slits, battlements):
    """Courses of staggered wedge blocks forming a hollow cylinder (tower body)."""
    n_courses = max(1, int(math.ceil(height / course_h)))
    ch = height / n_courses
    step = 2 * math.pi / sides
    r_in = radius - wall
    # Ragged broken top: each column survives up to its own height.
    col_h = []
    for i in range(sides):
        t = _fbm(Vector((math.cos(i * step) * 1.7, math.sin(i * step) * 1.7, 0.3)), seed, 3)
        col_h.append(max(height * 0.22, height * (1.0 - ruin * (0.2 + 0.95 * (t * 0.5 + 0.5)))))
    door_angle = -math.pi / 2
    slit_cols = {int(sides * f) % sides for f in (0.12, 0.45, 0.78)} if slits else set()
    for c in range(n_courses):
        z0, z1 = c * ch, (c + 1) * ch
        stagger = 0.5 if c % 2 else 0.0
        for i in range(sides):
            a0 = (i + stagger) * step
            a1 = a0 + step
            mid = (a0 + a1) / 2
            col = int(((mid % (2 * math.pi)) / step)) % sides
            if ruin > 0 and z0 > col_h[col]:
                continue
            if ruin > 0 and rng.random() < ruin * 0.22 * (z0 / height):
                continue
            # Doorway: an opening at the base facing -y.
            da = abs((mid - door_angle + math.pi) % (2 * math.pi) - math.pi)
            if door and z1 <= 2.6 and da < step * 0.95:
                continue
            if col in slit_cols and height * 0.45 <= z0 < height * 0.45 + 1.1:
                continue
            ro = radius + rng.uniform(-0.04, 0.05)
            ri = r_in + rng.uniform(-0.04, 0.04)
            corners = []
            for r, a, z in ((ri, a0, z0), (ri, a1, z0), (ro, a1, z0), (ro, a0, z0),
                            (ri, a0, z1), (ri, a1, z1), (ro, a1, z1), (ro, a0, z1)):
                corners.append((r * math.cos(a), r * math.sin(a), z))
            corners = [corners[0], corners[3], corners[2], corners[1], corners[4], corners[7], corners[6], corners[5]]
            b.hexa(corners, mat, color_fn(z0 / height, rng), rng, 0.018)
    if battlements and ruin < 0.15:
        top = height
        mh = 0.75
        for i in range(sides):
            if i % 2:
                continue
            a0, a1 = i * step, (i + 1) * step
            corners = []
            for r, a, z in ((r_in, a0, top), (r_in, a1, top), (radius + 0.06, a1, top), (radius + 0.06, a0, top),
                            (r_in, a0, top + mh), (r_in, a1, top + mh), (radius + 0.06, a1, top + mh), (radius + 0.06, a0, top + mh)):
                corners.append((r * math.cos(a), r * math.sin(a), z))
            corners = [corners[0], corners[3], corners[2], corners[1], corners[4], corners[7], corners[6], corners[5]]
            b.hexa(corners, mat, color_fn(1.0, rng), rng, 0.012)


def tower(name="Tower", radius=3.0, height=12.0, sides=14, wall_thickness=0.9, course_height=0.5,
          battlements=True, ruin=0.0, door=True, window_slits=True, rubble=True, seed=0):
    """A round masonry tower built from staggered stone blocks. `ruin` (0-1) breaks the top into a ragged
    silhouette, drops blocks and scatters rubble around the base; battlements only appear on intact towers.
    The doorway faces -y (+z after glTF export). Great base for castles and ruins."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_masonry", "stone", roughness=0.93)
    base = _PALETTES["weathered_stone"]
    grime = (0.16, 0.2, 0.1)

    def color_fn(t, r):
        c = _tint(base, r, 0.16)
        if t < 0.25:  # damp, mossy foot
            c = _mix(c, grime, (0.25 - t) * (1.2 + ruin))
        return c

    _masonry_ring(b, rng, mat, color_fn, radius, wall_thickness, height, int(sides), course_height, float(ruin), seed,
                  door, window_slits, battlements)
    # Floor slab so the interior is not a hole to the void.
    r_in = radius - wall_thickness
    ring_pts = [(r_in * math.cos(2 * math.pi * i / sides), r_in * math.sin(2 * math.pi * i / sides)) for i in range(int(sides))]
    b.slab(ring_pts, 0.0, 0.12, mat, _tint(base, rng, 0.05))
    if rubble:
        pieces = int(5 + ruin * 18)
        for _ in range(pieces):
            a = rng.uniform(0, 2 * math.pi)
            r = radius + rng.uniform(0.2, 2.2 + 2.0 * ruin)
            _add_blob(b, (r * math.cos(a), r * math.sin(a), 0), rng.uniform(0.18, 0.5) * (1 + ruin), rng, mat, base)
    return b.finish(name, origin_bottom=False)  # the tower axis stays at the origin


def wall(name="Wall", length=8.0, height=2.5, thickness=0.6, course_height=0.4, ruin=0.0, seed=0):
    """A straight dry-stone wall along +x built from staggered blocks; `ruin` (0-1) breaks and lowers the top."""
    rng = random.Random(seed)
    b = Builder()
    mat = _pbr(name + "_stone", "stone", roughness=0.93)
    base = _PALETTES["weathered_stone"]
    n = max(1, int(round(height / course_height)))
    ch = height / n
    for c in range(n):
        x = -rng.uniform(0.0, 0.6) if c % 2 else 0.0
        z0, z1 = c * ch, (c + 1) * ch
        while x < length:
            w = rng.uniform(0.55, 1.3)
            x0, x1 = max(0.0, x), min(length, x + w)
            x += w
            if x1 - x0 < 0.12:
                continue
            col = (x0 + x1) / 2
            limit = max(height * 0.2, height * (1.0 - ruin * (0.2 + 0.95 * (_fbm(Vector((col * 0.7, 0.2, 0.1)), seed, 3) * 0.5 + 0.5))))
            if ruin > 0 and (z0 > limit or rng.random() < ruin * 0.18 * (z0 / height)):
                continue
            t = rng.uniform(-0.03, 0.03)
            b.box((x0, -thickness / 2 + t, z0), (x1 - 0.015, thickness / 2 + t, z1 - 0.012), mat,
                  _mix(_tint(base, rng, 0.16), (0.16, 0.2, 0.1), max(0.0, 0.2 - z0 / height) * 1.5), rng, 0.012)
    return b.finish(name)


def building(name="Building", floors=2, width=8.0, depth=6.0, floor_height=3.0, windows_per_side=3,
             roof="gabled", wall_style="plaster", door=True, chimney=True, seed=0):
    """A house with windows, a door, floor bands and a roof. wall_style: plaster, brick, stone or wood;
    roof: gabled, hip or flat. The door is on the -y side (+z after glTF export). Size is in meters."""
    rng = random.Random(seed)
    b = Builder()
    floors = max(1, int(floors))
    H = floors * floor_height
    wall_key = {"plaster": "plaster", "brick": "brick", "stone": "stone", "wood": "wood"}.get(wall_style)
    if wall_key is None:
        raise ValueError("wall_style must be plaster, brick, stone or wood")
    wall_m = _pbr(name + "_walls", wall_key, roughness=0.9)
    trim_m = _pbr(name + "_trim", "dark_wood" if wall_key != "wood" else "wood", roughness=0.75)
    stone_m = _pbr(name + "_plinth", "stone", roughness=0.95)
    glass_m = B.make_material(name + "_glass", (0.08, 0.12, 0.16), roughness=0.08, metallic=0.0)
    roof_key = "terracotta" if wall_key in ("plaster", "wood") else "slate"
    roof_m = _pbr(name + "_roof", roof_key, roughness=0.8)
    door_m = _pbr(name + "_door", "dark_wood", roughness=0.7)
    wall_c, trim_c, stone_c = _PALETTES[wall_key], _PALETTES["dark_wood"] if wall_key != "wood" else _PALETTES["wood"], _PALETTES["stone"]
    hw, hd = width / 2, depth / 2

    # Walls and plinth
    b.box((-hw, -hd, 0), (hw, hd, H), wall_m, _tint(wall_c, rng, 0.03))
    b.box((-hw - 0.08, -hd - 0.08, 0), (hw + 0.08, hd + 0.08, 0.5), stone_m, _tint(stone_c, rng, 0.06))
    for f in range(1, floors):
        z = f * floor_height
        b.box((-hw - 0.05, -hd - 0.05, z - 0.09), (hw + 0.05, hd + 0.05, z + 0.09), trim_m, _tint(trim_c, rng, 0.05))
    # Corner posts for timber-frame feel on plaster walls
    if wall_key == "plaster":
        for sx in (-1, 1):
            for sy in (-1, 1):
                b.box((sx * hw - 0.08, sy * hd - 0.08, 0.5), (sx * hw + 0.08, sy * hd + 0.08, H), trim_m, trim_c)

    # Wall-local box: side is 'front' (-y), 'back' (+y), 'left' (-x), 'right' (+x).
    def wall_box(side, u, z, w, h, n_in, n_out, mat, color):
        if side == "front":
            lo, hi = (u - w / 2, -hd - n_out, z - h / 2), (u + w / 2, -hd - n_in, z + h / 2)
        elif side == "back":
            lo, hi = (u - w / 2, hd + n_in, z - h / 2), (u + w / 2, hd + n_out, z + h / 2)
        elif side == "left":
            lo, hi = (-hw - n_out, u - w / 2, z - h / 2), (-hw - n_in, u + w / 2, z + h / 2)
        else:
            lo, hi = (hw + n_in, u - w / 2, z - h / 2), (hw + n_out, u + w / 2, z + h / 2)
        b.box(lo, hi, mat, color)

    door_w, door_h = 1.1, min(2.2, floor_height * 0.78)
    sides = {"front": width, "back": width, "left": depth, "right": depth}
    for side, span in sides.items():
        n = max(1, int(round(windows_per_side * span / width))) if windows_per_side > 0 else 0
        for fi in range(floors):
            for k in range(n):
                u = -span / 2 + span * (k + 0.5) / n
                if side == "front" and fi == 0 and door and abs(u) < door_w / 2 + 0.7:
                    continue
                zc = fi * floor_height + floor_height * 0.58
                ww, wh = min(1.05, span / n * 0.55), min(1.45, floor_height * 0.5)
                wall_box(side, u, zc, ww, wh, 0.0, 0.025, glass_m, (1, 1, 1))
                fr = 0.07
                wall_box(side, u, zc + wh / 2, ww + 2 * fr, fr, 0.0, 0.07, trim_m, trim_c)       # head
                wall_box(side, u, zc - wh / 2, ww + 2 * fr, fr, 0.0, 0.07, trim_m, trim_c)       # bottom rail
                wall_box(side, u - ww / 2, zc, fr, wh, 0.0, 0.07, trim_m, trim_c)                # jambs
                wall_box(side, u + ww / 2, zc, fr, wh, 0.0, 0.07, trim_m, trim_c)
                wall_box(side, u, zc, fr * 0.6, wh, 0.0, 0.06, trim_m, trim_c)                   # mullion
                wall_box(side, u, zc - wh / 2 - 0.07, ww + 0.34, 0.07, 0.0, 0.17, stone_m, stone_c)  # sill
    if door:
        wall_box("front", 0, door_h / 2, door_w, door_h, 0.0, 0.05, door_m, _tint(_PALETTES["dark_wood"], rng, 0.08))
        wall_box("front", 0, door_h + 0.07, door_w + 0.3, 0.14, 0.0, 0.12, trim_m, trim_c)
        wall_box("front", -door_w / 2 - 0.07, door_h / 2, 0.14, door_h, 0.0, 0.1, trim_m, trim_c)
        wall_box("front", door_w / 2 + 0.07, door_h / 2, 0.14, door_h, 0.0, 0.1, trim_m, trim_c)
        wall_box("front", 0, 0.1, door_w + 0.9, 0.2, 0.0, 0.65, stone_m, stone_c)                # step

    # Roof
    over = 0.35
    if roof == "flat":
        b.box((-hw - 0.1, -hd - 0.1, H), (hw + 0.1, hd + 0.1, H + 0.18), stone_m, stone_c)
        for lo, hi in (((-hw - 0.1, -hd - 0.1, H + 0.18), (hw + 0.1, -hd + 0.05, H + 0.7)),
                       ((-hw - 0.1, hd - 0.05, H + 0.18), (hw + 0.1, hd + 0.1, H + 0.7)),
                       ((-hw - 0.1, -hd + 0.05, H + 0.18), (-hw + 0.05, hd - 0.05, H + 0.7)),
                       ((hw - 0.05, -hd + 0.05, H + 0.18), (hw + 0.1, hd - 0.05, H + 0.7))):
            b.box(lo, hi, stone_m, _tint(stone_c, rng, 0.05))
        top = H + 0.18
    else:
        rise = min(width, depth) * (0.36 if roof == "gabled" else 0.3)
        roof_c = _tint(_PALETTES[roof_key], rng, 0.05)
        if width >= depth:
            b.gable("x", -hw - over, hw + over, -hd - over, hd + over, 0.0, H, H + rise, roof_m, roof_c)
        else:
            b.gable("y", -hd - over, hd + over, -hw - over, hw + over, 0.0, H, H + rise, roof_m, roof_c)
        top = H + rise
    if chimney and roof != "flat":
        cx = hw * 0.45 if width >= depth else 0.0
        cy = 0.0 if width >= depth else hd * 0.45
        b.box((cx - 0.35, cy - 0.35, H + 0.2), (cx + 0.35, cy + 0.35, top + 0.9), stone_m, _tint(_PALETTES["brick"], rng, 0.05))
        b.box((cx - 0.45, cy - 0.45, top + 0.9), (cx + 0.45, cy + 0.45, top + 1.05), stone_m, stone_c)
    return b.finish(name)


def terrain_chunk(name="Terrain", size=32.0, resolution=40, height=3.0, roughness=0.5, flat_radius=0.0, seed=0):
    """A square heightfield chunk (`size` m on a side, centered) with hills, rock/grass/soil vertex
    colors and UVs. `flat_radius` flattens a circle in the middle for a building lot. The lowest point sits at height 0."""
    rng = random.Random(seed)
    res = max(4, min(256, int(resolution)))
    b = Builder()
    mat = B.make_material(name + "_ground", (1, 1, 1), roughness=0.95, vertex_colors=True)
    mi = b.mat(mat)
    uv = b.bm.loops.layers.uv.new("UVMap")
    half = size / 2
    grid = []
    for iy in range(res + 1):
        row = []
        for ix in range(res + 1):
            x = -half + size * ix / res
            y = -half + size * iy / res
            p = Vector((x * 0.045, y * 0.045, 0.0))
            h = _fbm(p, seed, 5) * height * (0.6 + roughness)
            if flat_radius > 0:
                d = math.hypot(x, y)
                h *= _smoothstep(flat_radius, flat_radius * 1.8, d)
            row.append(b.bm.verts.new((x, y, h)))
        grid.append(row)
    # Slope-based coloring needs normals: compute from the height field.
    def hz(ix, iy):
        return grid[max(0, min(res, iy))][max(0, min(res, ix))].co.z
    cell = size / res
    grass, soil, rock_c = (0.22, 0.34, 0.12), (0.34, 0.26, 0.17), (0.40, 0.38, 0.35)
    heights = [v.co.z for row in grid for v in row]
    hmin, hmax = min(heights), max(heights)
    for iy in range(res):
        for ix in range(res):
            vs = [grid[iy][ix], grid[iy][ix + 1], grid[iy + 1][ix + 1], grid[iy + 1][ix]]
            f = b.face(vs, mi, grass)
            if f is None:
                continue
            gx = (hz(ix + 1, iy) - hz(ix - 1, iy)) / (2 * cell)
            gy = (hz(ix, iy + 1) - hz(ix, iy - 1)) / (2 * cell)
            slope = math.hypot(gx, gy)
            hn = (sum(v.co.z for v in vs) / 4 - hmin) / max(1e-6, hmax - hmin)
            c = _mix(grass, soil, _smoothstep(0.55, 0.9, hn) * 0.6 + rng.uniform(0, 0.12))
            c = _mix(c, rock_c, _smoothstep(0.45, 0.95, slope))
            c = _tint(c, rng, 0.07)
            for loop, vert in zip(f.loops, vs):
                loop[b.col] = (c[0], c[1], c[2], 1.0)
                loop[uv].uv = ((vert.co.x + half) / size * (size / 4), (vert.co.y + half) / size * (size / 4))
    obj = b.finish(name, origin_bottom=False, recalc=False)
    # Rest the lowest point on z=0 but keep the chunk centered.
    lo = min(v.co.z for v in obj.data.vertices)
    obj.data.transform(Matrix.Translation((0, 0, -lo)))
    return obj


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

RECIPES = {f.__name__: f for f in (building, tower, wall, rock, stairs, arch, fence, column, barrel, terrain_chunk)}


def describe():
    """All recipes: [{'name', 'summary', 'params': {name: default}}]."""
    out = []
    for name, fn in RECIPES.items():
        sig = inspect.signature(fn)
        params = {p.name: p.default for p in sig.parameters.values() if p.name != "name"}
        out.append({"name": name, "summary": (inspect.getdoc(fn) or "").split("\n\n")[0].replace("\n", " "), "params": params})
    return out


def generate(recipe, name=None, **params):
    """Build `recipe` with `params` and return the object. Unknown recipes/parameters are errors with the valid choices listed."""
    fn = RECIPES.get(recipe)
    if fn is None:
        raise ValueError("unknown recipe %r (available: %s)" % (recipe, ", ".join(sorted(RECIPES))))
    valid = set(inspect.signature(fn).parameters)
    bad = sorted(set(params) - valid)
    if bad:
        raise ValueError("recipe %r has no parameter(s) %s (valid: %s)" % (recipe, ", ".join(bad), ", ".join(sorted(valid - {"name"}))))
    if name:
        params["name"] = name
    return fn(**params)
