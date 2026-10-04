"""Ashen Peaks — the Monastery of the Kindled Crane, modelled procedurally in Blender.

Runs inside Blender through the engine's `dcc_run_script` (the `sky` helper is preloaded).
Every building is written as one .glb per material part (`<asset>_<part>.glb`) so the engine
can dress each part with its own Poly Haven scan. UVs are in meters (u along the grain of
timber, along the eave for roof tiles), so materials tile at real-world scale. Coordinates
below are engine coordinates (x, y up, z toward the front of a building); they are converted
to Blender's Z-up on output.

sky.ARGS[0] is a JSON object: {"stairs": [[x, z, y], ...], "seed": n}
"""
import json
import math
import random

import bpy
from skywalker_dcc import blender as B

ARGS = json.loads(sky.ARGS[0]) if sky.ARGS else {}  # noqa: F821  (sky is injected by the runner)
R = random.Random(ARGS.get("seed", 7))


# --- vector helpers ------------------------------------------------------------------------
def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def mul(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def norm(a):
    n = length(a)
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-9 else (0.0, 1.0, 0.0)


def lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def rot_y(v, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


# --- geometry accumulator --------------------------------------------------------------------
class Part:
    def __init__(self):
        self.v, self.f, self.uv, self.col = [], [], [], []

    def verts(self, pts):
        base = len(self.v)
        self.v.extend(tuple(float(c) for c in p) for p in pts)
        return list(range(base, base + len(pts)))

    def face(self, idx, uvs, col=(1, 1, 1), outward=None):
        """Adds a polygon; flips it if its normal points against `outward`."""
        if outward is not None:
            p0, p1, p2 = self.v[idx[0]], self.v[idx[1]], self.v[idx[2]]
            n = cross(sub(p1, p0), sub(p2, p0))
            if len(idx) > 3 and length(n) < 1e-12:
                n = cross(sub(self.v[idx[2]], p0), sub(self.v[idx[3]], p0))
            if dot(n, outward) < 0:
                idx, uvs = list(reversed(idx)), list(reversed(uvs))
        self.f.append(list(idx))
        self.uv.append([tuple(u) for u in uvs])
        self.col.append(col)


class Asset:
    def __init__(self, name):
        self.name = name
        self.parts = {}

    def __getitem__(self, key):
        if key not in self.parts:
            self.parts[key] = Part()
        return self.parts[key]


def tint(base=1.0, var=0.08):
    k = base * (1.0 + R.uniform(-var, var))
    return (k, k * (1.0 + R.uniform(-0.02, 0.02)), k * (1.0 + R.uniform(-0.03, 0.03)))


# --- primitives ------------------------------------------------------------------------------
def box(p, c, ax, ay, az, col=None, uv_off=True):
    """Oriented box: center c and half-extent vectors ax, ay, az. UVs in meters, u along the
    longer edge of each face (timber grain runs along beams and columns)."""
    col = col or tint()
    corners = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                corners.append(add(add(add(c, mul(ax, sx)), mul(ay, sy)), mul(az, sz)))
    idx = p.verts(corners)

    def I(sx, sy, sz):
        return idx[((sx > 0) << 2) | ((sy > 0) << 1) | (sz > 0)]

    ou, ov = (R.uniform(0, 5), R.uniform(0, 5)) if uv_off else (0, 0)
    def signs(k, s, a, b):
        out = [0, 0, 0]
        out[k] = s
        others = [i for i in range(3) if i != k]
        out[others[0]], out[others[1]] = a, b
        return out

    for k, (axis, (e1, e2)) in enumerate(((ax, (ay, az)), (ay, (ax, az)), (az, (ax, ay)))):
        for s in (-1, 1):
            # four corners of this face
            ids = [I(*signs(k, s, a, b)) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            l1, l2 = 2 * length(e1), 2 * length(e2)
            if l1 >= l2:
                uvs = [(ou + (a + 1) / 2 * l1, ov + (b + 1) / 2 * l2) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            else:
                uvs = [(ou + (b + 1) / 2 * l2, ov + (a + 1) / 2 * l1) for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
            p.face(ids, uvs, col, outward=mul(axis, s))
    return I


def abox(p, c, size, yaw=0.0, col=None):
    """Axis-aligned (then yawed) box from a full size (sx, sy, sz)."""
    ax = rot_y((size[0] / 2, 0, 0), yaw)
    az = rot_y((0, 0, size[2] / 2), yaw)
    box(p, c, ax, (0, size[1] / 2, 0), az, col)


def beam(p, a, b, w, h, up=(0, 1, 0), col=None):
    """A timber between points a and b with a w x h section."""
    d = sub(b, a)
    L = length(d)
    if L < 1e-6:
        return
    x = norm(d)
    side = norm(cross(x, up)) if abs(dot(x, norm(up))) < 0.99 else norm(cross(x, (1, 0, 0)))
    upv = norm(cross(side, x))
    box(p, lerp(a, b, 0.5), mul(x, L / 2), mul(upv, h / 2), mul(side, w / 2), col)


def cyl(p, base, r0, h, seg=16, r1=None, axis=(0, 1, 0), caps=True, col=None, phase=0.0, bulge=0.0, rings=1):
    """Cylinder/cone/bulging drum along `axis`. u runs along the axis (grain), v around."""
    r1 = r0 if r1 is None else r1
    col = col or tint()
    axis = norm(axis)
    t1 = norm(cross(axis, (1, 0, 0))) if abs(axis[0]) < 0.9 else norm(cross(axis, (0, 0, 1)))
    t2 = cross(axis, t1)
    rows = []
    for k in range(rings + 1):
        t = k / rings
        r = r0 + (r1 - r0) * t + bulge * math.sin(math.pi * t)
        ring = []
        for i in range(seg):
            a = phase + 2 * math.pi * i / seg
            ring.append(add(add(base, mul(axis, h * t)), add(mul(t1, math.cos(a) * r), mul(t2, math.sin(a) * r))))
        rows.append(p.verts(ring))
    circ = 2 * math.pi * max(r0, r1)
    ou = R.uniform(0, 5)
    for k in range(rings):
        for i in range(seg):
            j = (i + 1) % seg
            ids = [rows[k][i], rows[k][j], rows[k + 1][j], rows[k + 1][i]]
            v0, v1 = circ * i / seg, circ * (i + 1) / seg
            u0, u1 = ou + h * k / rings, ou + h * (k + 1) / rings
            mid = lerp(p.v[ids[0]], p.v[ids[2]], 0.5)
            ctr = add(base, mul(axis, h * (k + 0.5) / rings))
            p.face(ids, [(u0, v0), (u0, v1), (u1, v1), (u1, v0)], col, outward=sub(mid, ctr))
    if caps:
        for k, sgn in ((0, -1), (rings, 1)):
            ids = rows[k]
            uvs = [(p.v[i][0], p.v[i][2]) for i in ids]
            p.face(list(ids), uvs, col, outward=mul(axis, sgn))


def grid_surface(p, fn, nu, nv, uv_fn, col, outward_fn):
    """A quad grid over (s, t) in [0, 1]^2: fn(s, t) -> point, uv_fn(s, t) -> uv."""
    rows = []
    for j in range(nv + 1):
        rows.append(p.verts([fn(i / nu, j / nv) for i in range(nu + 1)]))
    for j in range(nv):
        for i in range(nu):
            ids = [rows[j][i], rows[j][i + 1], rows[j + 1][i + 1], rows[j + 1][i]]
            st = [(i / nu, j / nv), ((i + 1) / nu, j / nv), ((i + 1) / nu, (j + 1) / nv), (i / nu, (j + 1) / nv)]
            p.face(ids, [uv_fn(s, t) for s, t in st], col, outward=outward_fn((i + 0.5) / nu, (j + 0.5) / nv))
    return rows


# --- roofs with upturned eaves ------------------------------------------------------------------
def roof(A, c, W, D, Wi, Di, rise, lift=1.2, flare=0.6, thick=0.32, nu=22, nv=9, sag=0.72, rafters=True, ridge=True,
         overhang=2.4, hips=True, horn=True, tiles="roof", wood="timber", trim="ridge"):
    """Hip/pyramid/skirt roof. Outer eave rectangle half extents (W, D) at height c.y, inner
    top rectangle (Wi, Di) at c.y + rise (Wi = W - D, Di = 0: hip with a ridge; 0, 0: pyramid;
    larger: a skirt roof around an upper storey). Concave profile, corners lifted by `lift`
    and swept out by `flare` (the upturned eaves)."""
    cx, cy, cz = c
    outer = [(-W, -D), (W, -D), (W, D), (-W, D)]
    inner = [(-Wi, -Di), (Wi, -Di), (Wi, Di), (-Wi, Di)]
    sides = [(0, 1, (0, 0, -1)), (1, 2, (1, 0, 0)), (2, 3, (0, 0, 1)), (3, 0, (-1, 0, 0))]

    def prof(t):
        return (1 - sag) * t + sag * t * t

    def point(e0, e1, i0, i1, s, t, down=0.0):
        eave = lerp(e0, e1, s)
        top = lerp(i0, i1, s)
        x, z = eave[0] + (top[0] - eave[0]) * t, eave[1] + (top[1] - eave[1]) * t
        cw = abs(2 * s - 1) ** 4
        corner = e0 if s < 0.5 else e1
        dn = norm((corner[0], 0, corner[1]))
        k = cw * (1 - t) ** 2
        x += dn[0] * flare * k
        z += dn[2] * flare * k
        y = cy + rise * prof(t) + lift * k - down
        return (cx + x, y, cz + z)

    hip_lines = []
    for a, b, n in sides:
        e0, e1, i0, i1 = outer[a], outer[b], inner[a], inner[b]
        eave_len = math.dist(e0, e1)
        slant = math.hypot(math.dist(lerp(e0, e1, 0.5), lerp(i0, i1, 0.5)), rise)
        ou = R.uniform(0, 3)

        def fn(s, t, e0=e0, e1=e1, i0=i0, i1=i1):
            return point(e0, e1, i0, i1, s, t)

        def fnb(s, t, e0=e0, e1=e1, i0=i0, i1=i1):
            return point(e0, e1, i0, i1, s, t, thick)

        def uvf(s, t, eave_len=eave_len, slant=slant, ou=ou):
            return (ou + s * eave_len, (1 - t) * slant)

        up = lambda s, t, n=n: add(n, (0, 1.4, 0))  # noqa: E731
        dn = lambda s, t, n=n: (0, -1, 0)  # noqa: E731
        col = tint(1.0, 0.05)
        grid_surface(A[tiles], fn, nu, nv, uvf, col, up)
        grid_surface(A[wood], fnb, nu, nv, lambda s, t, el=eave_len, sl=slant: (s * el, t * sl), tint(0.9, 0.03), dn)
        # eave edge: tiles' front lip and the fascia board below it
        for i in range(nu):
            s0, s1 = i / nu, (i + 1) / nu
            q = [fn(s0, 0), fn(s1, 0), fnb(s1, 0), fnb(s0, 0)]
            ids = A[tiles].verts(q)
            A[tiles].face(ids, [(s0 * eave_len, 0), (s1 * eave_len, 0), (s1 * eave_len, thick), (s0 * eave_len, thick)], col, outward=n)
            f0, f1 = fnb(s0, 0), fnb(s1, 0)
            q = [f0, f1, add(f1, (0, -0.28, 0)), add(f0, (0, -0.28, 0))]
            ids = A[wood].verts(q)
            A[wood].face(ids, [(s0 * eave_len, 0), (s1 * eave_len, 0), (s1 * eave_len, 0.28), (s0 * eave_len, 0.28)], tint(0.85, 0.03),
                         outward=n)
        # top edge of a skirt roof (closes the thickness against the upper wall)
        if Wi > 0 and Di > 0:
            for i in range(nu):
                s0, s1 = i / nu, (i + 1) / nu
                q = [fn(s0, 1), fn(s1, 1), fnb(s1, 1), fnb(s0, 1)]
                A[tiles].face(A[tiles].verts(q), [(0, 0), (1, 0), (1, 0.3), (0, 0.3)], col, outward=mul(n, -1))
        if rafters:
            steps = max(2, int(eave_len / 0.45))
            tw = min(1.0, overhang / max(math.dist(lerp(e0, e1, 0.5), lerp(i0, i1, 0.5)), 0.1))
            for k in range(1, steps):
                s = k / steps
                a0 = add(fnb(s, 0), (0, -0.16, 0))
                a1 = add(fnb(s, tw), (0, -0.16, 0))
                beam(A[wood], a0, a1, 0.13, 0.15, col=tint(0.8, 0.05))
        hip_lines.append([fn(0, t / nv) for t in range(nv + 1)])
    if hips:
        for line in hip_lines:
            for k in range(len(line) - 1):
                beam(A[trim], add(line[k], (0, 0.1, 0)), add(line[k + 1], (0, 0.1, 0)), 0.34, 0.3, col=tint(0.95, 0.04))
            if horn:
                # the upturned horn at the eave corner
                p0 = add(line[0], (0, 0.1, 0))
                d = norm(sub(line[0], line[1]))
                d = norm((d[0], 0, d[2]))
                p1 = add(add(p0, mul(d, 0.55)), (0, 0.32, 0))
                p2 = add(add(p1, mul(d, 0.35)), (0, 0.45, 0))
                beam(A[trim], p0, p1, 0.3, 0.26)
                beam(A[trim], p1, p2, 0.24, 0.22)
    if ridge and Di == 0 and Wi > 0:
        y = cy + rise + 0.1
        beam(A[trim], (cx - Wi - 0.3, y + 0.25, cz), (cx + Wi + 0.3, y + 0.25, cz), 0.5, 0.6)
        for sgn in (-1, 1):
            # ridge-end fins: a curl of stacked blocks (the "crane tail")
            base = (cx + sgn * (Wi + 0.2), y + 0.5, cz)
            pts = [base]
            ang = 0.0
            for k in range(5):
                ang += 0.42
                prev = pts[-1]
                pts.append((prev[0] - sgn * 0.08 + sgn * 0.12 * math.cos(ang), prev[1] + 0.32 * math.sin(ang + 0.6), prev[2]))
            for k in range(len(pts) - 1):
                beam(A[trim], pts[k], pts[k + 1], 0.36 - 0.04 * k, 0.4 - 0.05 * k)
    return hip_lines


# --- building pieces ------------------------------------------------------------------------------
def column(A, x, z, y0, h, r=0.3, base=True):
    if base:
        cyl(A["stone"], (x, y0, z), r * 1.7, 0.18, seg=8, r1=r * 1.5, col=tint(1.0, 0.1))
        cyl(A["stone"], (x, y0 + 0.18, z), r * 1.45, 0.16, seg=8, r1=r * 1.2, col=tint(1.0, 0.1))
        y0 += 0.34
        h -= 0.34
    cyl(A["lacquer"], (x, y0, z), r, h, seg=18, r1=r * 0.92, caps=False, col=tint(1.0, 0.05))


def bracket(A, x, y, z, yaw=0.0, s=1.0):
    """A simplified bracket set (dougong): block, crossed arms, block, arms."""
    abox(A["jade"], (x, y + 0.18 * s, z), (0.62 * s, 0.36 * s, 0.62 * s), yaw)
    abox(A["lacquer"], (x, y + 0.5 * s, z), (1.8 * s, 0.26 * s, 0.32 * s), yaw)
    abox(A["lacquer"], (x, y + 0.5 * s, z), (0.32 * s, 0.26 * s, 1.8 * s), yaw)
    abox(A["jade"], (x, y + 0.75 * s, z), (0.5 * s, 0.26 * s, 0.5 * s), yaw)
    abox(A["lacquer"], (x, y + 0.98 * s, z), (2.6 * s, 0.22 * s, 0.28 * s), yaw)
    abox(A["lacquer"], (x, y + 0.98 * s, z), (0.28 * s, 0.22 * s, 2.4 * s), yaw)


def lattice_bay(A, x0, x1, z, y0, y1, outward, glow=True, pitch=0.24):
    """A bay of lattice doors: frame, grid of thin bars, paper behind (softly lit inside)."""
    w = x1 - x0
    zf = z + outward * 0.02
    abox(A["timber"], ((x0 + x1) / 2, y0 + 0.05, zf), (w, 0.1, 0.16))
    abox(A["timber"], ((x0 + x1) / 2, y1 - 0.06, zf), (w, 0.12, 0.16))
    abox(A["timber"], ((x0 + x1) / 2, y0 + 0.75, zf), (w, 0.1, 0.16))
    for x in (x0 + 0.05, (x0 + x1) / 2, x1 - 0.05):
        abox(A["timber"], (x, (y0 + y1) / 2, zf), (0.1, y1 - y0, 0.16))
    # kick panels
    abox(A["panel"], ((x0 + x1) / 2, y0 + 0.42, z - outward * 0.02), (w - 0.1, 0.62, 0.05))
    ny = int((y1 - y0 - 0.95) / pitch)
    nx = int(w / pitch)
    for i in range(1, nx):
        x = x0 + w * i / nx
        abox(A["timber"], (x, (y0 + 0.8 + y1 - 0.12) / 2, zf), (0.045, y1 - y0 - 0.92, 0.06))
    for j in range(1, ny + 1):
        y = y0 + 0.8 + (y1 - 0.12 - y0 - 0.8) * j / (ny + 1)
        abox(A["timber"], ((x0 + x1) / 2, y, zf), (w - 0.1, 0.045, 0.06))
    abox(A["paper"], ((x0 + x1) / 2, (y0 + 0.8 + y1 - 0.1) / 2, z - outward * 0.04), (w - 0.1, y1 - y0 - 0.9, 0.02))


def wall_bay(A, x0, x1, z, y0, y1, outward, window=False):
    w = x1 - x0
    abox(A["timber"], ((x0 + x1) / 2, y0 + 0.3, z), (w, 0.6, 0.3))
    abox(A["timber"], ((x0 + x1) / 2, y1 - 0.12, z), (w, 0.24, 0.3))
    abox(A["timber"], ((x0 + x1) / 2, y0 + (y1 - y0) * 0.62, z), (w, 0.16, 0.26))
    abox(A["plaster"], ((x0 + x1) / 2, (y0 + 0.6 + y1 - 0.24) / 2, z - outward * 0.05), (w, y1 - y0 - 0.84, 0.16))
    if window:
        cx, cy = (x0 + x1) / 2, y0 + (y1 - y0) * 0.42
        ww, wh = min(1.4, w * 0.45), 1.0
        abox(A["timber"], (cx, cy, z + outward * 0.06), (ww + 0.2, wh + 0.2, 0.1))
        for i in range(-3, 4):
            abox(A["timber"], (cx + i * ww / 7, cy, z + outward * 0.1), (0.05, wh, 0.05))
        abox(A["paper"], (cx, cy, z + outward * 0.05), (ww, wh, 0.03))


def rect_walls(A, hw, hd, y0, y1, xs, zs, front="lattice", sides="wall", back="wall", windows=False):
    """Columns at xs (front/back) and zs (sides), walls/doors in the bays between them."""
    for x in xs:
        for z in (-hd, hd):
            column(A, x, z, y0, y1 - y0)
    for z in zs[1:-1]:
        for x in (-hw, hw):
            column(A, x, z, y0, y1 - y0)
    yw0 = y0 + 0.34
    for side, z, kind in ((1, hd, front), (-1, -hd, back)):
        for x0, x1 in zip(xs, xs[1:]):
            a, b = x0 + 0.3, x1 - 0.3
            if kind == "lattice":
                lattice_bay(A, a, b, z, yw0, y1 - 0.5, side)
            elif kind == "wall":
                wall_bay(A, a, b, z, yw0, y1 - 0.5, side, window=windows)
    for side, x in ((1, hw), (-1, -hw)):
        for z0, z1 in zip(zs, zs[1:]):
            a, b = z0 + 0.3, z1 - 0.3
            # same as wall_bay but along z: build in local frame by swapping axes
            w = b - a
            zc = (a + b) / 2
            abox(A["timber"], (x, yw0 + 0.3, zc), (0.3, 0.6, w))
            abox(A["timber"], (x, y1 - 0.62, zc), (0.3, 0.24, w))
            abox(A["timber"], (x, yw0 + (y1 - 0.5 - yw0) * 0.62, zc), (0.26, 0.16, w))
            abox(A["plaster"], (x - side * 0.05, (yw0 + 0.6 + y1 - 0.74) / 2, zc), (0.16, y1 - 0.5 - yw0 - 0.84, w))
            if sides == "window":
                cy = yw0 + (y1 - yw0) * 0.42
                abox(A["timber"], (x + side * 0.06, cy, zc), (0.1, 1.2, min(1.6, w * 0.5)))
                abox(A["paper"], (x + side * 0.08, cy, zc), (0.03, 1.0, min(1.4, w * 0.45)))
    # head beams (lacquer) and a painted frieze (jade) all around
    for y, h, mat in ((y1 - 0.25, 0.5, "lacquer"), (y1 + 0.12, 0.24, "jade")):
        abox(A[mat], (0, y, -hd), (2 * hw + 0.6, h, 0.36))
        abox(A[mat], (0, y, hd), (2 * hw + 0.6, h, 0.36))
        abox(A[mat], (-hw, y, 0), (0.36, h, 2 * hd + 0.6))
        abox(A[mat], (hw, y, 0), (0.36, h, 2 * hd + 0.6))


def brackets_on(A, hw, hd, y, xs, zs, s=1.0):
    for x in xs:
        for z in (-hd, hd):
            bracket(A, x, y, z, s=s)
    for z in zs[1:-1]:
        for x in (-hw, hw):
            bracket(A, x, y, z, s=s)
    # top plate the rafters rest on
    yt = y + 1.1 * s
    for z in (-hd, hd):
        abox(A["timber"], (0, yt, z), (2 * hw + 1.6, 0.22, 0.4))
    for x in (-hw, hw):
        abox(A["timber"], (x, yt, 0), (0.4, 0.22, 2 * hd + 1.6))
    return yt + 0.11


def plinth(A, hw, hd, h, steps_front=True, steps_w=6.0):
    abox(A["stone"], (0, h / 2, 0), (2 * hw, h, 2 * hd), col=tint(0.95, 0.04))
    abox(A["stone"], (0, h + 0.06, 0), (2 * hw + 0.3, 0.12, 2 * hd + 0.3), col=tint(1.05, 0.04))
    if steps_front:
        n = max(2, int(h / 0.2))
        for k in range(n):
            rise = h / n
            depth = 0.36
            abox(A["stone"], (0, rise * (k + 0.5), hd + depth * (n - k) - depth / 2), (steps_w, rise, depth + 0.02), col=tint(1.0, 0.08))
        for sgn in (-1, 1):
            # side cheeks
            L = depth * n
            a = (sgn * (steps_w / 2 + 0.25), h + 0.1, hd)
            b = (sgn * (steps_w / 2 + 0.25), 0.15, hd + L)
            beam(A["stone"], a, b, 0.45, 0.35)


# --- assets ------------------------------------------------------------------------------------------
def main_hall():
    A = Asset("hall")
    hw, hd = 11.0, 7.0
    ph_ = 1.5
    plinth(A, hw + 2.6, hd + 2.6, ph_, steps_w=7.5)
    xs = [-hw + 2 * hw * i / 6 for i in range(7)]
    zs = [-hd + 2 * hd * i / 4 for i in range(5)]
    y1 = ph_ + 5.4
    rect_walls(A, hw, hd, ph_ + 0.12, y1, xs, zs, front="lattice", sides="window", back="wall")
    yb = brackets_on(A, hw, hd, y1 + 0.24, xs, zs)
    # lower (skirt) roof around the upper storey
    uw, ud = 8.6, 4.8
    roof(A, (0, yb + 0.15, 0), hw + 3.4, hd + 3.4, uw + 0.25, ud + 0.25, 2.3, lift=1.5, flare=0.9, overhang=3.4, ridge=False)
    # upper storey
    y2 = yb + 2.2
    uxs = [-uw + 2 * uw * i / 4 for i in range(5)]
    uzs = [-ud + 2 * ud * i / 2 for i in range(3)]
    rect_walls(A, uw, ud, y2 - 0.6, y2 + 2.6, uxs, uzs, front="lattice", sides="wall", back="wall")
    yb2 = brackets_on(A, uw, ud, y2 + 2.6 + 0.24, uxs, uzs, s=0.85)
    roof(A, (0, yb2 + 0.15, 0), uw + 3.6, ud + 3.6, uw - ud, 0, 5.2, lift=1.9, flare=1.1, overhang=3.6)
    return A


def pagoda():
    A = Asset("pagoda")
    plinth(A, 7.2, 7.2, 1.4, steps_w=4.0)
    y = 1.52
    a = 4.4
    tiers = 5
    for i in range(tiers):
        h = 3.6 if i == 0 else 2.5
        xs = [-a + 2 * a * k / 3 for k in range(4)]
        rect_walls(A, a, a, y, y + h, xs, xs, front="lattice" if i == 0 else "wall", sides="window" if i else "wall",
                   back="wall", windows=i > 0)
        if i > 0:
            # balcony railing around the tier
            ry = y + 0.05
            ra = a + 0.9
            abox(A["timber"], (0, ry - 0.08, 0), (2 * ra + 0.2, 0.16, 2 * ra + 0.2))
            for sx, sz, L, along_x in ((0, -ra, 2 * ra, True), (0, ra, 2 * ra, True), (-ra, 0, 2 * ra, False), (ra, 0, 2 * ra, False)):
                size_top = (L, 0.1, 0.1) if along_x else (0.1, 0.1, L)
                abox(A["lacquer"], (sx, ry + 0.85, sz), size_top)
                abox(A["lacquer"], (sx, ry + 0.4, sz), (L, 0.06, 0.06) if along_x else (0.06, 0.06, L))
                n = int(L / 0.9)
                for k in range(n + 1):
                    t = -L / 2 + L * k / n
                    abox(A["lacquer"], (sx + (t if along_x else 0), ry + 0.42, sz + (0 if along_x else t)), (0.1, 0.84, 0.1))
        yb = brackets_on(A, a, a, y + h + 0.24, xs, xs, s=0.75)
        top = i == tiers - 1
        na = a - 0.55
        ow = a + 2.6 - 0.15 * i
        rise = 3.4 if top else 1.7
        roof(A, (0, yb + 0.1, 0), ow, ow, 0 if top else na + 0.2, 0 if top else na + 0.2, rise, lift=1.3 - 0.08 * i, flare=0.8,
             overhang=2.6, ridge=False, nu=18, nv=8)
        y = yb + 0.1 + rise - 0.25
        a = na
    # spire: base drum, nine rings, a flame-shaped finial
    SPIRE[0] = y + 3.0
    cyl(A["bronze"], (0, y - 0.3, 0), 0.9, 0.5, seg=12, r1=0.7)
    cyl(A["bronze"], (0, y + 0.2, 0), 0.16, 6.0, seg=10, r1=0.1)
    for k in range(9):
        cyl(A["bronze"], (0, y + 0.8 + k * 0.42, 0), 0.62 - 0.03 * k, 0.12, seg=16)
    cyl(A["gold"], (0, y + 4.8, 0), 0.05, 0.9, seg=12, bulge=0.32, rings=6)
    cyl(A["gold"], (0, y + 5.7, 0), 0.12, 0.7, seg=10, r1=0.0, bulge=0.1, rings=4)
    return A


def gate():
    A = Asset("gate")
    hw, hd = 6.0, 2.4
    plinth(A, hw + 1.2, hd + 1.0, 0.6, steps_w=4.4)
    xs = [-hw, -2.0, 2.0, hw]
    y0, y1 = 0.72, 6.0
    for x in xs:
        for z in (-hd, hd):
            column(A, x, z, y0, y1 - y0, r=0.34)
    for x0, x1 in ((-hw, -2.0), (2.0, hw)):
        for z, sgn in ((hd, 1), (-hd, -1)):
            lattice_bay(A, x0 + 0.34, x1 - 0.34, z, y0 + 0.34, y1 - 0.5, sgn)
    # open door leaves in the central bay
    for sgn in (-1, 1):
        cx = sgn * 1.0
        abox(A["lacquer"], (sgn * 1.85, (y0 + y1 - 0.6) / 2, 0.9), (0.14, y1 - y0 - 0.7, 1.9))
        for k in range(5):
            for j in range(4):
                cyl(A["bronze"], (sgn * 1.93, y0 + 0.9 + k * 0.9, 0.2 + j * 0.45), 0.05, 0.06, seg=8, axis=(sgn, 0, 0))
        _ = cx
    for y, h, mat in ((y1 - 0.25, 0.55, "lacquer"), (y1 + 0.15, 0.26, "jade")):
        for z in (-hd, hd):
            abox(A[mat], (0, y, z), (2 * hw + 0.8, h, 0.4))
        for x in (-hw, hw):
            abox(A[mat], (x, y, 0), (0.4, h, 2 * hd + 0.8))
    # name plaque above the central bay (sigil texture on the "plaque" part, UV 0..1)
    p = A["plaque"]
    z = hd + 0.3
    pts = [(-1.1, y1 + 0.95, z), (1.1, y1 + 0.95, z), (1.1, y1 + 2.15, z), (-1.1, y1 + 2.15, z)]
    p.face(p.verts(pts), [(0, 0), (1, 0), (1, 1), (0, 1)], (1, 1, 1), outward=(0, 0, 1))
    abox(A["lacquer"], (0, y1 + 1.55, hd + 0.22), (2.5, 1.45, 0.12))
    yb = brackets_on(A, hw, hd, y1 + 0.3, xs, [-hd, hd], s=0.8)
    roof(A, (0, yb + 0.1, 0), hw + 2.8, hd + 2.8, hw - hd, 0, 3.6, lift=1.5, flare=0.9, overhang=2.8)
    return A


def bell_pavilion():
    A = Asset("bell")
    plinth(A, 3.6, 3.6, 0.8, steps_w=2.4)
    xs = [-2.6, 2.6]
    for x in xs:
        for z in xs:
            column(A, x, z, 0.92, 4.2, r=0.26)
    for z in xs:
        abox(A["lacquer"], (0, 4.9, z), (6.0, 0.4, 0.32))
        abox(A["lacquer"], (z, 4.9, 0), (0.32, 0.4, 6.0))
    abox(A["timber"], (0, 4.6, 0), (5.6, 0.34, 0.4))
    # the bell
    cyl(A["bronze"], (0, 2.0, 0), 0.82, 2.2, seg=24, r1=0.62, bulge=0.04, rings=6, caps=True)
    cyl(A["bronze"], (0, 4.2, 0), 0.08, 0.4, seg=8)
    for k in range(4):
        cyl(A["bronze"], (0, 2.2 + k * 0.45, 0), 0.86 - k * 0.05, 0.06, seg=24)
    beam(A["timber"], (-2.9, 2.6, 0), (-0.95, 2.6, 0), 0.22, 0.22)
    yb = brackets_on(A, 2.6, 2.6, 5.1, xs, xs, s=0.6)
    roof(A, (0, yb + 0.05, 0), 5.0, 5.0, 0, 0, 3.2, lift=1.1, flare=0.7, overhang=2.4, ridge=False, nu=16, nv=8)
    cyl(A["bronze"], (0, yb + 3.2, 0), 0.25, 0.9, seg=10, r1=0.05, bulge=0.12, rings=3)
    return A


def stone_lantern():
    A = Asset("lantern")
    s = A["stone"]
    cyl(s, (0, 0, 0), 0.55, 0.22, seg=6, r1=0.5, col=tint(1, 0.06))
    cyl(s, (0, 0.22, 0), 0.34, 0.2, seg=6, r1=0.22, col=tint(1, 0.06))
    cyl(s, (0, 0.42, 0), 0.15, 1.05, seg=10, r1=0.13, col=tint(1, 0.06))
    cyl(s, (0, 1.47, 0), 0.22, 0.16, seg=6, r1=0.46, col=tint(1, 0.06))
    for k in range(6):
        a = math.radians(30 + 60 * k)
        cyl(s, (math.cos(a) * 0.31, 1.63, math.sin(a) * 0.31), 0.045, 0.46, seg=6, caps=True)
    cyl(A["paper"], (0, 1.65, 0), 0.27, 0.42, seg=6, phase=0.0)
    # cap with upturned corners
    roof(A, (0, 2.1, 0), 0.62, 0.62, 0, 0, 0.42, lift=0.16, flare=0.08, thick=0.1, nu=6, nv=4, rafters=False, ridge=False,
         hips=False, tiles="stone", wood="stone", trim="stone")
    cyl(s, (0, 2.5, 0), 0.07, 0.2, seg=8)
    cyl(s, (0, 2.62, 0), 0.04, 0.28, seg=8, bulge=0.09, rings=4)
    return A


def paper_lantern():
    A = Asset("paperlantern")
    cyl(A["redpaper"], (0, -0.62, 0), 0.2, 0.6, seg=16, bulge=0.13, rings=8)
    cyl(A["timber"], (0, -0.06, 0), 0.17, 0.08, seg=12)
    cyl(A["timber"], (0, -0.7, 0), 0.17, 0.08, seg=12)
    cyl(A["timber"], (0, -0.02, 0), 0.015, 0.5, seg=6)
    for k in range(4):
        cyl(A["timber"], (math.cos(k * 1.57) * 0.03, -0.95, math.sin(k * 1.57) * 0.03), 0.012, 0.25, seg=4)
    return A


def banner():
    A = Asset("banner")
    cyl(A["timber"], (0, 0, 0), 0.08, 7.6, seg=10, r1=0.06)
    beam(A["timber"], (-0.1, 7.2, 0), (1.15, 7.2, 0), 0.08, 0.08)
    cyl(A["bronze"], (0, 7.6, 0), 0.07, 0.45, seg=8, r1=0.0, bulge=0.05, rings=3)
    # cloth: 0.9 m x 4.4 m, gently rippled, UV 0..1 over the whole banner
    p = A["banner"]

    def fn(s, t):
        x = 0.05 + s * 0.95
        y = 7.15 - t * 4.4
        z = 0.06 * math.sin(t * 5.0 + s * 1.5) * t + 0.03 * math.sin(s * 9.0) * t
        return (x, y, z)

    grid_surface(p, fn, 6, 22, lambda s, t: (s, 1 - t), (1, 1, 1), lambda s, t: (0, 0, 1))
    beam(A["timber"], (0.05, 2.75, 0.0), (1.0, 2.75, 0.0), 0.05, 0.05)
    return A


def flag_line(span=16.0, sag=1.6, n=26):
    """A cord of small prayer-style pennants (five colors as vertex colors)."""
    A = Asset("flags")
    colors = [(0.75, 0.12, 0.06), (0.95, 0.68, 0.12), (0.12, 0.42, 0.32), (0.92, 0.88, 0.78), (0.12, 0.2, 0.5)]
    pts = []
    for k in range(41):
        t = k / 40
        x = -span / 2 + span * t
        pts.append((x, -sag * 4 * t * (1 - t), 0))
    for k in range(len(pts) - 1):
        beam(A["timber"], pts[k], pts[k + 1], 0.025, 0.025)
    for k in range(n):
        t = (k + 0.5) / n
        x = -span / 2 + span * t
        y = -sag * 4 * t * (1 - t)
        w, h = span / n * 0.82, 0.5
        c = colors[k % 5]
        tilt = R.uniform(-0.08, 0.08)
        q = [(x - w / 2, y, 0), (x + w / 2, y, 0), (x + w / 2 + tilt, y - h, 0.05), (x - w / 2 + tilt, y - h, 0.05)]
        A["cloth"].face(A["cloth"].verts(q), [(0, 0), (1, 0), (1, 1), (0, 1)], c, outward=(0, 0, 1))
    return A


SPIRE = [30.0]
FLAG_COLORS = [(0.75, 0.12, 0.06), (0.95, 0.68, 0.12), (0.12, 0.42, 0.32), (0.92, 0.88, 0.78), (0.12, 0.2, 0.5)]


def flags_world(cfg):
    """Pennant cords from the pagoda's spire down to short poles around the courtyard (world space)."""
    A = Asset("flagsworld")
    px, pz, _ = cfg["pagoda"]
    top = (px, cfg["pad"] + SPIRE[0], pz)
    for ax, az in cfg["anchors"]:
        foot = (ax, cfg["pad"] - 0.1, az)
        cyl(A["timber"], foot, 0.07, 3.4, seg=8, r1=0.05)
        end = (ax, cfg["pad"] + 3.2, az)
        L = length(sub(end, top))
        sag = 0.035 * L
        cord = [add(lerp(top, end, t / 60), (0, -sag * 4 * (t / 60) * (1 - t / 60), 0)) for t in range(61)]
        for k in range(60):
            beam(A["timber"], cord[k], cord[k + 1], 0.022, 0.022)
        n = int(L / 0.62)
        for k in range(1, n):
            t = k / n
            i = int(t * 60)
            p0 = lerp(cord[i], cord[min(60, i + 1)], t * 60 - i)
            d = norm(sub(cord[min(60, i + 1)], cord[i]))
            hd = norm((d[0], 0, d[2]))
            w = 0.26
            a = sub(p0, mul(d, w))
            b = add(p0, mul(d, w))
            h = 0.42
            sway = (R.uniform(-0.05, 0.05), 0, R.uniform(-0.05, 0.05))
            q = [a, b, add(add(b, (0, -h, 0)), sway), add(add(a, (0, -h, 0)), sway)]
            side = cross(hd, (0, 1, 0))
            A["cloth"].face(A["cloth"].verts(q), [(0, 0), (1, 0), (1, 1), (0, 1)], FLAG_COLORS[k % 5], outward=side)
    return A


def brazier():
    A = Asset("brazier")
    for k in range(3):
        a = math.radians(120 * k)
        beam(A["bronze"], (math.cos(a) * 0.55, 0, math.sin(a) * 0.55), (math.cos(a) * 0.3, 0.95, math.sin(a) * 0.3), 0.07, 0.07)
    cyl(A["bronze"], (0, 0.85, 0), 0.32, 0.38, seg=16, r1=0.55, rings=2)
    cyl(A["bronze"], (0, 1.23, 0), 0.58, 0.05, seg=16)
    cyl(A["ember"], (0, 1.1, 0), 0.5, 0.06, seg=12)
    return A


def terrace_wall(L=8.0, h=9.0):
    """A battered retaining wall: its top is at y = 0 (the terrace), its face leans back."""
    A = Asset("terracewall")
    box(A["stone"], (0, -h / 2, 0.2), (L / 2 + 0.05, 0, 0), (0, h / 2, -0.45), (0, 0, 1.1), col=tint(0.92, 0.05))
    abox(A["stone"], (0, 0.05, 0.75), (L + 0.1, 0.3, 1.4), col=tint(1.05, 0.04))
    return A


def wall_segment(L=8.0, h=1.3, t=0.7):
    """Courtyard parapet: stone wall with a little tiled coping."""
    A = Asset("parapet")
    abox(A["stone"], (0, h / 2, 0), (L, h, t), col=tint(1, 0.05))
    abox(A["plaster"], (0, h + 0.1, 0), (L, 0.2, t + 0.05))
    roof(A, (0, h + 0.2, 0), L / 2 + 0.25, t / 2 + 0.35, L / 2 - 0.05, 0, 0.32, lift=0.0, flare=0.0, thick=0.08, nu=16, nv=3,
         rafters=False, ridge=False, hips=False, horn=False)
    return A


def stairs(points, width=3.6):
    """The pilgrim stairs in world space: steps between control points, landings at them,
    and curb stones along both sides."""
    A = Asset("stairs")
    s = A["stone"]
    for (x0, z0, y0), (x1, z1, y1) in zip(points, points[1:]):
        dx, dz = x1 - x0, z1 - z0
        run = math.hypot(dx, dz)
        d = (dx / run, 0, dz / run)
        side = (-d[2], 0, d[0])
        land = ARGS.get("land", 2.6)  # flat landing at the start of each flight
        rise = y1 - y0
        n = max(1, int(round(rise / 0.19)))
        step_run = (run - land) / n
        # landing slab
        lc = (x0 + d[0] * land / 2, y0 - 0.2, z0 + d[2] * land / 2)
        box(s, lc, mul(d, land / 2 + 0.02), (0, 0.2, 0), mul(side, width / 2 + 0.4), col=tint(0.95, 0.06))
        for k in range(n):
            t0 = land + step_run * k
            yk = y0 + rise * (k + 1) / n
            c = (x0 + d[0] * (t0 + step_run / 2), yk - 0.35, z0 + d[2] * (t0 + step_run / 2))
            jitter = R.uniform(-0.04, 0.04)
            dd = rot_y(d, math.degrees(jitter) * 0.3)
            ss = rot_y(side, math.degrees(jitter) * 0.3)
            box(s, c, mul(dd, step_run / 2 + 0.06), (0, 0.35, 0), mul(ss, width / 2 + R.uniform(-0.08, 0.08)), col=tint(1.0, 0.12))
        # curbs: sloping stones on both sides
        for sgn in (-1, 1):
            off = mul(side, sgn * (width / 2 + 0.3))
            a = add((x0 + d[0] * land, y0 + 0.05, z0 + d[2] * land), off)
            b = add((x1, y1 + 0.05, z1), off)
            beam(A["stone"], add(a, (0, 0.1, 0)), add(b, (0, 0.1, 0)), 0.5, 0.55)
            a2 = add((x0, y0 + 0.05, z0), off)
            beam(A["stone"], add(a2, (0, 0.1, 0)), add(add((x0 + d[0] * land, y0 + 0.05, z0 + d[2] * land), off), (0, 0.1, 0)), 0.5, 0.55)
    return A


def bridge(span=24.0, width=3.4, rise=2.6):
    """A red arched footbridge (x along the span)."""
    A = Asset("bridge")
    n = 34

    def deck_y(t):
        return rise * math.sin(math.pi * t)

    for k in range(n):
        t0, t1 = k / n, (k + 1) / n
        x0, x1 = -span / 2 + span * t0, -span / 2 + span * t1
        c = ((x0 + x1) / 2, (deck_y(t0) + deck_y(t1)) / 2 + 0.6, 0)
        d = norm((x1 - x0, deck_y(t1) - deck_y(t0), 0))
        L = math.dist((x0, deck_y(t0)), (x1, deck_y(t1)))
        box(A["timber"], c, mul(d, L / 2 + 0.01), mul(norm(cross((0, 0, 1), d)), 0.06), (0, 0, width / 2), col=tint(1, 0.1))
    for sgn in (-1, 1):
        z = sgn * (width / 2 + 0.05)
        # stringer arches
        for k in range(n):
            t0, t1 = k / n, (k + 1) / n
            a = (-span / 2 + span * t0, deck_y(t0) + 0.3, z)
            b = (-span / 2 + span * t1, deck_y(t1) + 0.3, z)
            beam(A["lacquer"], a, b, 0.18, 0.5)
            beam(A["lacquer"], add(a, (0, 1.6, 0)), add(b, (0, 1.6, 0)), 0.14, 0.14)
            beam(A["lacquer"], add(a, (0, 0.95, 0)), add(b, (0, 0.95, 0)), 0.07, 0.07)
        for k in range(0, n + 1, 3):
            t = k / n
            p = (-span / 2 + span * t, deck_y(t) + 0.55, z)
            abox(A["lacquer"], add(p, (0, 0.55, 0)), (0.16, 1.25, 0.16))
            cyl(A["bronze"], add(p, (0, 1.18, 0)), 0.1, 0.16, seg=8, r1=0.02, bulge=0.04, rings=2)
    # piers
    for t in (0.3, 0.7):
        x = -span / 2 + span * t
        for sgn in (-1, 1):
            cyl(A["lacquer"], (x, -3.0, sgn * 1.2), 0.2, deck_y(t) + 3.4, seg=10, caps=False)
        abox(A["timber"], (x, deck_y(t) + 0.3, 0), (0.3, 0.3, width + 0.6))
    for sgn in (-1, 1):
        abox(A["stone"], (sgn * (span / 2 + 0.8), -0.4, 0), (2.4, 2.0, width + 1.4), col=tint(0.95, 0.05))
    return A


# --- output -------------------------------------------------------------------------------------
def to_blender(p, name):
    me = bpy.data.meshes.new(name)
    verts = [(x, -z, y) for x, y, z in p.v]
    me.from_pydata(verts, [], p.f)
    me.update()
    uv = me.uv_layers.new(name="UVMap")
    li = 0
    for poly, uvs in zip(me.polygons, p.uv):
        for k, loop in enumerate(poly.loop_indices):
            uv.data[loop].uv = uvs[k]
            li += 1
    cols = [c for c in p.col]
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    if any(abs(c[0] - 1) > 1e-3 or abs(c[1] - 1) > 1e-3 or abs(c[2] - 1) > 1e-3 for c in cols):
        B.set_vertex_colors(obj, [tuple(min(1.0, max(0.0, v)) for v in c) for c in cols])
    obj.data.materials.append(B.make_material(name + "_m", (0.8, 0.8, 0.8), roughness=0.7, vertex_colors=False))
    return obj


def export(asset):
    out = {}
    for key, part in asset.parts.items():
        if not part.f:
            continue
        name = f"{asset.name}_{key}"
        obj = to_blender(part, name)
        B.shade_smooth([obj], 38)
        path = sky.out_path(name + ".glb")  # noqa: F821
        B.export_glb(path, objects=[obj])
        out[key] = {"file": name + ".glb", "faces": len(part.f)}
        bpy.data.objects.remove(obj)
    return out


B.reset_scene()
result = {}
builders = [("hall", main_hall), ("pagoda", pagoda), ("gate", gate), ("bell", bell_pavilion), ("lantern", stone_lantern),
            ("paperlantern", paper_lantern), ("banner", banner), ("flags", flag_line), ("brazier", brazier),
            ("parapet", wall_segment), ("terracewall", terrace_wall), ("bridge", bridge)]
only = ARGS.get("only")
for key, fn in builders:
    if only and key not in only:
        continue
    result[key] = export(fn())
if ARGS.get("flags") and (not only or "flagsworld" in only):
    result["flagsworld"] = export(flags_world(ARGS["flags"]))
if ARGS.get("stairs") and (not only or "stairs" in only):
    result["stairs"] = export(stairs([tuple(p) for p in ARGS["stairs"]]))
sky.result(assets=result)  # noqa: F821
