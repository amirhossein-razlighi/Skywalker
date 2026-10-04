# SPDX-License-Identifier: GPL-3.0-or-later
# (runs inside Blender and uses its Python API, see docs/LICENSING.md)
"""Neon Requiem — street-food kit: the Ninth Street noodle stall and its stools.

Appended by `neon_requiem.py` to the library part of `neon_requiem_dcc.py` (Geo, Frame,
materials, sign(), export(), clear()), so this pass reuses the city kit's builders without
invalidating the big kit cache. JOB: {"stalls": [spec...], "stools": [spec...]}.

Stall local frame (Blender): x along the counter (centered), the customer side at -y, the
kitchen and back panel toward +y, z up; exported glTF faces +Z.
"""

MAT_DEFS.update({  # noqa: F821  (defined by the kit library this file is appended to)
    "wood_slat": dict(color=(0.20, 0.09, 0.05), roughness=0.55),
    "wood_top": dict(color=(0.26, 0.12, 0.05), roughness=0.22),
    "wood_light": dict(color=(0.62, 0.45, 0.28), roughness=0.55),
    "tile_cream": dict(color=(0.72, 0.66, 0.55), roughness=0.18),
    "ceramic_white": dict(color=(0.86, 0.85, 0.8), roughness=0.12),
    "ceramic_red": dict(color=(0.5, 0.05, 0.04), roughness=0.18),
    "broth": dict(color=(0.42, 0.2, 0.06), roughness=0.04),
    "noodle": dict(color=(0.86, 0.72, 0.38), roughness=0.4),
    "egg_white": dict(color=(0.92, 0.9, 0.84), roughness=0.3),
    "egg_yolk": dict(color=(0.95, 0.52, 0.06), roughness=0.25),
    "scallion": dict(color=(0.22, 0.52, 0.1), roughness=0.5),
    "nori": dict(color=(0.03, 0.05, 0.035), roughness=0.6),
    "chashu": dict(color=(0.58, 0.32, 0.22), roughness=0.45),
    "noren_indigo": dict(color=(0.035, 0.045, 0.12), roughness=0.9),
    "noren_ink": dict(color=(0.8, 0.76, 0.66), roughness=0.9),
    "vinyl_red": dict(color=(0.42, 0.02, 0.03), roughness=0.28),
    "chrome": dict(color=(0.8, 0.8, 0.82), roughness=0.12, metallic=1.0),
    "jar_amber": dict(color=(0.32, 0.13, 0.03), roughness=0.08),
    "sauce_red": dict(color=(0.45, 0.03, 0.02), roughness=0.12),
    "sauce_dark": dict(color=(0.03, 0.02, 0.01), roughness=0.08),
    "paper_cream": dict(color=(0.78, 0.72, 0.6), roughness=0.85),
    "plastic_teal": dict(color=(0.02, 0.32, 0.34), roughness=0.5),
    "lantern_rib": dict(color=(0.09, 0.02, 0.015), roughness=0.6),
    "tassel": dict(color=(0.35, 0.02, 0.02), roughness=0.8),
})


def lathe(g, profile, mat, segs=24, cx=0.0, cy=0.0, z=0.0):
    """Revolves a (radius, height) profile around a vertical axis (bowls, pots, jars)."""
    for i in range(len(profile) - 1):
        (r0, h0), (r1, h1) = profile[i], profile[i + 1]
        dr, dh = r1 - r0, h1 - h0
        for k in range(segs):
            a0, a1 = 2 * math.pi * k / segs, 2 * math.pi * (k + 1) / segs  # noqa: F821
            am = (a0 + a1) / 2
            c0, s0, c1, s1 = math.cos(a0), math.sin(a0), math.cos(a1), math.sin(a1)  # noqa: F821
            pts = [(cx + c0 * r0, cy + s0 * r0, z + h0), (cx + c1 * r0, cy + s1 * r0, z + h0),
                   (cx + c1 * r1, cy + s1 * r1, z + h1), (cx + c0 * r1, cy + s0 * r1, z + h1)]
            n = (math.cos(am) * dh, math.sin(am) * dh, -dr)  # noqa: F821  (outward for a rising profile)
            if r0 < 1e-5:
                pts = [pts[0], pts[2], pts[3]]
            elif r1 < 1e-5:
                pts = [pts[0], pts[1], pts[2]]
            g.poly(pts, n, mat)


def disc(g, cx, cy, z, r, mat, segs=24, up=True):
    pts = [(cx + math.cos(2 * math.pi * k / segs) * r, cy + math.sin(2 * math.pi * k / segs) * r, z) for k in range(segs)]  # noqa: F821
    g.poly(pts, (0, 0, 1 if up else -1), mat)


def ring(g, cx, cy, z, R, r, mat, n=20):
    pts = [(cx + math.cos(2 * math.pi * k / n) * R, cy + math.sin(2 * math.pi * k / n) * R, z) for k in range(n)]  # noqa: F821
    for k in range(n):
        g.tube(pts[k], pts[(k + 1) % n], r, mat, segs=5)


def noodle_bowl(g, w, cx, cy, z, rng, full=True):
    """A ramen bowl: ceramic body with a red rim band, broth, a nest of noodles, egg, chashu,
    nori and scallions; `w` is the smooth-shaded geometry, `g` the flat one."""
    R = 0.105
    lathe(w, [(0.0, 0.0), (0.045, 0.0), (0.05, 0.012), (0.085, 0.045), (R, 0.085), (R + 0.006, 0.09)], "ceramic_white", cx=cx, cy=cy, z=z)
    lathe(w, [(R + 0.006, 0.09), (R - 0.002, 0.094), (R - 0.008, 0.08)], "ceramic_red", cx=cx, cy=cy, z=z)
    if not full:
        disc(w, cx, cy, z + 0.02, 0.05, "ceramic_white")
        return
    disc(w, cx, cy, z + 0.074, R - 0.01, "broth", segs=28)
    # noodles: a few loose arcs above the broth
    for k in range(9):
        a = rng.uniform(0, 2 * math.pi)  # noqa: F821
        rr = rng.uniform(0.01, 0.055)
        p0 = (cx + math.cos(a) * rr, cy + math.sin(a) * rr, z + 0.077)  # noqa: F821
        b = a + rng.uniform(0.6, 1.6)
        p1 = (cx + math.cos(b) * (rr + 0.02), cy + math.sin(b) * (rr + 0.02), z + 0.082)  # noqa: F821
        g.tube(p0, p1, 0.0035, "noodle", segs=4)
    # half egg, chashu slices, nori sheet, scallions
    ex, ey = cx + 0.035, cy - 0.03
    lathe(w, [(0.0, 0.0), (0.024, 0.004), (0.028, 0.012)], "egg_white", cx=ex, cy=ey, z=z + 0.075, segs=14)
    disc(w, ex, ey, z + 0.087, 0.028, "egg_white", segs=14)
    disc(w, ex, ey, z + 0.0875, 0.014, "egg_yolk", segs=12)
    for k in range(2):
        g.box(cx - 0.06 + k * 0.03, cy + 0.01, z + 0.075, cx - 0.02 + k * 0.03, cy + 0.06, z + 0.083, "chashu")
    g.box(cx - 0.005, cy + 0.07, z + 0.06, cx + 0.06, cy + 0.075, z + 0.14, "nori")
    for k in range(10):
        x, y = cx + rng.uniform(-0.05, 0.05), cy + rng.uniform(-0.05, 0.05)
        g.box(x, y, z + 0.08, x + 0.008, y + 0.008, z + 0.084, "scallion")


def stall(spec):
    rng = np.random.default_rng(spec.get("seed", 9))  # noqa: F821
    W, D = spec.get("width", 3.4), spec.get("depth", 1.7)
    g, w = Geo(), Geo()  # noqa: F821
    x0, x1 = -W / 2, W / 2
    # --- customer counter: slatted wood front, varnished bar top with a brass-ish lip ---
    g.box(x0, 0.0, 0.0, x1, 0.55, 0.12, "metal_dark")                    # kick plate
    g.box(x0, 0.02, 0.12, x1, 0.55, 1.0, "wood_slat")
    n = int(W / 0.11)
    for i in range(n):
        sx = x0 + (i + 0.5) * W / n
        g.box(sx - 0.035, -0.02, 0.14, sx + 0.035, 0.02, 0.97, "wood_top" if i % 2 else "wood_slat")
    g.box(x0 - 0.04, -0.32, 1.0, x1 + 0.04, 0.62, 1.05, "wood_top")       # bar top
    g.box(x0 - 0.04, -0.34, 0.985, x1 + 0.04, -0.32, 1.05, "metal_galv")  # lip
    g.box(x0, -0.31, 0.975, x1, -0.26, 0.985, spec.get("under_glow", "glow_ffb060_2.5"))  # warm strip under the lip
    # pass shelf between bar and kitchen (condiments)
    g.box(x0 + 0.1, 0.48, 1.25, x1 - 0.1, 0.62, 1.28, "metal_galv")
    for sx in (x0 + 0.15, 0.0, x1 - 0.15):
        g.box(sx - 0.015, 0.53, 1.05, sx + 0.015, 0.57, 1.25, "metal_galv")
    # --- kitchen counter, burners, pots, wok ---
    g.box(x0, 1.05, 0.0, x1, D - 0.05, 0.92, "metal_galv")
    g.box(x0 - 0.02, 1.02, 0.92, x1 + 0.02, D - 0.03, 0.95, "chrome")
    for px, r, h in ((-1.05, 0.25, 0.44), (-0.4, 0.21, 0.36)):
        ring(g, px, 1.36, 0.97, 0.16, 0.012, "metal_dark", n=14)
        ring(g, px, 1.36, 0.955, 0.1, 0.008, "glow_4a8bff_6.0", n=14)       # gas flame ring
        lathe(w, [(0.0, 0.0), (r - 0.01, 0.0), (r, 0.01), (r, h), (r + 0.012, h + 0.004), (r + 0.012, h + 0.016)], "metal_galv",
              cx=px, cy=1.36, z=0.98)
        lathe(w, [(r + 0.015, 0.0), (r * 0.6, 0.04), (0.03, 0.05), (0.03, 0.07), (0.0, 0.07)], "metal_galv",
              cx=px + 0.04, cy=1.36 + 0.03, z=0.98 + h + 0.015)            # lid, nudged open
        g.tube((px - r - 0.08, 1.36, 0.98 + h - 0.05), (px - r, 1.36, 0.98 + h - 0.05), 0.012, "metal_dark")
        g.tube((px + r, 1.36, 0.98 + h - 0.05), (px + r + 0.08, 1.36, 0.98 + h - 0.05), 0.012, "metal_dark")
    wx = 0.45
    ring(g, wx, 1.35, 0.97, 0.16, 0.012, "metal_dark", n=14)
    ring(g, wx, 1.35, 0.955, 0.11, 0.009, "glow_4a8bff_7.0", n=14)
    lathe(w, [(0.0, 0.0), (0.12, 0.012), (0.24, 0.07), (0.31, 0.15), (0.315, 0.155)], "metal_dark", cx=wx, cy=1.35, z=0.99)
    g.tube((wx - 0.3, 1.35, 1.13), (wx - 0.65, 1.3, 1.2), 0.016, "wood_slat")
    # noodle baskets hanging over the pot rim, a ladle rail with ladles and strainers
    g.tube((x0 + 0.1, D - 0.2, 1.95), (x1 - 0.1, D - 0.2, 1.95), 0.012, "chrome")
    for i in range(7):
        lx = x0 + 0.35 + i * (W - 0.7) / 6
        g.tube((lx, D - 0.2, 1.95), (lx, D - 0.22, 1.55), 0.007, "chrome", segs=5)
        if i % 2:
            lathe(w, [(0.0, 0.0), (0.05, 0.01), (0.06, 0.05)], "chrome", cx=lx, cy=D - 0.22, z=1.5, segs=12)
        else:
            lathe(w, [(0.0, 0.0), (0.035, 0.008), (0.04, 0.04)], "metal_galv", cx=lx, cy=D - 0.22, z=1.51, segs=10)
    # --- back wall: cream tiles, glowing menu boards with invented glyphs, a shelf of jars and bowls ---
    g.box(x0, D - 0.05, 0.92, x1, D, 2.5, "tile_cream")
    for i in range(int(W / 0.15)):
        gx = x0 + i * 0.15
        g.box(gx, D - 0.056, 0.95, gx + 0.006, D - 0.05, 1.42, "trim")
    for j in range(4):
        g.box(x0, D - 0.056, 0.95 + j * 0.15, x1, D - 0.05, 0.956 + j * 0.15, "trim")
    for b in range(3):
        bx0 = x0 + 0.15 + b * (W - 0.3) / 3
        bx1 = bx0 + (W - 0.3) / 3 - 0.08
        g.box(bx0, D - 0.1, 1.98, bx1, D - 0.05, 2.45, "fascia")
        for row in range(3):
            zc = 2.37 - row * 0.15
            for c in range(3):
                gs = 0.07
                cxx = bx0 + 0.1 + c * 0.11
                for (a, bb) in glyph_strokes(np.random.default_rng(int(spec.get("seed", 9)) * 101 + b * 31 + row * 7 + c)):  # noqa: F821
                    pa = (cxx + (a[0] - 0.5) * gs, D - 0.105, zc + (a[1] - 0.5) * gs)
                    pb = (cxx + (bb[0] - 0.5) * gs, D - 0.105, zc + (bb[1] - 0.5) * gs)
                    g.tube(pa, pb, 0.004, "glow_ffe2b0_3.0", segs=4)
            g.box(bx1 - 0.2, D - 0.104, zc - 0.035, bx1 - 0.06, D - 0.1, zc + 0.035, "glow_ff8a3a_1.6" if row % 2 else "glow_ffc070_1.4")
    g.box(x0 + 0.1, D - 0.3, 1.62, x1 - 0.1, D - 0.05, 1.65, "wood_top")
    for i in range(9):
        jx = x0 + 0.25 + i * (W - 0.5) / 8
        if i % 3 == 1:
            for k in range(4):
                noodle_bowl(g, w, jx, D - 0.17, 1.65 + k * 0.03, rng, full=False)
        else:
            lathe(w, [(0.0, 0.0), (0.05, 0.0), (0.055, 0.12), (0.035, 0.15), (0.035, 0.17), (0.0, 0.17)],
                  "jar_amber" if i % 2 else "sauce_red", cx=jx, cy=D - 0.17, z=1.65, segs=12)
    # --- corner posts, a side wall on the downstream end, corrugated canopy, valance and noren ---
    for px, py in ((x0 + 0.04, -0.3), (x1 - 0.04, -0.3), (x0 + 0.04, D - 0.04), (x1 - 0.04, D - 0.04)):
        g.box(px - 0.04, py - 0.04, 0.0, px + 0.04, py + 0.04, 2.48, "metal_dark")
    g.box(x1 - 0.03, 0.62, 0.0, x1 + 0.02, D, 2.45, "wood_slat")
    g.box(x1 + 0.02, 0.8, 1.1, x1 + 0.025, 1.4, 1.9, "paper_cream")       # a faded poster
    zf, zb, yf, yb = 2.5, 2.78, -0.85, D + 0.15
    n = int((W + 0.6) / 0.08)
    for i in range(n):
        xa = x0 - 0.3 + i * (W + 0.6) / n
        xb = xa + (W + 0.6) / n
        da, db = (0.0, 0.025) if i % 2 == 0 else (0.025, 0.0)
        quad = [(xa, yf, zf + da), (xb, yf, zf + db), (xb, yb, zb + db), (xa, yb, zb + da)]
        g.poly(quad, (0, -0.1, 1), "awning_red")
        g.poly(quad, (0, 0.1, -1), "awning_red")
    g.box(x0 - 0.3, yf - 0.03, zf - 0.28, x1 + 0.3, yf, zf + 0.03, "fascia")         # valance board
    g.box(x0 - 0.3, yf - 0.035, zf - 0.27, x1 + 0.3, yf - 0.03, zf - 0.25, "glow_ff3a5a_5.0")  # neon edge
    panels = 5
    pw = (W + 0.4) / panels
    for p in range(panels):
        nx0 = x0 - 0.2 + p * pw + 0.02
        nx1 = nx0 + pw - 0.04
        g.box(nx0, yf + 0.04, 1.9, nx1, yf + 0.05, zf - 0.28, "noren_indigo")
        gs = 0.2
        for (a, bb) in glyph_strokes(np.random.default_rng(int(spec.get("seed", 9)) * 7 + p), 4):  # noqa: F821
            pa = ((nx0 + nx1) / 2 + (a[0] - 0.5) * gs, yf + 0.035, 2.06 + (a[1] - 0.5) * gs)
            pb = ((nx0 + nx1) / 2 + (bb[0] - 0.5) * gs, yf + 0.035, 2.06 + (bb[1] - 0.5) * gs)
            g.tube(pa, pb, 0.012, "noren_ink", segs=4)
    # warm light under the canopy: a tube and three bare bulbs on cords
    g.box(x0 + 0.1, 0.15, zf - 0.04, x1 - 0.1, 0.2, zf - 0.01, "glow_ffcf8a_3.5")
    for bx in (-1.0, 0.0, 1.0):
        g.tube((bx, 0.35, zf + 0.04), (bx, 0.35, 2.12), 0.004, "rubber", segs=4)
        lathe(w, [(0.0, 0.0), (0.03, 0.012), (0.038, 0.045), (0.025, 0.075), (0.012, 0.085)], "glow_ffb46a_5.0",
              cx=bx, cy=0.35, z=2.03, segs=12)
    # --- what's on the bar: ramen bowls, chopsticks, cups, condiments, a napkin box ---
    for k, bx in enumerate((-1.15, -0.3, 0.6)):
        noodle_bowl(g, w, bx, -0.08, 1.05, rng)
        g.tube((bx + 0.13, -0.2, 1.056), (bx + 0.16, 0.06, 1.058), 0.004, "wood_light", segs=4)
        g.tube((bx + 0.15, -0.2, 1.056), (bx + 0.18, 0.06, 1.058), 0.004, "wood_light", segs=4)
        lathe(w, [(0.0, 0.0), (0.03, 0.0), (0.036, 0.07), (0.037, 0.072)], "ceramic_white", cx=bx - 0.2, cy=0.05, z=1.05, segs=12)
    for k, cx in enumerate((-0.8, 0.15, 1.1)):
        lathe(w, [(0.0, 0.0), (0.025, 0.0), (0.025, 0.11), (0.012, 0.14), (0.012, 0.16), (0.0, 0.16)],
              "sauce_red" if k % 2 else "sauce_dark", cx=cx, cy=0.54, z=1.28, segs=10)
        lathe(w, [(0.0, 0.0), (0.02, 0.0), (0.02, 0.09), (0.0, 0.1)], "jar_amber", cx=cx + 0.07, cy=0.54, z=1.28, segs=10)
    g.box(1.25, 0.46, 1.05, 1.42, 0.58, 1.17, "metal_galv")
    g.box(1.27, 0.455, 1.12, 1.4, 0.46, 1.16, "paper_cream")
    # crates of produce stacked behind the counter
    for k, (cxx, zz) in enumerate(((-1.3, 0.0), (-1.3, 0.3))):
        g.box(cxx - 0.22, 0.65, zz, cxx + 0.22, 0.98, zz + 0.29, "plastic_teal")
    return [g.to_object(spec["name"]), w.to_object(spec["name"] + "_ware", smooth=True)]


def stool(spec):
    g, w = Geo(), Geo()  # noqa: F821
    lathe(w, [(0.0, 0.0), (0.2, 0.0), (0.2, 0.02), (0.06, 0.04), (0.035, 0.06)], "chrome")
    w.cyl(0, 0, 0.06, 0.66, 0.026, "chrome", segs=12, caps=False)
    ring(g, 0, 0, 0.26, 0.17, 0.011, "chrome", n=16)
    for k in range(3):
        a = 2 * math.pi * k / 3  # noqa: F821
        g.tube((0, 0, 0.26), (math.cos(a) * 0.17, math.sin(a) * 0.17, 0.26), 0.009, "chrome", segs=5)  # noqa: F821
    lathe(w, [(0.0, 0.0), (0.12, 0.0), (0.19, 0.01), (0.195, 0.02)], "chrome", z=0.65)
    lathe(w, [(0.195, 0.02), (0.2, 0.06), (0.19, 0.095), (0.15, 0.11), (0.0, 0.112)], spec.get("seat", "vinyl_red"), z=0.65)
    return [g.to_object(spec["name"]), w.to_object(spec["name"] + "_seat", smooth=True)]


def lantern(spec):
    """A smooth paper lantern: glowing red paper over bamboo hoops, black caps, a tassel and a cord
    (origin at its center)."""
    g, w = Geo(), Geo()  # noqa: F821
    R, H = spec.get("radius", 0.2), spec.get("height", 0.46)
    prof = []
    for i in range(17):
        t = -1 + 2 * i / 16
        prof.append((max(0.045, R * math.sqrt(max(0.0, 1 - (t * 0.97) ** 2))), t * H / 2))  # noqa: F821
    prof = [(0.0, -H / 2)] + prof + [(0.0, H / 2)]
    lathe(w, prof, spec.get("paper", "glow_ff2a12_1.4"), segs=28)
    for i in range(1, 8):
        t = -1 + 2 * i / 8
        r = R * math.sqrt(max(0.0, 1 - (t * 0.97) ** 2)) + 0.003  # noqa: F821
        ring(g, 0, 0, t * H / 2, r, 0.0035, "lantern_rib", n=24)
    lathe(w, [(0.0, H / 2 + 0.04), (0.06, H / 2 + 0.035), (0.065, H / 2 - 0.01), (0.04, H / 2 - 0.02)], "metal_dark", segs=16)
    lathe(w, [(0.04, -H / 2 + 0.02), (0.065, -H / 2 + 0.01), (0.06, -H / 2 - 0.035), (0.0, -H / 2 - 0.04)], "metal_dark", segs=16)
    g.tube((0, 0, H / 2 + 0.04), (0, 0, H / 2 + 0.45), 0.004, "rubber", segs=4)
    for k in range(6):
        a = 2 * math.pi * k / 6  # noqa: F821
        g.tube((0, 0, -H / 2 - 0.04), (math.cos(a) * 0.02, math.sin(a) * 0.02, -H / 2 - 0.2), 0.004, "tassel", segs=4)  # noqa: F821
    return [g.to_object(spec["name"]), w.to_object(spec["name"] + "_paper", smooth=True)]


def place_sign(spec):
    """A city-kit sign moved to its mounting point on the stall (spec['at'] = [x, y, z])."""
    objs = sign(spec)  # noqa: F821
    ax, ay, az = spec.get("at", (0, 0, 0))
    for ob in objs:
        ob.location = (ob.location[0] + ax, ob.location[1] + ay, ob.location[2] + az)
        bpy.context.view_layer.objects.active = ob  # noqa: F821
        ob.select_set(True)
        bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)  # noqa: F821
        ob.select_set(False)
    return objs


B.reset_scene()  # noqa: F821
os.makedirs(OUT, exist_ok=True)  # noqa: F821
made = 0
for spec in JOB.get("stalls", []):  # noqa: F821
    clear()  # noqa: F821
    objs = stall(spec)
    for s in spec.get("signs", []):
        objs += place_sign(s)
    export(objs, spec["name"] + ".glb")  # noqa: F821
    made += 1
for spec in JOB.get("stools", []):  # noqa: F821
    clear()  # noqa: F821
    export(stool(spec), spec["name"] + ".glb")  # noqa: F821
    made += 1
for spec in JOB.get("lanterns", []):  # noqa: F821
    clear()  # noqa: F821
    export(lantern(spec), spec["name"] + ".glb")  # noqa: F821
    made += 1
with open(os.path.join(OUT, "anchors.json"), "w") as f:  # noqa: F821
    json.dump({}, f)  # noqa: F821
sky.result(models=made)  # noqa: F821
