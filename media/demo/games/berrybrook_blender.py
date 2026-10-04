"""Berrybrook — the farm kit, modelled procedurally in Blender by the crew.

Runs inside Blender through the engine's `dcc_run_script` (the `sky` helper is preloaded).
Each asset is written as one .glb per material part (`<asset>_<part>.glb`) so the engine can
dress every part with its own material (painted wood, plaster, shingles, glass, leaves, berries,
glowing windows...). Colors are baked into vertex colors (the engine multiplies them into the
albedo), so a single "painted wood" material serves the sage door, the cream trim and the honey
porch boards. Shapes are soft: boxes are bevelled, organic parts are smooth-shaded.

Blender space: X right, Y back, Z up; the front of every asset faces -Y (the engine's +Z after
the glTF Y-up conversion). Units are meters.

sky.ARGS[0] is a JSON object: {"seed": n, "only": [asset names]?}
"""
import json
import math
import random

import bmesh
import bpy
from mathutils import Euler, Matrix, Vector
from skywalker_dcc import blender as B

ARGS = json.loads(sky.ARGS[0]) if sky.ARGS else {}  # noqa: F821  (sky is injected by the runner)
R = random.Random(ARGS.get("seed", 11))


# --- colors ----------------------------------------------------------------------------------
def lin(hexstr):
    """sRGB hex -> linear RGB (vertex colors are linear)."""
    h = hexstr.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i:i + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return tuple(out)


def jit(c, v=0.05, rnd=None):
    rnd = rnd or R
    k = 1.0 + rnd.uniform(-v, v)
    return tuple(max(0.0, min(1.0, x * k * (1.0 + rnd.uniform(-v * 0.3, v * 0.3)))) for x in c)


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


# --- the accumulator: geometry lists per (asset, part) ---------------------------------------------
# Every primitive is built in its own temporary bmesh (bmesh reuses freed slots after a bevel, so
# "faces created since index n" is unreliable in a shared one), then committed as plain lists.
class Part:
    def __init__(self):
        self.v, self.f, self.c = [], [], []   # vertex coords, faces (vertex indices), loop colors per face


class Asset:
    def __init__(self, name):
        self.name = name
        self.parts = {}
        self.pending = []

    def bm(self, part):
        bm = bmesh.new()
        bm.loops.layers.color.new("Col")
        self.pending.append((part, bm))
        return bm

    def flush(self):
        for part, bm in self.pending:
            p = self.parts.setdefault(part, Part())
            layer = bm.loops.layers.color["Col"]
            bm.verts.index_update()
            base = len(p.v)
            p.v.extend(tuple(v.co) for v in bm.verts)
            for f in bm.faces:
                p.f.append([base + v.index for v in f.verts])
                p.c.append([tuple(l[layer])[:3] for l in f.loops])
            bm.free()
        self.pending = []

    def mark(self):
        self.flush()
        return {k: len(p.v) for k, p in self.parts.items()}

    def translate_since(self, mark, vec):
        self.flush()
        for k, p in self.parts.items():
            for i in range(mark.get(k, 0), len(p.v)):
                x, y, z = p.v[i]
                p.v[i] = (x + vec[0], y + vec[1], z + vec[2])


CUR = None


def asset(name):
    global CUR
    CUR = Asset(name)
    return CUR


def prim(fn):
    """Commits the temporary geometry of a primitive into its asset's part lists."""
    def wrapper(*a, **kw):
        r = fn(*a, **kw)
        CUR.flush()
        return r
    wrapper.__name__ = fn.__name__
    return wrapper


def _new_faces(bm, before):
    bm.faces.ensure_lookup_table()
    return [bm.faces[i] for i in range(before, len(bm.faces))]


def _paint(bm, faces, color, var=0.0, grad=None):
    """Colors faces; `grad` = (axis, lo, hi, color_lo, color_hi) blends along a world axis per vertex
    (smooth, so organic shapes stay soft), with a gentle positional mottling instead of per-face jitter."""
    layer = bm.loops.layers.color["Col"]
    base = jit(color, var) if var else color
    for f in faces:
        for l in f.loops:
            c = base
            if grad:
                ax, lo, hi, c0, c1 = grad
                p = l.vert.co
                t = (p[ax] - lo) / max(1e-6, hi - lo)
                c = mix(c0, c1, max(0.0, min(1.0, t)))
                m = 1.0 + 0.06 * math.sin(p.x * 3.7 + p.y * 2.3) * math.sin(p.z * 4.1 - p.x * 1.3)
                c = tuple(min(1.0, v * m) for v in c)
            l[layer] = (c[0], c[1], c[2], 1.0)


def _mat(loc=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1)):
    m = Matrix.Translation(Vector(loc)) @ Euler([math.radians(a) for a in rot], "XYZ").to_matrix().to_4x4()
    s = Matrix.Diagonal(Vector((scale[0], scale[1], scale[2], 1.0)))
    return m @ s


@prim
def box(part, color, loc, size, rot=(0, 0, 0), bevel=0.02, var=0.06, seg=2, grad=None):
    """Bevelled box: center `loc`, full `size` (x, y, z), rotation in degrees."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    r = bmesh.ops.create_cube(bm, size=1.0)
    verts = r["verts"]
    bmesh.ops.transform(bm, matrix=Matrix.Diagonal(Vector((size[0], size[1], size[2], 1.0))), verts=verts)
    if bevel > 0:
        edges = list({e for v in verts for e in v.link_edges})
        w = min(bevel, 0.45 * min(size))
        bmesh.ops.bevel(bm, geom=edges + verts, offset=w, segments=seg, profile=0.5, affect="EDGES", clamp_overlap=True)
    faces = _new_faces(bm, n0)
    allv = list({v for f in faces for v in f.verts})
    bmesh.ops.transform(bm, matrix=_mat(loc, rot), verts=allv)
    _paint(bm, faces, color, var, grad)
    return faces


@prim
def cyl(part, color, loc, radius, depth, rot=(0, 0, 0), seg=12, r2=None, var=0.05, bevel=0.0, caps=True, grad=None):
    """Cylinder / cone frustum standing on `loc` (base center) along its local +Z."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    r = bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=seg, radius1=radius,
                              radius2=radius if r2 is None else r2, depth=depth)
    verts = r["verts"]
    bmesh.ops.translate(bm, vec=Vector((0, 0, depth / 2)), verts=verts)
    if bevel > 0:
        edges = [e for e in {e for v in verts for e in v.link_edges} if len(e.link_faces) == 2 and
                 abs(e.link_faces[0].normal.dot(e.link_faces[1].normal)) < 0.5]
        bmesh.ops.bevel(bm, geom=edges, offset=bevel, segments=2, profile=0.5, affect="EDGES", clamp_overlap=True)
    faces = _new_faces(bm, n0)
    allv = list({v for f in faces for v in f.verts})
    bmesh.ops.transform(bm, matrix=_mat(loc, rot), verts=allv)
    _paint(bm, faces, color, var, grad)
    return faces


@prim
def ball(part, color, loc, radius, scale=(1, 1, 1), rot=(0, 0, 0), subdiv=2, noise=0.0, var=0.05, grad=None, seed=None):
    """Icosphere blob; `noise` displaces it (puffy canopies, bushes, rocks)."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    r = bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=radius)
    verts = r["verts"]
    if noise > 0:
        rnd = random.Random(seed if seed is not None else R.random())
        ph = [rnd.uniform(0, 6.28) for _ in range(6)]
        for v in verts:
            d = v.co.normalized()
            n = (math.sin(d.x * 3.1 + ph[0]) * math.sin(d.y * 2.7 + ph[1]) * math.sin(d.z * 3.3 + ph[2]) +
                 0.5 * math.sin(d.x * 6.3 + ph[3]) * math.sin(d.y * 5.9 + ph[4]) * math.sin(d.z * 6.1 + ph[5]))
            v.co = v.co * (1.0 + noise * n)
    faces = _new_faces(bm, n0)
    bmesh.ops.transform(bm, matrix=_mat(loc, rot, scale), verts=verts)
    _paint(bm, faces, color, var, grad)
    return faces


@prim
def lathe(part, color, loc, profile, seg=12, rot=(0, 0, 0), var=0.04, colors=None, scale=(1, 1, 1), caps=True):
    """Surface of revolution around local Z from a profile [(radius, z), ...] (bottom to top).
    `colors` (optional) gives one color per profile ring band."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    rings = []
    for (rr, z) in profile:
        if rr < 1e-5:
            rings.append([bm.verts.new((0.0, 0.0, z))] * seg)  # an apex: one shared vertex
            continue
        ring = []
        for k in range(seg):
            a = 2 * math.pi * k / seg
            ring.append(bm.verts.new((rr * math.cos(a), rr * math.sin(a), z)))
        rings.append(ring)
    band_faces = []
    for i in range(len(rings) - 1):
        row = []
        for k in range(seg):
            a, b = rings[i][k], rings[i][(k + 1) % seg]
            c, d = rings[i + 1][(k + 1) % seg], rings[i + 1][k]
            if a is b:
                f = bm.faces.new((a, c, d))
            elif c is d:
                f = bm.faces.new((a, b, c))
            else:
                f = bm.faces.new((a, b, c, d))
            row.append(f)
        band_faces.append(row)
    if caps and profile[0][0] > 1e-5:
        bm.faces.new(list(reversed(rings[0])))
    if caps and profile[-1][0] > 1e-5:
        bm.faces.new(rings[-1])
    faces = _new_faces(bm, n0)
    allv = list({v for f in faces for v in f.verts})
    bmesh.ops.transform(bm, matrix=_mat(loc, rot, scale), verts=allv)
    if colors:
        layer = bm.loops.layers.color["Col"]
        for i, row in enumerate(band_faces):
            c = jit(colors[min(i, len(colors) - 1)], var)
            for f in row:
                if f.is_valid:
                    for l in f.loops:
                        l[layer] = (*c, 1.0)
        banded = {f for row in band_faces for f in row}
        _paint(bm, [f for f in faces if f not in banded], colors[-1], var)
    else:
        _paint(bm, faces, color, var)
    return faces


@prim
def poly(part, color, pts, var=0.03, both=False):
    """One polygon (or a fan of quads/tris) from explicit points; `both` adds the back face."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    vs = [bm.verts.new(p) for p in pts]
    bm.faces.new(vs)
    if both:
        vs2 = [bm.verts.new(p) for p in reversed(pts)]
        bm.faces.new(vs2)
    faces = _new_faces(bm, n0)
    _paint(bm, faces, color, var)
    return faces


@prim
def grid_surface(part, pts_fn, nu, nv, color_fn, both=False):
    """A (nu x nv) quad sheet from a point function f(u, v) -> (x, y, z); color_fn(u, v) per face."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    layer = bm.loops.layers.color["Col"]
    vs = [[bm.verts.new(pts_fn(i / nu, j / nv)) for j in range(nv + 1)] for i in range(nu + 1)]
    for i in range(nu):
        for j in range(nv):
            quad = (vs[i][j], vs[i + 1][j], vs[i + 1][j + 1], vs[i][j + 1])
            c = color_fn((i + 0.5) / nu, (j + 0.5) / nv)
            for q in ([quad] + ([tuple(reversed(quad))] if both else [])):
                if both and q is not quad:
                    q = tuple(bm.verts.new(v.co) for v in q)
                f = bm.faces.new(q)
                for l in f.loops:
                    l[layer] = (*c, 1.0)
    return _new_faces(bm, n0)


@prim
def beam(part, color, a, b, w, h=None, var=0.06, bevel=0.012):
    """A box beam from point a to point b (width w, height h)."""
    a, b = Vector(a), Vector(b)
    d = b - a
    L = d.length
    h = h or w
    c = (a + b) / 2
    q = Vector((0, 0, 1)).rotation_difference(d.normalized())
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    r = bmesh.ops.create_cube(bm, size=1.0)
    verts = r["verts"]
    bmesh.ops.transform(bm, matrix=Matrix.Diagonal(Vector((w, h, L, 1.0))), verts=verts)
    if bevel > 0:
        edges = list({e for v in verts for e in v.link_edges})
        bmesh.ops.bevel(bm, geom=edges + verts, offset=min(bevel, 0.4 * min(w, h)), segments=1, profile=0.5, affect="EDGES",
                        clamp_overlap=True)
    faces = _new_faces(bm, n0)
    allv = list({v for f in faces for v in f.verts})
    bmesh.ops.transform(bm, matrix=Matrix.Translation(c) @ q.to_matrix().to_4x4(), verts=allv)
    _paint(bm, faces, color, var)
    return faces


@prim
def tube(part, color, pts, radius, seg=6, var=0.04, r_end=None):
    """A tube along a polyline (stems, wire, branches); radius tapers to r_end."""
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    pts = [Vector(p) for p in pts]
    r_end = radius if r_end is None else r_end
    rings = []
    for i, p in enumerate(pts):
        t = i / max(1, len(pts) - 1)
        d = (pts[min(i + 1, len(pts) - 1)] - pts[max(i - 1, 0)]).normalized()
        ref = Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((1, 0, 0))
        u = d.cross(ref).normalized()
        v = d.cross(u).normalized()
        rr = radius + (r_end - radius) * t
        rings.append([bm.verts.new(p + (u * math.cos(2 * math.pi * k / seg) + v * math.sin(2 * math.pi * k / seg)) * rr)
                      for k in range(seg)])
    for i in range(len(rings) - 1):
        for k in range(seg):
            bm.faces.new((rings[i][k], rings[i][(k + 1) % seg], rings[i + 1][(k + 1) % seg], rings[i + 1][k]))
    faces = _new_faces(bm, n0)
    _paint(bm, faces, color, var)
    return faces


# --- palette (sRGB) -----------------------------------------------------------------------------
C = {k: lin(v) for k, v in dict(
    cream="#f4e7cf", trim="#fbf5ea", sage="#86a886", sage_dark="#5f805f", sky="#8fb6cc", rose="#c4705c", roof="#b65a48",
    roof_dark="#8f4436", honey="#c99760", wood="#a9764a", wood_dark="#6f4b31", wood_grey="#9a8c7c", stone="#bdb3a3",
    stone_dark="#8e8578", brick="#b0674f", glow="#ffd9a0", glass="#dcecf2", soil="#6e4c34", soil_dark="#523726",
    straw="#ecd9a2", straw_dark="#cdb57a", leaf="#4f8a3a", leaf_light="#7fb24c", leaf_dark="#2f5e2b", berry="#d4202c",
    berry_dark="#9c1420", berry_pale="#e6dfae", berry_pink="#f39a8a", petal="#fbf8f2", pollen="#f4c430", white="#ffffff",
    terracotta="#c8784e", iron="#3d3f44", canvas="#f2e8d4", pink="#f19cb0", lilac="#b39ddb", butter="#f6d86b",
    coral="#f08a6b", blue="#7da7d9", red="#d9473f", orange="#e8893a", chalk="#2f3a35", hay="#dcb664", tan="#d9a46a",
    fur="#d98b4a", fur_light="#f6e8d2", nose="#2a2220", pad="#4f8f3f", pad_light="#86b85a", lotus="#f6b3c6",
    lotus_light="#fde3ea", feather="#fbf7f0", comb="#df3f37", beak="#f0b43c", hen="#b8743f", hen_dark="#8a5228",
    awning1="#e8606f", awning2="#fff4e6", basket="#c2955c", blue_paint="#7fa9c6").items()}


# =============================================================================================
# Strawberry plants (the farm's heart)
# =============================================================================================
@prim
def leaflet(part, base, direction, length, width, droop, rnd, color):
    """One serrated, cupped, ovate leaflet; `direction` is a unit vector in the leaf plane."""
    d = Vector(direction).normalized()
    side = d.cross(Vector((0, 0, 1)))
    if side.length < 1e-4:
        side = Vector((1, 0, 0))
    side.normalize()
    up = side.cross(d).normalized()
    n_teeth = 7
    outline = []
    for i in range(n_teeth * 2 + 1):
        t = i / (n_teeth * 2)
        w = width * math.sin(math.pi * t ** 0.85) * (1.0 - 0.15 * t)
        serr = 1.0 if i % 2 == 0 else 0.86
        outline.append((t, w * serr))
    bm = CUR.bm(part)
    n0 = len(bm.faces)
    layer = bm.loops.layers.color["Col"]

    def P(t, s):
        """Leaf-local (t along the midrib 0..1, s across -1..1) -> world: cupped and drooping."""
        x = d * (length * t)
        y = side * (s)
        z = up * (abs(s) / max(width, 1e-6)) ** 2 * width * 0.32 - Vector((0, 0, 1)) * droop * t * t * length
        return Vector(base) + x + y + z

    mid = [bm.verts.new(P(t, 0.0)) for t, _ in outline]
    left = [bm.verts.new(P(t, -w)) for t, w in outline]
    right = [bm.verts.new(P(t, w)) for t, w in outline]
    dark, light = mix(color, C["leaf_dark"], 0.35), mix(color, C["leaf_light"], 0.35)
    for i in range(len(outline) - 1):
        for row, sgn in ((left, -1), (right, 1)):
            q = (mid[i], mid[i + 1], row[i + 1], row[i]) if sgn > 0 else (mid[i], row[i], row[i + 1], mid[i + 1])
            try:
                f = bm.faces.new(q)
            except ValueError:
                continue
            c = jit(mix(light, dark, 0.3 + 0.4 * (i / len(outline))), 0.04, rnd)
            for l in f.loops:
                l[layer] = (*c, 1.0)
    return _new_faces(bm, n0)


def strawberry(part, calyx_part, tip, axis, size, ripe, rnd):
    """A berry hanging from its stem: heart-shaped body (tip down) with a star calyx on top."""
    # a conical heart: pointed tip, widest high up, rounded shoulders under the calyx (tip at z = 0)
    shape = [(0.0, 0.0), (0.12, 0.07), (0.24, 0.18), (0.33, 0.31), (0.41, 0.45), (0.46, 0.6), (0.48, 0.73), (0.45, 0.84), (0.36, 0.93),
             (0.2, 0.99)]
    prof = [(r * size, z * size * 1.12) for r, z in shape]
    n = len(prof) - 1
    if ripe >= 1.0:
        cols = [C["berry"]] * (n - 2) + [mix(C["berry"], C["berry_pale"], 0.12), mix(C["berry"], C["berry_pale"], 0.28)]
    elif ripe > 0.4:
        cols = [C["berry"], C["berry"], mix(C["berry"], C["berry_pink"], 0.5), C["berry_pink"], mix(C["berry_pink"], C["berry_pale"], 0.5),
                C["berry_pale"], C["berry_pale"], C["berry_pale"], C["berry_pale"]]
    else:
        g = lin("#b8cc84")
        cols = [g, g, mix(g, C["berry_pale"], 0.3)] + [mix(g, C["berry_pale"], 0.6)] * (n - 3)
    a = Vector(axis).normalized()
    q = Vector((0, 0, 1)).rotation_difference(a)
    eul = q.to_euler("XYZ")
    rot = tuple(math.degrees(x) for x in eul)
    lathe(part, C["berry"], tip, prof, seg=12, rot=rot, var=0.03, colors=cols)
    top = Vector(tip) + a * size * 1.1
    # calyx: 7 small pointed sepals splayed over the shoulders
    for k in range(7):
        ang = 2 * math.pi * k / 7 + rnd.uniform(-0.2, 0.2)
        side = (q @ Vector((math.cos(ang), math.sin(ang), 0))).normalized()
        p0 = top
        p1 = top + side * size * 0.42 - a * size * 0.12
        perp = side.cross(a).normalized() * size * 0.09
        poly(calyx_part, jit(C["leaf"], 0.08, rnd), [p0 + perp, p0 - perp, p1], var=0.0, both=True)
    return top


def flower(part, center, normal, size, rnd):
    """Five white petals around a yellow center (strawberry blossom)."""
    n = Vector(normal).normalized()
    ref = Vector((1, 0, 0)) if abs(n.x) < 0.9 else Vector((0, 1, 0))
    u = n.cross(ref).normalized()
    v = n.cross(u).normalized()
    c = Vector(center)
    for k in range(5):
        a = 2 * math.pi * k / 5 + rnd.uniform(-0.1, 0.1)
        d = u * math.cos(a) + v * math.sin(a)
        e = u * math.cos(a + math.pi / 2) + v * math.sin(a + math.pi / 2)
        pts = []
        for j in range(7):
            t = j / 6
            ang = math.pi * t
            r = size * (0.55 + 0.45 * math.sin(ang))
            pts.append(c + d * (size * 0.18 + math.cos(ang - math.pi / 2) * 0.0) + d * (r * math.sin(ang) * 0.9) +
                       e * (math.cos(ang) * size * 0.42) + n * (size * 0.12 * math.sin(ang)))
        poly(part, jit(C["petal"], 0.02, rnd), [c + n * size * 0.04] + pts, var=0.0, both=True)
    ball(part, C["pollen"], tuple(c + n * size * 0.08), size * 0.22, scale=(1, 1, 0.55), subdiv=1, var=0.05)


def strawberry_plant(x, y, rnd, scale=1.0, berries=(5, 8), flowers=(1, 3), ripe_bias=0.86, leaves=(11, 15), lush=1.0):
    """One plant at (x, y) on the bed top (z = 0 local): crown of trifoliate leaves, hanging berries, blossoms."""
    s = scale
    nleaf = rnd.randint(*leaves)
    for k in range(nleaf):
        yaw = 2 * math.pi * k / nleaf + rnd.uniform(-0.25, 0.25)
        tilt = rnd.uniform(0.35, 0.95)  # how far the petiole leans out
        L = rnd.uniform(0.09, 0.17) * s * lush
        base = Vector((x, y, 0.0))
        out = Vector((math.cos(yaw), math.sin(yaw), 0))
        top = base + out * (L * math.sin(tilt)) + Vector((0, 0, L * math.cos(tilt) + 0.02 * s))
        mid = base + out * (L * math.sin(tilt) * 0.4) + Vector((0, 0, L * math.cos(tilt) * 0.75))
        tube("stem", jit(lin("#6f9a3c"), 0.08, rnd), [base, mid, top], 0.0045 * s, seg=4, r_end=0.003 * s)
        lc = jit(mix(C["leaf"], C["leaf_light"] if k % 3 == 0 else C["leaf_dark"], rnd.uniform(0.0, 0.4)), 0.05, rnd)
        for j, off in enumerate((-0.75, 0.0, 0.75)):
            dirv = Vector((math.cos(yaw + off), math.sin(yaw + off), 0.25 + rnd.uniform(-0.1, 0.25) - 0.15 * abs(off)))
            ln = rnd.uniform(0.085, 0.11) * s * (1.1 if j == 1 else 0.92) * lush
            leaflet("leaf", top, dirv, ln, ln * 0.4, 0.3, rnd, lc)
    # berries hang on arching runners at the edge of the crown, resting near the straw
    nb = rnd.randint(*berries)
    for k in range(nb):
        yaw = rnd.uniform(0, 2 * math.pi)
        r = rnd.uniform(0.12, 0.22) * s
        ripe = 1.0 if rnd.random() < ripe_bias else (0.6 if rnd.random() < 0.6 else 0.2)
        size = rnd.uniform(0.04, 0.056) * s * (1.0 if ripe >= 1 else 0.8)
        hang = Vector((x + math.cos(yaw) * r, y + math.sin(yaw) * r, size * 1.05 + rnd.uniform(0.0, 0.05) * s))
        axis = Vector((math.cos(yaw) * 0.6, math.sin(yaw) * 0.6, 1.0 + rnd.uniform(-0.2, 0.3)))
        tip = hang - axis.normalized() * size * 1.0
        top = strawberry("berry" if ripe >= 1 else "berry_unripe", "calyx", tuple(tip), tuple(axis), size, ripe, rnd)
        apex = Vector((x + math.cos(yaw) * r * 0.45, y + math.sin(yaw) * r * 0.45, top.z + 0.07 * s))
        tube("stem", jit(lin("#7a9a3e"), 0.08, rnd), [Vector((x, y, 0.02)), apex, top], 0.0028 * s, seg=4)
    nf = rnd.randint(*flowers)
    for k in range(nf):
        yaw = rnd.uniform(0, 2 * math.pi)
        r = rnd.uniform(0.05, 0.14) * s
        h = rnd.uniform(0.16, 0.24) * s * lush
        c = Vector((x + math.cos(yaw) * r, y + math.sin(yaw) * r, h))
        tube("stem", jit(lin("#7a9a3e"), 0.08, rnd), [Vector((x, y, 0.02)), (Vector((x, y, 0)) + c) / 2 + Vector((0, 0, 0.04)), c],
             0.0025 * s, seg=4)
        nrm = Vector((math.cos(yaw) * 0.5, math.sin(yaw) * 0.5, 1.0))
        flower("flower", c, nrm, 0.027 * s, rnd)


def bed_mound(length, width, height, x0=0.0):
    """A raised soil bed with rounded shoulders along X (length), bottom at z = 0."""
    nu, nv = max(4, int(length / 0.25)), 10

    def pf(u, v):
        x = x0 - length / 2 + length * u
        s = -1.0 + 2.0 * v
        prof = (1 - abs(s) ** 2.6)
        end = min(1.0, min(u, 1 - u) * length / 0.3)
        z = height * prof * (0.25 + 0.75 * end) + 0.012 * math.sin(x * 9.1 + s * 5.3)
        return (x, s * width / 2 * (1.0 + 0.12 * (1 - end)), max(0.0, z))

    grid_surface("soil", pf, nu, nv, lambda u, v: jit(mix(C["soil"], C["soil_dark"], 0.5 * abs(v - 0.5) * 2), 0.08))


def straw_mulch(length, width, height, rnd, x0=0.0, count=None):
    """Loose straw strands lying on top of the bed (short flat slivers)."""
    count = count or int(length * width * 1500)
    for _ in range(count):
        x = x0 + rnd.uniform(-length / 2 + 0.1, length / 2 - 0.1)
        s = rnd.uniform(-0.95, 0.95)
        y = s * width / 2
        z = height * (1 - abs(s) ** 2.6) + 0.006
        a = rnd.uniform(0, math.pi)
        L = rnd.uniform(0.06, 0.16)
        d = Vector((math.cos(a), math.sin(a), 0)) * L / 2
        w = Vector((-math.sin(a), math.cos(a), 0)) * 0.0045
        c = Vector((x, y, z))
        tiltz = Vector((0, 0, rnd.uniform(-0.01, 0.015)))
        col = jit(mix(C["straw"], C["straw_dark"], rnd.uniform(0, 0.6)), 0.05, rnd)
        poly("straw", col, [c - d - w, c + d - w + tiltz, c + d + w + tiltz, c - d + w], var=0.0)


def strawberry_row(length=16.0, spacing=0.42, seed=1):
    asset("strawberry_row")
    rnd = random.Random(seed)
    width, height = 0.78, 0.2
    bed_mound(length, width, height)
    straw_mulch(length, width * 0.92, height, rnd)
    n = int((length - 0.4) / spacing)
    for i in range(n):
        x = -length / 2 + 0.3 + i * spacing + rnd.uniform(-0.04, 0.04)
        for lane in (-0.17, 0.17):
            yy = lane + rnd.uniform(-0.03, 0.03)
            if rnd.random() < 0.06:
                continue  # a gap here and there
            plant_origin = Vector((x + (spacing / 2 if lane > 0 else 0), yy, height * (1 - abs(yy * 2 / width) ** 2.6)))
            _plant_at(plant_origin, rnd, scale=rnd.uniform(1.1, 1.35))
    return CUR


def _plant_at(origin, rnd, **kw):
    """Builds one plant into the current asset, translated to `origin`."""
    m = CUR.mark()
    strawberry_plant(0.0, 0.0, rnd, **kw)
    CUR.translate_since(m, tuple(origin))


def strawberry_single(stage):
    """The gameplay plant: 'sprout', 'young', 'flowering', 'ripe' (origin at the soil surface)."""
    asset(f"plant_{stage}")
    rnd = random.Random({"sprout": 3, "young": 5, "flowering": 7, "ripe": 9}[stage])
    if stage == "sprout":
        tube("stem", lin("#7fa84a"), [(0, 0, 0), (0.0, 0.0, 0.06)], 0.004, seg=5)
        for yaw in (0.3, 0.3 + math.pi):
            leaflet("leaf", (0, 0, 0.06), (math.cos(yaw), math.sin(yaw), 0.5), 0.07, 0.03, 0.1, rnd, C["leaf_light"])
    elif stage == "young":
        strawberry_plant(0, 0, rnd, scale=1.0, berries=(0, 0), flowers=(0, 0), leaves=(5, 6), lush=0.75)
    elif stage == "flowering":
        strawberry_plant(0, 0, rnd, scale=1.25, berries=(1, 2), flowers=(3, 4), ripe_bias=0.0, leaves=(8, 10))
    else:
        strawberry_plant(0, 0, rnd, scale=1.35, berries=(6, 8), flowers=(1, 2), ripe_bias=0.9, leaves=(10, 12))
    return CUR


def soil_patch():
    """A tilled square of soil (gameplay plot) with furrow ridges and a little stone border."""
    asset("soil_patch")
    rnd = random.Random(21)

    def pf(u, v):
        x, y = -0.6 + 1.2 * u, -0.6 + 1.2 * v
        edge = min(u, 1 - u, v, 1 - v)
        z = 0.08 * min(1.0, edge / 0.12) + 0.018 * math.sin(v * math.pi * 5) + 0.006 * math.sin(u * 17 + v * 11)
        return (x, y, z)

    grid_surface("soil", pf, 16, 16, lambda u, v: jit(mix(C["soil"], C["soil_dark"], 0.5 + 0.5 * math.sin(v * math.pi * 5)), 0.06, rnd))
    for k in range(14):
        a = 2 * math.pi * k / 14
        p = (math.cos(a) * 0.72, math.sin(a) * 0.72, 0.03)
        ball("stone", jit(C["stone"], 0.12, rnd), (max(-0.66, min(0.66, p[0] * 1.15)), max(-0.66, min(0.66, p[1] * 1.15)), 0.03),
             0.07, scale=(1.2, 1, 0.6), subdiv=1, noise=0.2)
    return CUR


# =============================================================================================
# Buildings
# =============================================================================================
def gable_roof(part, cx, cy, z0, w, d, rise, over=0.35, thick=0.14, color=None, rows=True, ridge_part=None):
    """A gable roof (ridge along Y) of overlapping shingle rows; eaves at z0."""
    color = color or C["roof"]
    hw = w / 2 + over
    L = math.hypot(hw, rise)
    ang = math.degrees(math.atan2(rise, hw))
    n = max(3, int(L / 0.28)) if rows else 1
    for sgn in (-1, 1):
        for i in range(n):
            t0 = i / n
            # each course is a slab slightly lifted at its lower edge (shingle overlap)
            px = cx + sgn * (hw - (hw * (t0 + 0.5 / n)))
            pz = z0 + rise * (t0 + 0.5 / n) + 0.02
            col = jit(mix(color, C["roof_dark"], 0.25 * ((i + 1) % 2)), 0.05)
            box(part, col, (px, cy, pz), (L / n + 0.03, d + 2 * over, thick * 0.6), rot=(0, sgn * ang + sgn * 3.5, 0),
                bevel=0.03, var=0.05)
    box(ridge_part or part, mix(color, C["roof_dark"], 0.5), (cx, cy, z0 + rise + 0.06), (0.24, d + 2 * over + 0.05, 0.2), bevel=0.06)


def window(cx, cy, cz, w, h, facing=-1, shutters=True, box_flowers=False, color_frame=None, axis="y"):
    """A framed window on a wall facing -Y (facing=-1) or +Y; axis='x' for walls facing +-X."""
    fc = color_frame or C["trim"]
    def P(lx, ly, lz):  # local: x along the wall, y outward, z up
        if axis == "y":
            return (cx + lx, cy + facing * ly, cz + lz)
        return (cx + facing * ly, cy + lx, cz + lz)
    def S(sx, sy, sz):
        return (sx, sy, sz) if axis == "y" else (sy, sx, sz)
    box("glow", C["glow"], P(0, 0.02, 0), S(w, 0.04, h), bevel=0.0)
    for zz in (-h / 2, h / 2):
        box("paint", fc, P(0, 0.06, zz), S(w + 0.16, 0.1, 0.09), bevel=0.02)
    for xx in (-w / 2, w / 2):
        box("paint", fc, P(xx, 0.06, 0), S(0.09, 0.1, h + 0.1), bevel=0.02)
    box("paint", fc, P(0, 0.07, 0), S(0.05, 0.06, h), bevel=0.01)
    box("paint", fc, P(0, 0.07, 0), S(w, 0.06, 0.05), bevel=0.01)
    box("paint", fc, P(0, 0.12, -h / 2 - 0.06), S(w + 0.3, 0.2, 0.07), bevel=0.02)  # sill
    if shutters:
        for sgn in (-1, 1):
            xx = sgn * (w / 2 + 0.24)
            box("paint", C["sky"], P(xx, 0.06, 0), S(0.36, 0.05, h + 0.06), bevel=0.02)
            for k in range(4):
                box("paint", mix(C["sky"], C["white"], 0.15), P(xx, 0.09, -h / 2 + (k + 0.5) * h / 4), S(0.28, 0.02, 0.05), bevel=0.008)
    if box_flowers:
        box("paint", C["wood"], P(0, 0.25, -h / 2 - 0.22), S(w + 0.2, 0.26, 0.22), bevel=0.03)
        rnd = random.Random(int(cx * 13 + cy * 7 + cz * 3))
        for k in range(9):
            lx = -w / 2 + (k + 0.5) * (w / 9) + rnd.uniform(-0.03, 0.03)
            ball("leafy", jit(C["leaf"], 0.1, rnd), P(lx, 0.25 + rnd.uniform(-0.05, 0.05), -h / 2 - 0.06), 0.09, subdiv=1, noise=0.25)
            col = [C["pink"], C["red"], C["coral"], C["butter"], C["lilac"]][rnd.randint(0, 4)]
            ball("petals", jit(col, 0.08, rnd), P(lx + rnd.uniform(-0.04, 0.04), 0.27 + rnd.uniform(-0.06, 0.06), -h / 2 + 0.02),
                 0.05, subdiv=1, noise=0.15)


def farmhouse():
    """A one-and-a-half storey cottage: stone plinth, cream plaster, timber trim, rose shingle roof,
    a dormer, a chimney, a deep front porch with posts, rail, bench, lantern and flower boxes."""
    asset("farmhouse")
    W, D, H = 8.4, 6.2, 3.3          # walls
    plinth = 0.55
    # plinth (rubble stone course)
    rnd = random.Random(5)
    box("stone", C["stone"], (0, 0, plinth / 2), (W + 0.3, D + 0.3, plinth), bevel=0.05)
    for k in range(70):
        side = k % 4
        t = rnd.uniform(-0.48, 0.48)
        if side in (0, 1):
            p = (t * W, (-1 if side == 0 else 1) * (D / 2 + 0.16), rnd.uniform(0.1, plinth - 0.1))
            s = (rnd.uniform(0.3, 0.55), 0.08, rnd.uniform(0.16, 0.26))
        else:
            p = ((-1 if side == 2 else 1) * (W / 2 + 0.16), t * D, rnd.uniform(0.1, plinth - 0.1))
            s = (0.08, rnd.uniform(0.3, 0.55), rnd.uniform(0.16, 0.26))
        box("stone", jit(mix(C["stone"], C["stone_dark"], rnd.uniform(0, 0.5)), 0.05, rnd), p, s, bevel=0.035)
    # walls
    box("plaster", C["cream"], (0, 0, plinth + H / 2), (W, D, H), bevel=0.04)
    # roof geometry (ridge along X): a base slab per side, then rows of rounded shingle tabs
    rise = 2.6
    hw = D / 2 + 0.55
    z_eave = plinth + H - 0.3
    L = math.hypot(hw, rise)
    ang = math.degrees(math.atan2(rise, hw))
    RW = W + 1.1
    # gable ends (plaster triangles under the roof) with a little attic window
    for sx in (-1, 1):
        x = sx * W / 2
        poly("plaster", C["cream"], [(x, -D / 2, plinth + H), (x, D / 2, plinth + H), (x, 0, z_eave + rise - 0.12)][::(1 if sx > 0 else -1)],
             var=0.02)
        window(x + sx * 0.02, 0, plinth + H + 0.8, 0.6, 0.7, facing=sx, axis="x", shutters=False)
        for sy in (-1, 1):  # bargeboards along the roof edge
            beam("paint", C["trim"], (sx * (RW / 2 - 0.04), sy * hw, z_eave - 0.1), (sx * (RW / 2 - 0.04), 0, z_eave + rise - 0.1), 0.1, 0.24)
    # corner boards and a belt course in trim
    for sx in (-1, 1):
        for sy in (-1, 1):
            box("paint", C["trim"], (sx * W / 2, sy * D / 2, plinth + H / 2), (0.2, 0.2, H), bevel=0.03)
    box("paint", C["trim"], (0, -D / 2 - 0.04, plinth + H - 0.08), (W + 0.1, 0.1, 0.16), bevel=0.03)
    box("paint", C["trim"], (0, D / 2 + 0.04, plinth + H - 0.08), (W + 0.1, 0.1, 0.16), bevel=0.03)
    roof_shades = [C["roof"], mix(C["roof"], C["roof_dark"], 0.35), mix(C["roof"], C["rose"], 0.5), mix(C["roof"], C["roof_dark"], 0.6)]
    for sgn in (-1, 1):
        nrm = Vector((0, sgn * math.sin(math.radians(ang)), math.cos(math.radians(ang))))
        box("roof", C["roof_dark"], tuple(Vector((0, sgn * hw / 2, z_eave + rise / 2)) + nrm * 0.0), (RW, L, 0.12),
            rot=(-sgn * ang, 0, 0), bevel=0.03)
        rows = int((L - 0.1) / 0.24)
        cols = int(RW / 0.34)
        for i in range(rows):
            dd = 0.12 + i * 0.24
            y = sgn * (hw - dd * math.cos(math.radians(ang)))
            z = z_eave + dd * math.sin(math.radians(ang))
            for k in range(cols + 1):
                xx = -RW / 2 + 0.17 + k * 0.34 - (0.17 if i % 2 else 0.0)
                if xx < -RW / 2 + 0.14 or xx > RW / 2 - 0.14:
                    continue
                c = jit(roof_shades[rnd.randint(0, 3)], 0.04, rnd)
                p = Vector((xx, y, z)) + nrm * 0.08
                box("roof", c, tuple(p), (0.32, 0.3, 0.05), rot=(-sgn * (ang + 5), 0, rnd.uniform(-2, 2)), bevel=0.022, seg=1, var=0.0)
    box("roof", C["roof_dark"], (0, 0, z_eave + rise + 0.1), (RW + 0.05, 0.4, 0.24), bevel=0.09)
    # dormer on the front slope
    dz = plinth + H + 0.55
    box("plaster", C["cream"], (0.9, -1.35, dz + 0.55), (1.5, 1.6, 1.3), bevel=0.04)
    window(0.9, -2.16, dz + 0.55, 0.75, 0.7, shutters=False)
    for sgn in (-1, 1):
        box("roof", C["roof"], (0.9 + sgn * 0.5, -1.45, dz + 1.42), (1.25, 2.1, 0.1), rot=(0, sgn * 38, 0), bevel=0.03)
    box("roof", C["roof_dark"], (0.9, -1.45, dz + 1.82), (0.18, 2.15, 0.16), bevel=0.05)
    # chimney
    box("stone", C["brick"], (-2.6, 1.0, plinth + H + rise * 0.6), (0.85, 0.85, rise * 1.5), bevel=0.04)
    box("stone", mix(C["brick"], C["stone_dark"], 0.4), (-2.6, 1.0, plinth + H + rise * 1.33), (1.0, 1.0, 0.2), bevel=0.04)
    for sx in (-0.18, 0.18):
        cyl("stone", C["terracotta"], (-2.6 + sx, 1.0, plinth + H + rise * 1.4), 0.12, 0.35, seg=10)
    # front: door, windows with shutters and flower boxes
    fy = -D / 2
    box("paint", C["sage"], (-0.6, fy - 0.04, plinth + 1.05), (1.0, 0.08, 2.1), bevel=0.03)
    for k in range(3):
        box("paint", mix(C["sage"], C["sage_dark"], 0.3), (-0.6, fy - 0.09, plinth + 0.45 + k * 0.6), (0.8, 0.03, 0.42), bevel=0.02)
    box("glow", C["glow"], (-0.6, fy - 0.1, plinth + 1.75), (0.36, 0.03, 0.36), bevel=0.05)
    box("paint", C["trim"], (-0.6, fy - 0.06, plinth + 2.15), (1.3, 0.12, 0.16), bevel=0.03)
    for sx in (-1, 1):
        box("paint", C["trim"], (-0.6 + sx * 0.58, fy - 0.06, plinth + 1.05), (0.12, 0.12, 2.2), bevel=0.03)
    cyl("metal", C["iron"], (-0.25, fy - 0.12, plinth + 1.0), 0.035, 0.06, rot=(90, 0, 0), seg=8)
    window(-2.75, fy, plinth + 1.55, 1.1, 1.15, box_flowers=True)
    window(1.65, fy, plinth + 1.55, 1.1, 1.15, box_flowers=True)
    window(3.2, fy, plinth + 1.55, 0.6, 1.0, shutters=False)
    # sides and back windows
    for sx in (-1, 1):
        window(sx * W / 2, -1.3, plinth + 1.55, 1.0, 1.1, facing=sx, axis="x", box_flowers=(sx < 0))
        window(sx * W / 2, 1.6, plinth + 1.55, 0.8, 1.0, facing=sx, axis="x")
    window(0.8, D / 2, plinth + 1.55, 1.0, 1.1, facing=1)
    # porch: deck, steps, posts, rail, roof
    PD = 2.3
    py0 = fy - PD / 2 - 0.1
    box("deck", C["honey"], (0, py0, plinth - 0.06), (W + 0.6, PD, 0.12), bevel=0.02)
    for k in range(int((W + 0.6) / 0.16)):
        x = -(W + 0.6) / 2 + 0.08 + k * 0.16
        box("deck", jit(C["honey"], 0.08, rnd), (x, py0, plinth + 0.005), (0.145, PD - 0.04, 0.03), bevel=0.008)
    box("deck", C["wood"], (0, py0, plinth / 2 - 0.1), (W + 0.5, PD - 0.1, plinth - 0.2), bevel=0.03)
    for k in range(3):
        box("deck", jit(C["honey"], 0.06, rnd), (-0.6, fy - PD - 0.25 - k * 0.3, plinth - 0.16 - k * 0.17), (1.6, 0.36, 0.1), bevel=0.02)
        box("deck", C["wood"], (-0.6, fy - PD - 0.25 - k * 0.3, (plinth - 0.16 - k * 0.17) / 2 - 0.03),
            (1.5, 0.3, plinth - 0.16 - k * 0.17), bevel=0.02)
    posts = [-W / 2 - 0.1, -2.0, 0.6, W / 2 + 0.1]
    for x in posts:
        box("paint", C["trim"], (x, fy - PD + 0.1, plinth + 1.25), (0.18, 0.18, 2.5), bevel=0.03)
        box("paint", C["trim"], (x, fy - PD + 0.1, plinth + 0.06), (0.26, 0.26, 0.12), bevel=0.03)
    # rail with balusters (leaving the steps open)
    for x0, x1 in ((-W / 2 - 0.1, -2.0), (0.6, W / 2 + 0.1)):
        box("paint", C["trim"], ((x0 + x1) / 2, fy - PD + 0.1, plinth + 0.92), (x1 - x0, 0.1, 0.08), bevel=0.02)
        box("paint", C["trim"], ((x0 + x1) / 2, fy - PD + 0.1, plinth + 0.18), (x1 - x0, 0.08, 0.06), bevel=0.02)
        nb = int((x1 - x0) / 0.16)
        for k in range(1, nb):
            box("paint", C["trim"], (x0 + k * (x1 - x0) / nb, fy - PD + 0.1, plinth + 0.55), (0.045, 0.045, 0.72), bevel=0.01)
    for sx in (-1, 1):
        box("paint", C["trim"], (sx * (W / 2 + 0.1), fy - PD / 2 + 0.05, plinth + 0.92), (0.1, PD - 0.1, 0.08), bevel=0.02)
    # porch roof: a lean-to of shingles on a beam
    box("paint", C["trim"], (0, fy - PD + 0.1, plinth + 2.55), (W + 0.5, 0.24, 0.22), bevel=0.04)
    pr_ang = 14
    for i in range(6):
        t = (i + 0.5) / 6
        box("roof", jit(mix(C["roof"], C["roof_dark"], 0.3 * (i % 2)), 0.04, rnd),
            (0, fy - PD - 0.25 + (PD + 0.25) * t, plinth + 2.64 + (PD + 0.25) * t * math.tan(math.radians(pr_ang))),
            (W + 0.8, (PD + 0.4) / 6 + 0.06, 0.09), rot=(pr_ang + 2, 0, 0), bevel=0.03)
    box("paint", C["wood"], (0, fy - PD / 2, plinth + 2.6), (W + 0.4, PD, 0.04), bevel=0.0)  # ceiling boards
    # bench, lantern, potted plants, rocking chair-ish stool, doormat
    box("deck", C["wood"], (-2.85, fy - 0.45, plinth + 0.45), (1.5, 0.45, 0.07), bevel=0.02)
    box("deck", C["wood"], (-2.85, fy - 0.27, plinth + 0.75), (1.5, 0.06, 0.4), rot=(-8, 0, 0), bevel=0.02)
    for sx in (-0.65, 0.65):
        box("deck", C["wood_dark"], (-2.85 + sx, fy - 0.45, plinth + 0.22), (0.07, 0.4, 0.45), bevel=0.015)
    box("paint", C["coral"], (-3.05, fy - 0.5, plinth + 0.53), (0.42, 0.35, 0.12), bevel=0.05)  # cushion
    box("paint", C["sky"], (-2.55, fy - 0.45, plinth + 0.53), (0.4, 0.33, 0.11), bevel=0.05)
    box("cloth", lin("#c9a46a"), (-0.6, fy - 0.55, plinth + 0.03), (1.1, 0.6, 0.02), bevel=0.01)  # mat
    cyl("metal", C["iron"], (0.25, fy - 0.12, plinth + 2.0), 0.02, 0.2, rot=(90, 0, 0), seg=6)
    lantern_at((0.25, fy - 0.32, plinth + 1.75))
    for x, col in ((1.2, C["pink"]), (-1.55, C["butter"]), (3.6, C["lilac"])):
        pot_plant((x, fy - 0.35, plinth), col)
    # a little string of fairy lights along the porch beam
    fairy_lights([(-W / 2 - 0.1, fy - PD + 0.02, plinth + 2.42), (W / 2 + 0.1, fy - PD + 0.02, plinth + 2.42)], sag=0.22, n=26)
    return CUR


def lantern_at(p):
    x, y, z = p
    cyl("metal", C["iron"], (x, y, z - 0.18), 0.1, 0.03, seg=8)
    box("glow", C["glow"], (x, y, z - 0.03), (0.13, 0.13, 0.24), bevel=0.02)
    for sx in (-1, 1):
        for sy in (-1, 1):
            box("metal", C["iron"], (x + sx * 0.075, y + sy * 0.075, z - 0.03), (0.025, 0.025, 0.3), bevel=0.005)
    cyl("metal", C["iron"], (x, y, z + 0.12), 0.11, 0.12, r2=0.02, seg=8)


def pot_plant(p, col):
    x, y, z = p
    rnd = random.Random(int(x * 100))
    lathe("pot", C["terracotta"], (x, y, z), [(0.13, 0.0), (0.18, 0.28), (0.21, 0.3), (0.21, 0.36), (0.17, 0.36)], seg=14)
    for k in range(6):
        ball("leafy", jit(C["leaf"], 0.1, rnd), (x + rnd.uniform(-0.1, 0.1), y + rnd.uniform(-0.1, 0.1), z + 0.42 + rnd.uniform(0, 0.12)),
             0.11, subdiv=1, noise=0.3)
    for k in range(7):
        ball("petals", jit(col, 0.08, rnd), (x + rnd.uniform(-0.14, 0.14), y + rnd.uniform(-0.14, 0.14), z + 0.52 + rnd.uniform(0, 0.12)),
             0.055, subdiv=1, noise=0.2)


def fairy_lights(ends, sag=0.2, n=20, bulb=0.035):
    a, b = Vector(ends[0]), Vector(ends[1])
    pts = []
    for k in range(n + 1):
        t = k / n
        p = a.lerp(b, t) - Vector((0, 0, sag * 4 * t * (1 - t)))
        pts.append(p)
    tube("metal", C["iron"], pts, 0.006, seg=4)
    for k in range(1, n):
        ball("bulb", C["glow"], tuple(pts[k] - Vector((0, 0, 0.04))), bulb, scale=(1, 1, 1.3), subdiv=1)


def greenhouse():
    """A white-framed glasshouse with a pitched glass roof; benches of potted seedlings inside."""
    asset("greenhouse")
    W, D, H, rise = 4.2, 7.0, 2.2, 1.4
    rnd = random.Random(8)
    box("stone", C["stone"], (0, 0, 0.2), (W + 0.2, D + 0.2, 0.4), bevel=0.04)
    frame = C["trim"]
    ny = 7
    for i in range(ny + 1):
        y = -D / 2 + i * D / ny
        for sx in (-1, 1):
            box("paint", frame, (sx * W / 2, y, 0.4 + H / 2), (0.08, 0.08, H), bevel=0.015)
            # rafters
            beam("paint", frame, (sx * W / 2, y, 0.4 + H), (0, y, 0.4 + H + rise), 0.07, 0.09)
    for z in (0.4 + 0.75, 0.4 + H):
        for sx in (-1, 1):
            box("paint", frame, (sx * W / 2, 0, z), (0.08, D, 0.08), bevel=0.015)
    box("paint", frame, (0, 0, 0.4 + H + rise), (0.12, D + 0.1, 0.12), bevel=0.02)
    for sy in (-1, 1):
        y = sy * D / 2
        for x in (-W / 2, -0.55, 0.55, W / 2):
            zt = 0.4 + H + rise * (1 - abs(x) / (W / 2))
            box("paint", frame, (x, y, (0.4 + zt) / 2), (0.08, 0.08, zt - 0.4), bevel=0.015)
        box("paint", frame, (0, y, 0.4 + 0.75), (W, 0.08, 0.08), bevel=0.015)
        box("paint", frame, (0, y, 0.4 + H), (W, 0.08, 0.08), bevel=0.015)
        for sx in (-1, 1):
            beam("paint", frame, (sx * W / 2, y, 0.4 + H), (0, y, 0.4 + H + rise), 0.08, 0.08)
    # glass skins: walls, gable ends, roof
    g = C["glass"]
    for sx in (-1, 1):
        poly("glass", g, [(sx * W / 2, -D / 2, 0.4), (sx * W / 2, D / 2, 0.4), (sx * W / 2, D / 2, 0.4 + H), (sx * W / 2, -D / 2, 0.4 + H)][::sx],
             var=0.0)
        poly("glass", g, [(sx * W / 2, -D / 2, 0.4 + H), (sx * W / 2, D / 2, 0.4 + H), (0, D / 2, 0.4 + H + rise), (0, -D / 2, 0.4 + H + rise)][::sx],
             var=0.0)
    for sy in (-1, 1):
        y = sy * D / 2
        pts = [(-W / 2, y, 0.4), (W / 2, y, 0.4), (W / 2, y, 0.4 + H), (0, y, 0.4 + H + rise), (-W / 2, y, 0.4 + H)]
        if sy < 0:  # front: an open door gap in the middle
            poly("glass", g, [(-W / 2, y, 0.4), (-0.55, y, 0.4), (-0.55, y, 0.4 + H), (-W / 2, y, 0.4 + H)][::-1], var=0.0)
            poly("glass", g, [(0.55, y, 0.4), (W / 2, y, 0.4), (W / 2, y, 0.4 + H), (0.55, y, 0.4 + H)][::-1], var=0.0)
            poly("glass", g, [(-W / 2, y, 0.4 + H), (W / 2, y, 0.4 + H), (0, y, 0.4 + H + rise)][::-1], var=0.0)
            poly("glass", g, [(-0.55, y, 0.4 + 2.05), (0.55, y, 0.4 + 2.05), (0.55, y, 0.4 + H), (-0.55, y, 0.4 + H)][::-1], var=0.0)
            box("paint", frame, (0, y, 0.4 + 2.05), (1.1, 0.08, 0.08), bevel=0.015)
        else:
            poly("glass", g, pts, var=0.0)
    # benches with pots and seedlings; a hanging basket
    for sx in (-1, 1):
        x = sx * (W / 2 - 0.55)
        box("deck", C["wood"], (x, 0.2, 0.4 + 0.8), (0.85, D - 1.4, 0.06), bevel=0.015)
        for y in (-D / 2 + 0.9, 0.2, D / 2 - 0.5):
            for xx in (-0.35, 0.35):
                box("deck", C["wood_dark"], (x + xx, y, 0.4 + 0.4), (0.06, 0.06, 0.8), bevel=0.01)
        for k in range(10):
            y = -D / 2 + 1.1 + k * 0.56
            for xx in (-0.2, 0.2):
                px, py, pz = x + xx + rnd.uniform(-0.03, 0.03), y + rnd.uniform(-0.05, 0.05), 0.4 + 0.83
                lathe("pot", C["terracotta"], (px, py, pz), [(0.08, 0), (0.11, 0.16), (0.125, 0.18), (0.11, 0.18)], seg=10)
                if rnd.random() < 0.3:
                    for j in range(5):
                        ball("leafy", jit(C["leaf_light"], 0.1, rnd), (px + rnd.uniform(-0.05, 0.05), py + rnd.uniform(-0.05, 0.05),
                                                                         pz + 0.25 + rnd.uniform(0, 0.1)), 0.07, subdiv=1, noise=0.3)
                    col = [C["pink"], C["butter"], C["coral"], C["red"]][rnd.randint(0, 3)]
                    for j in range(4):
                        ball("petals", jit(col, 0.08, rnd), (px + rnd.uniform(-0.07, 0.07), py + rnd.uniform(-0.07, 0.07),
                                                             pz + 0.33 + rnd.uniform(0, 0.06)), 0.04, subdiv=1)
                else:
                    _r = random.Random(rnd.random())
                    m = CUR.mark()
                    strawberry_plant(0, 0, _r, scale=0.75, berries=(0, 3), flowers=(1, 2), leaves=(5, 7), lush=0.8)
                    CUR.translate_since(m, (px, py, pz + 0.17))
    # hanging baskets of trailing flowers along the ridge, a potting table at the back, crates in the aisle
    for k, y in enumerate((-2.2, 0.0, 2.2)):
        top = 0.4 + H + rise - 0.1
        bz = 0.4 + H + 0.15
        tube("metal", C["iron"], [(0, y, top), (0, y, bz + 0.3)], 0.006, seg=4)
        lathe("basket", C["basket"], (0, y, bz), [(0.0, 0.0), (0.16, 0.02), (0.24, 0.16), (0.25, 0.2)], seg=14, caps=False)
        for j in range(9):
            a = 2 * math.pi * j / 9
            ball("leafy", jit(C["leaf"], 0.1, rnd), (math.cos(a) * 0.2, y + math.sin(a) * 0.2, bz + 0.12 + rnd.uniform(-0.12, 0.06)), 0.08,
                 subdiv=1, noise=0.3)
            ball("petals", jit([C["pink"], C["lilac"], C["coral"]][k], 0.08, rnd),
                 (math.cos(a) * 0.24, y + math.sin(a) * 0.24, bz + 0.05 + rnd.uniform(-0.2, 0.08)), 0.04, subdiv=1)
    box("deck", C["honey"], (0, D / 2 - 0.55, 0.4 + 0.85), (2.0, 0.7, 0.06), bevel=0.015)
    for sx in (-0.9, 0.9):
        for sy in (D / 2 - 0.85, D / 2 - 0.25):
            box("deck", C["wood_dark"], (sx, sy, 0.4 + 0.42), (0.06, 0.06, 0.84), bevel=0.01)
    lathe("cloth", jit(C["canvas"], 0.04, rnd), (-0.55, D / 2 - 0.55, 0.4 + 0.88),
          [(0.0, 0.0), (0.2, 0.02), (0.22, 0.2), (0.17, 0.38), (0.0, 0.42)], seg=10, scale=(1.0, 0.75, 1.0))
    for k in range(5):
        lathe("pot", C["terracotta"], (0.1 + k * 0.17, D / 2 - 0.45 + (k % 2) * 0.12, 0.4 + 0.88),
              [(0.06, 0), (0.08, 0.12), (0.09, 0.13), (0.08, 0.13)], seg=10)
    crate_at((0.15, -0.6, 0.4), rot=(0, 0, 8), fill="berries", rnd=rnd)
    crate_at((-0.1, 0.9, 0.4), rot=(0, 0, -5), fill="berries", rnd=rnd)
    # string lights along the ridge inside, and a watering can by the door
    fairy_lights([(0, -D / 2 + 0.3, 0.4 + H + rise - 0.22), (0, D / 2 - 0.3, 0.4 + H + rise - 0.22)], sag=0.3, n=22, bulb=0.03)
    watering_can_at((-0.95, -D / 2 - 0.45, 0.0))
    return CUR


def watering_can_at(p):
    x, y, z = p
    lathe("metal_paint", C["blue_paint"], (x, y, z), [(0.15, 0), (0.16, 0.05), (0.16, 0.3), (0.12, 0.33), (0.0, 0.33)], seg=14)
    tube("metal_paint", C["blue_paint"], [(x + 0.12, y, z + 0.06), (x + 0.3, y, z + 0.22), (x + 0.42, y, z + 0.36)], 0.025, seg=6)
    cyl("metal_paint", C["blue_paint"], (x + 0.42, y, z + 0.34), 0.045, 0.05, rot=(0, 50, 0), seg=8)
    tube("metal_paint", C["blue_paint"], [(x - 0.08, y, z + 0.33), (x - 0.02, y, z + 0.47), (x + 0.08, y, z + 0.33)], 0.016, seg=5)


def windmill():
    """A tapering octagonal smock mill on a stone base with a boat-shaped cap; sails are separate."""
    asset("windmill")
    rnd = random.Random(12)
    lathe("stone", C["stone"], (0, 0, 0), [(3.1, 0), (3.0, 1.6), (2.95, 1.75), (0.0, 1.75)], seg=8, var=0.03)
    for k in range(8 * 4):
        a = 2 * math.pi * (k % 8) / 8 + math.pi / 8 + (0.2 if (k // 8) % 2 else -0.2)
        z = 0.25 + (k // 8) * 0.36
        box("stone", jit(mix(C["stone"], C["stone_dark"], rnd.uniform(0, 0.5)), 0.05, rnd),
            (math.cos(a) * 3.02, math.sin(a) * 3.02, z), (0.1, 0.9, 0.26), rot=(0, 0, math.degrees(a)), bevel=0.03)
    H = 8.5
    # weatherboarded body: horizontal boards around an octagon tapering 2.7 -> 1.75
    n = int(H / 0.32)
    for i in range(n):
        t = (i + 0.5) / n
        r = 2.7 + (1.75 - 2.7) * t
        z = 1.75 + H * t
        col = jit(C["cream"], 0.03, rnd)
        col = jit(mix(C["cream"], C["trim"], 0.5 if i % 2 else 0.0), 0.02, rnd)
        lathe("boards", col, (0, 0, z - H / n / 2), [(r + 0.075, 0.0), (r + 0.075, 0.035), (r - 0.01, H / n + 0.01)], seg=8, rot=(0, 0, 22.5),
              var=0.0, caps=False)
    lathe("boards", C["cream"], (0, 0, 1.75), [(2.6, 0.0), (1.65, H), (0.0, H)], seg=8, rot=(0, 0, 22.5), var=0.0)
    # corner posts
    for k in range(8):
        a = math.radians(22.5 + 45 * k)
        beam("paint", C["wood_dark"], (math.cos(a) * 2.78, math.sin(a) * 2.78, 1.75), (math.cos(a) * 1.82, math.sin(a) * 1.82, 1.75 + H),
             0.14, 0.14)
    # stage (gallery) with railing at 1/3 height
    zs = 1.75 + 2.4
    lathe("deck", C["honey"], (0, 0, zs), [(3.6, 0), (3.6, 0.14), (0, 0.14)], seg=16)
    for k in range(16):
        a = 2 * math.pi * k / 16
        box("paint", C["trim"], (math.cos(a) * 3.5, math.sin(a) * 3.5, zs + 0.55), (0.08, 0.08, 1.0), bevel=0.015)
        a2 = 2 * math.pi * (k + 1) / 16
        beam("paint", C["trim"], (math.cos(a) * 3.5, math.sin(a) * 3.5, zs + 1.0), (math.cos(a2) * 3.5, math.sin(a2) * 3.5, zs + 1.0), 0.07, 0.07)
        beam("deck", C["wood_dark"], (math.cos(a) * 2.45, math.sin(a) * 2.45, zs - 0.9), (math.cos(a) * 3.5, math.sin(a) * 3.5, zs), 0.1, 0.1)
    # door and windows (front = -Y)
    box("paint", C["sage"], (0, -2.95, 0.0 + 1.05), (1.0, 0.2, 2.0), bevel=0.04)
    box("paint", C["trim"], (0, -3.02, 2.1), (1.3, 0.12, 0.16), bevel=0.03)
    for z, r in ((zs + 1.6, 2.27), (zs + 3.6, 2.02)):
        box("glow", C["glow"], (0, -r + 0.02, z), (0.55, 0.12, 0.7), bevel=0.02)
        box("paint", C["trim"], (0, -r - 0.04, z), (0.7, 0.08, 0.85), bevel=0.02)
        box("paint", C["trim"], (0, -r - 0.07, z), (0.06, 0.04, 0.7), bevel=0.01)
    # life at the foot of the mill: flour sacks, barrels, a cart wheel, window boxes
    for k, (x, y, rz) in enumerate(((1.0, -3.35, 10), (1.45, -3.25, -20), (1.2, -3.0, 40))):
        lathe("cloth", jit(C["canvas"], 0.04, rnd), (x, y, 0.0), [(0.0, 0.0), (0.24, 0.02), (0.27, 0.25), (0.22, 0.5), (0.1, 0.58), (0.0, 0.6)],
              seg=10, rot=(0, 0, rz), scale=(1.0, 0.8, 1.0))
    for k, (x, y) in enumerate(((-1.3, -3.4), (-1.85, -3.1))):
        lathe("deck", C["wood"], (x, y, 0.0), [(0.26, 0.0), (0.31, 0.38), (0.26, 0.76), (0.0, 0.76)], seg=14)
        for z in (0.12, 0.64):
            lathe("metal", C["iron"], (x, y, z - 0.03), [(0.285, 0.0), (0.285, 0.05)], seg=14, caps=False, scale=(1.02, 1.02, 1.0))
    lathe("deck", C["wood_dark"], (2.2, -3.35, 0.55), [(0.55, -0.04), (0.55, 0.04), (0.47, 0.04), (0.47, -0.04)], seg=16, rot=(78, 0, 20))
    for z, r in ((zs + 1.6, 2.27), (zs + 3.6, 2.02)):
        box("paint", C["wood"], (0, -r - 0.18, z - 0.48), (0.7, 0.22, 0.16), bevel=0.03)
        for k in range(5):
            ball("petals", jit([C["pink"], C["red"], C["butter"], C["coral"]][k % 4], 0.08, rnd), (-0.28 + k * 0.14, -r - 0.2, z - 0.36), 0.06,
                 subdiv=1)
    # cap: a curved boat-shaped roof
    zc = 1.75 + H
    lathe("deck", C["wood_dark"], (0, 0, zc - 0.1), [(2.0, 0), (2.0, 0.25), (0, 0.25)], seg=16)
    # the cap: a boat-shaped dome of curved boarding (ridge along Y), shingle bands in two shades
    def cap_pt(u, v):
        yy = -2.3 + 4.6 * u
        taper = max(0.0, 1.0 - abs(2 * u - 1) ** 3.2) ** 0.5
        a = math.pi * v
        rx = 2.05 * (0.25 + 0.75 * taper)
        return (-rx * math.cos(a), yy, zc + 0.12 + 2.0 * (0.2 + 0.8 * taper) * math.sin(a) * 0.9)
    grid_surface("roof", cap_pt, 24, 14, lambda u, v: jit(mix(C["roof"], C["roof_dark"], 0.35 * (int(v * 14) % 2)), 0.03, rnd))
    for k in range(5):  # rafters showing on the cap
        u = 0.15 + 0.7 * k / 4
        pts = [Vector(cap_pt(u, j / 12)) * 1.0 for j in range(13)]
        cx_ = Vector((0, pts[0].y, zc + 0.12))
        tube("paint", C["wood_dark"], [cx_ + (p - cx_) * 1.03 for p in pts], 0.05, seg=5)
    box("roof", C["roof_dark"], (0, 0, zc + 0.12 + 1.8 + 0.1), (0.4, 1.0, 0.3), bevel=0.1)
    # windshaft housing poking out the front of the cap (sails mount here)
    cyl("deck", C["wood_dark"], (0, -1.6, zc + 1.2), 0.35, 1.2, rot=(90, 0, 0), seg=12)
    cyl("metal", C["iron"], (0, -2.75, zc + 1.2), 0.42, 0.12, rot=(90, 0, 0), seg=14)
    # tail pole and wheel at the back (turns the cap to the wind)
    beam("deck", C["wood_dark"], (0, 1.6, zc + 0.6), (0, 4.6, 0.6), 0.16, 0.16)
    lathe("deck", C["wood_dark"], (0, 4.6, 0.6), [(0.55, -0.06), (0.55, 0.06), (0.45, 0.06), (0.45, -0.06)], seg=14, rot=(0, 90, 0))
    for sx in (-1, 1):
        beam("deck", C["wood_dark"], (sx * 1.6, 2.4, zs), (0, 1.6, zc + 0.4), 0.1, 0.1)
    return CUR


def windmill_sails():
    """Four lattice sails with canvas, hub at the origin, sails in the XZ plane (turning about Y)."""
    asset("windmill_sails")
    rnd = random.Random(31)
    cyl("hub", C["wood_dark"], (0, 0.15, 0), 0.38, 0.3, rot=(90, 0, 0), seg=14)
    ball("hub", C["iron"], (0, -0.2, 0), 0.22, scale=(1, 0.8, 1), subdiv=2)
    for k in range(4):
        a = math.radians(45 + 90 * k)
        u = Vector((math.cos(a), 0, math.sin(a)))
        v = Vector((-math.sin(a), 0, math.cos(a)))
        beam("sail", C["wood"], (0, 0, 0), tuple(u * 8.6), 0.24, 0.2)
        L0, L1, w = 1.6, 8.4, 1.5
        # lattice: rails and bars
        for side, ww in ((1, w), (-1, 0.35)):
            beam("sail", C["wood"], tuple(u * L0 + v * ww * side), tuple(u * L1 + v * ww * side), 0.08, 0.08)
        nb = 12
        for j in range(nb + 1):
            t = L0 + (L1 - L0) * j / nb
            beam("sail", C["wood"], tuple(u * t - v * 0.35), tuple(u * t + v * w), 0.06, 0.06)
        # canvas: slightly billowing sheet on the wide side
        def pf(s, t, u=u, v=v):
            p = u * (L0 + 0.1 + (L1 - L0 - 0.2) * s) + v * (0.05 + (w - 0.12) * t)
            return tuple(p + Vector((0, -0.12 * math.sin(math.pi * t) * math.sin(math.pi * s), 0)))
        grid_surface("canvas", pf, 10, 4, lambda s, t: jit(C["canvas"], 0.03, rnd), both=True)
    return CUR


def market_stall():
    """A wooden farm stand with a scalloped striped awning, crates of berries, baskets and a chalkboard."""
    asset("market_stall")
    rnd = random.Random(44)
    W, D = 3.2, 1.3
    # counter
    box("deck", C["honey"], (0, 0, 0.92), (W, D, 0.08), bevel=0.02)
    for k in range(int(W / 0.2)):
        x = -W / 2 + 0.1 + k * 0.2
        box("deck", jit(C["wood"], 0.08, rnd), (x, -D / 2 + 0.03, 0.46), (0.18, 0.05, 0.86), bevel=0.012)
    box("deck", C["wood_dark"], (0, 0, 0.46), (W - 0.1, D - 0.1, 0.86), bevel=0.02)
    # tilted display shelf at the front
    box("deck", C["honey"], (0, -0.25, 1.12), (W - 0.2, 0.7, 0.05), rot=(14, 0, 0), bevel=0.015)
    # posts and roof frame
    for sx in (-1, 1):
        for sy in (-1, 1):
            box("deck", C["wood"], (sx * (W / 2 - 0.06), sy * (D / 2 - 0.06), 1.25), (0.11, 0.11, 2.5), bevel=0.02)
    box("deck", C["wood"], (0, -D / 2 + 0.06, 2.45), (W + 0.1, 0.12, 0.12), bevel=0.02)
    box("deck", C["wood"], (0, D / 2 - 0.06, 2.75), (W + 0.1, 0.12, 0.12), bevel=0.02)
    # striped awning sloping forward with a scalloped valance
    nstr = 10
    ang = math.degrees(math.atan2(0.45, D + 0.6))
    for k in range(nstr):
        x = -W / 2 - 0.15 + (k + 0.5) * (W + 0.3) / nstr
        col = C["awning1"] if k % 2 == 0 else C["awning2"]
        def pf(s, t, x=x):
            yy = D / 2 + 0.05 - (D + 0.75) * t
            zz = 2.82 - 0.5 * t + 0.05 * math.sin(math.pi * s) * (1 - t)
            return (x - (W + 0.3) / nstr / 2 + (W + 0.3) / nstr * s, yy, zz)
        grid_surface("canvas", pf, 2, 6, lambda s, t, col=col: jit(col, 0.02, rnd), both=True)
        # scallop flap
        xc = x
        pts = [(xc - (W + 0.3) / nstr / 2, -D / 2 - 0.7, 2.32)]
        for j in range(7):
            a = math.pi * j / 6
            pts.append((xc - (W + 0.3) / nstr / 2 * math.cos(a), -D / 2 - 0.7, 2.32 - 0.18 * math.sin(a) - 0.02))
        pts.append((xc + (W + 0.3) / nstr / 2, -D / 2 - 0.7, 2.32))
        poly("canvas", col, pts[::-1], var=0.02, both=True)
    # crates of berries on the shelf and counter
    for k, x in enumerate((-1.05, -0.35, 0.35, 1.05)):
        crate_at((x, -0.25, 1.17), rot=(14, 0, rnd.uniform(-4, 4)), fill="berries" if k != 2 else "jam", rnd=rnd, size=(0.6, 0.42, 0.18))
    for x in (-1.15, 1.2):
        basket_at((x, 0.32, 0.96), rnd)
    # little paper price tags (chalk) on sticks
    for x in (-1.05, -0.35, 1.05):
        box("paint", lin("#f5ecd6"), (x, -0.62, 1.26), (0.16, 0.02, 0.1), bevel=0.005)
    # chalkboard A-frame in front
    for sy in (-1, 1):
        box("deck", C["wood"], (-2.2, -0.9 + sy * 0.18, 0.5), (0.62, 0.05, 0.95), rot=(sy * 12, 0, 0), bevel=0.02)
        box("chalk", C["chalk"], (-2.2, -0.9 + sy * 0.21, 0.52), (0.52, 0.02, 0.8), rot=(sy * 12, 0, 0), bevel=0.0)
    # bunting along the front beam
    for k in range(9):
        x = -W / 2 + 0.15 + k * (W - 0.3) / 8
        col = [C["awning1"], C["butter"], C["sky"], C["sage"]][k % 4]
        poly("canvas", col, [(x - 0.13, -D / 2 - 0.05, 2.38), (x + 0.13, -D / 2 - 0.05, 2.38), (x, -D / 2 - 0.05, 2.12)], var=0.02, both=True)
    return CUR


def crate_at(p, rot=(0, 0, 0), fill="berries", rnd=None, size=(0.6, 0.4, 0.28)):
    rnd = rnd or R
    w, d, h = size
    m = _mat(p, rot)

    def T(q):
        return tuple(m @ Vector(q))
    for z in (0.05, h - 0.05):
        for sy in (-1, 1):
            box("deck", jit(C["wood"], 0.08, rnd), T((0, sy * (d / 2 - 0.015), z)), (w, 0.03, 0.08), rot=rot, bevel=0.01)
        for sx in (-1, 1):
            box("deck", jit(C["wood"], 0.08, rnd), T((sx * (w / 2 - 0.015), 0, z)), (0.03, d - 0.03, 0.08), rot=rot, bevel=0.01)
    for sx in (-1, 1):
        for sy in (-1, 1):
            box("deck", C["wood_dark"], T((sx * (w / 2 - 0.03), sy * (d / 2 - 0.03), h / 2)), (0.045, 0.045, h), rot=rot, bevel=0.008)
    box("deck", C["wood"], T((0, 0, 0.015)), (w - 0.04, d - 0.04, 0.03), rot=rot, bevel=0.005)
    if fill == "berries":
        n = int(w * d * 520)
        for k in range(n):
            q = (rnd.uniform(-w / 2 + 0.05, w / 2 - 0.05), rnd.uniform(-d / 2 + 0.05, d / 2 - 0.05), h - 0.06 + rnd.uniform(-0.02, 0.03))
            ax = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.2, 0.6)))
            size_b = rnd.uniform(0.038, 0.05)
            tip = Vector(T(q)) - (m.to_3x3() @ ax.normalized()) * size_b * 0.5
            strawberry("berry", "calyx", tuple(tip), tuple(m.to_3x3() @ ax), size_b, 1.0, rnd)
    elif fill == "jam":
        for k in range(6):
            q = (-w / 2 + 0.1 + (k % 3) * (w - 0.2) / 2, -d / 4 + (k // 3) * d / 2, 0.03)
            c = T(q)
            lathe("jar", lin("#b3202a"), c, [(0.05, 0), (0.055, 0.1), (0.045, 0.11), (0.0, 0.11)], seg=10)
            cyl("paint", lin("#f2e6cc"), (c[0], c[1], c[2] + 0.105), 0.05, 0.03, seg=10)
            box("paint", C["awning1"], (c[0], c[1], c[2] + 0.13), (0.11, 0.11, 0.008), bevel=0.0)


def basket_at(p, rnd):
    x, y, z = p
    lathe("basket", C["basket"], (x, y, z), [(0.16, 0), (0.22, 0.16), (0.23, 0.18), (0.2, 0.18), (0.13, 0.02), (0, 0.02)], seg=16)
    tube("basket", C["basket"], [(x - 0.21, y, z + 0.17), (x - 0.14, y, z + 0.38), (x, y, z + 0.45), (x + 0.14, y, z + 0.38), (x + 0.21, y, z + 0.17)],
         0.015, seg=5)
    for k in range(26):
        a, r = rnd.uniform(0, 2 * math.pi), rnd.uniform(0, 0.17)
        q = Vector((x + math.cos(a) * r, y + math.sin(a) * r, z + 0.14 + rnd.uniform(0, 0.04)))
        ax = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(0, 0.6)))
        strawberry("berry", "calyx", tuple(q), tuple(ax), rnd.uniform(0.036, 0.046), 1.0, rnd)


def crate_stack():
    """Harvest crates stacked by the field: two full of berries, one empty, a basket."""
    asset("crate_stack")
    rnd = random.Random(52)
    crate_at((0, 0, 0), fill="berries", rnd=rnd)
    crate_at((0.65, 0.05, 0), rot=(0, 0, 6), fill="berries", rnd=rnd)
    crate_at((0.3, 0.02, 0.29), rot=(0, 0, -4), fill="berries", rnd=rnd)
    basket_at((-0.6, -0.2, 0), rnd)
    return CUR


# =============================================================================================
# Garden furniture: fences, sign, cart, hay, well-loved odds and ends
# =============================================================================================
def fence_rail(length=3.0):
    """A rustic split-rail fence segment along X, posts at both ends (origin at the left post)."""
    asset("fence_rail")
    rnd = random.Random(61)
    for x in (0.0, length):
        box("deck", jit(C["wood_grey"], 0.06, rnd), (x, 0, 0.55), (0.13, 0.13, 1.15), rot=(0, rnd.uniform(-2, 2), 0), bevel=0.03)
        cyl("deck", C["wood_dark"], (x, 0, 1.12), 0.08, 0.06, seg=8)
    for z in (0.45, 0.88):
        beam("deck", jit(C["wood_grey"], 0.08, rnd), (0.0, 0.02, z + rnd.uniform(-0.03, 0.03)), (length, 0.02, z + rnd.uniform(-0.03, 0.03)),
             0.08, 0.1)
    return CUR


def fence_picket(length=2.4):
    """A white picket fence segment along X (origin at the left post)."""
    asset("fence_picket")
    for x in (0.0, length):
        box("paint", C["trim"], (x, 0, 0.5), (0.1, 0.1, 1.0), bevel=0.02)
        ball("paint", C["trim"], (x, 0, 1.03), 0.07, subdiv=1)
    for z in (0.28, 0.72):
        box("paint", C["trim"], (length / 2, 0.06, z), (length, 0.04, 0.08), bevel=0.012)
    n = int(length / 0.16)
    for k in range(1, n):
        x = k * length / n
        box("paint", C["trim"], (x, 0.1, 0.43), (0.075, 0.03, 0.86), bevel=0.012)
        poly("paint", C["trim"], [(x - 0.0375, 0.115, 0.86), (x + 0.0375, 0.115, 0.86), (x, 0.115, 0.95)], var=0.0, both=True)
    return CUR


def farm_sign():
    """The farm's gate sign: two posts and a hanging board (text is added in the engine)."""
    asset("farm_sign")
    for sx in (-1, 1):
        box("deck", C["wood"], (sx * 1.2, 0, 1.2), (0.16, 0.16, 2.4), bevel=0.03)
    box("deck", C["wood"], (0, 0, 2.45), (2.8, 0.18, 0.16), bevel=0.03)
    for sx in (-0.75, 0.75):
        tube("metal", C["iron"], [(sx, 0, 2.37), (sx, 0, 2.05)], 0.012, seg=4)
    box("paint", C["cream"], (0, 0, 1.75), (2.0, 0.08, 0.62), bevel=0.04)
    box("paint", C["sage"], (0, 0, 1.75), (2.12, 0.06, 0.72), bevel=0.05)
    # a painted strawberry on each end of the board
    rnd = random.Random(70)
    for sx in (-0.82, 0.82):
        strawberry("berry", "calyx", (sx, -0.06, 1.6), (0, -0.4, 1.0), 0.2, 1.0, rnd)
    return CUR


def hay_bale():
    asset("hay_bale")
    rnd = random.Random(80)
    box("hay", C["hay"], (0, 0, 0.3), (1.0, 0.5, 0.6), bevel=0.07, seg=3)
    for x in (-0.22, 0.22):
        box("metal", lin("#8c6a3c"), (x, 0, 0.3), (0.025, 0.52, 0.62), bevel=0.01)
    for k in range(60):
        p = (rnd.uniform(-0.5, 0.5), rnd.choice((-0.255, 0.255)), rnd.uniform(0.05, 0.58))
        a = rnd.uniform(0, math.pi)
        d = Vector((math.cos(a), 0, math.sin(a))) * 0.06
        poly("hay", jit(C["straw"], 0.08, rnd), [Vector(p) - d, Vector(p) + d, Vector(p) + d + Vector((0, 0, 0.01))], var=0.0, both=True)
    return CUR


def wheelbarrow():
    asset("wheelbarrow")
    lathe("metal_paint", C["red"], (0, 0, 0.0), [(0.0, 0.0), (0.28, 0.0), (0.42, 0.3), (0.44, 0.32), (0.4, 0.32)], seg=4,
          rot=(0, 0, 45), scale=(1.5, 1.0, 1.0))
    tube("deck", C["wood_dark"], [(0.6, -0.25, 0.1), (1.4, -0.3, 0.45)], 0.025, seg=6)
    tube("deck", C["wood_dark"], [(0.6, 0.25, 0.1), (1.4, 0.3, 0.45)], 0.025, seg=6)
    lathe("metal", C["iron"], (-0.55, 0, 0.0), [(0.0, -0.05), (0.18, -0.05), (0.18, 0.05), (0.0, 0.05)], seg=14, rot=(90, 0, 0))
    for sy in (-1, 1):
        box("metal", C["iron"], (0.45, sy * 0.2, -0.05), (0.04, 0.04, 0.3), bevel=0.005)
    # filled with hay and a few berries on top
    ball("hay", C["hay"], (0.05, 0, 0.22), 0.36, scale=(1.3, 0.85, 0.35), subdiv=2, noise=0.08)
    return CUR


# =============================================================================================
# Pond life
# =============================================================================================
def lily_pads():
    """A cluster of lily pads (notched discs) with two pink water lilies; lies at z = 0."""
    asset("lily_pads")
    rnd = random.Random(90)
    for k in range(9):
        r = rnd.uniform(0.18, 0.36)
        cx, cy = rnd.uniform(-1.4, 1.4), rnd.uniform(-1.4, 1.4)
        notch = rnd.uniform(0, 2 * math.pi)
        pts = []
        for j in range(20):
            a = notch + 0.35 + (2 * math.pi - 0.7) * j / 19
            pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r, 0.01 + 0.012 * math.sin(j * 1.7)))
        pts.append((cx, cy, 0.005))
        base = jit(mix(C["pad"], C["pad_light"], rnd.uniform(0, 0.6)), 0.05, rnd)
        poly("pad", base, pts, var=0.0)
    for k in range(3):
        cx, cy = rnd.uniform(-1.0, 1.0), rnd.uniform(-1.0, 1.0)
        for ring, (n, L, up) in enumerate(((8, 0.14, 0.35), (7, 0.11, 0.8), (5, 0.08, 1.2))):
            for j in range(n):
                a = 2 * math.pi * j / n + ring * 0.4
                d = Vector((math.cos(a), math.sin(a), 0))
                e = Vector((-math.sin(a), math.cos(a), 0))
                base = Vector((cx, cy, 0.03))
                tipp = base + d * L * math.cos(up * 0.6) + Vector((0, 0, L * math.sin(up * 0.6) + 0.02))
                col = jit(mix(C["lotus"], C["lotus_light"], 0.3 + 0.3 * ring), 0.04, rnd)
                poly("lily", col, [base + e * 0.02, base + d * L * 0.5 + e * L * 0.32 + Vector((0, 0, 0.02 + 0.03 * up)), tipp,
                                   base + d * L * 0.5 - e * L * 0.32 + Vector((0, 0, 0.02 + 0.03 * up)), base - e * 0.02], var=0.0, both=True)
        ball("lily", C["pollen"], (cx, cy, 0.06), 0.03, subdiv=1)
    return CUR


def reeds():
    asset("reeds")
    rnd = random.Random(95)
    for k in range(26):
        x, y = rnd.uniform(-0.4, 0.4), rnd.uniform(-0.4, 0.4)
        h = rnd.uniform(0.7, 1.4)
        lean = Vector((rnd.uniform(-0.15, 0.15), rnd.uniform(-0.15, 0.15), 0))
        tube("leafy", jit(C["leaf"], 0.12, rnd), [(x, y, 0), Vector((x, y, h * 0.6)) + lean * 0.4, Vector((x, y, h)) + lean], 0.01, seg=4,
             r_end=0.002)
        if rnd.random() < 0.35:
            top = Vector((x, y, h)) + lean
            cyl("cattail", lin("#7a4a2a"), tuple(top - Vector((0, 0, 0.2))), 0.025, 0.16, seg=6)
    return CUR


# =============================================================================================
# Trees and bushes (storybook: puffy canopies of leafy blobs)
# =============================================================================================
def tree(name, seed, height=6.0, spread=2.6, blooms=None, fruit=None, palette=("#3f7a33", "#9ccc5a")):
    """A storybook deciduous tree: a short stout trunk and a full, low crown of puffy leaf clusters."""
    asset(name)
    rnd = random.Random(seed)
    bark = lin("#7b5a42")
    trunk_top = Vector((rnd.uniform(-0.25, 0.25), rnd.uniform(-0.25, 0.25), height * 0.42))
    tube("bark", bark, [(0, 0, 0), (0.05, 0.02, height * 0.18), tuple(trunk_top)], 0.3, seg=10, r_end=0.18)
    for k in range(3):  # root flare
        a = 2 * math.pi * k / 3 + rnd.uniform(0, 1)
        tube("bark", bark, [(math.cos(a) * 0.55, math.sin(a) * 0.55, -0.05), (math.cos(a) * 0.15, math.sin(a) * 0.15, 0.4)], 0.1, seg=6,
             r_end=0.13)
    blobs = []
    nb = rnd.randint(5, 7)
    for k in range(nb):
        a = 2 * math.pi * k / nb + rnd.uniform(-0.3, 0.3)
        r = spread * rnd.uniform(0.5, 0.72)
        z = height * rnd.uniform(0.5, 0.66)
        end = Vector((math.cos(a) * r * 0.6, math.sin(a) * r * 0.6, z))
        tube("bark", bark, [tuple(trunk_top), tuple(trunk_top.lerp(end, 0.5) + Vector((0, 0, 0.3))), tuple(end)], 0.13, seg=6, r_end=0.05)
        blobs.append((Vector((math.cos(a) * r, math.sin(a) * r, z)), spread * rnd.uniform(0.44, 0.56)))
    blobs.append((Vector((0, 0, height * 0.86)), spread * 0.6))
    for k in range(4):
        a = rnd.uniform(0, 2 * math.pi)
        blobs.append((Vector((math.cos(a) * spread * 0.38, math.sin(a) * spread * 0.38, height * rnd.uniform(0.72, 0.9))), spread * 0.48))
    base, top = lin(palette[0]), lin(palette[1])
    for c, r in blobs:
        hue = rnd.uniform(-0.08, 0.08)
        b0 = jit(mix(base, lin("#2a5228"), 0.35), 0.04, rnd)
        b1 = jit(mix(top, lin("#d8e27a"), 0.15 + hue), 0.04, rnd)
        ball("canopy", base, tuple(c), r, scale=(1.0, 1.0, 0.85), subdiv=3, noise=0.1, seed=rnd.random(),
             grad=(2, c.z - r * 0.9, c.z + r * 0.9, b0, b1))
        if blooms:
            for j in range(int(r * 16)):
                d = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.3, 1))).normalized()
                ball("blossom", jit(blooms, 0.08, rnd), tuple(c + d * r * 0.98), rnd.uniform(0.07, 0.11), subdiv=1)
        if fruit:
            for j in range(int(r * 7)):
                d = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.6, 0.6))).normalized()
                ball("fruit", jit(fruit, 0.08, rnd), tuple(c + d * r * 0.95), rnd.uniform(0.08, 0.1), subdiv=2)
    return CUR


def poplar(name, seed, height=11.0):
    """A tall columnar poplar (lane windbreaks): a slim trunk and a stack of narrow leafy lobes."""
    asset(name)
    rnd = random.Random(seed)
    tube("bark", lin("#7b6a58"), [(0, 0, 0), (0, 0, height * 0.35)], 0.2, seg=8, r_end=0.13)
    base, top = lin("#3d6e30"), lin("#a2c65a")
    n = 9
    for k in range(n):
        t = k / (n - 1)
        z = height * (0.22 + 0.72 * t)
        r = 1.35 * math.sin(math.pi * (0.15 + 0.8 * t)) ** 0.8 + 0.25
        for j in range(3):
            a = rnd.uniform(0, 2 * math.pi)
            c = Vector((math.cos(a) * r * 0.35, math.sin(a) * r * 0.35, z + rnd.uniform(-0.3, 0.3)))
            ball("canopy", base, tuple(c), r * rnd.uniform(0.75, 0.95), scale=(1.0, 1.0, 1.35), subdiv=2, noise=0.15, seed=rnd.random(),
                 grad=(2, height * 0.15, height * 1.0, jit(mix(base, lin("#25492a"), 0.3), 0.03, rnd), jit(top, 0.04, rnd)))
    return CUR


def bush(name, seed, size=0.8, blooms=None, heads=False):
    """A round shrub; `blooms` dots it with flowers, or with big flower heads (hydrangea) when `heads`."""
    asset(name)
    rnd = random.Random(seed)
    base, top = lin("#3c7632"), lin("#8fc455")
    for k in range(rnd.randint(4, 6)):
        a = rnd.uniform(0, 2 * math.pi)
        r = size * rnd.uniform(0.2, 0.5)
        c = (math.cos(a) * r, math.sin(a) * r, size * rnd.uniform(0.45, 0.7))
        rr = size * rnd.uniform(0.45, 0.6)
        ball("canopy", base, c, rr, scale=(1, 1, 0.85), subdiv=2, noise=0.15, seed=rnd.random(),
             grad=(2, 0.0, size * 1.3, mix(base, lin("#2a5228"), 0.4), top))
        if blooms and heads:
            for j in range(int(rr * 9)):
                d = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(0.0, 1))).normalized()
                col = jit(mix(blooms, lin("#e6b8e8"), rnd.uniform(0, 0.5)), 0.06, rnd)
                ball("blossom", col, tuple(Vector(c) + d * rr * 0.9), rnd.uniform(0.09, 0.13) * (size / 0.8), subdiv=2, noise=0.18,
                     seed=rnd.random(), scale=(1, 1, 0.8))
        elif blooms:
            for j in range(int(rr * 60)):
                d = Vector((rnd.uniform(-1, 1), rnd.uniform(-1, 1), rnd.uniform(-0.2, 1))).normalized()
                ball("blossom", jit(blooms, 0.1, rnd), tuple(Vector(c) + d * rr * 0.97), rnd.uniform(0.03, 0.05) * (size / 0.8) ** 0.5,
                     subdiv=1)
    return CUR


def flower_clump(name, seed, petal, kind="daisy"):
    """A small clump of garden flowers for flower beds (foliage layer)."""
    asset(name)
    rnd = random.Random(seed)
    for k in range(rnd.randint(5, 8)):
        x, y = rnd.uniform(-0.12, 0.12), rnd.uniform(-0.12, 0.12)
        h = rnd.uniform(0.22, 0.42)
        lean = Vector((rnd.uniform(-0.06, 0.06), rnd.uniform(-0.06, 0.06), 0))
        top = Vector((x, y, h)) + lean
        tube("stem", jit(lin("#5f8d3a"), 0.08, rnd), [(x, y, 0), Vector((x, y, h * 0.5)) + lean * 0.3, top], 0.006, seg=4)
        for j in range(2):
            a = rnd.uniform(0, 2 * math.pi)
            leaflet("stem", (x, y, h * rnd.uniform(0.15, 0.4)), (math.cos(a), math.sin(a), 0.5), 0.09, 0.025, 0.2, rnd, C["leaf"])
        col = jit(petal, 0.06, rnd)
        if kind == "tulip":
            lathe("petals", col, tuple(top), [(0.0, -0.01), (0.035, 0.0), (0.045, 0.04), (0.04, 0.075), (0.03, 0.08)], seg=6, var=0.0)
        else:
            n = 10 if kind == "daisy" else 6
            for j in range(n):
                a = 2 * math.pi * j / n
                d = Vector((math.cos(a), math.sin(a), 0.15))
                e = Vector((-math.sin(a), math.cos(a), 0)) * 0.012
                L = 0.05 if kind == "daisy" else 0.045
                poly("petals", col, [top - e, top + d * L * 0.6 - e * 1.4, top + d * L, top + d * L * 0.6 + e * 1.4, top + e], var=0.0,
                     both=True)
            ball("petals", C["pollen"] if kind == "daisy" else lin("#5a3a2a"), tuple(top + Vector((0, 0, 0.008))), 0.014, subdiv=1)
    return CUR


# =============================================================================================
# Animals
# =============================================================================================
def chicken(name="chicken", body=None, accent=None):
    """A round little hen, facing -Y, feet at z = 0."""
    asset(name)
    body = body or C["feather"]
    accent = accent or mix(body, C["straw"], 0.3)
    ball("feather", body, (0, 0.02, 0.32), 0.2, scale=(0.85, 1.15, 0.95), subdiv=3)
    ball("feather", body, (0, -0.16, 0.5), 0.12, scale=(0.9, 1.0, 1.05), subdiv=3)            # head
    ball("feather", accent, (0, 0.22, 0.45), 0.12, scale=(0.5, 0.8, 1.1), rot=(30, 0, 0), subdiv=2)  # tail
    for sx in (-1, 1):
        ball("feather", mix(body, accent, 0.5), (sx * 0.16, 0.04, 0.34), 0.11, scale=(0.35, 1.0, 0.7), rot=(10, 0, 0), subdiv=2)  # wings
        ball("eye", lin("#1a1410"), (sx * 0.07, -0.24, 0.53), 0.018, subdiv=1)
    for k in range(3):  # comb
        ball("comb", C["comb"], (0, -0.18 + k * 0.05, 0.62 + 0.012 * (1 - abs(k - 1))), 0.04, scale=(0.6, 1, 1.2), subdiv=1)
    ball("comb", C["comb"], (0, -0.27, 0.43), 0.03, scale=(0.6, 0.8, 1.3), subdiv=1)  # wattle
    cyl("beak", C["beak"], (0, -0.26, 0.5), 0.035, 0.07, r2=0.0, rot=(90, 0, 0), seg=6)
    for sx in (-1, 1):
        tube("beak", C["beak"], [(sx * 0.06, 0.02, 0.18), (sx * 0.06, 0.0, 0.02)], 0.015, seg=5)
        for a in (-0.5, 0, 0.5):
            tube("beak", C["beak"], [(sx * 0.06, 0.0, 0.01), (sx * 0.06 + math.sin(a) * 0.06, -math.cos(a) * 0.06, 0.005)], 0.008, seg=4)
    return CUR


def dog():
    """A sleepy shiba-ish farm dog curled up (lying), facing -Y."""
    asset("dog")
    fur, light = C["fur"], C["fur_light"]
    ball("fur", fur, (0, 0.05, 0.22), 0.3, scale=(0.95, 1.5, 0.75), subdiv=3)                       # body
    ball("fur", light, (0, -0.25, 0.18), 0.2, scale=(0.9, 0.8, 0.75), subdiv=2)                      # chest
    ball("fur", fur, (0.05, -0.48, 0.24), 0.17, scale=(1.0, 1.05, 0.95), subdiv=3)                   # head
    ball("fur", light, (0.05, -0.62, 0.19), 0.085, scale=(0.9, 1.1, 0.75), subdiv=2)                 # muzzle
    ball("nose", C["nose"], (0.05, -0.7, 0.215), 0.025, subdiv=1)
    for sx in (-1, 1):
        cyl("fur", fur, (0.05 + sx * 0.09, -0.47, 0.35), 0.06, 0.12, r2=0.005, rot=(sx * -15, 0, 0), seg=6)   # ears
        poly("nose", C["nose"], [(0.05 + sx * 0.06 - 0.025, -0.62, 0.29), (0.05 + sx * 0.06 + 0.025, -0.62, 0.29),
                                 (0.05 + sx * 0.06, -0.625, 0.282)], var=0.0)  # closed eyes (little arcs)
        ball("fur", light, (sx * 0.17, -0.45, 0.06), 0.07, scale=(0.8, 1.6, 0.6), subdiv=2)          # front paws
        ball("fur", fur, (sx * 0.22, 0.32, 0.1), 0.12, scale=(0.8, 1.2, 0.8), subdiv=2)                # haunches
    tube("fur", light, [(0.2, 0.45, 0.25), (0.32, 0.3, 0.42), (0.18, 0.15, 0.48)], 0.07, seg=8, r_end=0.04)  # curled tail
    return CUR


def butterfly_wing():
    """One wing (right side, +X), hinge at the origin; vertex colors: pale center, dark edge, dots."""
    asset("butterfly_wing")
    def wing(pts_upper, part="wing"):
        bm = CUR.bm(part)
        n0 = len(bm.faces)
        layer = bm.loops.layers.color["Col"]
        c0 = bm.verts.new((0, 0, 0))
        ring = [bm.verts.new(p) for p in pts_upper]
        for i in range(len(ring) - 1):
            f = bm.faces.new((c0, ring[i], ring[i + 1]))
            for l in f.loops:
                d = l.vert.co.length
                c = C["white"] if d < 0.025 else mix(C["white"], lin("#2b2220"), min(1.0, (d - 0.025) / 0.02))
                l[layer] = (*c, 1.0)
        CUR.flush()
    up = []
    for k in range(9):
        a = math.radians(-20 + 105 * k / 8)
        r = 0.045 * (0.75 + 0.35 * math.sin(math.radians(105 * k / 8)))
        up.append((math.cos(a) * r, -math.sin(a) * r * 0.9, 0))
    wing(up[::-1])
    lo = []
    for k in range(7):
        a = math.radians(-10 - 90 * k / 6)
        r = 0.032 * (0.8 + 0.3 * math.sin(math.radians(90 * k / 6)))
        lo.append((math.cos(a) * r, -math.sin(a) * r, 0))
    wing(lo)
    return CUR


def butterfly_body():
    asset("butterfly_body")
    ball("body", lin("#2b2220"), (0, 0, 0), 0.008, scale=(1, 3.2, 1), subdiv=2)
    for sx in (-1, 1):
        tube("body", lin("#2b2220"), [(0, -0.02, 0.002), (sx * 0.008, -0.035, 0.01)], 0.0012, seg=3)
    return CUR


def bird_body():
    """A swallow-ish songbird body (beak toward -Y), about 16 cm long."""
    asset("bird_body")
    ball("feather", lin("#2f3440"), (0, 0, 0), 0.03, scale=(0.8, 2.4, 0.8), subdiv=2)
    ball("feather", lin("#efe6da"), (0, -0.01, -0.012), 0.024, scale=(0.75, 2.0, 0.6), subdiv=2)
    ball("feather", lin("#2f3440"), (0, -0.07, 0.006), 0.022, subdiv=2)
    ball("feather", lin("#c8553d"), (0, -0.083, -0.004), 0.012, subdiv=1)
    cyl("beak", lin("#2a2420"), (0, -0.088, 0.006), 0.006, 0.014, r2=0.0, rot=(90, 0, 0), seg=5)
    for sx in (-1, 1):
        poly("feather", lin("#2f3440"), [(0, 0.05, 0), (sx * 0.025, 0.13, 0), (sx * 0.008, 0.09, 0)], var=0.0, both=True)
    return CUR


def bird_wing():
    """One swept wing (right side, +X), hinge at the origin."""
    asset("bird_wing")
    pts = [(0, -0.02, 0), (0.05, -0.03, 0), (0.12, -0.012, 0), (0.17, 0.03, 0), (0.1, 0.03, 0), (0.04, 0.035, 0), (0, 0.03, 0)]
    poly("wing", lin("#2f3440"), pts, var=0.0, both=True)
    return CUR


# =============================================================================================
# Output
# =============================================================================================
SMOOTH_PARTS = {"canopy", "blossom", "fruit", "feather", "fur", "berry", "berry_unripe", "leafy", "petals", "hay", "pad", "comb"}


def export(a):
    a.flush()
    out = {}
    for key, p in a.parts.items():
        if not p.f:
            continue
        name = f"{a.name}_{key}"
        me = bpy.data.meshes.new(name)
        me.from_pydata(p.v, [], p.f)
        me.update()
        attr = me.color_attributes.new("Col", "BYTE_COLOR", "CORNER")
        flat = []
        for poly_, cols in zip(me.polygons, p.c):
            for c in cols:
                flat.extend((min(1.0, max(0.0, c[0])), min(1.0, max(0.0, c[1])), min(1.0, max(0.0, c[2])), 1.0))
        attr.data.foreach_set("color", flat)
        me.color_attributes.active_color = attr
        try:
            me.color_attributes.render_color_index = 0
        except Exception:  # noqa: BLE001
            pass
        obj = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(obj)
        obj.data.materials.append(B.make_material(name + "_m", (1, 1, 1), roughness=0.7, vertex_colors=True))
        B.shade_smooth([obj], 180 if key in SMOOTH_PARTS else 50)
        path = sky.out_path(name + ".glb")  # noqa: F821
        B.export_glb(path, objects=[obj])
        out[key] = {"file": name + ".glb", "faces": len(me.polygons)}
        bpy.data.objects.remove(obj)
    return out


BUILDERS = [
    ("strawberry_row", lambda: strawberry_row(16.0, seed=1)),
    ("strawberry_row_b", lambda: strawberry_row(16.0, seed=2)),
    ("plant_sprout", lambda: strawberry_single("sprout")), ("plant_young", lambda: strawberry_single("young")),
    ("plant_flowering", lambda: strawberry_single("flowering")), ("plant_ripe", lambda: strawberry_single("ripe")),
    ("soil_patch", soil_patch), ("farmhouse", farmhouse), ("greenhouse", greenhouse), ("windmill", windmill),
    ("windmill_sails", windmill_sails), ("market_stall", market_stall), ("crate_stack", crate_stack),
    ("fence_rail", fence_rail), ("fence_picket", fence_picket), ("farm_sign", farm_sign), ("hay_bale", hay_bale),
    ("wheelbarrow", wheelbarrow), ("lily_pads", lily_pads), ("reeds", reeds),
    ("tree_a", lambda: tree("tree_a", 101, 6.8, 3.1, palette=("#2f6b2e", "#7fb04a"))),
    ("tree_b", lambda: tree("tree_b", 202, 5.4, 2.6, palette=("#4a8a34", "#b8d860"))),
    ("tree_c", lambda: tree("tree_c", 707, 7.5, 3.4, palette=("#356f38", "#93bf55"))),
    ("tree_poplar", lambda: poplar("tree_poplar", 808, 11.0)),
    ("tree_blossom", lambda: tree("tree_blossom", 303, 5.2, 2.6, blooms=lin("#f6c1d0"), palette=("#4f8a3a", "#b5d36a"))),
    ("tree_apple", lambda: tree("tree_apple", 404, 4.6, 2.4, fruit=lin("#d8392f"), palette=("#3e7a34", "#a6cc5c"))),
    ("bush", lambda: bush("bush", 501, 0.8)), ("bush_hydrangea", lambda: bush("bush_hydrangea", 502, 0.75, blooms=lin("#9fb4f0"), heads=True)),
    ("bush_rose", lambda: bush("bush_rose", 503, 0.7, blooms=lin("#f07a95"))),
    ("flowers_daisy", lambda: flower_clump("flowers_daisy", 601, C["petal"], "daisy")),
    ("flowers_cosmos", lambda: flower_clump("flowers_cosmos", 602, C["pink"], "cosmos")),
    ("flowers_tulip", lambda: flower_clump("flowers_tulip", 603, C["coral"], "tulip")),
    ("flowers_butter", lambda: flower_clump("flowers_butter", 604, C["butter"], "cosmos")),
    ("flowers_lilac", lambda: flower_clump("flowers_lilac", 605, C["lilac"], "daisy")),
    ("chicken", lambda: chicken("chicken")), ("hen", lambda: chicken("hen", C["hen"], C["hen_dark"])), ("dog", dog),
    ("butterfly_wing", butterfly_wing), ("butterfly_body", butterfly_body), ("bird_body", bird_body), ("bird_wing", bird_wing),
]

B.reset_scene()
result = {}
only = ARGS.get("only")
for key, fn in BUILDERS:
    if only and key not in only:
        continue
    a = fn()
    a.name = key
    result[key] = export(a)
    sky.log(f"{key}: " + ", ".join(f"{k}={v['faces']}" for k, v in result[key].items()))  # noqa: F821
sky.result(assets=result)  # noqa: F821
