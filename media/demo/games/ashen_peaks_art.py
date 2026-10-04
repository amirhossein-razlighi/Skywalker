"""Ashen Peaks — painted textures for the Monastery of the Kindled Crane (original designs).

The order's sigil is a crane with flame-tipped wings inside a broken ring; the "script" on
banners and the gate plaque is an invented ornamental glyph set (not a real writing system).
Painted procedurally with Pillow so the build is reproducible.
"""
import math
import random

from PIL import Image, ImageDraw, ImageFilter

VERMILION = (156, 34, 22)
GOLD = (222, 176, 92)
IVORY = (236, 226, 204)
INK = (32, 22, 18)


def _cloth_noise(img, rng, strength=10):
    px = img.load()
    w, h = img.size
    for y in range(h):
        row = rng.uniform(-strength, strength) * 0.15
        for x in range(w):
            n = rng.uniform(-strength, strength) * 0.6 + row
            r, g, b = px[x, y][:3]
            px[x, y] = (max(0, min(255, int(r + n))), max(0, min(255, int(g + n * 0.8))), max(0, min(255, int(b + n * 0.7))))


def sigil(draw, cx, cy, r, color, bg):
    """The Kindled Crane: a broken ring, a crane rising with flame-tipped wings."""
    w = max(3, int(r * 0.09))
    # broken ring (gap at the top where the crane's head breaks through)
    draw.arc([cx - r, cy - r, cx + r, cy + r], start=-62, end=242, fill=color, width=w)
    # body: a slim teardrop rising
    body = [(cx, cy - r * 0.95)]
    for k in range(21):
        a = math.pi * k / 20
        body.append((cx + math.sin(a) * r * 0.27 * (1 - k / 30), cy - r * 0.55 + (1 - math.cos(a)) * r * 0.55))
    draw.polygon(body, fill=color)
    draw.ellipse([cx - r * 0.07, cy - r * 1.02, cx + r * 0.07, cy - r * 0.88], fill=color)
    # wings: bold crescents sweeping up, feathered below, ending in flame tongues
    def bez(p0, p1, p2, n=24):
        return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0],
                 (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]) for t in (k / n for k in range(n + 1))]

    for sgn in (-1, 1):
        shoulder = (cx + sgn * r * 0.06, cy - r * 0.3)
        tip = (cx + sgn * r * 0.82, cy - r * 0.78)
        outer = bez(shoulder, (cx + sgn * r * 0.86, cy + r * 0.12), tip)
        inner = bez(tip, (cx + sgn * r * 0.3, cy - r * 0.42), (cx + sgn * r * 0.06, cy - r * 0.52))
        draw.polygon(outer + inner, fill=color)
        for k in range(4):
            px, py = outer[5 + k * 4]
            draw.polygon([(px, py), (px + sgn * r * 0.04, py + r * 0.2), (px + sgn * r * 0.16, py + r * 0.02)], fill=color)
        for f, (dx, dy, L) in enumerate(((0.0, 0.0, 0.3), (-0.08, 0.1, 0.22), (0.06, 0.08, 0.18))):
            fx, fy = tip[0] + sgn * dx * r, tip[1] + dy * r
            draw.polygon([(fx - sgn * r * 0.06, fy + r * 0.04), (fx + sgn * r * L * 0.5, fy - r * L), (fx + sgn * r * 0.08, fy + r * 0.06)],
                         fill=color)
    # tail: three trailing strokes
    for k in (-1, 0, 1):
        draw.line([(cx, cy + r * 0.5), (cx + k * r * 0.22, cy + r * 0.82)], fill=color, width=w)
    # ember under the crane
    draw.ellipse([cx - r * 0.09, cy + r * 0.86, cx + r * 0.09, cy + r * 1.04], fill=bg)


def glyph(draw, x, y, s, color, rng):
    """One invented ornamental glyph: a few strokes on a 3x3 lattice with a flourish."""
    w = max(2, int(s * 0.11))
    nodes = [(x + i * s / 2, y + j * s / 2) for j in range(3) for i in range(3)]
    used = set()
    for _ in range(rng.randint(3, 5)):
        a, b = rng.sample(range(9), 2)
        if (a, b) in used:
            continue
        used.add((a, b))
        draw.line([nodes[a], nodes[b]], fill=color, width=w)
    if rng.random() < 0.6:
        cx, cy = nodes[rng.choice([1, 3, 5, 7])]
        draw.ellipse([cx - w, cy - w, cx + w, cy + w], fill=color)
    draw.line([(x - s * 0.08, y + s * 1.08), (x + s * 1.08, y + s * 1.08)], fill=color, width=max(1, w // 2))


def banner(path, seed=3):
    rng = random.Random(seed)
    W, H = 300, 1460
    img = Image.new("RGB", (W, H), VERMILION)
    d = ImageDraw.Draw(img)
    d.rectangle([12, 12, W - 13, H - 13], outline=GOLD, width=6)
    d.rectangle([26, 26, W - 27, H - 27], outline=GOLD, width=2)
    d.ellipse([W / 2 - 112, 70, W / 2 + 112, 294], fill=IVORY)
    sigil(d, W / 2, 182, 92, VERMILION, IVORY)
    for k in range(5):
        glyph(d, W / 2 - 46, 360 + k * 150, 92, GOLD, rng)
    # frayed, darker hem
    for x in range(0, W, 6):
        d.line([(x, H - 40), (x + rng.randint(-3, 3), H - 1)], fill=(110, 22, 14), width=4)
    _cloth_noise(img, rng, 9)
    img = img.filter(ImageFilter.GaussianBlur(0.6))
    img.save(path)


def plaque(path, seed=5):
    rng = random.Random(seed)
    W, H = 880, 480
    img = Image.new("RGB", (W, H), (40, 24, 20))
    d = ImageDraw.Draw(img)
    d.rectangle([10, 10, W - 11, H - 11], outline=GOLD, width=10)
    d.rectangle([34, 34, W - 35, H - 35], outline=GOLD, width=3)
    sigil(d, 150, H / 2, 120, GOLD, (40, 24, 20))
    for k in range(3):
        glyph(d, 300 + k * 190, 150, 150, GOLD, rng)
    _cloth_noise(img, rng, 6)
    img.save(path)


if __name__ == "__main__":
    import sys
    banner(sys.argv[1] + "/banner_crane.png")
    plaque(sys.argv[1] + "/plaque_crane.png")
