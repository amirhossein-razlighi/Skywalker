#!/usr/bin/env python3
"""Opt-in GPU check for point / spot light shadows (macOS, Metal). Not part of ctest.

Renders examples/render_tests/local_shadows/scenes/room.sky.json with local shadows on and off and
checks statistics of the images (not pixel goldens):
  * leak: seen from outside, the ground around the closed room must be dark with shadows (the
    lamp inside lights it through the walls without them);
  * shadow: inside, the floor under the table must be much darker than the open floor.

Usage: python3 tools/render_checks/local_shadows.py [--skywalker build/release/bin/skywalker] [--keep DIR]
Exit status 0 = pass. Renders are 640x360 with 4 samples (GPU-safe sizes).
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCENE = os.path.join(ROOT, "examples", "render_tests", "local_shadows", "scenes", "room.sky.json")


def read_png(path):
    """Minimal 8-bit RGB/RGBA PNG decoder (what the engine writes)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w, h, ctype = 8, b"", 0, 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
            assert depth == 8, "8-bit only"
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def mean_luma(img, x0, y0, x1, y1):
    """Mean luma (0..255) of a box given in fractions of the image size."""
    w, h, bpp, rows = img
    total, n = 0.0, 0
    for y in range(int(y0 * h), int(y1 * h)):
        row = rows[y]
        for x in range(int(x0 * w), int(x1 * w)):
            r, g, b = row[x * bpp], row[x * bpp + 1], row[x * bpp + 2]
            total += 0.2126 * r + 0.7152 * g + 0.0722 * b
            n += 1
    return total / max(n, 1)


def capture(exe, scene, out, eye, target):
    args = {"eye": eye, "target": target, "width": 640, "height": 360, "samples": 4, "annotate": False,
            "overlays": False, "save_path": out, "include_image": False}
    subprocess.run([exe, "call", "viewport_capture", json.dumps(args), "--scene", scene], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return read_png(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skywalker", default=os.path.join(ROOT, "build", "release", "bin", "skywalker"))
    ap.add_argument("--keep", help="keep the renders in this folder")
    a = ap.parse_args()
    work = a.keep or tempfile.mkdtemp(prefix="sky_local_shadows_")
    os.makedirs(work, exist_ok=True)
    scene = json.load(open(SCENE))
    scene["environment"]["localShadowLights"] = 0
    off_scene = os.path.join(work, "room_no_shadows.sky.json")
    json.dump(scene, open(off_scene, "w"))

    outside = ([11, 6, 9], [2, 0.5, 0])
    inside = ([-2.5, 1.7, 2.6], [1.0, 0.4, -0.8])
    on_out = capture(a.skywalker, SCENE, os.path.join(work, "outside_on.png"), *outside)
    off_out = capture(a.skywalker, off_scene, os.path.join(work, "outside_off.png"), *outside)
    on_in = capture(a.skywalker, SCENE, os.path.join(work, "inside_on.png"), *inside)

    ring = (0.25, 0.53, 0.36, 0.62)  # ground just outside the left walls (no window there)
    leak_on, leak_off = mean_luma(on_out, *ring), mean_luma(off_out, *ring)
    under = mean_luma(on_in, 0.43, 0.56, 0.52, 0.6)   # floor under the table
    open_floor = mean_luma(on_in, 0.66, 0.8, 0.78, 0.9)
    results = {
        "leak_off": round(leak_off, 1), "leak_on": round(leak_on, 1),
        "under_table": round(under, 1), "open_floor": round(open_floor, 1),
    }
    ok = leak_off > 12.0 and leak_on < leak_off * 0.2 and under < open_floor * 0.5
    print(json.dumps(results), "PASS" if ok else "FAIL", "renders in", work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
