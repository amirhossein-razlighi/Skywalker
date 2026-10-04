"""Berrybrook — HUD icons, painted procedurally with PIL (original art, regenerated on build).

Each icon is drawn at 4x on a transparent canvas with soft shapes and an ink outline, then
downsampled (anti-aliased). Palette matches the farm: strawberry red, leaf green, honey wood,
cream paper, sky blue.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

S = 4  # supersampling


def _canvas(size):
    return Image.new("RGBA", (size * S, size * S), (0, 0, 0, 0))


def _finish(im, size, path, shadow=True):
    if shadow:
        a = im.split()[3].filter(ImageFilter.GaussianBlur(3 * S))
        sh = Image.new("RGBA", im.size, (60, 30, 20, 0))
        sh.putalpha(a.point(lambda v: int(v * 0.35)))
        base = Image.new("RGBA", im.size, (0, 0, 0, 0))
        base.alpha_composite(sh, (0, 3 * S))
        base.alpha_composite(im)
        im = base
    im = im.resize((size, size), Image.LANCZOS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


INK = (74, 44, 34, 255)


def _poly(d, pts, fill, w=3):
    d.polygon(pts, fill=fill)
    d.line(pts + [pts[0]], fill=INK, width=w * S, joint="curve")


def _ellipse(d, box, fill, w=3):
    d.ellipse(box, fill=fill, outline=INK, width=w * S)


def strawberry(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    c = size * S / 2
    right = []
    for k in range(33):
        t = k / 32  # 0 = top center, 1 = tip
        w = 0.34 * (1 - t ** 1.7) ** 0.55 * (0.82 + 0.18 * math.sin(math.pi * min(1.0, t * 2.2)))
        y = -0.25 + 0.66 * t
        right.append((c + w * size * S, c + y * size * S))
    tip = (c, c + 0.43 * size * S)
    pts = [(c, c - 0.27 * size * S)] + right + [tip] + [(2 * c - x, y) for x, y in reversed(right)]
    _poly(d, pts, (226, 52, 58, 255))
    # highlight and seeds
    d.ellipse([c - 0.2 * size * S, c - 0.12 * size * S, c - 0.08 * size * S, c + 0.06 * size * S], fill=(255, 140, 140, 150))
    for k in range(14):
        a = k * 2.4
        rr = 0.06 + 0.2 * ((k * 37) % 10) / 10
        x, y = c + math.cos(a) * rr * size * S * 0.9, c + 0.1 * size * S + math.sin(a) * rr * size * S * 0.85
        d.ellipse([x - 1.6 * S, y - 2.4 * S, x + 1.6 * S, y + 2.4 * S], fill=(255, 220, 120, 255))
    # leafy calyx: five pointed sepals splayed over the shoulders
    for k in range(5):
        a = math.pi * (0.1 + 0.8 * k / 4)
        tip = (c - math.cos(a) * 0.27 * size * S, c - 0.24 * size * S + math.sin(a) * 0.1 * size * S)
        base_l = (c - math.cos(a + 0.35) * 0.05 * size * S, c - 0.27 * size * S)
        base_r = (c - math.cos(a - 0.35) * 0.05 * size * S, c - 0.27 * size * S)
        _poly(d, [base_l, tip, base_r], (96, 170, 74, 255), 2)
    d.line([(c, c - 0.27 * size * S), (c + 0.05 * size * S, c - 0.42 * size * S)], fill=INK, width=4 * S)
    _finish(im, size, path)


def seed_packet(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    _poly(d, [(0.24 * u, 0.16 * u), (0.76 * u, 0.12 * u), (0.8 * u, 0.86 * u), (0.22 * u, 0.88 * u)], (246, 232, 204, 255))
    d.rectangle([0.27 * u, 0.2 * u, 0.73 * u, 0.3 * u], fill=(226, 92, 100, 255))
    # a tiny berry drawing on the packet
    _ellipse(d, [0.38 * u, 0.42 * u, 0.62 * u, 0.7 * u], (226, 52, 58, 255), 2)
    _poly(d, [(0.42 * u, 0.42 * u), (0.5 * u, 0.34 * u), (0.58 * u, 0.42 * u)], (88, 160, 70, 255), 2)
    for k in range(3):
        d.ellipse([0.33 * u + k * 0.12 * u, 0.76 * u, 0.37 * u + k * 0.12 * u, 0.8 * u], fill=(140, 100, 60, 255))
    _finish(im, size, path)


def watering_can(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    d.line([(0.62 * u, 0.55 * u), (0.88 * u, 0.3 * u)], fill=INK, width=int(0.09 * u))
    d.line([(0.62 * u, 0.55 * u), (0.88 * u, 0.3 * u)], fill=(120, 170, 210, 255), width=int(0.055 * u))
    _poly(d, [(0.84 * u, 0.24 * u), (0.95 * u, 0.3 * u), (0.9 * u, 0.38 * u)], (120, 170, 210, 255), 2)
    _poly(d, [(0.2 * u, 0.36 * u), (0.66 * u, 0.36 * u), (0.68 * u, 0.82 * u), (0.18 * u, 0.82 * u)], (126, 176, 216, 255))
    d.arc([0.24 * u, 0.14 * u, 0.6 * u, 0.5 * u], 180, 360, fill=INK, width=int(0.05 * u))
    d.rectangle([0.2 * u, 0.5 * u, 0.66 * u, 0.56 * u], fill=(90, 140, 190, 255))
    for k, (x, y) in enumerate(((0.96, 0.44), (0.92, 0.52), (0.99, 0.56))):
        d.ellipse([x * u - 2 * S, y * u - 3 * S, x * u + 2 * S, y * u + 3 * S], fill=(150, 200, 240, 255))
    _finish(im, size, path)


def hoe(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    d.line([(0.22 * u, 0.86 * u), (0.72 * u, 0.2 * u)], fill=INK, width=int(0.09 * u))
    d.line([(0.22 * u, 0.86 * u), (0.72 * u, 0.2 * u)], fill=(196, 146, 92, 255), width=int(0.055 * u))
    _poly(d, [(0.6 * u, 0.14 * u), (0.86 * u, 0.2 * u), (0.82 * u, 0.36 * u), (0.66 * u, 0.3 * u)], (176, 184, 192, 255))
    _finish(im, size, path)


def basket(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    d.arc([0.22 * u, 0.12 * u, 0.78 * u, 0.7 * u], 180, 360, fill=INK, width=int(0.07 * u))
    d.arc([0.22 * u, 0.12 * u, 0.78 * u, 0.7 * u], 180, 360, fill=(204, 156, 96, 255), width=int(0.04 * u))
    for k, x in enumerate((0.34, 0.47, 0.6)):
        _ellipse(d, [x * u - 0.09 * u, 0.33 * u, x * u + 0.09 * u, 0.53 * u], (226, 52, 58, 255), 2)
    _poly(d, [(0.14 * u, 0.44 * u), (0.86 * u, 0.44 * u), (0.76 * u, 0.86 * u), (0.24 * u, 0.86 * u)], (214, 164, 100, 255))
    for k in range(4):
        y = 0.52 * u + k * 0.085 * u
        d.line([(0.18 * u + k * 0.02 * u, y), (0.82 * u - k * 0.02 * u, y)], fill=(170, 120, 70, 255), width=3 * S)
    _finish(im, size, path)


def jam(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    _poly(d, [(0.26 * u, 0.3 * u), (0.74 * u, 0.3 * u), (0.76 * u, 0.86 * u), (0.24 * u, 0.86 * u)], (196, 40, 56, 255))
    d.rectangle([0.3 * u, 0.5 * u, 0.7 * u, 0.7 * u], fill=(250, 238, 214, 255), outline=INK, width=2 * S)
    _ellipse(d, [0.43 * u, 0.53 * u, 0.57 * u, 0.67 * u], (226, 52, 58, 255), 2)
    _poly(d, [(0.22 * u, 0.18 * u), (0.78 * u, 0.18 * u), (0.8 * u, 0.32 * u), (0.2 * u, 0.32 * u)], (236, 110, 120, 255))
    for k in range(6):
        x = 0.24 * u + k * 0.1 * u
        d.rectangle([x, 0.18 * u, x + 0.05 * u, 0.32 * u], fill=(255, 248, 236, 255))
    d.ellipse([0.3 * u, 0.36 * u, 0.36 * u, 0.46 * u], fill=(255, 160, 170, 200))
    _finish(im, size, path)


def flower(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    c = 0.5 * u
    d.line([(c, 0.55 * u), (c, 0.92 * u)], fill=INK, width=int(0.06 * u))
    d.line([(c, 0.55 * u), (c, 0.92 * u)], fill=(100, 170, 80, 255), width=int(0.032 * u))
    _poly(d, [(c, 0.78 * u), (0.72 * u, 0.66 * u), (0.66 * u, 0.82 * u)], (110, 180, 90, 255), 2)
    for k in range(6):
        a = 2 * math.pi * k / 6
        x, y = c + math.cos(a) * 0.17 * u, 0.4 * u + math.sin(a) * 0.17 * u
        _ellipse(d, [x - 0.12 * u, y - 0.12 * u, x + 0.12 * u, y + 0.12 * u], (246, 170, 196, 255), 2)
    _ellipse(d, [c - 0.1 * u, 0.3 * u, c + 0.1 * u, 0.5 * u], (250, 204, 80, 255), 2)
    _finish(im, size, path)


def coin(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    _ellipse(d, [0.14 * u, 0.14 * u, 0.86 * u, 0.86 * u], (242, 186, 64, 255), 3)
    d.ellipse([0.24 * u, 0.24 * u, 0.76 * u, 0.76 * u], outline=(196, 130, 40, 255), width=3 * S)
    # an embossed berry on the coin
    _ellipse(d, [0.38 * u, 0.42 * u, 0.62 * u, 0.68 * u], (226, 150, 50, 255), 2)
    _poly(d, [(0.42 * u, 0.42 * u), (0.5 * u, 0.33 * u), (0.58 * u, 0.42 * u)], (226, 150, 50, 255), 2)
    d.ellipse([0.26 * u, 0.24 * u, 0.42 * u, 0.36 * u], fill=(255, 236, 170, 200))
    _finish(im, size, path)


def sun(path, size=96):
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    c = 0.5 * u
    for k in range(10):
        a = 2 * math.pi * k / 10
        p0 = (c + math.cos(a) * 0.26 * u, c + math.sin(a) * 0.26 * u)
        p1 = (c + math.cos(a) * 0.42 * u, c + math.sin(a) * 0.42 * u)
        d.line([p0, p1], fill=(246, 168, 60, 255), width=int(0.06 * u))
    _ellipse(d, [c - 0.22 * u, c - 0.22 * u, c + 0.22 * u, c + 0.22 * u], (252, 200, 80, 255), 3)
    _finish(im, size, path)


def portrait(path, size=256):
    """Juniper, the neighbour: a round friendly face with a sunhat and freckles (dialogue portrait)."""
    im = _canvas(size)
    d = ImageDraw.Draw(im)
    u = size * S
    _ellipse(d, [0.06 * u, 0.06 * u, 0.94 * u, 0.94 * u], (250, 226, 196, 255), 3)  # backdrop disc
    _ellipse(d, [0.17 * u, 0.3 * u, 0.83 * u, 0.92 * u], (170, 96, 60, 255), 3)    # hair (behind the face)
    _ellipse(d, [0.26 * u, 0.3 * u, 0.74 * u, 0.84 * u], (247, 214, 186, 255), 3)   # face
    d.chord([0.26 * u, 0.3 * u, 0.74 * u, 0.62 * u], 180, 360, fill=(170, 96, 60, 255))  # bangs
    _ellipse(d, [0.08 * u, 0.26 * u, 0.92 * u, 0.46 * u], (232, 196, 120, 255), 3)  # hat brim
    _poly(d, [(0.3 * u, 0.34 * u), (0.34 * u, 0.12 * u), (0.66 * u, 0.12 * u), (0.7 * u, 0.34 * u)], (240, 206, 132, 255))  # crown
    d.rectangle([0.31 * u, 0.26 * u, 0.69 * u, 0.32 * u], fill=(226, 92, 100, 255))  # ribbon
    for x in (0.4, 0.6):
        d.arc([x * u - 0.05 * u, 0.52 * u, x * u + 0.05 * u, 0.6 * u], 200, 340, fill=INK, width=3 * S)  # smiling eyes
        for k in range(3):
            fx, fy = x * u + (k - 1) * 0.025 * u, 0.65 * u + (k % 2) * 0.012 * u
            d.ellipse([fx - 1.5 * S, fy - 1.5 * S, fx + 1.5 * S, fy + 1.5 * S], fill=(210, 130, 100, 255))
    d.ellipse([0.33 * u, 0.62 * u, 0.4 * u, 0.67 * u], fill=(246, 160, 160, 160))
    d.ellipse([0.6 * u, 0.62 * u, 0.67 * u, 0.67 * u], fill=(246, 160, 160, 160))
    d.arc([0.43 * u, 0.64 * u, 0.57 * u, 0.74 * u], 20, 160, fill=INK, width=3 * S)  # smile
    _poly(d, [(0.36 * u, 0.84 * u), (0.64 * u, 0.84 * u), (0.74 * u, 0.94 * u), (0.26 * u, 0.94 * u)], (134, 168, 134, 255))  # collar
    _finish(im, size, path, shadow=False)


ICONS = {"strawberry": strawberry, "seeds": seed_packet, "watering_can": watering_can, "hoe": hoe, "basket": basket, "jam": jam,
         "flower": flower, "coin": coin, "sun": sun}


def berry_seeds(path, size=256, n=14):
    """A tileable stipple of strawberry seeds (achenes): yellow seeds in little dimples, on white.
    Multiplied with the berry's red vertex colour through a triplanar material."""
    im = Image.new("RGB", (size * S, size * S), (255, 255, 255))
    d = ImageDraw.Draw(im)
    cell = size * S / n
    import random
    r = random.Random(5)
    for j in range(n):
        for i in range(n):
            x = (i + 0.5 + (0.5 if j % 2 else 0.0) + r.uniform(-0.15, 0.15)) * cell
            y = (j + 0.5 + r.uniform(-0.15, 0.15)) * cell
            for ox in (-size * S, 0, size * S):  # wrap so the tile is seamless
                rx, ry = cell * 0.22, cell * 0.3
                d.ellipse([x + ox - rx * 1.6, y - ry * 1.5, x + ox + rx * 1.6, y + ry * 1.5], fill=(214, 200, 196))
                d.ellipse([x + ox - rx, y - ry, x + ox + rx, y + ry], fill=(255, 226, 120))
    im = im.resize((size, size), Image.LANCZOS)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)


def paint_all(project):
    berry_seeds(os.path.join(project, "textures", "berry_seeds.png"))
    for name, fn in ICONS.items():
        fn(os.path.join(project, "ui", "icons", f"{name}.png"))
    portrait(os.path.join(project, "portraits", "juniper.png"))
