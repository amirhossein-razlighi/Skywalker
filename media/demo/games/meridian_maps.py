"""Procedural continent for "Meridian Accord": relief, rivers, provinces, nations, map modes.

Everything is deterministic for a seed and written into <project>/maps/:

  height.png          16-bit grayscale relief for terrain_create (row 0 = north = -Z)
  political_<k>.png   nation colors, province/nation borders, rivers; k = front-line stage
  terrain.png         rivers, coast ink and faint borders over the natural ground
  supply.png          supply strength per province, railways, hubs
  paper.jpg           parchment map (opaque), shown with the sea hidden
  minimap_<k>.png     small political map for the HUD
  flags/<id>.png      nation flags (original designs)
  world.json          nations, provinces, cities, rivers, front stages (world coordinates)

The province map is a Voronoi diagram of Poisson-disk seeds over the land, with
noise-perturbed coordinates so borders wander like real ones. Nations grow from their
capitals over the province graph (Dijkstra), with mountain ridges as expensive borders.
"""
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SIZE_M = 2400.0          # terrain extent (m)
H_MIN, H_MAX = -16.0, 150.0
N_H = 1024               # relief grid
N_O = 2048               # overlay resolution
SEED = 1937
FONT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
                        "assets", "fonts")

NATIONS = [
    # id, name, long name, color, capital (u, v), weight (bigger grows further)
    ("aldmark", "Aldmark", "Commonwealth of Aldmark", "#5b7ea8", (0.24, 0.30), 1.0),
    ("varosse", "Varosse", "Kingdom of Varosse", "#a83c38", (0.17, 0.56), 1.35),
    ("kestria", "Kestria", "Republic of Kestria", "#4f8b4c", (0.44, 0.47), 0.85),
    ("orvane", "Orvane", "Orvane Directorate", "#7a519b", (0.76, 0.42), 0.85),
    ("belmora", "Belmora", "Free State of Belmora", "#c8a23c", (0.47, 0.79), 1.0),
    ("drayholt", "Drayholt", "Drayholt Union", "#6d5b49", (0.50, 0.17), 1.1),
    ("sundral", "Sundral", "Sundral Republic", "#d07b35", (0.80, 0.76), 1.0),
    ("ilvenreach", "Ilvenreach", "Grand Duchy of Ilvenreach", "#3f8f8b", (0.83, 0.18), 0.9),
    ("corvanne", "Corvanne", "Corvanne Confederacy", "#86acd0", (0.27, 0.80), 0.95),
    ("halcyra", "Halcyra", "Halcyran Assembly", "#b2527c", (0.63, 0.62), 1.2),
]
CITIES = {  # name, (u, v) near which the city is placed (snapped to a province center)
    "aldmark": [("Aldhaven", (0.24, 0.30)), ("Wrenford", (0.30, 0.40)), ("Skelby", (0.13, 0.27))],
    "varosse": [("Castelvaro", (0.17, 0.56)), ("Lisandre", (0.10, 0.66)), ("Pont-Aurel", (0.27, 0.53))],
    "kestria": [("Mirovan", (0.44, 0.47)), ("Delvar", (0.36, 0.58)), ("Ostrin", (0.52, 0.38))],
    "orvane": [("Sel Orva", (0.76, 0.42)), ("Kaldris", (0.87, 0.48))],
    "belmora": [("Aurenna", (0.47, 0.79)), ("Port Vey", (0.40, 0.90))],
    "drayholt": [("Grimholt", (0.50, 0.17)), ("Ironmere", (0.62, 0.26))],
    "sundral": [("Ashkara", (0.80, 0.76)), ("Ammerat", (0.90, 0.86))],
    "ilvenreach": [("Ilven", (0.83, 0.18)), ("Thornwick", (0.92, 0.27))],
    "corvanne": [("Corvel", (0.27, 0.80)), ("Brisa", (0.20, 0.90))],
    "halcyra": [("Seravel", (0.63, 0.62)), ("Lune", (0.70, 0.54))],
}
RANGES = [  # mountain ranges: polyline (u, v), half width (u), peak height (m)
    ([(0.20, 0.21), (0.33, 0.15), (0.47, 0.23), (0.60, 0.16), (0.70, 0.12)], 0.035, 140.0),
    ([(0.62, 0.30), (0.66, 0.44), (0.60, 0.56), (0.57, 0.70)], 0.028, 115.0),
    ([(0.12, 0.70), (0.22, 0.68), (0.33, 0.72)], 0.04, 70.0),
    ([(0.80, 0.60), (0.88, 0.67), (0.93, 0.74)], 0.03, 85.0),
]
# The war: Kestria's offensive pushes into Varosse over five stages.
ATTACKER, DEFENDER = "kestria", "varosse"
STAGES = 5


# --------------------------------------------------------------------------- noise
def _resize(a, n):
    return np.asarray(Image.fromarray(a.astype(np.float32)).resize((n, n), Image.BICUBIC))


def blur(a, r):
    """Approximate Gaussian blur (three separable box passes) of a float array."""
    r = max(int(r), 1)
    out = a.astype(np.float32)
    for _ in range(3):
        for axis in (0, 1):
            p = np.pad(out, [(r + 1, r) if ax == axis else (0, 0) for ax in (0, 1)], mode="edge")
            cs = np.cumsum(p, axis=axis, dtype=np.float64)
            hi = np.take(cs, np.arange(2 * r + 1, cs.shape[axis]), axis=axis)
            lo = np.take(cs, np.arange(0, cs.shape[axis] - 2 * r - 1), axis=axis)
            out = ((hi - lo) / (2 * r + 1)).astype(np.float32)
    return out


def fbm(n, base, octaves, seed, gain=0.5):
    rng = np.random.default_rng(seed)
    out = np.zeros((n, n), np.float32)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        cells = base * 2 ** o + 3
        out += _resize(rng.random((cells, cells)), n) * amp
        norm += amp
        amp *= gain
    return out / norm


def bilinear(field, x, y):
    """Samples `field` at fractional pixel coordinates (clamped)."""
    n = field.shape[0]
    x = np.clip(x, 0, n - 1.001)
    y = np.clip(y, 0, n - 1.001)
    x0, y0 = x.astype(np.int32), y.astype(np.int32)
    tx, ty = x - x0, y - y0
    a = field[y0, x0] * (1 - tx) + field[y0, x0 + 1] * tx
    b = field[y0 + 1, x0] * (1 - tx) + field[y0 + 1, x0 + 1] * tx
    return a * (1 - ty) + b * ty


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def seg_dist(u, v, pts):
    d = np.full(u.shape, 1e9, np.float32)
    for (ax, ay), (bx, by) in zip(pts[:-1], pts[1:]):
        dx, dy = bx - ax, by - ay
        t = np.clip(((u - ax) * dx + (v - ay) * dy) / (dx * dx + dy * dy), 0, 1)
        d = np.minimum(d, np.hypot(u - ax - t * dx, v - ay - t * dy))
    return d


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def to_world(u, v):
    return (u - 0.5) * SIZE_M, (v - 0.5) * SIZE_M


# --------------------------------------------------------------------------- relief
def continent_field(n):
    """Signed continentality c (> 0 on land) and its warped coordinates."""
    v, u = (np.mgrid[0:n, 0:n].astype(np.float32) + 0.5) / n
    wu = u + (fbm(n, 3, 5, SEED + 1) - 0.5) * 0.12 + (fbm(n, 9, 4, SEED + 4) - 0.5) * 0.035
    wv = v + (fbm(n, 3, 5, SEED + 2) - 0.5) * 0.12 + (fbm(n, 9, 4, SEED + 5) - 0.5) * 0.035
    ell = np.hypot((wu - 0.5) / 0.40, (wv - 0.52) / 0.36)
    c = (0.56 - ell * 0.62 + (fbm(n, 4, 6, SEED + 3) - 0.5) * 0.46 + (fbm(n, 14, 4, SEED + 6) - 0.5) * 0.16)
    bumps = [  # (u, v, radius, amount): gulfs (-) and peninsulas / islands (+)
        (0.05, 0.47, 0.07, -0.34), (0.97, 0.55, 0.06, -0.2), (0.40, 0.04, 0.06, -0.2),
        (0.88, 0.86, 0.07, 0.26), (0.10, 0.84, 0.06, 0.2), (0.90, 0.14, 0.05, 0.22), (0.96, 0.26, 0.025, 0.2),
        (0.15, 0.12, 0.05, 0.16), (0.70, 0.94, 0.035, 0.22), (0.04, 0.30, 0.03, 0.2), (0.30, 0.93, 0.05, 0.2),
        # The Meridian Sea: an inland sea joined to the southern ocean by the Vey Strait.
        (0.50, 0.60, 0.085, -0.62), (0.45, 0.64, 0.06, -0.3), (0.53, 0.72, 0.035, -0.5), (0.55, 0.80, 0.03, -0.55),
        (0.56, 0.88, 0.035, -0.55), (0.57, 0.96, 0.05, -0.5),
    ]
    for bu, bv, r, a in bumps:
        c += a * np.exp(-((wu - bu) ** 2 + (wv - bv) ** 2) / (r * r))
    # Keep a margin of sea around the map edge.
    edge = np.minimum(np.minimum(u, 1 - u), np.minimum(v, 1 - v))
    c -= (1 - smoothstep(0.02, 0.09, edge)) * 0.5
    return c, u, v, wu, wv


def relief():
    n = N_H
    c, u, v, wu, wv = continent_field(n)
    land = c > 0
    hills = fbm(n, 6, 6, SEED + 7)
    ridged = 1 - np.abs(fbm(n, 16, 5, SEED + 8) * 2 - 1)
    base = 3 + 34 * smoothstep(0.0, 0.32, c) + (hills - 0.5) * 36 * smoothstep(0.0, 0.1, c)
    mount = np.zeros_like(c)
    for pts, w, peak in RANGES:
        d = seg_dist(wu, wv, pts)
        m = np.exp(-(d / (w * 1.4)) ** 2) * (0.2 + 0.8 * ridged ** 2.2) * peak * (0.75 + 0.5 * hills)
        mount = np.maximum(mount, m)
    h = np.where(land, base + mount * smoothstep(0.0, 0.05, c), 0)
    sea = -1.5 - 11 * smoothstep(0, 0.07, -c) + (hills - 0.5) * 2
    h = np.where(land, np.maximum(h, 0.6), sea)
    # Beaches: a soft ramp across the coastline.
    h = np.where(land & (c < 0.012), 0.6 + c / 0.012 * 1.4, h)
    rivers = trace_rivers(h, c)
    for path in rivers:
        for k, (x, y) in enumerate(path):
            r = 0.8 + 1.4 * k / max(len(path), 1)
            y0, y1 = int(max(y - r - 1, 0)), int(min(y + r + 2, n))
            x0, x1 = int(max(x - r - 1, 0)), int(min(x + r + 2, n))
            yy, xx = np.mgrid[y0:y1, x0:x1]
            d = np.hypot(xx - x, yy - y)
            cut = np.clip(1 - d / (r + 1), 0, 1) * (1.2 + 1.6 * k / max(len(path), 1))
            h[y0:y1, x0:x1] = np.where(h[y0:y1, x0:x1] > 0.3, np.maximum(h[y0:y1, x0:x1] - cut, 0.35), h[y0:y1, x0:x1])
    return h, c, rivers


def trace_rivers(h, c, count=10):
    """Rivers from highland springs down to the sea: steepest descent on the smoothed relief
    plus a pull toward the coast (no dead-end basins). A river that meets an earlier one
    joins it as a tributary."""
    import heapq
    n = h.shape[0]
    # Depression-filled relief (priority flood from the sea), so steepest descent always
    # reaches the coast; computed at half resolution and upsampled.
    m = n // 2
    z = blur(h, 5)[::2, ::2] + fbm(m, 20, 3, SEED + 11) * 7.0 + np.clip(c[::2, ::2], 0, None) * 40
    filled = np.full((m, m), np.inf)
    heap = []
    for y, x in zip(*np.where(z <= 0.2)):
        filled[y, x] = z[y, x]
        heap.append((float(z[y, x]), int(y), int(x)))
    heapq.heapify(heap)
    while heap:
        f, y, x = heapq.heappop(heap)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            yy, xx = y + dy, x + dx
            if 0 <= yy < m and 0 <= xx < m and filled[yy, xx] == np.inf:
                filled[yy, xx] = max(z[yy, xx], f + 1e-3)
                heapq.heappush(heap, (filled[yy, xx], yy, xx))
    sm = _resize(filled.astype(np.float32), n)
    taken = np.zeros((n, n), bool)
    rng = np.random.default_rng(SEED + 12)
    cands = [(x, y) for y in range(24, n - 24, 28) for x in range(24, n - 24, 28) if h[y, x] > 28]
    rng.shuffle(cands)
    cands.sort(key=lambda p: -h[p[1], p[0]])
    paths = []
    for sx, sy in cands:
        if len(paths) >= count:
            break
        x, y = sx + 0.5, sy + 0.5
        path, seen, joined = [], set(), False
        for _ in range(4000):
            ix, iy = int(x), int(y)
            if not (2 <= ix < n - 2 and 2 <= iy < n - 2) or h[iy, ix] <= 0.2:
                break
            path.append((x, y))
            if taken[iy, ix]:
                joined = True
                break
            seen.add((ix, iy))
            best, bd = None, 0.0
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if dx == dy == 0 or (ix + dx, iy + dy) in seen:
                        continue
                    drop = (sm[iy, ix] - sm[iy + dy, ix + dx]) / math.hypot(dx, dy)
                    if best is None or drop > bd:
                        best, bd = (dx, dy), drop
            if best is None:
                break
            x, y = ix + best[0] + 0.5, iy + best[1] + 0.5
        if len(path) < (30 if joined else 70):
            continue
        p = np.array(path)
        for _ in range(14):  # meanders without stair steps
            p[1:-1] = (p[:-2] + p[1:-1] * 2 + p[2:]) / 4
        for qx, qy in p:
            taken[max(int(qy) - 3, 0):int(qy) + 4, max(int(qx) - 3, 0):int(qx) + 4] = True
        paths.append([tuple(q) for q in p])
    return paths


# --------------------------------------------------------------------------- provinces
def poisson(mask, r, rng, tries=30000):
    n = mask.shape[0]
    pts = []
    cell = r / math.sqrt(2)
    gw = int(n / cell) + 1
    grid = -np.ones((gw, gw), np.int32)
    for _ in range(tries):
        x, y = rng.random() * n, rng.random() * n
        if not mask[int(y), int(x)]:
            continue
        gx, gy = int(x / cell), int(y / cell)
        ok = True
        for i in range(max(gx - 2, 0), min(gx + 3, gw)):
            for j in range(max(gy - 2, 0), min(gy + 3, gw)):
                k = grid[j, i]
                if k >= 0 and (pts[k][0] - x) ** 2 + (pts[k][1] - y) ** 2 < r * r:
                    ok = False
                    break
            if not ok:
                break
        if ok:
            grid[gy, gx] = len(pts)
            pts.append((x, y))
    return pts


def provinces(c_hi):
    n = N_O
    land = c_hi > 0
    rng = np.random.default_rng(SEED + 20)
    seeds = poisson(land, 92, rng)
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    px = xx + (fbm(n, 10, 5, SEED + 21) - 0.5) * 70 + (fbm(n, 40, 3, SEED + 23) - 0.5) * 14
    py = yy + (fbm(n, 10, 5, SEED + 22) - 0.5) * 70 + (fbm(n, 40, 3, SEED + 24) - 0.5) * 14
    best = np.full((n, n), 1e12, np.float32)
    label = np.zeros((n, n), np.int32)
    for k, (sx, sy) in enumerate(seeds):
        d = (px - sx) ** 2 + (py - sy) ** 2
        m = d < best
        best[m] = d[m]
        label[m] = k
    label[~land] = -1
    # Centroids, areas and adjacency.
    count = len(seeds)
    flat = label.ravel()
    ok = flat >= 0
    area = np.bincount(flat[ok], minlength=count).astype(np.float64)
    cx = np.bincount(flat[ok], weights=xx.ravel()[ok], minlength=count) / np.maximum(area, 1)
    cy = np.bincount(flat[ok], weights=yy.ravel()[ok], minlength=count) / np.maximum(area, 1)
    adj = {k: set() for k in range(count)}
    for a, b in ((label[:, :-1], label[:, 1:]), (label[:-1, :], label[1:, :])):
        m = (a != b) & (a >= 0) & (b >= 0)
        for p, q in set(zip(a[m].tolist(), b[m].tolist())):
            adj[p].add(q)
            adj[q].add(p)
    return label, seeds, area, np.stack([cx, cy], 1), adj


def assign_nations(label, cent, area, adj, h_hi):
    import heapq
    n = N_O
    count = len(cent)
    owner = -np.ones(count, np.int32)
    dist = np.full(count, 1e18)
    heap = []
    for i, (_, _, _, _, (cu, cv), w) in enumerate(NATIONS):
        k = int(np.argmin((cent[:, 0] - cu * n) ** 2 + (cent[:, 1] - cv * n) ** 2 + (area <= 0) * 1e12))
        dist[k] = 0
        owner[k] = i
        heapq.heappush(heap, (0.0, k, i))
    weights = [nat[5] for nat in NATIONS]

    def ridge(p, q):  # max relief on the segment between two province centers
        (x0, y0), (x1, y1) = cent[p], cent[q]
        ts = np.linspace(0, 1, 12)
        hs = bilinear(h_hi, x0 + (x1 - x0) * ts, y0 + (y1 - y0) * ts)
        return float(hs.max())

    while heap:
        d, k, i = heapq.heappop(heap)
        if d > dist[k] or owner[k] != i:
            continue
        for q in adj[k]:
            step = np.hypot(*(cent[q] - cent[k])) * (1 + ridge(k, q) / 40.0) / weights[i]
            if d + step < dist[q]:
                dist[q] = d + step
                owner[q] = i
                heapq.heappush(heap, (d + step, q, i))
    # Islands with no path: nearest nation by distance.
    for k in range(count):
        if owner[k] < 0 and area[k] > 0:
            best = min(range(count), key=lambda j: np.hypot(*(cent[j] - cent[k])) if owner[j] >= 0 else 1e18)
            owner[k] = owner[best]
    return owner


# --------------------------------------------------------------------------- drawing helpers
def edges(label):
    """Boolean masks of province edges and nation edges (both sides, 1 px)."""
    e = np.zeros(label.shape, bool)
    e[:, :-1] |= label[:, :-1] != label[:, 1:]
    e[:-1, :] |= label[:-1, :] != label[1:, :]
    e[:, 1:] |= label[:, :-1] != label[:, 1:]
    e[1:, :] |= label[:-1, :] != label[1:, :]
    return e


def soft(mask, radius):
    img = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(img).astype(np.float32) / 255.0


def grow(mask, px):
    img = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(px * 2 + 1))
    return np.asarray(img) > 127


def composite(dst_rgb, dst_a, rgb, a):
    """Alpha-over in straight alpha."""
    a = a[..., None]
    out_a = a + dst_a[..., None] * (1 - a)
    out = (np.asarray(rgb, np.float32) * a + dst_rgb * dst_a[..., None] * (1 - a)) / np.maximum(out_a, 1e-5)
    return out, out_a[..., 0]


def save_rgba(path, rgb, a):
    img = np.concatenate([np.clip(rgb, 0, 255), np.clip(a[..., None] * 255, 0, 255)], 2).astype(np.uint8)
    Image.fromarray(img).save(path, optimize=True)


def draw_rivers(rivers, n, scale, width=2.2):
    img = Image.new("L", (n * 2, n * 2), 0)  # 2x supersampled
    d = ImageDraw.Draw(img)
    for path in rivers:
        pts = [(x * scale * 2, y * scale * 2) for x, y in path]
        for k in range(len(pts) - 1):
            w = int(round((0.8 + width * k / len(pts)) * 2))
            d.line([pts[k], pts[k + 1]], fill=255, width=max(w, 2))
    return np.asarray(img.resize((n, n), Image.LANCZOS)).astype(np.float32) / 255.0


# --------------------------------------------------------------------------- flags
def flag(nid, color, size=(360, 240)):
    w, h = size
    img = Image.new("RGB", size, color)
    d = ImageDraw.Draw(img)
    c = hexrgb(color)
    dark = tuple(int(v * 0.45) for v in c)
    light = (236, 228, 208)
    gold = (214, 176, 84)
    if nid == "aldmark":  # off-centre cross
        d.rectangle([0, 0, w, h], fill=c)
        d.rectangle([w * 0.28, 0, w * 0.40, h], fill=light)
        d.rectangle([0, h * 0.42, w, h * 0.58], fill=light)
        d.rectangle([w * 0.31, 0, w * 0.37, h], fill=dark)
        d.rectangle([0, h * 0.46, w, h * 0.54], fill=dark)
    elif nid == "varosse":  # crimson with golden chevron and crown circle
        d.polygon([(0, 0), (w * 0.42, h / 2), (0, h)], fill=gold)
        d.polygon([(0, h * 0.12), (w * 0.33, h / 2), (0, h * 0.88)], fill=dark)
        d.ellipse([w * 0.62, h * 0.32, w * 0.78, h * 0.68], outline=gold, width=8)
    elif nid == "kestria":  # green-white-green, with a rising sun
        d.rectangle([0, 0, w, h], fill=light)
        d.rectangle([0, 0, w, h / 3], fill=c)
        d.rectangle([0, h * 2 / 3, w, h], fill=c)
        d.pieslice([w * 0.38, h * 0.30, w * 0.62, h * 0.66], 180, 360, fill=gold)
        for k in range(9):
            a = math.pi + math.pi * (k + 0.5) / 9
            d.line([(w / 2, h * 0.48), (w / 2 + math.cos(a) * w * 0.17, h * 0.48 + math.sin(a) * w * 0.17)], fill=gold, width=4)
    elif nid == "orvane":  # vertical bicolor with star
        d.rectangle([0, 0, w / 3, h], fill=dark)
        d.rectangle([w / 3, 0, w, h], fill=c)
        star(d, w / 6, h / 2, h * 0.16, light)
    elif nid == "belmora":  # diagonal
        d.polygon([(0, h), (w, 0), (w, h)], fill=tuple(int(v * 0.7) for v in c))
        d.line([(0, h), (w, 0)], fill=light, width=16)
    elif nid == "drayholt":  # horizontal tricolor dark/umber/dark with a tower
        d.rectangle([0, 0, w, h * 0.25], fill=(40, 34, 30))
        d.rectangle([0, h * 0.75, w, h], fill=(40, 34, 30))
        d.rectangle([w * 0.45, h * 0.32, w * 0.55, h * 0.68], fill=light)
        d.rectangle([w * 0.42, h * 0.30, w * 0.58, h * 0.36], fill=light)
    elif nid == "sundral":  # sun disc
        d.rectangle([0, h * 0.75, w, h], fill=dark)
        d.ellipse([w * 0.36, h * 0.18, w * 0.64, h * 0.6], fill=light)
    elif nid == "ilvenreach":  # canton with three stars
        d.rectangle([0, 0, w * 0.42, h * 0.5], fill=dark)
        for k in range(3):
            star(d, w * (0.09 + 0.12 * k), h * 0.25, h * 0.07, light)
        for k in range(3):
            d.rectangle([w * 0.42, h * (0.5 + k * 0.17), w, h * (0.58 + k * 0.17)], fill=light)
    elif nid == "corvanne":  # quartered
        d.rectangle([0, 0, w / 2, h / 2], fill=light)
        d.rectangle([w / 2, h / 2, w, h], fill=light)
        d.ellipse([w * 0.4, h * 0.35, w * 0.6, h * 0.65], fill=dark)
    elif nid == "halcyra":  # bordered with a wave
        d.rectangle([0, 0, w, h], fill=light)
        d.rectangle([w * 0.06, h * 0.09, w * 0.94, h * 0.91], fill=c)
        pts = [(w * 0.06 + t * w * 0.88, h * 0.55 + math.sin(t * math.pi * 4) * h * 0.06) for t in np.linspace(0, 1, 40)]
        d.line(pts, fill=light, width=10)
    return img


def star(d, cx, cy, r, fill):
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        rr = r if k % 2 == 0 else r * 0.42
        pts.append((cx + math.cos(a) * rr, cy + math.sin(a) * rr))
    d.polygon(pts, fill=fill)


def counter_face(kind, color, path):
    """NATO-style unit symbol on a cream plate (the front of a wooden war-game block)."""
    w, h = 256, 192
    img = Image.new("RGB", (w, h), (226, 214, 186))
    d = ImageDraw.Draw(img)
    c = hexrgb(color)
    d.rectangle([0, 0, w - 1, h - 1], outline=c, width=14)
    box = [52, 46, w - 52, h - 46]
    d.rectangle(box, outline=(30, 26, 22), width=7)
    if kind in ("infantry", "mechanized"):
        d.line([box[0], box[1], box[2], box[3]], fill=(30, 26, 22), width=7)
        d.line([box[0], box[3], box[2], box[1]], fill=(30, 26, 22), width=7)
    if kind in ("armor", "mechanized"):
        d.rounded_rectangle([box[0] + 22, box[1] + 22, box[2] - 22, box[3] - 22], radius=26, outline=(30, 26, 22), width=7)
    if kind == "artillery":
        d.ellipse([w / 2 - 14, h / 2 - 14, w / 2 + 14, h / 2 + 14], fill=(30, 26, 22))
    for k in range(3 if kind != "armor" else 2):  # size marks above the box
        x = w / 2 + (k - (1 if kind != "armor" else 0.5)) * 18
        d.line([x, 18, x, 38], fill=(30, 26, 22), width=6)
    img.save(path)


def ui_art(project):
    """Resource icons, a leader portrait and the event illustration (sepia 'photograph')."""
    art = os.path.join(project, "ui")
    os.makedirs(art, exist_ok=True)
    gold, cream = (222, 186, 104, 255), (232, 222, 196, 255)

    def icon(name, draw):
        im = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
        draw(ImageDraw.Draw(im))
        im.resize((64, 64), Image.LANCZOS).save(os.path.join(art, f"icon_{name}.png"))

    def pp(d):
        star(d, 64, 66, 52, gold)
        d.ellipse([50, 52, 78, 80], fill=(40, 30, 20, 255))

    def manpower(d):
        d.ellipse([44, 14, 84, 54], fill=cream)
        d.pieslice([22, 58, 106, 150], 180, 360, fill=cream)

    def industry(d):
        d.polygon([(10, 118), (10, 60), (40, 76), (40, 60), (70, 76), (70, 60), (100, 76), (100, 20), (118, 20), (118, 118)],
                  fill=(176, 180, 186, 255))

    def fuel(d):
        d.polygon([(64, 10), (100, 74), (28, 74)], fill=(70, 70, 76, 255))
        d.ellipse([28, 46, 100, 118], fill=(70, 70, 76, 255))
        d.ellipse([46, 70, 62, 92], fill=(150, 150, 160, 255))

    def stability(d):
        d.line([(64, 14), (64, 112)], fill=cream, width=8)
        d.line([(20, 34), (108, 34)], fill=cream, width=8)
        for x in (20, 108):
            d.line([(x, 34), (x - 16, 76)], fill=cream, width=4)
            d.line([(x, 34), (x + 16, 76)], fill=cream, width=4)
            d.chord([x - 20, 56, x + 20, 96], 0, 180, fill=cream)
        d.rectangle([40, 108, 88, 118], fill=cream)

    def war(d):
        d.line([(18, 110), (110, 18)], fill=(200, 200, 206, 255), width=12)
        d.line([(18, 18), (110, 110)], fill=(200, 200, 206, 255), width=12)
        d.line([(30, 76), (52, 98)], fill=gold, width=12)
        d.line([(98, 76), (76, 98)], fill=gold, width=12)

    for name, fn in (("pp", pp), ("manpower", manpower), ("industry", industry), ("fuel", fuel),
                     ("stability", stability), ("war", war)):
        icon(name, fn)

    # Leader portrait: a sepia silhouette with a peaked cap and a high collar.
    w, h = 256, 320
    rng = np.random.default_rng(SEED + 60)
    yy, xx = np.mgrid[0:h, 0:w]
    bg = 150 + 60 * np.exp(-((xx - w * 0.55) ** 2 + (yy - h * 0.35) ** 2) / (2 * 90 ** 2)) + rng.normal(0, 5, (h, w))
    im = Image.fromarray(np.clip(np.stack([bg * 1.0, bg * 0.86, bg * 0.66], 2), 0, 255).astype(np.uint8)).convert("RGBA")
    d = ImageDraw.Draw(im)
    ink = (44, 34, 26, 255)
    d.pieslice([10, 210, 246, 420], 180, 360, fill=ink)           # shoulders
    d.rectangle([98, 170, 158, 236], fill=ink)                      # neck / collar
    d.ellipse([84, 92, 172, 196], fill=ink)                         # head
    d.chord([66, 64, 190, 132], 180, 360, fill=ink)                 # cap crown
    d.rectangle([70, 96, 186, 112], fill=ink)                       # cap band
    d.polygon([(76, 110), (180, 110), (196, 122), (84, 120)], fill=ink)  # visor
    for k in range(3):
        d.ellipse([118, 250 + k * 24, 130, 262 + k * 24], fill=(190, 160, 96, 255))  # buttons
    d.rectangle([150, 236, 214, 252], fill=(120, 40, 34, 255))     # ribbon bar
    im = im.filter(ImageFilter.GaussianBlur(0.8))
    im.save(os.path.join(art, "portrait_leader.png"))

    # Event illustration: a river crossing at dawn, as a period photograph.
    w, h = 1120, 480
    sky = np.linspace(205, 150, h)[:, None] * np.ones((1, w))
    img = Image.fromarray(np.stack([sky, sky * 0.9, sky * 0.74], 2).astype(np.uint8)).convert("RGB")
    d = ImageDraw.Draw(img)
    for k, (y0, amp, col) in enumerate(((250, 40, (128, 112, 92)), (290, 26, (96, 82, 66)))):
        pts = [(x, y0 - amp * (0.5 + 0.5 * math.sin(x * 0.006 + k * 2) * math.sin(x * 0.017 + k))) for x in range(0, w + 20, 20)]
        d.polygon(pts + [(w, h), (0, h)], fill=col)
    d.rectangle([0, 340, w, h], fill=(150, 136, 112))               # river
    for x in range(0, w, 6):                                         # ripples
        d.line([(x, 360 + (x * 37) % 90), (x + 18, 360 + (x * 37) % 90)], fill=(176, 160, 132), width=1)
    d.rectangle([200, 300, 980, 318], fill=(52, 44, 36))             # bridge deck
    for k in range(7):                                               # arches
        x = 220 + k * 110
        d.rectangle([x, 318, x + 18, 372], fill=(52, 44, 36))
        d.arc([x + 18, 300, x + 110, 372], 180, 360, fill=(52, 44, 36), width=10)
    for k in range(46):                                              # soldiers crossing
        x = 230 + k * 15 + (k % 3) * 2
        d.ellipse([x, 268, x + 7, 276], fill=(40, 32, 26))
        d.rectangle([x, 276, x + 7, 300], fill=(40, 32, 26))
        d.line([(x + 6, 262), (x + 9, 300)], fill=(40, 32, 26), width=1)  # rifles
    smoke = Image.new("L", (w, h), 0)
    sd = ImageDraw.Draw(smoke)
    for k in range(9):
        x, y, r = 760 + k * 34, 230 - k * 18, 40 + k * 9
        sd.ellipse([x - r, y - r, x + r, y + r], fill=150)
    smoke = smoke.filter(ImageFilter.GaussianBlur(18))
    img = Image.composite(Image.new("RGB", (w, h), (210, 196, 170)), img, smoke)
    arr = np.asarray(img).astype(np.float32)
    lum = arr.mean(2, keepdims=True)
    sep = lum * np.array([1.08, 0.94, 0.74]) + rng.normal(0, 7, (h, w, 1))
    gy_, gx_ = np.mgrid[0:h, 0:w]
    vig = 1 - 0.99 * ((gx_ / w - 0.5) ** 2 + (gy_ / h - 0.5) ** 2)[..., None]
    Image.fromarray(np.clip(sep * vig, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.7)).save(
        os.path.join(art, "event_crossing.jpg"), quality=90)


def hubs_early(cent, area, n):
    """(province, nation id, city name) for every city, snapped to the nearest province center."""
    out = []
    for nid, *_rest in NATIONS:
        for name, (cu, cv) in CITIES[nid]:
            k = int(np.argmin((cent[:, 0] - cu * n) ** 2 + (cent[:, 1] - cv * n) ** 2 + (area <= 0) * 1e12))
            out.append((k, nid, name))
    return out


# --------------------------------------------------------------------------- main
def generate(project):
    maps = os.path.join(project, "maps")
    os.makedirs(os.path.join(maps, "flags"), exist_ok=True)
    os.makedirs(os.path.join(project, "art"), exist_ok=True)

    h, c, rivers = relief()
    hn = np.clip((h - H_MIN) / (H_MAX - H_MIN), 0, 1)
    img = Image.fromarray((hn * 65535).astype(np.uint16))
    Image.fromarray((hn * 65535).astype(np.uint16)).save(os.path.join(maps, "height.png"))

    n = N_O
    c_hi = _resize(c, n)
    h_hi = _resize(h, n)
    land = c_hi > 0
    label, seeds, area, cent, adj = provinces(c_hi)
    owner = assign_nations(label, cent, area, adj, h_hi)
    nat_of = np.where(label >= 0, owner[np.maximum(label, 0)], -1)

    # Front-line stages: attacker provinces adjacent to the defender advance one ring per stage.
    controller = [owner.copy()]
    a_idx = [nat[0] for nat in NATIONS].index(ATTACKER)
    d_idx = [nat[0] for nat in NATIONS].index(DEFENDER)
    ctrl = owner.copy()
    for s in range(1, STAGES):
        frontier = [k for k in range(len(cent)) if ctrl[k] == d_idx and any(ctrl[q] == a_idx for q in adj[k])]
        # Take the front provinces nearest the attacker capital first (a bulge, not a wall).
        cap = np.array(NATIONS[a_idx][4]) * n
        frontier.sort(key=lambda k: np.hypot(*(cent[k] - cap)))
        for k in frontier[: max(2, int(len(frontier) * 0.4))]:
            ctrl[k] = a_idx
        controller.append(ctrl.copy())

    rgb_nat = np.array([hexrgb(nat[3]) for nat in NATIONS], np.float32)
    prov_edge = edges(label) & land
    nat_edge = edges(nat_of) & land
    coast = edges(land.astype(np.int32))
    river = draw_rivers(rivers, n, n / N_H)
    yy, xx = np.mgrid[0:n, 0:n]
    stripes = ((xx + yy) // 9) % 2 == 0
    nat_glow = soft(grow(nat_edge, 1), 7)

    def political(stage):
        ctl = controller[stage]
        ctl_px = np.where(label >= 0, ctl[np.maximum(label, 0)], -1)
        base = rgb_nat[np.maximum(nat_of, 0)]
        occ = (ctl_px != nat_of) & land
        occ_rgb = rgb_nat[np.maximum(ctl_px, 0)]
        rgb = np.where((occ & stripes)[..., None], occ_rgb, base)
        # Paradox-style: saturated toward national borders, lighter inside.
        rgb = rgb * (0.86 + 0.14 * (1 - nat_glow[..., None])) + (rgb * 0.75) * nat_glow[..., None] * 0.14
        a = np.where(land, 0.60 + 0.22 * nat_glow, 0).astype(np.float32)
        # Province borders, nation borders, coast and rivers.
        pe = soft(prov_edge, 0.6)
        rgb, a = composite(rgb, a, rgb * 0.55, pe * 0.55)
        ne = soft(grow(nat_edge, 1), 0.8)
        rgb, a = composite(rgb, a, (24, 20, 18), ne * 0.85)
        co = soft(grow(coast, 1), 0.9)
        rgb, a = composite(rgb, a, (30, 34, 40), co * 0.75)
        rgb, a = composite(rgb, a, (52, 98, 140), river * 0.9)
        # The front: where the attacker's control meets the defender's.
        if stage > 0:
            fl = edges(np.where(land, (ctl_px == a_idx) * 1 + (ctl_px == d_idx) * 2, 0)) & land
            near = grow(np.isin(ctl_px, [a_idx]) & land, 2) & grow(np.isin(ctl_px, [d_idx]) & land, 2)
            fl &= near
            glow = soft(grow(fl, 3), 6)
            rgb, a = composite(rgb, a, (210, 40, 30), glow * 0.85)
            core = soft(grow(fl, 1), 0.9)
            rgb, a = composite(rgb, a, (255, 236, 200), core)
        return rgb, a

    for s in range(STAGES):
        rgb, a = political(s)
        save_rgba(os.path.join(maps, f"political_{s}.png"), rgb, a)
        mini = np.concatenate([rgb, a[..., None] * 255], 2)
        sea = np.array([38, 62, 84, 255], np.float32)
        al = a[..., None]
        mm = mini[..., :3] * al + sea[:3] * (1 - al)
        Image.fromarray(mm.astype(np.uint8)).resize((512, 512), Image.LANCZOS).save(os.path.join(maps, f"minimap_{s}.png"))

    # Terrain mode: biomes (forests, farmland patchwork, dry steppe) and lines over the ground.
    z = np.zeros((n, n, 3), np.float32)
    a = np.zeros((n, n), np.float32)
    lowland = land & (h_hi > 1.5) & (h_hi < 85)
    forest = soft(lowland & (fbm(n, 9, 5, SEED + 50) > 0.56), 2.5)
    rgb, a = composite(z, a, (34, 58, 30), forest * 0.62)
    hub_mask = np.zeros((n, n), bool)
    for k, _, _ in hubs_early(cent, area, n):
        cx_, cy_ = cent[k]
        hub_mask |= (xx - cx_) ** 2 + (yy - cy_) ** 2 < 105 ** 2
    farm = lowland & (forest < 0.3) & (hub_mask | (fbm(n, 6, 4, SEED + 51) > 0.67))
    ang = 0.35
    fu = (xx * math.cos(ang) + yy * math.sin(ang)) / 15.0
    fv = (-xx * math.sin(ang) + yy * math.cos(ang)) / 10.0
    cell = (np.floor(fu).astype(np.int64) * 7919 + np.floor(fv).astype(np.int64) * 104729) % 4
    palette = np.array([(168, 152, 86), (112, 134, 62), (182, 160, 98), (92, 116, 52)], np.float32)
    rgb, a = composite(rgb, a, palette[cell], soft(farm, 1.0) * 0.5)
    hedges = farm & ((np.abs(fu - np.round(fu)) < 0.07) | (np.abs(fv - np.round(fv)) < 0.1))
    rgb, a = composite(rgb, a, (52, 62, 36), soft(hedges, 0.6) * 0.35)
    steppe = soft(land, 1) * smoothstep(0.55, 0.85, (xx / n) * 0.6 + (yy / n) * 0.6) * (1 - forest)
    rgb, a = composite(rgb, a, (186, 156, 98), steppe * 0.35)
    rgb, a = composite(rgb, a, (20, 18, 16), soft(prov_edge, 0.6) * 0.22)
    rgb, a = composite(rgb, a, (20, 18, 16), soft(grow(nat_edge, 1), 0.8) * 0.6)
    rgb, a = composite(rgb, a, (30, 34, 40), soft(grow(coast, 1), 0.9) * 0.55)
    rgb, a = composite(rgb, a, (52, 98, 140), river * 0.9)
    save_rgba(os.path.join(maps, "terrain.png"), rgb, a)

    # Supply mode: distance (graph hops) from capitals and big cities, railways between them.
    import collections
    hubs = hubs_early(cent, area, n)
    hops = np.full(len(cent), 99)
    dq = collections.deque()
    for k, _, _ in hubs:
        hops[k] = 0
        dq.append(k)
    while dq:
        k = dq.popleft()
        for q in adj[k]:
            if hops[q] > hops[k] + 1:
                hops[q] = hops[k] + 1
                dq.append(q)
    supply = np.clip(1 - hops / 5.0, 0, 1)
    sp = np.where(label >= 0, supply[np.maximum(label, 0)], 0)
    ramp = np.stack([np.interp(sp, [0, 0.5, 1], [178, 214, 70]), np.interp(sp, [0, 0.5, 1], [52, 178, 150]),
                     np.interp(sp, [0, 0.5, 1], [40, 60, 70])], 2)
    rgb, a = ramp, np.where(land, 0.58, 0).astype(np.float32)
    rgb, a = composite(rgb, a, (20, 18, 16), soft(prov_edge, 0.6) * 0.3)
    rgb, a = composite(rgb, a, (20, 18, 16), soft(grow(nat_edge, 1), 0.8) * 0.7)
    rgb, a = composite(rgb, a, (30, 34, 40), soft(grow(coast, 1), 0.9) * 0.6)
    rails = Image.new("L", (n, n), 0)
    rd = ImageDraw.Draw(rails)
    railways = []
    for nid, *_ in NATIONS:
        ks = [k for k, hn_, _ in hubs if hn_ == nid]
        for p, q in zip(ks[:-1], ks[1:]):
            railways.append((p, q))
    for (p, q) in railways + [(hubs[0][0], hubs[3][0]), (hubs[3][0], hubs[6][0]), (hubs[6][0], hubs[9][0])]:
        rd.line([tuple(cent[p]), tuple(cent[q])], fill=255, width=5)
    rail = np.asarray(rails).astype(np.float32) / 255
    rgb, a = composite(rgb, a, (25, 22, 20), rail * 0.9)
    ties = rail * (((xx + yy) // 6) % 2 == 0)
    rgb, a = composite(rgb, a, (235, 225, 200), soft(ties > 0.5, 0.5) * 0.9)
    rgb, a = composite(rgb, a, (52, 98, 140), river * 0.8)
    save_rgba(os.path.join(maps, "supply.png"), rgb, a)

    # Paper mode: parchment, ink coastline with offshore ripples, dashed borders.
    paper_img = parchment(n, c_hi, land, coast, prov_edge, nat_edge, river, xx, yy)
    paper_img.save(os.path.join(maps, "paper.jpg"), quality=88)

    for nid, name, long_name, col, *_ in NATIONS:
        flag(nid, col).save(os.path.join(maps, "flags", f"{nid}.png"))
        for kind in ("infantry", "armor"):
            counter_face(kind, col, os.path.join(project, "art", f"counter_{nid}_{kind}.png"))

    # World data for the scene builder.
    def w(k):
        return to_world(cent[k][0] / n, cent[k][1] / n)

    world = {"size": SIZE_M, "minHeight": H_MIN, "maxHeight": H_MAX, "nations": [], "cities": [], "rivers": [],
             "stages": STAGES, "provinces": len(cent)}
    for i, (nid, name, long_name, col, cap, _) in enumerate(NATIONS):
        ks = np.where(owner == i)[0]
        if len(ks) == 0:
            continue
        # Label anchor: area-weighted centroid, and the main axis for the label's angle.
        pts = cent[ks]
        wts = area[ks]
        mx, my = (pts * wts[:, None]).sum(0) / wts.sum()
        cov = np.cov((pts - [mx, my]).T, aweights=wts)
        ev, evec = np.linalg.eigh(cov)
        ax = evec[:, 1]
        ang = math.degrees(math.atan2(ax[1], ax[0]))
        if ang > 90:
            ang -= 180
        if ang < -90:
            ang += 180
        spread = math.sqrt(max(ev[1], 1)) / n * SIZE_M
        world["nations"].append({"id": nid, "name": name, "long": long_name, "color": col,
                                 "label": list(to_world(mx / n, my / n)), "angle": max(-35, min(35, ang)),
                                 "extent": spread, "provinces": int(len(ks))})
    for k, nid, name in hubs:
        x, zz = w(k)
        world["cities"].append({"name": name, "nation": nid, "x": x, "z": zz,
                                "capital": name == CITIES[nid][0][0], "province": int(k)})
    for path in rivers:
        world["rivers"].append([list(to_world(x / N_H, y / N_H)) for x, y in path[::6]])
    # Front units: one attacker and one defender block on each side of every front segment, per stage.
    world["front"] = []
    for s in range(STAGES):
        ctl = controller[s]
        pairs = sorted({(min(p, q), max(p, q)) for p in range(len(cent)) for q in adj[p]
                        if {ctl[p], ctl[q]} == {a_idx, d_idx}})
        world["front"].append([{"a": list(w(p if ctl[p] == a_idx else q)), "d": list(w(q if ctl[p] == a_idx else p))}
                               for p, q in pairs])
    world["capitals"] = {nid: list(w(int(np.argmin((cent[:, 0] - cap[0] * n) ** 2 + (cent[:, 1] - cap[1] * n) ** 2 +
                                                       (area <= 0) * 1e12))))
                         for nid, _, _, _, cap, _ in NATIONS}
    with open(os.path.join(maps, "world.json"), "w") as f:
        json.dump(world, f, indent=1)
    return world


def parchment(n, c_hi, land, coast, prov_edge, nat_edge, river, xx, yy):
    rng_f = fbm(n, 5, 7, SEED + 40)
    fine = fbm(n, 60, 3, SEED + 41)
    stains = fbm(n, 3, 4, SEED + 42)
    base = np.stack([np.full((n, n), 222.0), np.full((n, n), 203.0), np.full((n, n), 160.0)], 2)
    base *= (0.9 + 0.12 * rng_f[..., None] + 0.05 * fine[..., None])
    base *= (1 - 0.18 * smoothstep(0.55, 0.85, stains)[..., None])
    u = (xx + 0.5) / n
    v = (yy + 0.5) / n
    edge = np.minimum(np.minimum(u, 1 - u), np.minimum(v, 1 - v))
    base *= (0.72 + 0.28 * smoothstep(0.0, 0.12, edge))[..., None]
    # Sea: a faint wash and offshore ripple lines following the coast.
    sea = ~land
    base = np.where(sea[..., None], base * np.array([0.9, 0.94, 0.98]), base)
    rgb = base
    a = np.ones((n, n), np.float32)
    ink = (58, 42, 30)
    for k, off in enumerate((0.006, 0.014, 0.024, 0.037)):
        ring = edges((c_hi > -off).astype(np.int32)) & sea
        rgb, a = composite(rgb, a, ink, soft(ring, 0.7) * (0.45 - k * 0.08))
    rgb, a = composite(rgb, a, ink, soft(grow(coast, 1), 0.9) * 0.9)
    rgb, a = composite(rgb, a, (70, 60, 52), soft(prov_edge, 0.6) * 0.18)
    dashed = nat_edge & ((((xx // 7) + (yy // 7)) % 2) == 0)
    rgb, a = composite(rgb, a, (120, 30, 24), soft(grow(dashed, 1), 0.8) * 0.75)
    rgb, a = composite(rgb, a, (60, 80, 100), river * 0.8)
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(os.path.join(FONT_DIR, "EBGaramond.ttf"), 64)
        small = ImageFont.truetype(os.path.join(FONT_DIR, "EBGaramond.ttf"), 30)
    except OSError:
        font = small = ImageFont.load_default()
    # Cartouche and compass rose in the empty sea.
    cx, cy = int(n * 0.86), int(n * 0.07)
    d.text((cx, cy), "MERIDIAN", font=font, fill=ink, anchor="mm")
    d.text((cx, cy + 56), "Charta of the Continent · 1937", font=small, fill=ink, anchor="mm")
    rx, ry, r = int(n * 0.08), int(n * 0.9), 70
    for k in range(16):
        ang = k * math.pi / 8
        rr = r if k % 2 == 0 else r * 0.55
        p1 = (rx + math.cos(ang) * rr, ry + math.sin(ang) * rr)
        p2 = (rx + math.cos(ang + 0.2) * 12, ry + math.sin(ang + 0.2) * 12)
        p3 = (rx + math.cos(ang - 0.2) * 12, ry + math.sin(ang - 0.2) * 12)
        d.polygon([p1, p2, (rx, ry), p3], fill=ink if k % 4 == 0 else (140, 110, 80))
    d.ellipse([rx - r * 0.8, ry - r * 0.8, rx + r * 0.8, ry + r * 0.8], outline=ink, width=2)
    d.text((rx, ry - r - 26), "N", font=small, fill=ink, anchor="mm")
    return img


if __name__ == "__main__":
    import sys
    import time
    t0 = time.time()
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    wd = generate(out)
    print(f"{wd['provinces']} provinces, {len(wd['cities'])} cities in {time.time() - t0:.1f}s")
