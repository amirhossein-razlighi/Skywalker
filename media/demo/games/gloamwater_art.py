"""Gloamwater art: every texture of the 2D showcase, painted procedurally (numpy + PIL).

  python3 gloamwater_art.py OUT_DIR [--only NAME]

Art direction: a drowned, bioluminescent underground kingdom. Limited palette (abyss teal-black,
haze teal, bioluminescent cyan, a violet accent, one warm amber for lanterns and the hero's
scarf), strong value contrast (dark masses with cyan rims against hazy depth), and atmospheric
depth through five parallax layers. Deterministic (seeded).
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from paint2d import Layer, Mask, blur, blur_x, cubic, curve, edge_light, fbm, fbm1d, mix, normal_map, rgb, smoothstep  # noqa: E402

PAL = dict(
    abyss="#03080b", deep="#071419", night="#0a1f26", teal="#0f3540", haze="#1c5560", mist="#3a8a8e",
    cyan="#62f2e2", ice="#b8fff6", violet="#8f6bff", lilac="#d3b5ff", moss="#2c8a6c", jade="#7ff0b8",
    amber="#ffb347", gold="#ffe0a0", rust="#c24e2c", bone="#ddd5c4", ink="#020405",
)


def C(name):
    return rgb(PAL[name])


# ============================================================================================
# Parallax backgrounds
# ============================================================================================


def _grad(H, W, stops):
    """Vertical gradient through (t, hex) stops -> (H, W, 3)."""
    y = np.linspace(0, 1, H, dtype=np.float32)
    ts = [t for t, _ in stops]
    cols = np.stack([rgb(c) for _, c in stops])
    out = np.stack([np.interp(y, ts, cols[:, k]) for k in range(3)], 1).astype(np.float32)
    return np.broadcast_to(out[:, None, :], (H, W, 3)).copy()


def _mass(L, mask, grad, tex, rim=None, rim_dir=(0, 6), rim_sigma=2.5, rim_amt=0.5, ao=0.35, ao_sigma=40, soften=0.0):
    """Paints a rock/ruin mass: gradient color, painterly texture, interior occlusion and a rim light."""
    m = blur(mask, soften, wrap=True) if soften > 0 else mask
    col = grad * (0.78 + 0.44 * tex[..., None])
    if ao > 0:
        inner = blur(mask, ao_sigma)
        col = col * (1 - ao * smoothstep(0.5, 1.0, inner))[..., None]
    L.paint(m, col)
    if rim is not None:
        r = edge_light(mask, rim_dir[0], rim_dir[1], rim_sigma) * m
        L.add(r, rgb(rim), rim_amt)
    return m


def _brush(W, H, scale, seed, stretch=(2.5, 0.7)):
    """Painterly texture: stretched fBm mixed with a little fine grain."""
    a = fbm(W, H, scale, 5, seed=seed, stretch=stretch)
    b = fbm(W, H, scale * 0.25, 3, seed=seed + 1)
    return np.clip(a * 0.8 + b * 0.2, 0, 1)


def _glow_into(L, emit, color, spill_sigma, spill_amt, air_amt=0.0):
    """Adds an emissive mask, its light spill on the painted surfaces and (optionally) a glow in the air."""
    g = blur(emit, spill_sigma)
    L.add(g * L.a, rgb(color), spill_amt)
    if air_amt > 0:
        halo = np.clip(g * air_amt, 0, 1)
        na = np.maximum(L.a, halo)
        # keep the existing color where opaque; tint the newly covered air
        fresh = np.clip(na - L.a, 0, 1)
        L.rgb = L.rgb * (1 - fresh[..., None]) + rgb(color) * fresh[..., None]
        L.a = na
    L.paint(emit, color)


def backdrop(out):
    """L0 (fixed to the camera): the abyss — teal light from the drowned sky far above, shafts, silt."""
    W, H = 1920, 1080
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    x = np.linspace(0, 1, W, dtype=np.float32)[None, :] * np.ones((H, 1), np.float32)
    L = Layer(W, H)
    L.rgb = _grad(H, W, [(0, "#2a7a80"), (0.3, "#155058"), (0.62, "#0b2d36"), (1, "#05141a")])
    L.a[:] = 1
    glow = np.exp(-(((x - 0.42) / 0.38) ** 2 + ((y + 0.1) / 0.5) ** 2))
    L.add(glow, rgb("#3fa0a0"), 0.5)
    shafts = np.zeros((H, W), np.float32)
    rng = np.random.default_rng(3)
    for k in range(11):
        cx = rng.uniform(-0.1, 0.95)
        width = rng.uniform(0.012, 0.06)
        d = (x - cx) - (y * 0.28)
        shafts += np.exp(-(d / width) ** 2) * (1 - y) ** 1.3 * rng.uniform(0.25, 0.9)
    shafts *= 0.5 + 0.5 * fbm(W, H, 240, 4, seed=4, stretch=(0.4, 1.8))
    L.add(blur(shafts, 8), rgb("#8fe8de"), 0.2)
    silt = fbm(W, H, 380, 5, seed=5)
    L.add(silt * (1 - y), rgb("#1d5a62"), 0.3)
    m = Mask(W, H, ss=2)
    for _ in range(320):
        px, py = rng.uniform(0, W), rng.uniform(0, H * 0.9)
        m.circle(px, py, rng.uniform(0.6, 1.6), int(rng.uniform(40, 170)))
    L.add(blur(m.get(), 0.9), rgb("#a8fff2"), 0.3)
    L.rgb += (np.random.default_rng(6).random((H, W, 1)).astype(np.float32) - 0.5) / 255.0 * 2.5
    L.save(os.path.join(out, "bg_abyss.jpg"), quality=93)


def _city_row(m, W, H, ground, rng, scale, kinds):
    """Draws a wrapping row of drowned buildings into mask m; returns lit-window candidate rects."""
    windows = []
    x = rng.uniform(0, 40)
    while x < W:
        kind = rng.choice(list(kinds.keys()), p=list(kinds.values()))
        w = rng.uniform(46, 120) * scale
        h = rng.uniform(0.2, 0.55) * H * scale
        cx = x + w / 2
        top = ground - h
        pieces = []
        if kind == "tower":
            pieces.append([(cx - w / 2, ground), (cx - w / 2, top), (cx + w / 2, top), (cx + w / 2, ground)])
            rh = rng.uniform(0.25, 0.6) * h
            pieces.append([(cx - w * 0.6, top + 3), (cx - w * 0.15, top - rh * 0.8), (cx, top - rh), (cx + w * 0.15, top - rh * 0.8), (cx + w * 0.6, top + 3)])
            for k in range(int(h // (34 * scale))):
                windows.append((cx + rng.uniform(-0.25, 0.25) * w, top + 20 * scale + k * 34 * scale, 4 * scale, 9 * scale))
        elif kind == "spire":
            w *= 0.5
            pieces.append([(cx - w / 2, ground), (cx - w * 0.28, top), (cx, top - h * 0.55), (cx + w * 0.28, top), (cx + w / 2, ground)])
        elif kind == "dome":
            w *= 1.6
            body_top = top + h * 0.45
            pieces.append([(cx - w / 2, ground), (cx - w / 2, body_top), (cx + w / 2, body_top), (cx + w / 2, ground)])
            m.ellipse(cx, body_top, w * 0.46, h * 0.36)
            m.ellipse(cx - W, body_top, w * 0.46, h * 0.36)
            pieces.append([(cx - 3 * scale, body_top - h * 0.3), (cx, body_top - h * 0.55), (cx + 3 * scale, body_top - h * 0.3)])
            for k in range(5):
                windows.append((cx - w * 0.35 + k * w * 0.17, body_top + 16 * scale, 5 * scale, 12 * scale))
        elif kind == "aqueduct":
            w *= 3.0
            h *= 0.45
            top = ground - h
            pieces.append([(cx - w / 2, top), (cx + w / 2, top), (cx + w / 2, top + 16 * scale), (cx - w / 2, top + 16 * scale)])
            n = 4
            for k in range(n + 1):
                px = cx - w / 2 + k * w / n
                pieces.append([(px - 8 * scale, top), (px + 8 * scale, top), (px + 10 * scale, ground), (px - 10 * scale, ground)])
        for p in pieces:
            m.poly(p)
            m.poly([(px - W, py) for px, py in p])
            m.poly([(px + W, py) for px, py in p])
        x += w * rng.uniform(0.55, 1.3)
    return windows


def far_city(out):
    """L1: a drowned city far away, dissolved in haze; a few windows still burn."""
    W, H = 2560, 1024
    rng = np.random.default_rng(11)
    ground = H * 0.66
    back = Mask(W, H)
    _city_row(back, W, H, ground - 20, rng, 0.75, {"tower": 0.35, "spire": 0.25, "dome": 0.25, "aqueduct": 0.15})
    back.poly([(0, ground - 24), (W, ground - 24), (W, H), (0, H)])
    front = Mask(W, H)
    wins = _city_row(front, W, H, ground + 30, rng, 1.0, {"tower": 0.4, "spire": 0.15, "dome": 0.3, "aqueduct": 0.15})
    rubble = _contour(W, ground + 40, 18, 200, 15)
    front.poly([(0, H)] + [(x, rubble[x]) for x in range(0, W, 8)] + [(W, rubble[-1]), (W, H)])
    bm, fm = back.get(), front.get()
    L = Layer(W, H)
    tex = _brush(W, H, 160, 13)
    _mass(L, bm, _grad(H, W, [(0, "#2a6e74"), (0.66, "#1b525a"), (1, "#123d45")]), tex, rim="#6ccfc6", rim_dir=(-3, 4),
          rim_sigma=2, rim_amt=0.25, ao=0.1, soften=1.2)
    _mass(L, fm, _grad(H, W, [(0, "#1f5a62"), (0.6, "#143f48"), (1, "#0d2e36")]), tex, rim="#5fc4bc", rim_dir=(-3, 5),
          rim_sigma=2, rim_amt=0.35, ao=0.2, soften=0.8)
    win = Mask(W, H, ss=2)
    for (x, y, w, h) in wins:
        if rng.random() < 0.45:
            win.poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)])
    _glow_into(L, win.get() * fm, "#bdfcf2", 10, 0.35)
    # Haze pooling at the base and a soft light-shaft wash.
    yy = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    L.tint(smoothstep(0.55, 0.95, yy) * L.a, rgb("#1a4f57"), 0.75)
    L.a = np.clip(L.a * (1 - smoothstep(0.9, 1.0, yy)) + 0.0, 0, 1)
    L.save(os.path.join(out, "bg_city.png"))


def _contour(W, base, amp, scale, seed, octaves=5):
    return base + fbm1d(W, scale, octaves, seed) * amp


def deep_cavern(out):
    """L2: the cavern vault — natural arches, hanging root curtains, giant drifting jellyfish."""
    W, H = 2640, 1080
    rng = np.random.default_rng(21)
    m = Mask(W, H)
    top = _contour(W, H * 0.08, H * 0.05, 320, 22)
    m.poly([(0, -5)] + [(x, top[x]) for x in range(0, W, 6)] + [(W, top[-1]), (W, -5)])
    for _ in range(18):
        cx = rng.uniform(0, W)
        ln = rng.uniform(0.06, 0.22) * H
        w = rng.uniform(26, 80)
        t0 = top[int(cx) % W] - 10
        poly = [(cx - w, t0), (cx - w * 0.4, t0 + ln * 0.55), (cx + rng.uniform(-8, 8), t0 + ln), (cx + w * 0.35, t0 + ln * 0.5), (cx + w, t0)]
        for dx in (0, -W, W):
            m.poly([(px + dx, py) for px, py in poly])
    floor = _contour(W, H * 0.97, H * 0.02, 260, 23)
    m.poly([(0, H + 5)] + [(x, floor[x]) for x in range(0, W, 6)] + [(W, floor[-1]), (W, H + 5)])
    # Natural arches: thick bridges of rock between pillars.
    for cx in (W * 0.18, W * 0.62):
        span = rng.uniform(520, 700)
        thick = rng.uniform(70, 100)
        apex = H * rng.uniform(0.2, 0.28)
        outer, inner = [], []
        for i in range(41):
            t = i / 40
            x = cx - span / 2 + span * t
            yo = floor[int(x) % W] - (floor[int(x) % W] - apex) * math.sin(math.pi * t) ** 0.6
            outer.append((x, yo - thick * (0.6 + 0.4 * math.sin(math.pi * t))))
            inner.append((x, yo + thick * 0.15))
        legw = 90
        pts = [(cx - span / 2 - legw, floor[int(cx - span / 2) % W] + 20)] + outer + [(cx + span / 2 + legw, floor[int(cx + span / 2) % W] + 20)]
        pts += [(cx + span / 2 + 10, floor[int(cx + span / 2) % W] + 20)] + inner[::-1] + [(cx - span / 2 - 10, floor[int(cx - span / 2) % W] + 20)]
        for dx in (0, -W, W):
            m.poly([(px + dx, py) for px, py in pts])
    mask = m.get()
    tex = _brush(W, H, 120, 24, stretch=(1.0, 2.0))
    L = Layer(W, H)
    _mass(L, mask, _grad(H, W, [(0, "#0c2c34"), (0.45, "#113842"), (1, "#0a252c")]), tex, rim="#4fb8b2", rim_dir=(-4, 6),
          rim_sigma=3, rim_amt=0.4, ao=0.3, ao_sigma=50, soften=0.8)
    # Hanging root curtains.
    roots = Mask(W, H)
    for _ in range(26):
        cx = rng.uniform(0, W)
        t0 = top[int(cx) % W]
        ln = rng.uniform(0.15, 0.5) * H
        sway = rng.uniform(-40, 40)
        pts = cubic((cx, t0), (cx + sway * 0.3, t0 + ln * 0.4), (cx + sway, t0 + ln * 0.75), (cx + sway * 0.6, t0 + ln), 18)
        for dx in (0, -W, W):
            roots.tapered([(px + dx, py) for px, py in pts], rng.uniform(4, 9), 1.2)
    rm = roots.get()
    L.paint(rm, mix(rgb("#0e3138"), rgb("#174a50"), 0.5))
    L.add(edge_light(rm, -2, 0, 1) * rm, rgb("#5fd0c0"), 0.3)
    # Kelp rising from the floor.
    kelp = Mask(W, H)
    for _ in range(18):
        cx = rng.uniform(0, W)
        hgt = rng.uniform(0.12, 0.3) * H
        b0 = floor[int(cx) % W] + 12
        sway = rng.uniform(-50, 50)
        pts = cubic((cx, b0), (cx + sway * 0.3, b0 - hgt * 0.35), (cx - sway * 0.5, b0 - hgt * 0.7), (cx + sway, b0 - hgt), 20)
        for dx in (0, -W, W):
            kelp.tapered([(px + dx, py) for px, py in pts], rng.uniform(6, 12), 2)
    km = kelp.get()
    L.paint(km, rgb("#0f3d3c"))
    L.add(edge_light(km, -2, 2, 1) * km, rgb("#5fe0b8"), 0.35)
    # Giant jellyfish drifting in the vault (glowing bells with trailing tentacles).
    jel = Mask(W, H)
    core = Mask(W, H)
    for _ in range(7):
        cx, cy = rng.uniform(0, W), rng.uniform(0.25, 0.55) * H
        r = rng.uniform(16, 30)
        for dx in (0, -W, W):
            jel.ellipse(cx + dx, cy, r, r * 0.72)
            jel.poly([(cx + dx - r, cy), (cx + dx + r, cy), (cx + dx + r * 0.8, cy + r * 0.25), (cx + dx - r * 0.8, cy + r * 0.25)])
            core.ellipse(cx + dx, cy - r * 0.1, r * 0.45, r * 0.3)
            for j in range(5):
                x0 = cx + dx - r * 0.6 + j * r * 0.3
                jel.tapered(cubic((x0, cy + r * 0.2), (x0 - 6, cy + r * 1.4), (x0 + 6, cy + r * 2.4), (x0 - 3, cy + r * 3.6), 12), 3.2, 0.8, 150)
    jm, cm = jel.get(), core.get()
    _glow_into(L, jm * 0.55, "#58d8e0", 24, 0.25, air_amt=0.35)
    L.add(cm, rgb("#d4fffa"), 0.6)
    yy = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    L.tint(smoothstep(0.6, 1.0, yy) * L.a, rgb("#0f3a42"), 0.6)
    L.save(os.path.join(out, "bg_vault.png"))


def _column(m, cx, base, h, w, broken=False, rng=None):
    """A drowned stone column with capital and base (optionally broken off)."""
    top = base - h
    m.poly([(cx - w * 0.75, base), (cx - w * 0.75, base - w * 0.4), (cx + w * 0.75, base - w * 0.4), (cx + w * 0.75, base)])
    if broken:
        jag = [(cx - w * 0.5, top + w), (cx - w * 0.2, top + w * 0.2), (cx + w * 0.05, top + w * 0.7), (cx + w * 0.3, top), (cx + w * 0.5, top + w * 0.6)]
        m.poly([(cx - w * 0.5, base)] + jag + [(cx + w * 0.5, base)])
    else:
        m.poly([(cx - w * 0.5, base), (cx - w * 0.45, top), (cx + w * 0.45, top), (cx + w * 0.5, base)])
        m.poly([(cx - w * 0.8, top), (cx + w * 0.8, top), (cx + w * 0.6, top + w * 0.35), (cx - w * 0.6, top + w * 0.35)])


def near_cavern(out):
    """L3: nearer grotto — sunken colonnade of the old kingdom, outcrops, glowing fungus and coral."""
    W, H = 2800, 1260
    rng = np.random.default_rng(31)
    m = Mask(W, H)
    floor = _contour(W, H * 0.7, H * 0.06, 340, 33)
    m.poly([(0, H + 5)] + [(x, floor[x]) for x in range(0, W, 5)] + [(W, floor[-1]), (W, H + 5)])
    for _ in range(12):
        cx = rng.uniform(0, W)
        r = rng.uniform(60, 160)
        b0 = floor[int(cx) % W] + 10
        for dx in (0, -W, W):
            m.ellipse(cx + dx, b0, r * 1.4, r * 0.7)
    top = _contour(W, H * 0.1, H * 0.06, 380, 32)
    m.poly([(0, -5)] + [(x, top[x]) for x in range(0, W, 5)] + [(W, top[-1]), (W, -5)])
    for _ in range(22):
        cx = rng.uniform(0, W)
        ln = rng.uniform(0.05, 0.2) * H
        w = rng.uniform(22, 64)
        t0 = top[int(cx) % W] - 8
        poly = [(cx - w, t0), (cx - w * 0.35, t0 + ln * 0.65), (cx + rng.uniform(-6, 6), t0 + ln), (cx + w * 0.3, t0 + ln * 0.55), (cx + w, t0)]
        for dx in (0, -W, W):
            m.poly([(px + dx, py) for px, py in poly])
    # The colonnade: columns with a broken entablature.
    cols = Mask(W, H)
    for k in range(2):
        x0 = W * (0.15 + 0.5 * k) + rng.uniform(-100, 100)
        n = rng.integers(3, 5)
        gap = rng.uniform(150, 210)
        h = rng.uniform(0.3, 0.4) * H
        for j in range(n):
            cx = x0 + j * gap
            base = floor[int(cx) % W] + 30
            broken = rng.random() < 0.35
            hh = h * (rng.uniform(0.35, 0.7) if broken else 1)
            for dx in (0, -W, W):
                _column(cols, cx + dx, base, hh, rng.uniform(34, 44), broken)
        lintel_y = floor[int(x0) % W] + 30 - h
        for dx in (0, -W, W):
            cols.poly([(x0 - 60 + dx, lintel_y), (x0 + gap * min(n - 1, 2) + 50 + dx, lintel_y - 6), (x0 + gap * min(n - 1, 2) + 40 + dx, lintel_y + 34),
                       (x0 - 50 + dx, lintel_y + 30)])
    tex = _brush(W, H, 110, 34)
    L = Layer(W, H)
    cm = cols.get()
    _mass(L, cm, _grad(H, W, [(0, "#0d2f37"), (1, "#081f26")]), _brush(W, H, 60, 36, stretch=(0.5, 3.0)), rim="#69d6cc",
          rim_dir=(-4, 5), rim_sigma=2.5, rim_amt=0.5, ao=0.25, soften=0.6)
    mask = m.get()
    _mass(L, mask, _grad(H, W, [(0, "#071a20"), (0.6, "#0a252c"), (1, "#06161b")]), tex, rim="#3fb0aa", rim_dir=(0, 7),
          rim_sigma=3, rim_amt=0.55, ao=0.35, ao_sigma=45, soften=0.6)
    # Fungus clusters on the floor (cyan and violet caps) — light spills on the rock around them.
    caps_c, caps_v, stalks = Mask(W, H), Mask(W, H), Mask(W, H)
    for _ in range(26):
        cx0 = rng.uniform(0, W)
        violet = rng.random() < 0.3
        for j in range(rng.integers(2, 6)):
            cx = cx0 + rng.uniform(-50, 50)
            base = floor[int(cx) % W] + rng.uniform(4, 16)
            r = rng.uniform(7, 22)
            sh = rng.uniform(1.0, 2.6) * r
            lean = rng.uniform(-8, 8)
            for dx in (0, -W, W):
                stalks.tapered([(cx + dx, base), (cx + dx + lean, base - sh)], r * 0.32, r * 0.22)
                (caps_v if violet else caps_c).ellipse(cx + dx + lean, base - sh, r, r * 0.46)
    sm = stalks.get()
    L.paint(sm, rgb("#9fd8d0"), 0.85)
    _glow_into(L, caps_c.get(), "#6ff4e2", 22, 0.55, air_amt=0.18)
    _glow_into(L, caps_v.get(), "#b494ff", 22, 0.5, air_amt=0.16)
    # Coral fans.
    c = Mask(W, H)
    for _ in range(14):
        cx = rng.uniform(0, W)
        b0 = floor[int(cx) % W] + 6
        r = rng.uniform(34, 80)
        for j in range(9):
            a = math.radians(-160 + j * 17 + rng.uniform(-5, 5))
            pts = curve((cx, b0), (cx + math.cos(a) * r * 0.5, b0 + math.sin(a) * r * 0.6), (cx + math.cos(a) * r, b0 + math.sin(a) * r), 8)
            for dx in (0, -W, W):
                c.tapered([(px + dx, py) for px, py in pts], 4.5, 1.5)
    cmk = c.get()
    L.paint(cmk, rgb("#1d6a68"))
    L.add(edge_light(cmk, 0, 2, 1) * cmk, rgb("#8ff5d8"), 0.5)
    yy = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    L.tint(smoothstep(0.75, 1.0, yy) * L.a, rgb("#0a2229"), 0.5)
    L.save(os.path.join(out, "bg_grotto.png"))


def shafts(out):
    """Light shafts falling through the water: soft slanted bands (alpha-blended over the vault)."""
    W, H = 2400, 1100
    x = np.linspace(0, 1, W, dtype=np.float32)[None, :] * np.ones((H, 1), np.float32)
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    rng = np.random.default_rng(51)
    a = np.zeros((H, W), np.float32)
    for _ in range(7):
        cx = rng.uniform(0, 1)
        width = rng.uniform(0.008, 0.03)
        for dx in (-1, 0, 1):
            d = (x - cx - dx) - y * 0.3
            a += np.exp(-(d / width) ** 2) * rng.uniform(0.4, 1.0)
    a *= (1 - y) ** 1.2 * (0.45 + 0.55 * fbm(W, H, 200, 4, seed=52, stretch=(0.3, 2.0)))
    L = Layer(W, H, color="#9ff0e6")
    L.a = np.clip(blur(a, 5) * 0.32, 0, 1)
    L.save(os.path.join(out, "bg_shafts.png"))


def mist(out):
    """A drifting band of silt-mist (low alpha), tileable horizontally."""
    W, H = 2400, 400
    y = np.linspace(0, 1, H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    n = fbm(W, H, 260, 5, seed=61, stretch=(3.0, 0.7))
    band = np.exp(-((y - 0.55) / 0.22) ** 2)
    L = Layer(W, H, color="#4aa3a3")
    L.a = np.clip(band * smoothstep(0.35, 0.85, n) * 0.55, 0, 1)
    L.save(os.path.join(out, "bg_mist.png"))


def foreground(out):
    """FG: a few near-black hanging roots and fern clusters that pass close to the lens (sparse)."""
    W, H = 2880, 760
    rng = np.random.default_rng(41)
    m = Mask(W, H)
    for k in range(2):
        cx = W * (k + rng.uniform(0.1, 0.9)) / 2
        ln = rng.uniform(0.22, 0.4) * H
        sway = rng.uniform(-60, 60)
        pts = cubic((cx, -10), (cx + sway * 0.2, ln * 0.4), (cx + sway * 0.8, ln * 0.7), (cx + sway, ln), 24)
        w0 = rng.uniform(18, 34)
        for dx in (0, -W, W):
            m.tapered([(px + dx, py) for px, py in pts], w0, 2)
            for j in range(2):
                i = int(rng.integers(6, 20))
                px, py = pts[i]
                side = rng.choice([-1, 1])
                m.tapered(curve((px + dx, py), (px + dx + side * 30, py + 26), (px + dx + side * 46, py + rng.uniform(50, 110)), 10), w0 * 0.3, 1)
    for k in range(7):
        cx = W * (k + rng.uniform(0.1, 0.9)) / 7
        hgt = rng.uniform(0.16, 0.34) * H
        for j in range(int(rng.integers(6, 11))):
            a = math.radians(-90 + rng.uniform(-60, 60))
            ln = hgt * rng.uniform(0.6, 1.0)
            tip = (cx + math.cos(a) * ln, H + math.sin(a) * ln)
            ctrl = (cx + math.cos(a) * ln * 0.3 + rng.uniform(-20, 20), H + math.sin(a) * ln * 0.75)
            for dx in (0, -W, W):
                m.tapered(curve((cx + dx, H + 6), (ctrl[0] + dx, ctrl[1]), (tip[0] + dx, tip[1]), 14), rng.uniform(10, 20), 1.5)
    mask = blur(m.get(), 1.5)  # slightly out of focus: it is close to the lens
    L = Layer(W, H)
    n = fbm(W, H, 80, 4, seed=42)
    L.paint(mask, mix(rgb("#010304"), rgb("#061115"), n[..., None]))
    L.add(edge_light(mask, 0, 5, 2) * mask, rgb("#1f6f72"), 0.3)
    L.save(os.path.join(out, "fg_roots.png"))



# ============================================================================================
# Tileset: 47 auto-tiling stone tiles (blob47) + interior variants + bridge planks
# ============================================================================================

T = 128          # tile size in pixels
DN, DS, DSIDE = 3, 16, 9   # surface insets: top, underside, sides (global, so every tile pair matches)
R_OUT = 30       # outer corner radius
MARGIN, SPACING = 4, 8   # sheet layout: edge pixels are extruded into the gaps


def blob_masks():
    def reduce(m):
        n, e, s, w = m & 1, m & 4, m & 16, m & 64
        r = m & (1 | 4 | 16 | 64)
        if n and e and m & 2:
            r |= 2
        if s and e and m & 8:
            r |= 8
        if s and w and m & 32:
            r |= 32
        if n and w and m & 128:
            r |= 128
        return r
    return sorted({reduce(m) for m in range(256)})


def _wobble(t, seed, amp):
    """Edge displacement along a tile edge: zero at both ends (so neighbours always meet)."""
    rng = np.random.default_rng(seed)
    out = np.zeros_like(t)
    for k in (2, 3, 5):
        out += np.sin(math.pi * k * t) * rng.uniform(-1, 1) / k
    return out * amp * np.sin(math.pi * t) ** 0.5


def _tile_solid(mask, ss=4):
    """Anti-aliased solid coverage of one tile for a reduced blob mask (quadrant construction)."""
    N, NE, E, SE, S, SW, Wb, NW = [(mask >> i) & 1 for i in range(8)]
    n = T * ss
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32) / ss + 0.5 / ss
    t_top = xx / T
    t_side = yy / T
    dn = DN + _wobble(t_top, 1, 2.5)
    ds = DS + _wobble(t_top, 2, 6.0)
    dw = DSIDE + _wobble(t_side, 3, 4.0)
    de = DSIDE + _wobble(t_side, 4, 4.0)
    solid = np.ones((n, n), bool)
    # Straight open edges.
    if not N:
        solid &= yy >= dn
    if not S:
        solid &= yy <= T - ds
    if not Wb:
        solid &= xx >= dw
    if not E:
        solid &= xx <= T - de
    # Outer corners (both edges open): round them.
    def outer(cx, cy, ix, iy, rx, ry):
        # corner of the inset rectangle at (cx, cy); inward direction (ix, iy)
        ox, oy = cx + ix * rx, cy + iy * ry
        inq = ((xx - ox) * ix < 0) & ((yy - oy) * iy < 0)
        return ~inq | (((xx - ox) / rx) ** 2 + ((yy - oy) / ry) ** 2 <= 1)
    if not N and not Wb:
        solid &= outer(DSIDE, DN, 1, 1, R_OUT, R_OUT * 0.8)
    if not N and not E:
        solid &= outer(T - DSIDE, DN, -1, 1, R_OUT, R_OUT * 0.8)
    if not S and not Wb:
        solid &= outer(DSIDE, T - DS, 1, -1, R_OUT * 0.8, R_OUT * 0.7)
    if not S and not E:
        solid &= outer(T - DSIDE, T - DS, -1, -1, R_OUT * 0.8, R_OUT * 0.7)
    # Inner corners (both edges closed, the diagonal open): an elliptical notch with the insets as radii.
    def notch(cx, cy, rx, ry):
        return ((xx - cx) / rx) ** 2 + ((yy - cy) / ry) ** 2 > 1
    if N and Wb and not NW:
        solid &= notch(0, 0, DSIDE, DN)
    if N and E and not NE:
        solid &= notch(T, 0, DSIDE, DN)
    if S and Wb and not SW:
        solid &= notch(0, T, DSIDE, DS)
    if S and E and not SE:
        solid &= notch(T, T, DSIDE, DS)
    img = Image.fromarray((solid * 255).astype(np.uint8), "L").resize((T, T), Image.LANCZOS)
    return np.asarray(img, np.float32) / 255.0


def _open_points(mask, solid):
    """Open sample points (tile pixel coords) around and inside the tile, for distances to the surface."""
    N, NE, E, SE, S, SW, Wb, NW = [(mask >> i) & 1 for i in range(8)]
    pts = []
    ys, xs = np.nonzero(solid < 0.5)
    pts.append(np.stack([xs + 0.5, ys + 0.5], 1))
    edge = np.arange(0, T, 2, dtype=np.float32) + 1
    far = np.full_like(edge, -1.0)
    if not N:
        pts.append(np.stack([edge, far], 1))
    if not S:
        pts.append(np.stack([edge, far * -1 + T], 1))
    if not Wb:
        pts.append(np.stack([far, edge], 1))
    if not E:
        pts.append(np.stack([far * -1 + T, edge], 1))
    for bit, (cx, cy), a, b in ((NE, (T + 1, -1), N, E), (SE, (T + 1, T + 1), S, E), (SW, (-1, T + 1), S, Wb), (NW, (-1, -1), N, Wb)):
        if a and b and not bit:
            pts.append(np.array([[cx, cy]], np.float32))
    return np.concatenate(pts, 0).astype(np.float32)


def _distance(points, maxd=64.0):
    """Distance from every tile pixel to the nearest open point, plus the direction to it."""
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32) + 0.5
    best = np.full((T, T), maxd, np.float32)
    bdx = np.zeros((T, T), np.float32)
    bdy = np.zeros((T, T), np.float32)
    P = points
    for i in range(0, len(P), 512):
        chunk = P[i:i + 512]
        dx = chunk[:, 0][:, None, None] - xx[None]
        dy = chunk[:, 1][:, None, None] - yy[None]
        d = np.sqrt(dx * dx + dy * dy)
        k = d.argmin(0)
        dm = np.take_along_axis(d, k[None], 0)[0]
        better = dm < best
        best = np.where(better, dm, best)
        bdx = np.where(better, np.take_along_axis(dx, k[None], 0)[0], bdx)
        bdy = np.where(better, np.take_along_axis(dy, k[None], 0)[0], bdy)
    ln = np.maximum(1e-3, np.sqrt(bdx * bdx + bdy * bdy))
    return best, bdx / ln, bdy / ln


def _rock_texture(seed=0):
    """A 128-periodic stone texture (tiles seamlessly with itself)."""
    base = fbm(T, T, 64, 5, seed=seed, wrap_x=True, wrap_y=True)
    cracks = fbm(T, T, 40, 4, seed=seed + 1, wrap_x=True, wrap_y=True)
    ridge = 1 - np.abs(cracks * 2 - 1)
    strata = fbm(T, T, 48, 3, seed=seed + 2, wrap_x=True, wrap_y=True, stretch=(3.0, 0.5))
    return base, smoothstep(0.82, 0.97, ridge), strata


def _paint_tile(mask, tex, variant_tex=None):
    solid = _tile_solid(mask)
    d, ux, uy = _distance(_open_points(mask, solid))
    base, cracks, strata = tex if variant_tex is None else variant_tex
    # Face direction of the nearest surface: up (moss), down (underside), sides.
    up = np.clip(-uy, 0, 1) ** 1.5
    down = np.clip(uy, 0, 1) ** 1.5
    side = np.clip(np.abs(ux), 0, 1) ** 1.5
    depth = smoothstep(0, 70, d)
    rock = mix(rgb("#2a5961"), rgb("#13292f"), depth[..., None])
    rock = rock * (0.78 + 0.4 * base[..., None]) * (0.9 + 0.15 * strata[..., None])
    rock = rock * (1 - 0.22 * cracks[..., None] * (1 - depth[..., None] * 0.5))
    # Rim light: cool on sides and tops, darker under overhangs.
    band = np.exp(-d / 7.0)
    rock = rock + rgb("#3aa0a0") * (band * (0.6 * side + 0.8 * up))[..., None]
    rock = rock * (1 - 0.45 * (np.exp(-d / 18.0) * down))[..., None]
    L = Layer(T, T)
    L.paint(solid, rock, 1.0)
    # Moss on top surfaces: a wobbling cushion with glowing beads.
    xs = np.arange(T, dtype=np.float32) / T
    thick = 9 + 5 * np.sin(2 * math.pi * xs * 2 + 0.7) + 3 * np.sin(2 * math.pi * xs * 5 + 2.1)
    moss = (d < thick[None, :] * up) & (up > 0.35)
    mossf = np.asarray(Image.fromarray((moss * 255).astype(np.uint8)).filter(__import__("PIL.ImageFilter", fromlist=["x"]).GaussianBlur(0.7)), np.float32) / 255
    mossf *= solid
    mcol = mix(rgb("#3fae86"), rgb("#1a5b4a"), np.clip(d / np.maximum(thick[None, :], 1), 0, 1)[..., None])
    mcol = mcol * (0.8 + 0.4 * base[..., None])
    L.paint(mossf, mcol, 1.0)
    # Glow beads at the moss surface.
    rng = np.random.default_rng(mask + 5)
    beads = Mask(T, T, ss=4)
    for i in range(0, T, 9):
        x = (i + rng.uniform(0, 8)) % T
        yi = int(np.clip(np.argmax(solid[:, int(x)] > 0.5), 0, T - 1))
        if up[yi, int(x)] > 0.6 and solid[yi, int(x)] > 0.5 and 4 < x < T - 4:
            beads.circle(x, yi + 1.5, rng.uniform(0.8, 1.7))
    bm = beads.get() * solid
    L.add(bm, rgb("#b8fff0"), 1.2)
    return L


def tileset(out):
    masks = blob_masks()
    assert len(masks) == 47
    cols, rows = 8, 7
    sheet = Image.new("RGBA", (MARGIN * 2 + cols * (T + SPACING) - SPACING, MARGIN * 2 + rows * (T + SPACING) - SPACING), (0, 0, 0, 0))

    def put(img, slot):
        """Pastes a tile with its edge pixels extruded into the spacing (no bleeding under filtering/mips)."""
        x = MARGIN + (slot % cols) * (T + SPACING)
        y = MARGIN + (slot // cols) * (T + SPACING)
        a = np.asarray(img)
        e = SPACING // 2
        big = np.pad(a, ((e, e), (e, e), (0, 0)), mode="edge")
        sheet.paste(Image.fromarray(big, "RGBA"), (x - e, y - e))
    tex = _rock_texture(7)
    for i, m in enumerate(masks):
        put(_paint_tile(m, tex).image(), i)
    # Interior variants (ids 48-51): different stone, blended to the shared texture at the borders.
    yy, xx = np.mgrid[0:T, 0:T].astype(np.float32)
    border = np.minimum(np.minimum(xx, T - 1 - xx), np.minimum(yy, T - 1 - yy))
    keep = smoothstep(6, 30, border)
    for v in range(4):
        vt = _rock_texture(100 + v * 7)
        mixed = tuple(a * (1 - keep) + b * keep for a, b in zip(tex, vt))
        put(_paint_tile(255, tex, mixed).image(), 47 + v)
    # Bridge planks (ids 52-54: left, middle, right) — old wood with brass bands.
    for k in range(3):
        L = Layer(T, T)
        m = Mask(T, T)
        m.poly([(0, 6), (T, 4), (T, 34), (0, 36)])
        wood = fbm(T, T, 30, 4, seed=60 + k, wrap_x=True, stretch=(4, 0.4))
        grain = mix(rgb("#3a2a1f"), rgb("#1b130e"), wood[..., None])
        L.paint(m.get(), grain)
        seams = Mask(T, T)
        for sx in (0, 42, 86):
            seams.line([(sx + 2, 6), (sx + 1, 35)], 2)
        L.paint(seams.get(), "#0b0806", 0.9)
        L.add(edge_light(m.get(), 0, 3, 1) * m.get(), rgb("#6fbfb2"), 0.35)
        if k != 1:
            post = Mask(T, T)
            x0 = 10 if k == 0 else T - 26
            post.poly([(x0, 0), (x0 + 16, 0), (x0 + 14, T), (x0 + 2, T)])
            L.paint(post.get(), "#20160f")
            L.add(edge_light(post.get(), 3, 0, 1) * post.get(), rgb("#5fb0a6"), 0.3)
        put(L.image(), 51 + k)
    sheet.save(os.path.join(out, "tiles_stone.png"), optimize=True)
    meta = {"format": "skywalker.tileset", "image": "tiles_stone.png", "tileSize": T, "margin": MARGIN, "spacing": SPACING,
            "names": {"plank_left": 52, "plank": 53, "plank_right": 54}}
    with open(os.path.join(out, "tiles_stone.tileset.json"), "w") as f:
        json.dump(meta, f, indent=2)


# ============================================================================================
# Wick, the lantern-bearer (hero): idle / run / jump frames + normal maps + glow frames
# ============================================================================================

HF = 220          # hero frame size (px); feet at the bottom center
HERO_PPU = 140    # 220 px frame = 1.57 world units; the character is ~1.45 units tall


def _rot(px, py, cx, cy, deg):
    a = math.radians(deg)
    dx, dy = px - cx, py - cy
    return cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a)


def _leg(m, hip, thigh_deg, shin_deg, L1=22, L2=21, w=8.5):
    """Draws a leg from the hip (angles from straight down, + = forward); returns the foot point."""
    a1, a2 = math.radians(thigh_deg), math.radians(thigh_deg + shin_deg)
    knee = (hip[0] + math.sin(a1) * L1, hip[1] + math.cos(a1) * L1)
    foot = (knee[0] + math.sin(a2) * L2, knee[1] + math.cos(a2) * L2)
    m.tapered([hip, knee], w * 1.1, w * 0.9)
    m.tapered([knee, foot], w * 0.9, w * 0.75)
    m.circle(knee[0], knee[1], w * 0.45)
    return foot, a2


def _boot(m, foot, ang, w=8.5):
    # a small rounded boot pointing forward (along +x when the shin is vertical)
    fx, fy = foot
    pts = [(-4, -5), (13, -3), (16, 2), (12, 5), (-5, 5)]
    rot = [_rot(fx + x, fy + y, fx, fy, -math.degrees(ang) * 0.6) for x, y in pts]
    m.poly(rot)


def hero_pose(kind, i, n):
    """Pose parameters for frame i of n of a clip."""
    p = i / n
    s = math.sin(2 * math.pi * p)
    c = math.cos(2 * math.pi * p)
    pose = dict(bob=0.0, lean=0.0, front=(4, 0), back=(-4, 0), flare=0.0, hem=p, scarf=0.25, scarf_lift=0.0,
                swing=6 * s, breathe=0.0, hat=0.0, arm=0.0, blink=False, air=0.0, phase=p)
    if kind == "idle":
        pose.update(bob=1.2 * s, breathe=s, scarf=0.18 + 0.05 * s, swing=5 * math.sin(2 * math.pi * p + 0.6), hat=1.5 * s,
                    front=(3, 2), back=(-3, 1), blink=(i == 5))
    elif kind == "run":
        # Contact at p=0 and 0.5; passing at 0.25/0.75. Legs swing opposite.
        fa = 38 * s
        ba = -38 * s
        fk = max(0.0, -c) * 55 + 8   # knee bends while the leg swings forward
        bk = max(0.0, c) * 55 + 8
        pose.update(bob=-5.5 * abs(math.sin(2 * math.pi * p + math.pi / 2)) + 2, lean=9, front=(fa, fk), back=(ba, bk),
                    flare=0.55 + 0.15 * c, scarf=0.75, scarf_lift=0.35 + 0.1 * s, swing=-24 + 8 * s, hat=-3 + 2 * c,
                    arm=10)
    elif kind == "jump":
        # 0 takeoff, 1 rise, 2 apex, 3 fall, 4 land
        table = [
            dict(bob=4, lean=6, front=(25, 70), back=(-10, 40), flare=0.3, scarf=0.5, scarf_lift=0.0, swing=-10, hat=-2, air=0),
            dict(bob=-2, lean=4, front=(40, 85), back=(-25, 60), flare=-0.4, scarf=0.6, scarf_lift=-0.5, swing=-28, hat=-4, air=1),
            dict(bob=-2, lean=0, front=(30, 70), back=(-15, 75), flare=0.2, scarf=0.55, scarf_lift=0.2, swing=-5, hat=0, air=1),
            dict(bob=-2, lean=-4, front=(20, 30), back=(-20, 35), flare=0.9, scarf=0.6, scarf_lift=0.9, swing=18, hat=4, air=1),
            dict(bob=6, lean=8, front=(28, 75), back=(-18, 70), flare=0.5, scarf=0.4, scarf_lift=0.4, swing=10, hat=3, air=0),
        ]
        pose.update(table[i])
    return pose


def hero_frame(kind, i, n):
    """Returns (color Layer, height map, glow Layer) for one frame."""
    P = hero_pose(kind, i, n)
    W = H = HF
    base = H - 8
    cx = W * 0.5 - 6
    hip = (cx, base - 42 + P["bob"])
    lean = P["lean"]

    def L(pt):  # lean the upper body around the hip
        return _rot(pt[0], pt[1], hip[0], hip[1], lean)

    layers = []  # (mask, color, height_bevel, depth order)
    # --- scarf tail (behind) --------------------------------------------------------------
    sc = Mask(W, H)
    neck = L((cx + 2, hip[1] - 66))
    tail = []
    ln = 46 + 40 * P["scarf"]
    theta = math.radians(18 + 72 * P["scarf"] - 25 * P["scarf_lift"])
    for k in range(18):
        t = k / 17
        wave = math.sin(t * 5.5 - P["phase"] * 2 * math.pi * 2) * (2 + 8 * t) * (0.3 + P["scarf"])
        x = neck[0] - 6 - math.sin(theta) * t * ln + math.cos(theta) * wave * 0.5
        y = neck[1] + 4 + math.cos(theta) * t * ln + math.sin(theta) * wave
        tail.append((x, y))
    sc.tapered(tail, 11, 6)
    end = tail[-1]
    sc.poly([(end[0] + 2, end[1] - 5), (end[0] - 9, end[1] - 7), (end[0] - 5, end[1] + 1), (end[0] - 10, end[1] + 6), (end[0] + 2, end[1] + 4)])
    layers.append((sc.get(), "#a8442a", 2.5, "scarf"))
    # --- back leg ---------------------------------------------------------------------------
    bl = Mask(W, H)
    foot, ang = _leg(bl, (hip[0] - 4, hip[1]), P["back"][0], P["back"][1])
    _boot(bl, foot, ang)
    layers.append((bl.get(), "#0b1519", 2.0, "back_leg"))
    # --- cloak --------------------------------------------------------------------------------
    cl = Mask(W, H)
    sh_y = hip[1] - 62 - P["breathe"] * 0.8
    hem_y = hip[1] + 10
    fl = P["flare"]
    pts = []
    # front edge (right side) from shoulder down to hem
    pts += cubic((cx + 10, sh_y), (cx + 22, sh_y + 18), (cx + 26 - fl * 4, hem_y - 22), (cx + 27 - fl * 10, hem_y - fl * 6), 14)
    # hem with ragged points, flaring back with speed
    hem = []
    for k in range(9):
        t = k / 8
        x = (cx + 27 - fl * 10) + (-(56 + fl * 14) * t)
        y = hem_y - fl * 6 + (fl * 18) * t ** 1.5
        wv = math.sin(t * 9 + P["hem"] * 2 * math.pi) * 2.5
        jag = 6 if k % 2 else 0
        hem.append((x, y + wv + jag))
    pts += hem
    # back edge up to the shoulder
    bx = cx + 27 - fl * 10 - (56 + fl * 14)
    pts += cubic((bx, hem_y + fl * 12), (cx - 30 - fl * 6, hem_y - 30), (cx - 20, sh_y + 14), (cx - 8, sh_y), 14)
    cl.poly([L(p) for p in pts])
    cmask = cl.get()
    layers.append((cmask, "#183740", 7.0, "cloak"))
    # --- front leg ----------------------------------------------------------------------------
    fl_m = Mask(W, H)
    foot, ang = _leg(fl_m, (hip[0] + 4, hip[1]), P["front"][0], P["front"][1])
    _boot(fl_m, foot, ang)
    layers.append((fl_m.get(), "#142228", 2.0, "front_leg"))
    # --- arm, staff and lantern ----------------------------------------------------------------
    arm = Mask(W, H)
    shoulder = L((cx + 12, sh_y + 12))
    hand = L((cx + 30 + P["arm"], hip[1] - 22))
    arm.tapered([shoulder, ((shoulder[0] + hand[0]) / 2 + 3, (shoulder[1] + hand[1]) / 2 + 4), hand], 13, 9)
    arm.circle(hand[0], hand[1], 4.5)
    layers.append((arm.get(), "#13303a", 2.5, "arm"))
    staff = Mask(W, H)
    tip = (hand[0] + 16, hand[1] - 14)
    staff.tapered([(hand[0] - 9, hand[1] + 9), tip], 3.2, 2.6)
    staff.tapered([tip, (tip[0] + 6, tip[1] - 2)], 2.6, 2.2)
    layers.append((staff.get(), "#3b2a1c", 1.0, "staff"))
    # Lantern hangs from the hook and swings.
    hook = (tip[0] + 6, tip[1] - 1)
    sw = P["swing"]
    lan_c = _rot(hook[0], hook[1] + 20, hook[0], hook[1], -sw)
    chain = Mask(W, H)
    chain.line([hook, _rot(hook[0], hook[1] + 9, hook[0], hook[1], -sw)], 1.6)
    layers.append((chain.get(), "#6b5530", 0.5, "chain"))
    lan = Mask(W, H)
    body = [(-7, -9), (7, -9), (9, -4), (9, 7), (6, 10), (-6, 10), (-9, 7), (-9, -4)]
    lan.poly([_rot(lan_c[0] + x, lan_c[1] + y, lan_c[0], lan_c[1], -sw) for x, y in body])
    cap = [(-5, -13), (5, -13), (8, -9), (-8, -9)]
    lan.poly([_rot(lan_c[0] + x, lan_c[1] + y, lan_c[0], lan_c[1], -sw) for x, y in cap])
    layers.append((lan.get(), "#8c6a32", 2.0, "lantern"))
    glass = Mask(W, H)
    gl = [(-5, -6), (5, -6), (6, 6), (-6, 6)]
    glass.poly([_rot(lan_c[0] + x, lan_c[1] + y, lan_c[0], lan_c[1], -sw) for x, y in gl])
    gmask = glass.get()
    layers.append((gmask, "#ffd98a", 1.0, "glass"))
    # --- head: face under the hat ----------------------------------------------------------
    head_c = L((cx + 4, sh_y - 12))
    face = Mask(W, H)
    face.ellipse(head_c[0] + 3, head_c[1] + 2, 13, 12)
    layers.append((face.get(), "#d9cdb8", 3.0, "face"))
    # Scarf wrap around the neck (front).
    wrap = Mask(W, H)
    wrap.ellipse(neck[0] + 1, neck[1] + 6, 17, 7.5)
    wrap.poly([(neck[0] + 8, neck[1] + 8), (neck[0] + 15, neck[1] + 22 - P["scarf_lift"] * 6), (neck[0] + 6, neck[1] + 20), (neck[0] + 2, neck[1] + 10)])
    layers.append((wrap.get(), "#c4532f", 3.0, "wrap"))
    # Hat: wide brim and a cone whose tip bends back.
    hat = Mask(W, H)
    hb = L((cx + 4, sh_y - 17))
    ht = P["hat"]
    ang = lean * 0.6 + ht * 0.3
    R = lambda x, y: _rot(hb[0] + x, hb[1] + y, hb[0], hb[1], ang)  # noqa: E731
    # A wide, shallow traveller's hat (woven, like a lamplighter's kasa) with a small knob.
    hat.poly([R(x, y) for x, y in [(-48, 6), (-40, 1), (-20, -12), (-6, -22), (6, -22), (20, -12), (40, 1), (50, 6),
                                     (44, 9), (20, 5), (0, 4), (-22, 5), (-42, 9)]])
    k = R(0, -24)
    hat.circle(k[0], k[1], 4.5)
    hmask = hat.get()
    layers.append((hmask, "#1f2a3b", 4.0, "hat"))
    # Hat band (brass ring).
    band = Mask(W, H)
    for r0 in (-30, -14, 2, 18, 34):  # woven ribs
        band.line([R(r0 * 0.25, -20), R(r0, 4)], 1.3)
    layers.append((band.get(), "#2d3b4f", 0.6, "band"))

    # --- composite color + height ------------------------------------------------------------
    col = Layer(W, H)
    height = np.zeros((H, W), np.float32)
    shade_dir = (-3, 4)  # light from upper right (rim on the right/top edges)
    for m, c, bev, name in layers:
        cc = rgb(c)
        col.paint(m, cc)
        h = blur(m, bev, wrap=False) * m
        height = np.maximum(height * (1 - m), h + height * m * 0.15)
    # Cloak folds: vertical darker streaks.
    folds = Mask(W, H)
    for k, off in enumerate((-12, 2, 14)):
        a = L((cx + off, sh_y + 20))
        b = L((cx + off - 6 - P["flare"] * 10, hem_y))
        folds.tapered([a, b], 2, 6)
    col.tint(blur(folds.get(), 2) * cmask, rgb("#0c2229"), 0.55)
    # Rim light on the upper-right edges of every part + a darker underside: reads as volume.
    alpha = col.a
    rim = edge_light(alpha, -3, 3, 1.2) * alpha
    col.add(rim, rgb("#5fd0c8"), 0.55)
    under = edge_light(alpha, 2, -4, 2.0) * alpha
    col.multiply(1 - 0.35 * under)
    # Face shadow from the brim.
    fy = head_c[1]
    yy = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    col.tint(smoothstep(fy + 6, fy - 8, yy) * layers[-6][0], rgb("#3a3a40"), 0.75)
    # Eye: a warm glint.
    eye = Mask(W, H)
    ex, ey = head_c[0] + 9, head_c[1] + 2
    if P["blink"]:
        eye.line([(ex - 3, ey), (ex + 3, ey)], 1.4)
    else:
        eye.ellipse(ex, ey, 2.6, 3.2)
    em = eye.get()
    col.paint(em, "#ffcf7a")
    # Glow frame: lantern core + eye (drawn unlit and bright by a second sprite).
    glow = Layer(W, H)
    core = blur(gmask, 3) * 1.6
    glow.paint(np.clip(blur(gmask, 9) * 1.2, 0, 1), "#ffb347", 0.55)
    glow.paint(np.clip(core, 0, 1), "#ffe2a8", 1.0)
    glow.paint(em, "#ffd890", 0.9)
    return col, height, glow


def hero(out):
    clips = [("idle", 8), ("run", 8), ("jump", 5)]
    d = os.path.join(out, "wick")
    dn = os.path.join(out, "wick_n")
    for p in (d, dn):
        os.makedirs(p, exist_ok=True)
    for kind, n in clips:
        for i in range(n):
            col, hgt, glow = hero_frame(kind, i, n)
            name = f"wick_{kind}_{i:02d}.png"
            col.image().save(os.path.join(d, name))
            normal_map(col.a, hgt, bevel=2.0, strength=1.2, detail=1.0).save(os.path.join(dn, name))


def portrait_sheet(out):
    """A contact sheet of the hero frames (look-dev only)."""
    d = os.path.join(out, "wick")
    names = sorted(os.listdir(d))
    sheet = Image.new("RGBA", (HF * 8, HF * 3), (40, 70, 80, 255))
    for k, nm in enumerate(names):
        im = Image.open(os.path.join(d, nm))
        kind = nm.split("_")[1]
        row = {"idle": 0, "run": 1, "jump": 2}[kind]
        col = int(nm.split("_")[2][:2])
        sheet.alpha_composite(im, (col * HF, row * HF))
    sheet.save(os.path.join(out, "_wick_sheet.png"))


# ============================================================================================
# Props (packed into an atlas by the engine; normal maps packed with the identical layout)
# ============================================================================================

PROPS = {}


def prop(fn):
    PROPS[fn.__name__] = fn
    return fn


def _save_prop(out, name, L, height=None, bevel=4.0):
    d, dn = os.path.join(out, "props"), os.path.join(out, "props_n")
    os.makedirs(d, exist_ok=True)
    os.makedirs(dn, exist_ok=True)
    L.image().save(os.path.join(d, f"{name}.png"))
    normal_map(L.a, height, bevel=bevel, strength=1.0, detail=1.0).save(os.path.join(dn, f"{name}.png"))


def _height_of(*masks_bevels):
    h = None
    for m, bev in masks_bevels:
        v = blur(m, bev, wrap=False) * m
        h = v if h is None else np.maximum(h * (1 - m), v + h * m * 0.2)
    return h


def _mushroom(W, H, cap_col, glow_col, seed, cap_w=0.8, cap_h=0.28, lean=0.0, stalk_w=0.13, spots=True):
    rng = np.random.default_rng(seed)
    base = (W * 0.5, H - 4)
    top = (W * 0.5 + lean * W * 0.18, H * (0.18 + cap_h * 0.5))
    stalk = Mask(W, H)
    pts = cubic(base, (base[0] - lean * 10, base[1] - H * 0.3), (top[0] - lean * 25, top[1] + H * 0.2), top, 24)
    stalk.tapered(pts, W * stalk_w * 1.35, W * stalk_w * 0.8)
    stalk.ellipse(base[0], base[1] - 4, W * stalk_w * 1.2, 8)
    sm = stalk.get()
    cap = Mask(W, H)
    cw, ch = W * cap_w * 0.5, H * cap_h
    cx, cy = top
    pts = []
    for i in range(41):
        a = math.pi * i / 40
        r = 1 + 0.04 * math.sin(i * 1.3 + seed)
        pts.append((cx - math.cos(a) * cw * r, cy - math.sin(a) * ch * r))
    # wavy rim underneath
    for i in range(21):
        t = i / 20
        x = cx + cw - 2 * cw * t
        pts.append((x, cy + 6 + 5 * math.sin(t * math.pi * 6 + seed)))
    cap.poly(pts)
    cm = cap.get()
    gills = Mask(W, H)
    gills.ellipse(cx, cy + 4, cw * 0.86, ch * 0.18)
    gm = gills.get() * (1 - smoothstep(0.0, 0.5, cm * 0))
    L = Layer(W, H)
    yy = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    xx = np.arange(W, dtype=np.float32)[None, :] * np.ones((H, 1), np.float32)
    # Stalk: pale, translucent, lit from the cap above.
    st_col = mix(rgb("#d6f4ee"), rgb("#4f8e8c"), smoothstep(cy, H, yy)[..., None])
    st_col = st_col * (0.85 + 0.25 * fbm(W, H, 30, 3, seed=seed, stretch=(0.3, 3))[..., None])
    L.paint(sm, st_col)
    L.add(edge_light(sm, -3, 0, 2) * sm, rgb(glow_col), 0.3)
    # Glow halo around the cap (in the air), then the cap itself.
    border = np.minimum(np.minimum(xx, W - 1 - xx), np.minimum(yy, H - 1 - yy))
    halo = np.clip(blur(cm, W * 0.07) * 0.9, 0, 1) * (1 - cm) * smoothstep(0, W * 0.08, border)
    L.paint(halo, glow_col, 0.55)
    L.paint(gm, mix(rgb(glow_col), rgb("#ffffff"), 0.35), 1.0)
    capv = smoothstep(cy - ch, cy + 4, yy)
    ccol = mix(rgb(cap_col) * 0.55, mix(rgb(cap_col), rgb("#ffffff"), 0.35), capv[..., None])
    ccol = ccol * (0.85 + 0.3 * fbm(W, H, 24, 4, seed=seed + 1)[..., None])
    L.paint(cm, ccol)
    L.add(edge_light(cm, 0, -3, 1.5) * cm, rgb("#ffffff"), 0.35)
    if spots:
        sp = Mask(W, H)
        for _ in range(int(cw / 6)):
            a = rng.uniform(0.15, 0.85) * math.pi
            r = rng.uniform(0.2, 0.85)
            sp.circle(cx - math.cos(a) * cw * r, cy - math.sin(a) * ch * r * 0.9, rng.uniform(2, 6))
        L.paint(sp.get() * cm, "#f2fffd", 0.8)
    h = _height_of((sm, 4), (cm, 8))
    return L, h


@prop
def mushroom_giant_cyan(out):
    L, h = _mushroom(300, 470, "#4fe8da", "#6ff6e6", 1, cap_w=0.92, cap_h=0.24, lean=0.25)
    _save_prop(out, "mushroom_giant_cyan", L, h)


@prop
def mushroom_giant_violet(out):
    L, h = _mushroom(260, 380, "#a988ff", "#b99cff", 2, cap_w=0.9, cap_h=0.26, lean=-0.3)
    _save_prop(out, "mushroom_giant_violet", L, h)


@prop
def mushroom_small(out):
    W, H = 220, 160
    L = Layer(W, H)
    hs = []
    for k, (x, sz, col) in enumerate(((50, 0.7, "#4fe8da"), (110, 1.0, "#6ff6e6"), (165, 0.55, "#a988ff"), (85, 0.45, "#4fe8da"))):
        w, hgt = int(90 * sz), int(140 * sz)
        m, hh = _mushroom(w, hgt, col, col, 10 + k, cap_w=0.95, cap_h=0.3, lean=(k - 1.5) * 0.3, stalk_w=0.16, spots=sz > 0.6)
        part = Layer(W, H)
        ox, oy = int(x - w / 2), H - hgt
        part.rgb[oy:oy + hgt, ox:ox + w] = m.rgb
        part.a[oy:oy + hgt, ox:ox + w] = m.a
        L.over(part)
    _save_prop(out, "mushroom_small", L, None)


def _blades(W, H, n, seed, col_base, col_tip, lean=0.0, width=(4, 9), height=(0.5, 1.0), glow_tips=0):
    rng = np.random.default_rng(seed)
    m = Mask(W, H)
    tips = Mask(W, H)
    for _ in range(n):
        x = rng.uniform(W * 0.1, W * 0.9)
        hgt = rng.uniform(*height) * H
        a = math.radians(-90 + rng.uniform(-35, 35) + lean)
        tip = (x + math.cos(a) * hgt, H + math.sin(a) * hgt)
        ctrl = (x + math.cos(a) * hgt * 0.2 + rng.uniform(-6, 6), H - hgt * 0.6)
        m.tapered(curve((x, H + 2), ctrl, tip, 12), rng.uniform(*width), 1.0)
        if glow_tips and rng.random() < glow_tips:
            tips.circle(tip[0], tip[1], rng.uniform(2.5, 4.5))
    mm = m.get()
    L = Layer(W, H)
    yy = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    L.paint(mm, mix(rgb(col_tip), rgb(col_base), smoothstep(0, H, yy)[..., None]))
    L.add(edge_light(mm, -2, 1, 1) * mm, rgb("#8ff0d0"), 0.3)
    if glow_tips:
        tm = tips.get()
        L.paint(np.clip(blur(tm, 5) * 1.5, 0, 1) * (1 - tm), "#9ffff0", 0.5)
        L.paint(tm, "#e8fffb")
    return L, _height_of((mm, 2))


@prop
def grass_tufts(out):
    for k in range(4):
        L, h = _blades(150, 70, 14 + k * 3, 70 + k, "#123c38", "#3fa886", lean=(k - 1.5) * 6, glow_tips=0.12 if k % 2 else 0)
        _save_prop(out, f"grass_{k}", L, h)


@prop
def glow_sprouts(out):
    for k in range(3):
        W, H = 90, 120
        rng = np.random.default_rng(80 + k)
        m, bulbs = Mask(W, H), Mask(W, H)
        for j in range(3 + k):
            x = rng.uniform(25, 65)
            hgt = rng.uniform(0.4, 0.9) * H
            bend = rng.uniform(-20, 20)
            pts = curve((x, H), (x + bend * 0.3, H - hgt * 0.5), (x + bend, H - hgt), 10)
            m.tapered(pts, 4.5, 2)
            bulbs.ellipse(x + bend, H - hgt, rng.uniform(5, 8), rng.uniform(6, 9))
        mm, bm = m.get(), bulbs.get()
        L = Layer(W, H)
        L.paint(mm, "#1f5a50")
        L.add(edge_light(mm, -2, 0, 1) * mm, rgb("#8ff0d0"), 0.4)
        L.paint(np.clip(blur(bm, 7) * 1.4, 0, 1) * (1 - bm), "#7ff6e0", 0.55)
        L.paint(bm, "#d8fff8")
        _save_prop(out, f"sprout_{k}", L, _height_of((mm, 2), (bm, 4)))


@prop
def stalactites(out):
    for k in range(3):
        W, H = 110, 260 + 60 * k
        rng = np.random.default_rng(90 + k)
        m = Mask(W, H)
        pts_l, pts_r = [], []
        for i in range(21):
            t = i / 20
            w = (W * 0.45) * (1 - t) ** 1.3 + 1.5
            wob = math.sin(t * 9 + k) * 4 * (1 - t)
            pts_l.append((W / 2 - w + wob, t * H))
            pts_r.append((W / 2 + w * 0.85 + wob, t * H))
        m.poly(pts_l + pts_r[::-1])
        mm = m.get()
        L = Layer(W, H)
        yy = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
        col = mix(rgb("#163c44"), rgb("#2c6a70"), smoothstep(0, H, yy)[..., None])
        col = col * (0.8 + 0.35 * fbm(W, H, 30, 4, seed=95 + k, stretch=(0.3, 3))[..., None])
        L.paint(mm, col)
        L.add(edge_light(mm, -4, 0, 2) * mm, rgb("#5fd0c8"), 0.45)
        # a glowing drip at the tip
        tip = Mask(W, H)
        tip.ellipse(W / 2 + 1, H - 6, 3.5, 5)
        tm = tip.get()
        L.paint(np.clip(blur(tm, 6) * 1.6, 0, 1) * (1 - tm), "#8ff8ee", 0.5)
        L.paint(tm, "#e8fffd")
        _save_prop(out, f"stalactite_{k}", L, _height_of((mm, 10)))


@prop
def coral(out):
    for k in range(2):
        W, H = 200, 160
        rng = np.random.default_rng(100 + k)
        m = Mask(W, H)
        tips = Mask(W, H)
        def branch(x, y, a, ln, w, depth):
            ex, ey = x + math.cos(a) * ln, y + math.sin(a) * ln
            m.tapered([(x, y), ((x + ex) / 2 + rng.uniform(-4, 4), (y + ey) / 2), (ex, ey)], w, w * 0.6)
            if depth == 0:
                tips.circle(ex, ey, 3.2)
                return
            for s in (-1, 1):
                branch(ex, ey, a + s * rng.uniform(0.25, 0.6), ln * 0.72, w * 0.68, depth - 1)
        for j in range(3):
            branch(W / 2 + (j - 1) * 14, H, math.radians(-90 + (j - 1) * 28), 44, 10, 3)
        mm, tm = m.get(), tips.get()
        L = Layer(W, H)
        col = "#d9566e" if k == 0 else "#ff8a5c"
        L.paint(mm, mix(rgb(col), rgb("#3a1a2a"), 0.55))
        L.add(edge_light(mm, -2, 2, 1) * mm, rgb("#ffb0a0"), 0.35)
        L.paint(np.clip(blur(tm, 5) * 1.4, 0, 1) * (1 - tm), "#ff9a8a", 0.4)
        L.paint(tm, "#ffd8cc")
        _save_prop(out, f"coral_{k}", L, _height_of((mm, 3)))


@prop
def lantern_post(out):
    W, H = 150, 360
    post, cage, glass, cap = Mask(W, H), Mask(W, H), Mask(W, H), Mask(W, H)
    px = 40
    post.poly([(px - 7, H), (px - 5, 60), (px + 5, 60), (px + 7, H)])
    post.poly([(px - 16, H), (px + 16, H), (px + 11, H - 20), (px - 11, H - 20)])
    # arm with a curl
    post.tapered(curve((px, 70), (px + 40, 40), (px + 78, 64), 16), 7, 5)
    post.circle(px, 60, 8)
    # hanging lantern
    lx, ly = px + 78, 120
    post.line([(lx, 64), (lx, ly - 34)], 2)
    cap.poly([(lx - 18, ly - 30), (lx + 18, ly - 30), (lx + 8, ly - 44), (lx - 8, ly - 44)])
    cage.poly([(lx - 20, ly - 30), (lx + 20, ly - 30), (lx + 16, ly + 22), (lx - 16, ly + 22)])
    cage.poly([(lx - 12, ly + 22), (lx + 12, ly + 22), (lx + 6, ly + 30), (lx - 6, ly + 30)])
    glass.poly([(lx - 14, ly - 24), (lx + 14, ly - 24), (lx + 11, ly + 17), (lx - 11, ly + 17)])
    pm, km, gm, cpm = post.get(), cage.get(), glass.get(), cap.get()
    L = Layer(W, H)
    L.paint(pm, "#16252a")
    L.add(edge_light(pm, -3, 2, 1.2) * pm, rgb("#6fc8c0"), 0.45)
    L.paint(np.clip(blur(gm, 14) * 1.2, 0, 1) * (1 - km), "#7ff4e4", 0.45)
    L.paint(km, "#2a3a3a")
    L.paint(cpm, "#2a3a3a")
    L.paint(gm, mix(rgb("#a8fff2"), rgb("#ffffff"), 0.3))
    bars = Mask(W, H)
    for bx in (-6, 6):
        bars.line([(lx + bx, ly - 24), (lx + bx * 0.8, ly + 17)], 2)
    L.paint(bars.get() * gm, "#1a2a2a")
    _save_prop(out, "lantern_post", L, _height_of((pm, 3), (km, 4)))


@prop
def hanging_lantern(out):
    W, H = 70, 300
    chain, cage, glass = Mask(W, H), Mask(W, H), Mask(W, H)
    for k in range(0, 220, 10):
        chain.ellipse(W / 2, k + 5, 2.5, 5)
    lx, ly = W / 2, 250
    cage.poly([(lx - 17, ly - 26), (lx + 17, ly - 26), (lx + 13, ly + 20), (lx - 13, ly + 20)])
    cage.poly([(lx - 9, ly - 26), (lx + 9, ly - 26), (lx, ly - 38)])
    glass.ellipse(lx, ly - 3, 11, 17)
    cm, km, gm = chain.get(), cage.get(), glass.get()
    L = Layer(W, H)
    L.paint(cm, "#1b2526")
    L.add(edge_light(cm, -2, 0, 1) * cm, rgb("#6fb8b0"), 0.35)
    L.paint(np.clip(blur(gm, 10) * 1.3, 0, 1) * (1 - km), "#ffc070", 0.5)
    L.paint(km, "#3a2c1c")
    L.paint(gm, "#ffe4b0")
    _save_prop(out, "hanging_lantern", L, _height_of((km, 4), (cm, 1)))


@prop
def lore_stone(out):
    W, H = 200, 240
    m = Mask(W, H)
    m.poly([(30, H), (22, 90), (44, 30), (100, 12), (156, 34), (178, 96), (172, H)])
    mm = m.get()
    L = Layer(W, H)
    yy = np.arange(H, dtype=np.float32)[:, None] * np.ones((1, W), np.float32)
    col = mix(rgb("#2f5d64"), rgb("#13272d"), smoothstep(0, H, yy)[..., None]) * (0.8 + 0.4 * fbm(W, H, 40, 5, seed=111)[..., None])
    L.paint(mm, col)
    L.add(edge_light(mm, -3, 3, 2) * mm, rgb("#6fd8cc"), 0.4)
    moss = Mask(W, H)
    moss.poly([(30, H), (24, 170), (60, 190), (90, 210), (140, 200), (172, 175), (172, H)])
    L.paint(moss.get() * mm, "#2f7a62", 0.85)
    _save_prop(out, "lore_stone", L, _height_of((mm, 12)))


@prop
def rope_bridge(out):
    """Ropes and posts drawn over the plank tiles of the chasm bridge (7 cells wide)."""
    W, H = 760, 200
    m = Mask(W, H)
    for (x0, x1) in ((20, 740),):
        for sag, y0 in ((26, 40), (18, 92)):
            pts = [(x0 + (x1 - x0) * t / 30, y0 + sag * math.sin(math.pi * t / 30)) for t in range(31)]
            m.line(pts, 4)
        for k in range(15):
            x = x0 + (x1 - x0) * k / 14
            t = k / 14
            m.line([(x, 40 + 26 * math.sin(math.pi * t)), (x, 180)], 2.2)
    for x in (16, 744):
        m.poly([(x - 9, 20), (x + 9, 20), (x + 11, H), (x - 11, H)])
    mm = m.get()
    L = Layer(W, H)
    L.paint(mm, "#2c2118")
    L.add(edge_light(mm, -2, 2, 1) * mm, rgb("#7fc8b8"), 0.35)
    _save_prop(out, "rope_bridge", L, _height_of((mm, 2)))


# ============================================================================================
# Entry
# ============================================================================================

JOBS = dict(hero=hero, tileset=tileset, backdrop=backdrop, far_city=far_city, deep_cavern=deep_cavern, near_cavern=near_cavern,
            shafts=shafts, mist=mist, foreground=foreground)


def main(out, only=None):
    os.makedirs(out, exist_ok=True)
    for name, fn in list(JOBS.items()) + list(PROPS.items()):
        if only and name not in only and not ("props" in only and name in PROPS):
            continue
        fn(out)
        print("painted", name, flush=True)


if __name__ == "__main__":
    args = sys.argv[1:]
    only = None
    if "--only" in args:
        i = args.index("--only")
        only = args[i + 1].split(",")
        args = args[:i] + args[i + 2:]
    main(args[0] if args else "art", only)
