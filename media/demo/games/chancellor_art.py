"""The Chancellor's Desk — painted and typeset art (deterministic, PIL + numpy).

  chancellor_art.py OUT_DIR

Writes: the rainy city at dusk seen from the office window, rain on the glass, the rug, the
republic's flag and the chancellor's standard, the seal, advisor portraits (rim-lit studio
portraits in the manner of 1940s press photographs), the newspaper photograph, paper textures
for the documents on the desk and in the UI, and the stamps.
"""
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(HERE))), "assets", "fonts")
SERIF = os.path.join(FONTS, "EBGaramond.ttf")
SANS = os.path.join(FONTS, "Inter.ttf")
MONO = os.path.join(FONTS, "JetBrainsMono.ttf")
rng = np.random.default_rng(1946)


def font(path, size):
    return ImageFont.truetype(path, size)


def fbm(h, w, base, octaves, seed, gain=0.55):
    r = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        cy, cx = base * 2 ** o + 2, int(base * 2 ** o * w / h) + 2
        g = r.random((cy, cx)).astype(np.float32)
        out += np.asarray(Image.fromarray(g).resize((w, h), Image.BICUBIC)) * amp
        norm += amp
        amp *= gain
    return out / norm


def to_img(a):
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


# ============================================================================================
# The city at dusk, in the rain (a backdrop beyond the window; it glows on its own)
# ============================================================================================
def city(path, W=2400, H=1200):
    y = np.linspace(0, 1, H)[:, None] * np.ones((1, W))
    x = np.linspace(0, 1, W)[None, :] * np.ones((H, 1))
    # sky: storm blue above, a last bruised glow of dusk low on the horizon
    top = np.array([22, 30, 48], np.float32)
    mid = np.array([58, 70, 96], np.float32)
    low = np.array([150, 112, 98], np.float32)
    t = y[..., None]
    sky = np.where(t < 0.55, top + (mid - top) * (t / 0.55), mid + (low - mid) * ((t - 0.55) / 0.2).clip(0, 1))
    clouds = fbm(H, W, 3, 6, 11)
    sky = sky * (0.82 + 0.3 * clouds[..., None]) + np.array([30, 24, 26]) * np.exp(-((x - 0.62) ** 2) / 0.03)[..., None] * np.clip((t - 0.3) / 0.3, 0, 1)
    img = to_img(sky).convert("RGB")
    d = ImageDraw.Draw(img, "RGBA")
    r = np.random.default_rng(7)
    horizon = int(H * 0.72)

    def skyline(base_y, hmin, hmax, wmin, wmax, color, lit, seed, spires=0, dome=None):
        rr = np.random.default_rng(seed)
        xx = -20
        while xx < W + 20:
            bw = int(rr.uniform(wmin, wmax))
            bh = int(rr.uniform(hmin, hmax))
            top_y = base_y - bh
            d.rectangle([xx, top_y, xx + bw, H], fill=color)
            if rr.random() < 0.25:  # stepped roofs / mansards
                d.polygon([(xx - 2, top_y), (xx + bw + 2, top_y), (xx + bw - bw * 0.2, top_y - bh * 0.12), (xx + bw * 0.2, top_y - bh * 0.12)],
                          fill=color)
            # windows: warm, a few cold, most dark
            if lit > 0:
                for wy in range(top_y + 8, H, 14):
                    for wx in range(xx + 5, xx + bw - 6, 11):
                        p = rr.random()
                        if p < lit:
                            c = (255, int(rr.uniform(170, 215)), int(rr.uniform(100, 150)), int(rr.uniform(150, 235)))
                            if p < lit * 0.12:
                                c = (190, 215, 255, 190)
                            d.rectangle([wx, wy, wx + 4, wy + 6], fill=c)
            xx += bw + int(rr.uniform(0, 8))
        for k in range(spires):
            sx = int(rr.uniform(0.05, 0.95) * W)
            sh = rr.uniform(hmax * 1.3, hmax * 2.0)
            d.rectangle([sx - 9, base_y - sh * 0.7, sx + 9, H], fill=color)
            d.polygon([(sx - 11, base_y - sh * 0.7), (sx + 11, base_y - sh * 0.7), (sx, base_y - sh)], fill=color)
        if dome:
            cx, rad = dome
            d.rectangle([cx - rad * 1.9, base_y - rad * 1.2, cx + rad * 1.9, H], fill=color)
            d.rectangle([cx - rad * 0.8, base_y - rad * 1.9, cx + rad * 0.8, base_y - rad * 1.2], fill=color)
            d.pieslice([cx - rad, base_y - rad * 2.9, cx + rad, base_y - rad * 0.9], 180, 360, fill=color)
            d.rectangle([cx - 3, base_y - rad * 3.25, cx + 3, base_y - rad * 1.9], fill=color)
            # the Assembly's lit colonnade
            for k in range(-6, 7):
                d.rectangle([cx + k * rad * 0.27 - 1, base_y - rad * 1.0, cx + k * rad * 0.27 + 1, base_y - rad * 0.4],
                            fill=(255, 205, 140, 90))

    skyline(horizon - 10, 40, 120, 40, 120, (78, 84, 104), 0.0, 1, spires=4)
    haze = Image.new("RGBA", (W, H), (120, 112, 120, 0))
    hd = ImageDraw.Draw(haze)
    for k in range(40):
        hd.rectangle([0, horizon - 120 + k * 4, W, horizon - 116 + k * 4], fill=(132, 120, 126, 6))
    img.paste(haze, (0, 0), haze)
    skyline(horizon + 30, 60, 190, 50, 140, (46, 50, 66), 0.10, 2, spires=3)
    skyline(horizon + 120, 120, 320, 80, 220, (24, 26, 36), 0.16, 3, spires=2, dome=(int(W * 0.6), 105))
    skyline(H + 40, 220, 420, 160, 340, (12, 13, 19), 0.1, 4)
    # street glow and lamps far below
    glow = np.zeros((H, W), np.float32)
    for k in range(30):
        gx, gy = r.uniform(0, W), r.uniform(horizon + 40, H)
        glow += np.exp(-(((np.arange(W)[None, :] - gx) / 12.0) ** 2 + ((np.arange(H)[:, None] - gy) / 6.0) ** 2))
    a = np.asarray(img).astype(np.float32)
    a += glow[..., None] * np.array([255, 190, 120]) * 0.45
    # rain: thin bright slanted streaks
    rain = Image.new("L", (W, H), 0)
    rd = ImageDraw.Draw(rain)
    for k in range(5200):
        rx, ry = r.uniform(-100, W), r.uniform(-40, H)
        L = r.uniform(18, 46)
        rd.line([(rx, ry), (rx + L * 0.12, ry + L)], fill=int(r.uniform(25, 70)), width=1)
    rain = np.asarray(rain.filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255
    a = a * (1 - rain[..., None] * 0.4) + rain[..., None] * np.array([190, 200, 215]) * 0.6
    img = to_img(a).filter(ImageFilter.GaussianBlur(1.2))
    img.save(path, quality=90)


def rain_glass(path, n=1024):
    """Droplets and runnels on a window pane (RGBA, mostly transparent)."""
    r = np.random.default_rng(23)
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for k in range(1400):
        x, y = r.uniform(0, n), r.uniform(0, n)
        rad = r.uniform(1.2, 4.5) if r.random() < 0.9 else r.uniform(5, 9)
        d.ellipse([x - rad, y - rad * 1.15, x + rad, y + rad * 1.15], fill=(200, 214, 230, int(r.uniform(40, 110))))
        d.ellipse([x - rad * 0.45, y - rad * 0.8, x - rad * 0.05, y - rad * 0.35], fill=(255, 255, 255, int(r.uniform(90, 200))))
    for k in range(60):  # runnels
        x = r.uniform(0, n)
        y0 = r.uniform(-50, n * 0.6)
        L = r.uniform(80, 420)
        pts = []
        for s in range(30):
            yy = y0 + L * s / 29
            pts.append((x + math.sin(yy * 0.03 + k) * 3, yy))
        d.line(pts, fill=(205, 218, 232, 70), width=int(r.uniform(2, 4)))
        d.ellipse([pts[-1][0] - 4, pts[-1][1] - 5, pts[-1][0] + 4, pts[-1][1] + 6], fill=(220, 230, 240, 120))
    img.filter(ImageFilter.GaussianBlur(0.5)).save(path)


# ============================================================================================
# Textiles, flags, seal
# ============================================================================================
def rug(path, W=2048, H=1440):
    base = np.zeros((H, W, 3), np.float32) + np.array([96, 22, 26])
    n = fbm(H, W, 30, 3, 5)
    base *= (0.85 + 0.25 * n[..., None])
    img = to_img(base).convert("RGB")
    d = ImageDraw.Draw(img)
    gold, navy, cream = (182, 140, 70), (24, 30, 52), (214, 196, 160)
    for k, (inset, col, wdt) in enumerate([(30, navy, 70), (110, gold, 8), (130, navy, 46), (186, gold, 5)]):
        d.rectangle([inset, inset, W - inset, H - inset], outline=col, width=wdt)
    # border motifs
    for x in range(160, W - 140, 64):
        for yy in (153, H - 153):
            d.polygon([(x, yy - 14), (x + 14, yy), (x, yy + 14), (x - 14, yy)], fill=gold)
    for yy in range(160, H - 140, 64):
        for x in (153, W - 153):
            d.polygon([(x, yy - 14), (x + 14, yy), (x, yy + 14), (x - 14, yy)], fill=gold)
    # central medallion
    cx, cy = W // 2, H // 2
    for k, (rx, ry, col) in enumerate([(520, 360, navy), (500, 340, gold), (470, 314, (110, 28, 30)), (300, 200, navy),
                                       (280, 184, gold), (250, 160, (120, 30, 32))]):
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=col)
    for k in range(16):
        a = 2 * math.pi * k / 16
        d.polygon([(cx + math.cos(a) * 120, cy + math.sin(a) * 80), (cx + math.cos(a + 0.12) * 240, cy + math.sin(a + 0.12) * 150),
                   (cx + math.cos(a - 0.12) * 240, cy + math.sin(a - 0.12) * 150)], fill=cream)
    d.ellipse([cx - 70, cy - 46, cx + 70, cy + 46], fill=gold)
    a = np.asarray(img).astype(np.float32)
    wear = fbm(H, W, 6, 4, 9)
    a *= (0.8 + 0.3 * wear[..., None])
    a += (rng.random((H, W, 1)) - 0.5) * 18   # pile
    to_img(a).filter(ImageFilter.GaussianBlur(0.6)).save(path, quality=90)


def republic_flag(path, w=900, h=600):
    """Republic of Kestria (post-war): green-white-green with a rising sun, and a laurel at the hoist."""
    img = Image.new("RGB", (w, h), (238, 230, 210))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w, h * 0.32], fill=(52, 104, 62))
    d.rectangle([0, h * 0.68, w, h], fill=(52, 104, 62))
    cx, cy = w * 0.5, h * 0.62
    for k in range(13):
        a = math.pi + math.pi * k / 12
        d.line([(cx + math.cos(a) * 70, cy + math.sin(a) * 70), (cx + math.cos(a) * 128, cy + math.sin(a) * 128)],
               fill=(214, 168, 64), width=8)
    d.pieslice([cx - 62, cy - 62, cx + 62, cy + 62], 180, 360, fill=(214, 168, 64))
    img = img.resize((w, h), Image.LANCZOS)
    img.save(path)


def standard(path, w=900, h=600):
    """The Chancellor's standard: deep blue, a gold border and the seal."""
    img = Image.new("RGB", (w, h), (26, 36, 72))
    d = ImageDraw.Draw(img)
    d.rectangle([18, 18, w - 18, h - 18], outline=(206, 166, 82), width=14)
    seal_img = seal_draw(360, (206, 166, 82), None)
    img.paste(seal_img, (w // 2 - 180, h // 2 - 180), seal_img)
    img.save(path)


def seal_draw(n, ink, bg):
    img = Image.new("RGBA", (n * 2, n * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = n
    if bg:
        d.ellipse([0, 0, 2 * n, 2 * n], fill=bg)
    d.ellipse([18, 18, 2 * n - 18, 2 * n - 18], outline=ink, width=12)
    d.ellipse([70, 70, 2 * n - 70, 2 * n - 70], outline=ink, width=5)
    f = font(SERIF, int(n * 0.16))
    text = "REPUBLIC  OF  KESTRIA  ·  MCMXLVI  ·  "
    for i, ch in enumerate(text):
        a = -math.pi / 2 + 2 * math.pi * i / len(text)
        ci = Image.new("RGBA", (int(n * 0.3), int(n * 0.3)), (0, 0, 0, 0))
        ImageDraw.Draw(ci).text((n * 0.15, n * 0.15), ch, font=f, fill=ink, anchor="mm")
        ci = ci.rotate(-math.degrees(a) - 90, resample=Image.BICUBIC)
        r = n - 44
        img.paste(ci, (int(c + math.cos(a) * r - n * 0.15), int(c + math.sin(a) * r - n * 0.15)), ci)
    # rising sun over a bridge: the republic's emblem
    cy = c + n * 0.12
    for k in range(11):
        a = math.pi + math.pi * k / 10
        d.line([(c + math.cos(a) * n * 0.28, cy + math.sin(a) * n * 0.28), (c + math.cos(a) * n * 0.48, cy + math.sin(a) * n * 0.48)],
               fill=ink, width=int(n * 0.035))
    d.pieslice([c - n * 0.24, cy - n * 0.24, c + n * 0.24, cy + n * 0.24], 180, 360, fill=ink)
    d.rectangle([c - n * 0.5, cy + n * 0.04, c + n * 0.5, cy + n * 0.09], fill=ink)
    for k in range(4):
        x = c - n * 0.42 + k * n * 0.28
        d.arc([x, cy + n * 0.06, x + n * 0.28, cy + n * 0.36], 180, 360, fill=ink, width=int(n * 0.03))
    return img.resize((n, n), Image.LANCZOS)


def seal(path, n=512):
    seal_draw(n, (206, 166, 82, 255), None).save(path)


def stamp(path, text, color, n=640):
    img = Image.new("RGBA", (n, n // 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([12, 12, n - 12, n // 2 - 12], radius=18, outline=color, width=12)
    d.rounded_rectangle([34, 34, n - 34, n // 2 - 34], radius=10, outline=color, width=4)
    d.text((n // 2, n // 4 + 4), text, font=font(SERIF, 104), fill=color, anchor="mm")
    a = np.asarray(img).astype(np.float32)
    grit = fbm(n // 2, n, 20, 3, 31)
    a[..., 3] *= np.clip(grit * 1.8 - 0.25, 0, 1)
    to_img(a).convert("RGBA").rotate(-8, resample=Image.BICUBIC, expand=True).save(path)


# ============================================================================================
# Advisors: studio portraits, rim-lit, in the manner of 1940s press photographs
# ============================================================================================
def _smooth(pts, n=8):
    """Closed Catmull-Rom through pts -> dense polygon."""
    out = []
    P = list(pts)
    for i in range(len(P)):
        p0, p1, p2, p3 = (np.array(P[(i + k - 1) % len(P)], np.float32) for k in range(4))
        for s in range(n):
            t = s / n
            out.append(tuple(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                                    (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3)))
    return out


def portrait(path, who, W=600, H=740):
    """A cameo: the advisor's profile in silhouette inside an oval, lit from behind by the lamp,
    with a cool rim from the window: the way 1940s dossiers and banknotes drew officials."""
    S = 3
    w, h = W * S, H * S
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # paper card
    card = np.zeros((h, w, 3), np.float32) + np.array(who["card"], np.float32)
    card *= (0.92 + 0.1 * fbm(h, w, 5, 5, who["seed"])[..., None])
    # the oval's lit ground: a warm glow behind the head
    ox, oy, orx, ory = w * 0.5, h * 0.47, w * 0.4, h * 0.41
    inside = (((xx - ox) / orx) ** 2 + ((yy - oy) / ory) ** 2) < 1.0
    g = np.exp(-(((xx - w * 0.56) / (w * 0.32)) ** 2 + ((yy - h * 0.36) / (h * 0.3)) ** 2))
    ground = np.array(who["ground_dark"], np.float32) + (np.array(who["ground_lit"], np.float32) -
                                                         np.array(who["ground_dark"], np.float32)) * g[..., None]
    img = np.where(inside[..., None], ground, card)
    # the profile (facing right): a polygon in head units
    u = w * 0.105                     # head unit
    cx, cy = w * 0.47, h * 0.40       # center of the skull
    prof = who["profile"]
    pts = [(cx + px * u, cy + py * u) for px, py in prof]
    sil = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(sil)
    d.polygon(_smooth(pts, 10), fill=255)
    # neck, shoulders and chest seen in profile (facing right)
    body = [(-0.58, 1.3), (0.44, 1.45), (0.5, 1.95), (0.7, 2.35), (1.2, 2.7), (1.5, 3.3), (1.62, 4.4), (1.7, 6.0), (-3.6, 6.0),
            (-3.4, 3.6), (-2.9, 2.95), (-1.9, 2.55), (-1.05, 2.2), (-0.7, 1.8)]
    d.polygon(_smooth([(cx + x * u, cy + y * u) for x, y in body], 8), fill=255)
    who["dress"](d, cx, cy, u)
    m = np.asarray(sil.filter(ImageFilter.GaussianBlur(1.5))).astype(np.float32) / 255
    m *= inside
    # silhouette tone: near-black, a faint warm turn on the lamp side
    tone = np.array(who["ink"], np.float32) * (0.85 + 0.3 * np.clip((xx - cx) / (u * 4), -0.5, 1))[..., None]
    img = img * (1 - m[..., None]) + tone * m[..., None]
    # rim light along the face (the lamp is ahead of them) and a cool kick from behind
    for dx, col, k in ((-int(u * 0.06), (255, 214, 160), 1.3), (int(u * 0.05), (150, 180, 230), 0.6)):
        shifted = np.roll(m, dx, axis=1)
        rim = np.clip(m - shifted, 0, 1)
        rim = np.asarray(Image.fromarray((rim * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(S * 1.2))).astype(np.float32) / 255
        img += rim[..., None] * np.array(col, np.float32) * k * inside[..., None] * (yy < cy + u * 3.0)[..., None]
    # details drawn on top (spectacle glint, pearl, medals) in light colors
    det = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    who.get("details", lambda *a: None)(ImageDraw.Draw(det), cx, cy, u)
    da = np.asarray(det).astype(np.float32) / 255
    img = img * (1 - da[..., 3:]) + da[..., :3] * 255 * da[..., 3:]
    # the oval frame: a gilt double line, and the caption plate
    fr = Image.new("L", (w, h), 0)
    fd = ImageDraw.Draw(fr)
    fd.ellipse([ox - orx, oy - ory, ox + orx, oy + ory], outline=255, width=int(S * 4))
    fd.ellipse([ox - orx - S * 12, oy - ory - S * 12, ox + orx + S * 12, oy + ory + S * 12], outline=255, width=int(S * 1.5))
    fm = np.asarray(fr.filter(ImageFilter.GaussianBlur(0.8))).astype(np.float32)[..., None] / 255
    img = img * (1 - fm) + np.array((188, 150, 84), np.float32) * fm
    # grain and wear
    img += (np.random.default_rng(who["seed"]).random((h, w, 1)) - 0.5) * 14
    out = to_img(img).resize((W, H), Image.LANCZOS)
    dd = ImageDraw.Draw(out)
    dd.text((W // 2, H - 40), who["caption"], font=font(SERIF, 26), fill=(70, 52, 36), anchor="mm")
    out.save(path)


# Profiles in head units (x right = forward, y down); the skull's center is (0, 0).
_FEMALE = [(-1.0, -0.2), (-0.85, -0.85), (-0.3, -1.12), (0.35, -1.08), (0.8, -0.78), (0.95, -0.35), (0.98, -0.1), (1.12, 0.08),
           (1.28, 0.38), (1.13, 0.45), (1.12, 0.6), (1.18, 0.68), (1.1, 0.76), (1.12, 0.88), (1.0, 1.05), (0.72, 1.12),
           (0.5, 1.15), (0.42, 1.6), (-0.55, 1.6), (-0.55, 0.9), (-0.9, 0.5)]
_GENERAL = [(-1.0, -0.15), (-0.9, -0.8), (-0.35, -1.1), (0.4, -1.06), (0.85, -0.8), (1.0, -0.38), (1.04, -0.12), (1.2, 0.1),
            (1.4, 0.42), (1.2, 0.5), (1.2, 0.64), (1.25, 0.72), (1.15, 0.8), (1.2, 0.95), (1.08, 1.15), (0.75, 1.25),
            (0.55, 1.3), (0.5, 1.75), (-0.6, 1.75), (-0.6, 0.95), (-0.95, 0.55)]
_MINISTER = [(-1.02, -0.1), (-0.95, -0.75), (-0.4, -1.08), (0.35, -1.05), (0.82, -0.82), (1.02, -0.4), (1.06, -0.08), (1.3, 0.2),
             (1.38, 0.46), (1.18, 0.5), (1.16, 0.66), (1.22, 0.74), (1.12, 0.82), (1.2, 1.0), (1.0, 1.3), (0.6, 1.36),
             (0.5, 1.7), (-0.62, 1.7), (-0.65, 0.95), (-1.0, 0.55)]


def _dress_dornach(d, cx, cy, u):
    d.ellipse([cx - u * 1.5, cy - u * 0.2, cx - u * 0.68, cy + u * 0.62], fill=255)     # the low bun
    d.polygon([(cx + u * 0.5, cy + u * 1.95), (cx + u * 1.05, cy + u * 2.2), (cx + u * 0.9, cy + u * 2.75)], fill=255)   # lapel


def _det_dornach(d, cx, cy, u):
    d.ellipse([cx - u * 0.12, cy + u * 0.62, cx + u * 0.02, cy + u * 0.76], fill=(240, 232, 214, 230))   # earring
    for k in range(9):
        t = k / 8
        x, y = cx + u * (-0.5 + 1.0 * t), cy + u * (1.86 + 0.16 * math.sin(math.pi * t) + 0.08 * t)
        d.ellipse([x - u * 0.05, y - u * 0.05, x + u * 0.05, y + u * 0.05], fill=(236, 228, 210, 220))


def _dress_vass(d, cx, cy, u):
    # peaked cap: crown, band and the visor reaching past the brow
    d.chord([cx - u * 1.25, cy - u * 1.75, cx + u * 1.05, cy - u * 0.25], 180, 360, fill=255)
    d.rectangle([cx - u * 1.05, cy - u * 1.02, cx + u * 0.98, cy - u * 0.48], fill=255)
    d.polygon([(cx + u * 0.75, cy - u * 0.6), (cx + u * 1.62, cy - u * 0.42), (cx + u * 1.55, cy - u * 0.32), (cx + u * 0.75, cy - u * 0.4)], fill=255)
    d.polygon([(cx - u * 0.7, cy + u * 1.25), (cx + u * 0.6, cy + u * 1.4), (cx + u * 0.62, cy + u * 2.0), (cx - u * 0.78, cy + u * 1.9)],
              fill=255)      # high collar
    d.rounded_rectangle([cx - u * 2.7, cy + u * 2.62, cx - u * 0.9, cy + u * 2.95], radius=int(u * 0.12), fill=255)   # epaulette


def _det_vass(d, cx, cy, u):
    d.ellipse([cx + u * 0.12, cy - u * 0.95, cx + u * 0.42, cy - u * 0.62], fill=(206, 166, 82, 230))   # cap badge
    for row in range(2):
        for k in range(3):
            x = cx + u * (0.35 + k * 0.26 + row * 0.13)
            y = cy + u * (3.05 + row * 0.2)
            d.rectangle([x, y, x + u * 0.22, y + u * 0.14], fill=[(150, 40, 34, 230), (40, 70, 120, 230), (206, 166, 82, 230)][k])


def _dress_ardanne(d, cx, cy, u):
    d.polygon([(cx + u * 0.48, cy + u * 1.9), (cx + u * 1.0, cy + u * 2.3), (cx + u * 0.85, cy + u * 2.9)], fill=255)   # lapel


def _det_ardanne(d, cx, cy, u):
    # round spectacles (seen edge-on: a lens and the temple arm), a bow tie, a fringe of grey hair
    d.ellipse([cx + u * 0.78, cy - u * 0.18, cx + u * 1.06, cy + u * 0.12], outline=(220, 200, 160, 230), width=max(2, int(u * 0.035)))
    d.line([(cx + u * 0.8, cy - u * 0.03), (cx - u * 0.2, cy - u * 0.05)], fill=(200, 180, 140, 200), width=max(2, int(u * 0.03)))
    d.polygon([(cx + u * 0.42, cy + u * 1.62), (cx + u * 0.72, cy + u * 1.48), (cx + u * 0.74, cy + u * 1.82)], fill=(150, 40, 40, 220))
    d.polygon([(cx + u * 0.42, cy + u * 1.62), (cx + u * 0.16, cy + u * 1.46), (cx + u * 0.14, cy + u * 1.8)], fill=(120, 30, 30, 200))


ADVISORS = {
    "dornach": dict(seed=3, card=(214, 202, 178), ground_dark=(46, 58, 54), ground_lit=(196, 160, 112), ink=(22, 20, 22),
                    profile=_FEMALE, dress=_dress_dornach, details=_det_dornach, caption="Ilse Dornach · Chief of Staff"),
    "vass": dict(seed=5, card=(206, 200, 184), ground_dark=(40, 46, 58), ground_lit=(176, 150, 118), ink=(24, 24, 22),
                 profile=_GENERAL, dress=_dress_vass, details=_det_vass, caption="Gen. Teodor Vass · Engineer Corps"),
    "ardanne": dict(seed=9, card=(218, 204, 180), ground_dark=(60, 44, 34), ground_lit=(206, 168, 116), ink=(26, 22, 20),
                    profile=_MINISTER, dress=_dress_ardanne, details=_det_ardanne, caption="Pell Ardanne · Treasury"),
}


# ============================================================================================
# Paper: the decree, the newspaper, letters (textures for the desk) and the UI paper
# ============================================================================================
def paper_base(W, H, seed, tone=(236, 226, 204)):
    a = np.zeros((H, W, 3), np.float32) + np.array(tone)
    a *= (0.93 + 0.08 * fbm(H, W, 6, 5, seed)[..., None])
    a *= (1 - 0.06 * fbm(H, W, 40, 2, seed + 1)[..., None])
    yy, xx = np.mgrid[0:H, 0:W]
    edge = np.minimum(np.minimum(xx, W - xx), np.minimum(yy, H - yy)) / max(W, H)
    a *= (0.86 + 0.14 * np.clip(edge * 14, 0, 1))[..., None]
    return to_img(a).convert("RGB")


def ui_paper(path, W=1200, H=1500, seed=41):
    paper_base(W, H, seed).save(path, quality=90)


def newsprint(path, W=1200, H=1600, seed=43):
    paper_base(W, H, seed, tone=(226, 218, 196)).save(path, quality=90)


def news_photo(path, W=900, H=520):
    """The Assembly steps in the rain, crowd with umbrellas (a halftone press photograph)."""
    r = np.random.default_rng(77)
    img = Image.new("L", (W, H), 150)
    d = ImageDraw.Draw(img)
    for k in range(H):
        d.line([(0, k), (W, k)], fill=int(170 - k * 0.12))
    # the Assembly: columns and a pediment
    d.polygon([(120, 170), (W - 120, 170), (W // 2, 70)], fill=96)
    d.rectangle([120, 170, W - 120, 190], fill=80)
    for k in range(12):
        x = 150 + k * (W - 300) / 11
        d.rectangle([x - 12, 190, x + 12, 360], fill=118)
        d.rectangle([x - 12, 190, x - 4, 360], fill=86)
    for k in range(7):
        d.rectangle([60 + k * 10, 360 + k * 14, W - 60 - k * 10, 374 + k * 14], fill=124 - k * 4)
    # crowd and umbrellas
    for k in range(260):
        x, y = r.uniform(0, W), r.uniform(370, H)
        s = 0.6 + (y - 370) / (H - 370) * 1.6
        d.ellipse([x - 6 * s, y - 16 * s, x + 6 * s, y - 4 * s], fill=40)
        d.rectangle([x - 8 * s, y - 4 * s, x + 8 * s, y + 30 * s], fill=34)
        if r.random() < 0.35:
            d.chord([x - 24 * s, y - 34 * s, x + 24 * s, y - 2 * s], 180, 360, fill=22)
    for k in range(900):
        x, y = r.uniform(0, W), r.uniform(0, H)
        d.line([(x, y), (x + 2, y + 18)], fill=200)
    img = img.filter(ImageFilter.GaussianBlur(1.4))
    # halftone
    a = np.asarray(img).astype(np.float32) / 255
    yy, xx = np.mgrid[0:H, 0:W]
    cell = 5.0
    u = (xx % cell) / cell - 0.5
    v = (yy % cell) / cell - 0.5
    dist = np.sqrt(u * u + v * v)
    dots = (dist < (1 - a) * 0.62).astype(np.float32)
    out = (1 - dots[..., None] * 0.85) * np.array([232, 224, 204])[None, None, :] / 255
    to_img(out * 255).filter(ImageFilter.GaussianBlur(0.4)).save(path, quality=90)


def wrap(text, f, width, draw):
    words, lines, cur = text.split(), [], ""
    for w_ in words:
        t = (cur + " " + w_).strip()
        if draw.textlength(t, font=f) > width and cur:
            lines.append(cur)
            cur = w_
        else:
            cur = t
    if cur:
        lines.append(cur)
    return lines


def decree_page(path, W=1100, H=1500):
    img = paper_base(W, H, 51, tone=(240, 232, 212))
    d = ImageDraw.Draw(img)
    ink = (34, 28, 24)
    s = seal_draw(150, (150, 40, 34, 255), None)
    img.paste(s, (W // 2 - 75, 70), s)
    d.text((W // 2, 260), "REPUBLIC OF KESTRIA", font=font(SERIF, 40), fill=ink, anchor="mm")
    d.text((W // 2, 312), "Office of the Chancellor", font=font(SERIF, 30), fill=(90, 70, 60), anchor="mm")
    d.line([(160, 350), (W - 160, 350)], fill=(150, 40, 34), width=3)
    d.text((W // 2, 410), "DECREE No. 14", font=font(SERIF, 58), fill=ink, anchor="mm")
    d.text((W // 2, 470), "On the Restoration of the Vey Bridges", font=font(SERIF, 36), fill=ink, anchor="mm")
    body = ("By authority of the Provisional Charter, and with the consent of the Assembly, the Chancellor decrees "
            "that the seven bridges of the River Vey destroyed in the late war shall be rebuilt within two years. "
            "The Treasury shall raise the sum of nine hundred million crowns by the Reconstruction Bond. The Army "
            "Engineer Corps shall be placed under civil authority for the duration of the works. Every province "
            "east of the Vey shall receive a share of the contracts in proportion to its losses.")
    y = 560
    f = font(SERIF, 30)
    for line in wrap(body, f, W - 280, d):
        d.text((140, y), line, font=f, fill=ink)
        y += 44
    y += 30
    for k, art in enumerate(["I. The Bond shall be offered at four percent.", "II. No province shall be excluded.",
                             "III. The works shall begin before the thaw."]):
        d.text((160, y), art, font=f, fill=ink)
        y += 46
    d.text((140, H - 230), "Given at Mirovan, the fourth of November, 1946.", font=font(SERIF, 28), fill=(80, 64, 54))
    d.line([(W - 520, H - 140), (W - 140, H - 140)], fill=ink, width=2)
    d.text((W - 330, H - 112), "Chancellor of the Republic", font=font(SERIF, 26), fill=(80, 64, 54), anchor="mm")
    img.save(path, quality=90)


def newspaper_page(path, W=1100, H=1500, photo=None):
    img = paper_base(W, H, 61, tone=(228, 220, 198))
    d = ImageDraw.Draw(img)
    ink = (24, 22, 20)
    d.text((W // 2, 92), "The Mirovan Ledger", font=font(SERIF, 104), fill=ink, anchor="mm")
    d.line([(50, 160), (W - 50, 160)], fill=ink, width=3)
    d.text((60, 182), "VOL. LXXII · No. 18,204", font=font(SANS, 20), fill=ink, anchor="lm")
    d.text((W // 2, 182), "MIROVAN, MONDAY, NOVEMBER 4, 1946", font=font(SANS, 20), fill=ink, anchor="mm")
    d.text((W - 60, 182), "TWO CROWNS", font=font(SANS, 20), fill=ink, anchor="rm")
    d.line([(50, 202), (W - 50, 202)], fill=ink, width=1)
    d.text((W // 2, 280), "ASSEMBLY SENDS BRIDGES DECREE", font=font(SERIF, 66), fill=ink, anchor="mm")
    d.text((W // 2, 350), "TO THE CHANCELLOR'S DESK", font=font(SERIF, 52), fill=ink, anchor="mm")
    d.text((W // 2, 410), "Vote of 212 to 198 · Army Demands Command of Works · Treasury Warns of Bond", font=font(SERIF, 28),
           fill=(60, 56, 50), anchor="mm")
    if photo:
        ph = Image.open(photo).convert("RGB").resize((W - 120, int((W - 120) * 520 / 900)))
        img.paste(ph, (60, 450))
        y = 450 + ph.size[1] + 14
    else:
        y = 460
    d.text((60, y), "Crowds wait in the rain on the Assembly steps for the result of the vote.", font=font(SERIF, 22),
           fill=(70, 64, 58))
    y += 46
    col_w = (W - 160) // 3
    text = ("The fate of the seven bridges, and perhaps of the Provisional Charter itself, now rests with one signature. "
            "After nine hours of debate the Assembly passed the Reconstruction Bill by fourteen votes. Members from the "
            "eastern provinces cheered from the gallery; the Army benches sat in silence. Marshal-turned-deputy Vass told "
            "reporters the Engineer Corps would not serve under civilian clerks. The Treasury has not denied that the "
            "Bond may fail to sell. ") * 3
    f = font(SERIF, 21)
    lines = wrap(text, f, col_w - 10, d)
    per = (H - y - 60) // 28
    for c in range(3):
        for k, line in enumerate(lines[c * per:(c + 1) * per]):
            d.text((60 + c * (col_w + 20), y + k * 28), line, font=f, fill=ink)
        if c < 2:
            d.line([(60 + (c + 1) * (col_w + 20) - 12, y), (60 + (c + 1) * (col_w + 20) - 12, H - 60)], fill=(90, 86, 80), width=1)
    img.save(path, quality=90)


def letter_page(path, seed, W=900, H=1200):
    img = paper_base(W, H, seed, tone=(232, 228, 216))
    d = ImageDraw.Draw(img)
    f = font(MONO, 22)
    r = np.random.default_rng(seed)
    y = 120
    d.text((90, 70), "MINISTRY OF THE TREASURY · CONFIDENTIAL", font=font(SANS, 22), fill=(60, 50, 50))
    while y < H - 140:
        n = int(r.uniform(20, 46))
        d.text((90, y), "".join(r.choice(list("abcdefghijklmnopqrstuvwxyz     ")) for _ in range(n)), font=f, fill=(46, 42, 40))
        y += 34
    img.save(path, quality=88)


def generate(out):
    os.makedirs(out, exist_ok=True)
    os.makedirs(os.path.join(out, "portraits"), exist_ok=True)
    city(os.path.join(out, "city_dusk.jpg"))
    rain_glass(os.path.join(out, "rain_glass.png"))
    rug(os.path.join(out, "rug.jpg"))
    republic_flag(os.path.join(out, "flag_republic.png"))
    standard(os.path.join(out, "flag_standard.png"))
    seal(os.path.join(out, "seal.png"))
    stamp(os.path.join(out, "stamp_signed.png"), "SIGNED", (40, 92, 60, 235))
    stamp(os.path.join(out, "stamp_vetoed.png"), "VETOED", (160, 36, 30, 235))
    for k, who in ADVISORS.items():
        portrait(os.path.join(out, "portraits", f"{k}.png"), who)
    ui_paper(os.path.join(out, "paper.jpg"))
    newsprint(os.path.join(out, "newsprint.jpg"))
    news_photo(os.path.join(out, "news_photo.jpg"))
    decree_page(os.path.join(out, "decree_page.jpg"))
    newspaper_page(os.path.join(out, "newspaper_page.jpg"), photo=os.path.join(out, "news_photo.jpg"))
    for k in range(3):
        letter_page(os.path.join(out, f"letter_{k}.jpg"), 90 + k)


if __name__ == "__main__":
    generate(sys.argv[1] if len(sys.argv) > 1 else "art")
