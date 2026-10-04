#!/usr/bin/env python3
"""Trace the engine's pencil-contour renders (debug_view "sketch") into ordered SVG strokes.

    python3 scripts/trace_lines.py public/footage public/linework [--max 2600]

For every *_sketch.png it writes public/linework/<slot>.json:
    {"w": 1920, "h": 1080, "paths": [[t0, t1, length, "M x y L x y ..."], ...]}
t0/t1 (0..1) are when each stroke starts and finishes drawing, so the film can animate the drawing at any
speed: long structural strokes first, details after, swept roughly left to right like a hand would.
Only numpy and Pillow are needed (threshold -> Zhang-Suen thinning -> chain tracing -> Douglas-Peucker).
"""
import json
import math
import os
import sys

import numpy as np
from PIL import Image

INK = 150  # contour lines are ~115 grey, hatching ~170+, paper ~245
SCALE = 1  # trace at full resolution


def thin(img):
    """Zhang-Suen thinning, vectorised. img: bool array."""
    a = np.pad(img.astype(np.uint8), 1)
    while True:
        changed = False
        for step in (0, 1):
            p2, p3, p4 = a[:-2, 1:-1], a[:-2, 2:], a[1:-1, 2:]
            p5, p6, p7 = a[2:, 2:], a[2:, 1:-1], a[2:, :-2]
            p8, p9 = a[1:-1, :-2], a[:-2, :-2]
            c = a[1:-1, 1:-1]
            nb = [p2, p3, p4, p5, p6, p7, p8, p9]
            b = sum(x.astype(np.int32) for x in nb)
            seq = nb + [p2]
            trans = sum(((seq[i] == 0) & (seq[i + 1] == 1)).astype(np.int32) for i in range(8))
            if step == 0:
                cond = (p2 * p4 * p6 == 0) & (p4 * p6 * p8 == 0)
            else:
                cond = (p2 * p4 * p8 == 0) & (p2 * p6 * p8 == 0)
            rm = (c == 1) & (b >= 2) & (b <= 6) & (trans == 1) & cond
            if rm.any():
                c[rm] = 0
                changed = True
        if not changed:
            return a[1:-1, 1:-1].astype(bool)


NB = [(-1, 0), (0, 1), (1, 0), (0, -1), (-1, 1), (1, 1), (1, -1), (-1, -1)]


def chains(sk):
    ys, xs = np.nonzero(sk)
    pts = set(zip(ys.tolist(), xs.tolist()))
    nbrs = {}
    for p in pts:
        y, x = p
        # 8-connected, but skip a diagonal link when a 4-neighbour already bridges it (staircase pixels
        # otherwise look like junctions and shatter every slanted line into fragments)
        nbrs[p] = [(y + dy, x + dx) for dy, dx in NB if (y + dy, x + dx) in pts
                   and not (dy and dx and ((y + dy, x) in pts or (y, x + dx) in pts))]
    seen_edges = set()
    out = []

    def walk(start, nxt):
        path = [start]
        prev, cur = start, nxt
        while True:
            seen_edges.add((prev, cur))
            seen_edges.add((cur, prev))
            path.append(cur)
            if len(nbrs[cur]) != 2:
                break
            a, b = nbrs[cur]
            n = b if a == prev else a
            if (cur, n) in seen_edges:
                break
            prev, cur = cur, n
        return path

    # endpoints and junctions first, then leftover loops
    order = sorted(pts, key=lambda p: (len(nbrs[p]) == 2, p))
    for p in order:
        for n in nbrs[p]:
            if (p, n) not in seen_edges:
                out.append(walk(p, n))
    return out


def rdp(pts, eps):
    if len(pts) < 3:
        return pts
    a, b = np.array(pts[0], float), np.array(pts[-1], float)
    arr = np.array(pts, float)
    ab = b - a
    L = np.hypot(*ab)
    if L == 0:
        d = np.hypot(*(arr - a).T)
    else:
        d = np.abs(np.cross(ab, arr - a)) / L
    i = int(np.argmax(d))
    if d[i] > eps:
        return rdp(pts[: i + 1], eps)[:-1] + rdp(pts[i:], eps)
    return [pts[0], pts[-1]]


def trace(path_in, max_paths):
    g = np.asarray(Image.open(path_in).convert("L")).astype(np.float32)
    h, w = g.shape
    hh, ww = h // SCALE, w // SCALE
    small = g[: hh * SCALE, : ww * SCALE].reshape(hh, SCALE, ww, SCALE).min(axis=(1, 3))
    ink = small < INK
    # drop isolated specks
    sk = thin(ink)
    lines = []
    for c in chains(sk):
        if len(c) < 3:
            continue
        simp = rdp(c, 0.9)
        pts = [(x * SCALE + SCALE / 2, y * SCALE + SCALE / 2) for y, x in simp]
        length = sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
        if length < 5:
            continue
        lines.append((length, pts))
    lines.sort(key=lambda l: -l[0])
    lines = lines[:max_paths]
    # Timing: three passes like a draughtsman (structure, forms, detail); each pass sweeps left to right.
    n = len(lines)
    timed = []
    for rank, (length, pts) in enumerate(lines):
        tier = 0 if rank < n * 0.12 else 1 if rank < n * 0.45 else 2
        cx = sum(p[0] for p in pts) / len(pts) / w
        cy = sum(p[1] for p in pts) / len(pts) / h
        base = [0.0, 0.28, 0.55][tier]
        span = [0.38, 0.35, 0.33][tier]
        jitter = (math.sin(rank * 12.9898) * 43758.5453) % 1 * 0.06
        t0 = base + span * (0.75 * cx + 0.25 * cy) * 0.85 + jitter
        dur = min(0.28, 0.03 + length / (w * 2.2))
        t1 = min(1.0, t0 + dur)
        d = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in pts)
        timed.append([round(t0, 4), round(t1, 4), round(length, 1), d])
    timed.sort(key=lambda p: p[0])
    return {"w": w, "h": h, "paths": timed}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    max_paths = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 2600
    os.makedirs(dst, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if not f.endswith("_sketch.png"):
            continue
        slot = f[: -len("_sketch.png")]
        data = trace(os.path.join(src, f), max_paths)
        out = os.path.join(dst, slot + ".json")
        with open(out, "w") as fh:
            json.dump(data, fh, separators=(",", ":"))
        print(f"  {slot}: {len(data['paths'])} strokes, {os.path.getsize(out) // 1024} KB")


if __name__ == "__main__":
    main()
