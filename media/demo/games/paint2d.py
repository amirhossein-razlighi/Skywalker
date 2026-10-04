"""Tiny procedural painting kit (numpy + PIL) for the 2D showcase art.

Everything works on float32 RGBA arrays in [0, 1] (straight alpha, sRGB values). Helpers cover
tileable fBm noise, anti-aliased shapes (supersampled PIL drawing), FFT blurs (wrap-around, so
tileable panels stay seamless), compositing, rim lights and normal maps derived from coverage.
Deterministic: every random source takes a seed.
"""
import math

import numpy as np
from PIL import Image, ImageDraw

# --------------------------------------------------------------------------------------------
# Colors
# --------------------------------------------------------------------------------------------


def rgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)], dtype=np.float32)


def mix(a, b, t):
    return a + (b - a) * t


# --------------------------------------------------------------------------------------------
# Noise
# --------------------------------------------------------------------------------------------


def _smooth_grid(w, h, cells_x, cells_y, rng, wrap_x=True, wrap_y=False):
    """Value noise: a random grid upsampled with a smooth (cubic) filter."""
    cx, cy = max(1, int(cells_x)), max(1, int(cells_y))
    g = rng.random((cy + 3, cx + 3)).astype(np.float32)
    if wrap_x:
        g[:, cx:] = g[:, :3]
    if wrap_y:
        g[cy:, :] = g[:3, :]
    img = Image.fromarray(g, mode="F")
    # Resize the (cells + 3) grid so that `cells` spans exactly w x h, then crop.
    sw = int(round(w * (cx + 3) / cx))
    sh = int(round(h * (cy + 3) / cy))
    big = np.asarray(img.resize((sw, sh), Image.BICUBIC), dtype=np.float32)
    ox = int(round(w / cx * 1.0))
    oy = int(round(h / cy * 1.0))
    return big[oy:oy + h, ox:ox + w]


def fbm(w, h, scale, octaves=5, seed=0, wrap_x=True, wrap_y=False, gain=0.5, lacunarity=2.0, stretch=(1.0, 1.0)):
    """Fractal noise in [0, 1]. `scale` = size of the largest feature in pixels."""
    rng = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    s = float(scale)
    for _ in range(octaves):
        cx = max(1, round(w / (s * stretch[0])))
        cy = max(1, round(h / (s * stretch[1])))
        out += _smooth_grid(w, h, cx, cy, rng, wrap_x, wrap_y) * amp
        total += amp
        amp *= gain
        s /= lacunarity
        if s < 1.5:
            break
    out /= total
    lo, hi = np.percentile(out, 1), np.percentile(out, 99)
    return np.clip((out - lo) / max(1e-6, hi - lo), 0, 1)


def fbm1d(n, scale, octaves=5, seed=0, wrap=True):
    """1D fractal noise in [-1, 1] (for contours)."""
    return fbm(n, 4, scale, octaves, seed, wrap_x=wrap)[1] * 2 - 1


# --------------------------------------------------------------------------------------------
# Shapes (anti-aliased by supersampling)
# --------------------------------------------------------------------------------------------

SS = 3


class Mask:
    """A supersampled 'L' canvas to draw shapes on; .get() returns a float mask in [0, 1]."""

    def __init__(self, w, h, ss=SS):
        self.w, self.h, self.ss = w, h, ss
        self.img = Image.new("L", (w * ss, h * ss), 0)
        self.d = ImageDraw.Draw(self.img)

    def _p(self, pts):
        return [(x * self.ss, y * self.ss) for x, y in pts]

    def poly(self, pts, v=255):
        self.d.polygon(self._p(pts), fill=v)
        return self

    def ellipse(self, cx, cy, rx, ry, v=255):
        s = self.ss
        self.d.ellipse([(cx - rx) * s, (cy - ry) * s, (cx + rx) * s, (cy + ry) * s], fill=v)
        return self

    def circle(self, cx, cy, r, v=255):
        return self.ellipse(cx, cy, r, r, v)

    def line(self, pts, width, v=255):
        s = self.ss
        self.d.line(self._p(pts), fill=v, width=max(1, int(round(width * s))), joint="curve")
        for (x, y) in pts:  # round caps
            self.d.ellipse([(x - width / 2) * s, (y - width / 2) * s, (x + width / 2) * s, (y + width / 2) * s], fill=v)
        return self

    def tapered(self, pts, w0, w1, v=255):
        """A stroke whose width goes from w0 to w1 along the polyline (roots, tendrils, kelp)."""
        n = len(pts)
        left, right = [], []
        for i, (x, y) in enumerate(pts):
            a = pts[max(0, i - 1)]
            b = pts[min(n - 1, i + 1)]
            dx, dy = b[0] - a[0], b[1] - a[1]
            ln = math.hypot(dx, dy) or 1.0
            nx, ny = -dy / ln, dx / ln
            wv = (w0 + (w1 - w0) * i / max(1, n - 1)) / 2
            left.append((x + nx * wv, y + ny * wv))
            right.append((x - nx * wv, y - ny * wv))
        return self.poly(left + right[::-1], v)

    def get(self):
        return np.asarray(self.img.resize((self.w, self.h), Image.LANCZOS), np.float32) / 255.0


def curve(p0, p1, p2, n=24):
    """Quadratic Bezier points."""
    out = []
    for i in range(n + 1):
        t = i / n
        a = (1 - t) * (1 - t)
        b = 2 * (1 - t) * t
        c = t * t
        out.append((a * p0[0] + b * p1[0] + c * p2[0], a * p0[1] + b * p1[1] + c * p2[1]))
    return out


def cubic(p0, p1, p2, p3, n=32):
    out = []
    for i in range(n + 1):
        t = i / n
        a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
        out.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))
    return out


# --------------------------------------------------------------------------------------------
# Filters
# --------------------------------------------------------------------------------------------


def blur(a, sigma, wrap=True):
    """Gaussian blur of a 2D array (FFT; wrap-around edges keep tileable panels seamless)."""
    if sigma <= 0.3:
        return a
    h, w = a.shape
    if not wrap:
        p = int(sigma * 3) + 1
        padded = np.pad(a, p, mode="edge")
        return blur(padded, sigma, True)[p:p + h, p:p + w]
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.rfftfreq(w)[None, :]
    g = np.exp(-2 * (math.pi ** 2) * (sigma ** 2) * (fx ** 2 + fy ** 2)).astype(np.float32)
    return np.fft.irfft2(np.fft.rfft2(a) * g, s=(h, w)).astype(np.float32)


def blur_x(a, sigma, wrap=True):
    """Horizontal-only blur (brush streaks)."""
    h, w = a.shape
    fx = np.fft.rfftfreq(w)[None, :]
    g = np.exp(-2 * (math.pi ** 2) * (sigma ** 2) * fx ** 2).astype(np.float32)
    return np.fft.irfft(np.fft.rfft(a, axis=1) * g, n=w, axis=1).astype(np.float32)


def shift(a, dx, dy, wrap_x=True):
    out = np.roll(a, (int(dy), int(dx)), axis=(0, 1))
    return out


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


# --------------------------------------------------------------------------------------------
# Layers
# --------------------------------------------------------------------------------------------


class Layer:
    """Straight-alpha RGBA float canvas."""

    def __init__(self, w, h, color=None, alpha=0.0):
        self.w, self.h = w, h
        self.rgb = np.zeros((h, w, 3), np.float32)
        self.a = np.full((h, w), alpha, np.float32)
        if color is not None:
            self.rgb[:] = rgb(color) if isinstance(color, str) else color

    def paint(self, mask, color, opacity=1.0):
        """Paints `color` (hex, rgb triple or an (h, w, 3) array) through `mask`."""
        c = rgb(color) if isinstance(color, str) else np.asarray(color, np.float32)
        m = np.clip(mask * opacity, 0, 1)
        na = m + self.a * (1 - m)
        safe = np.maximum(na, 1e-6)[..., None]
        self.rgb = (c * m[..., None] + self.rgb * (self.a * (1 - m))[..., None]) / safe
        self.a = na
        return self

    def tint(self, mask, color, amount=1.0):
        """Recolors existing pixels (keeps alpha)."""
        c = rgb(color) if isinstance(color, str) else np.asarray(color, np.float32)
        t = np.clip(mask * amount, 0, 1)[..., None]
        self.rgb = self.rgb * (1 - t) + c * t
        return self

    def add(self, mask, color, amount=1.0):
        """Additive light on existing pixels (keeps alpha)."""
        c = rgb(color) if isinstance(color, str) else np.asarray(color, np.float32)
        self.rgb = self.rgb + c * (mask * amount)[..., None]
        return self

    def multiply(self, factor):
        f = factor if np.ndim(factor) == 0 else factor[..., None]
        self.rgb = self.rgb * f
        return self

    def over(self, other):
        """Composites `other` over this layer."""
        m = other.a
        na = m + self.a * (1 - m)
        safe = np.maximum(na, 1e-6)[..., None]
        self.rgb = (other.rgb * m[..., None] + self.rgb * (self.a * (1 - m))[..., None]) / safe
        self.a = na
        return self

    def image(self, bleed=True):
        rgbv = np.clip(self.rgb, 0, 1)
        if bleed:
            rgbv = bleed_colors(rgbv, self.a)
        arr = np.dstack([rgbv, np.clip(self.a, 0, 1)])
        return Image.fromarray((arr * 255 + 0.5).astype(np.uint8), "RGBA")

    def save(self, path, opaque=False, quality=92):
        img = self.image()
        if opaque or path.endswith(".jpg"):
            img = img.convert("RGB")
            if path.endswith(".jpg"):
                img.save(path, quality=quality, optimize=True)
                return
        img.save(path, optimize=True)


def bleed_colors(rgbv, alpha, passes=(2, 6, 18)):
    """Fills the color of (nearly) transparent pixels from nearby opaque ones, so bilinear
    filtering and mipmaps never pull dark fringes into a sprite's edges."""
    out = rgbv.copy()
    known = alpha > 0.02
    if known.all() or not known.any():
        return out
    w = known.astype(np.float32)
    for s in passes:
        num = np.dstack([blur(rgbv[..., c] * w, s, wrap=False) for c in range(3)])
        den = blur(w, s, wrap=False)[..., None]
        fill = num / np.maximum(den, 1e-6)
        need = (~known) & (den[..., 0] > 1e-4)
        out[need] = fill[need]
        known = known | need
        rgbv = out
        w = known.astype(np.float32)
    return out


def edge_light(mask, dx, dy, sigma):
    """Rim term: how much of the shape lies away from the light direction (dx, dy in px)."""
    shifted = np.roll(mask, (int(round(dy)), int(round(dx))), axis=(0, 1))
    rim = np.clip(mask - shifted, 0, 1)
    return blur(rim, sigma) if sigma > 0 else rim


def normal_map(alpha, height_detail=None, bevel=6.0, strength=2.0, detail=0.4):
    """Tangent-space normal map (OpenGL: +Y up) from coverage: a soft pillow from the blurred alpha,
    plus optional painted height detail. Returns an RGBA uint8 image (alpha copied)."""
    hgt = blur(alpha, bevel, wrap=False)
    hgt = np.minimum(hgt, alpha * 2 + 0.0)
    if height_detail is not None:
        hgt = hgt + height_detail * detail
    gy, gx = np.gradient(hgt * strength * 10.0)
    nx, ny = -gx, gy  # image y grows down; normals use +Y up
    nz = np.ones_like(nx)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz)
    n = np.dstack([nx / ln, ny / ln, nz / ln]) * 0.5 + 0.5
    arr = np.dstack([n, np.clip(alpha, 0, 1)])
    return Image.fromarray((arr * 255 + 0.5).astype(np.uint8), "RGBA")
