#!/usr/bin/env python3
"""Skywalker brand SVG generator. Pure Python, no deps. Used by build.sh / build.py.

Everything (icon, glyph, wordmark) is hand-built geometry, no font outlines are embedded,
so the artwork has no third-party font licensing attached. Only the optional tagline is <text>.
"""
import math, random

# ---------- palette ----------
INK = "#1b1d2b"
NAVY_TOP, NAVY_MID, NAVY_BOT = "#222a68", "#131838", "#070919"
SKY, VIOLET, SUNSET, ROSE, MINT = "#4f8dff", "#9a86ff", "#ffb873", "#ed6b7a", "#54ccad"

# ---------- geometry ----------
def squircle(x, y, size, n=4.3, steps=360):
    a = size / 2.0
    cx, cy = x + a, y + a
    pts = []
    for i in range(steps):
        t = 2 * math.pi * i / steps
        c, s = math.cos(t), math.sin(t)
        px = cx + a * math.copysign(abs(c) ** (2 / n), c)
        py = cy + a * math.copysign(abs(s) ** (2 / n), s)
        pts.append(f"{px:.2f},{py:.2f}")
    return "M" + " L".join(pts) + " Z"

# Cloud body in local coords (bbox ~ x15..595, y20..400). Shapes carry no fill: <use fill=...> sets it.
CLOUD_SHAPES = """
    <rect x="30" y="190" width="540" height="210" rx="105"/>
    <circle cx="118" cy="292" r="100"/>
    <circle cx="238" cy="172" r="150"/>
    <circle cx="398" cy="212" r="116"/>
    <circle cx="488" cy="292" r="104"/>
"""
CLOUD_CX, CLOUD_CY = 305, 210  # visual centre of cloud in local coords

LENS = "M-72 -44 C-72 -52 -66 -54 -58 -54 L58 -54 C66 -54 72 -52 72 -44 L72 -8 C72 32 40 54 0 54 C-40 54 -72 32 -72 -8 Z"
LENS_L, LENS_R, LENS_Y = (-100), 100, 262   # lens centres relative to cloud centre x=305

def face_defs(p):
    return f"""
    <linearGradient id="{p}lens" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#2a3170"/><stop offset=".55" stop-color="#0e1130"/><stop offset="1" stop-color="#05060f"/>
    </linearGradient>
    <linearGradient id="{p}refl" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffb873" stop-opacity=".95"/><stop offset=".38" stop-color="#ed6b7a" stop-opacity=".75"/>
      <stop offset=".7" stop-color="#7a6cf0" stop-opacity=".35"/><stop offset="1" stop-color="#4f8dff" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="{p}frame" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#fff1cf"/><stop offset=".5" stop-color="#e2b062"/><stop offset="1" stop-color="#a8702f"/>
    </linearGradient>
    <clipPath id="{p}lensclip"><path d="{LENS}"/></clipPath>"""

def lens(p, cx, cy, flip=1):
    return f"""
    <g transform="translate({cx} {cy})">
      <path d="{LENS}" fill="url(#{p}lens)"/>
      <g clip-path="url(#{p}lensclip)">
        <path d="{LENS}" fill="url(#{p}refl)" opacity=".8"/>
        <path d="M-90 20 C-40 -4 40 -4 90 20 L90 80 L-90 80 Z" fill="#0b0d26" opacity=".38"/>
        <path d="M-52 -60 L-4 -60 L-44 60 L-92 60 Z" fill="#fff" opacity=".20"/>
        <path d="M10 -60 L24 -60 L-16 60 L-30 60 Z" fill="#fff" opacity=".12"/>
      </g>
      <path d="{LENS}" fill="none" stroke="url(#{p}frame)" stroke-width="7" stroke-linejoin="round"/>
    </g>"""

def face(p, detail=True):
    cx = CLOUD_CX
    out = f"""
    <path d="M{cx-28} {LENS_Y-34} Q{cx} {LENS_Y-52} {cx+28} {LENS_Y-34}" fill="none" stroke="url(#{p}frame)" stroke-width="7" stroke-linecap="round"/>
    <path d="M{cx+LENS_L-72} {LENS_Y-30} L{cx+LENS_L-92} {LENS_Y-24}" stroke="url(#{p}frame)" stroke-width="7" stroke-linecap="round"/>
    <path d="M{cx+LENS_R+72} {LENS_Y-30} L{cx+LENS_R+92} {LENS_Y-24}" stroke="url(#{p}frame)" stroke-width="7" stroke-linecap="round"/>
    {lens(p, cx+LENS_L, LENS_Y)}{lens(p, cx+LENS_R, LENS_Y)}"""
    if detail:
        out += f"""
    <path d="M{cx-30} 346 Q{cx} 374 {cx+30} 346" fill="none" stroke="{INK}" stroke-width="10" stroke-linecap="round"/>
    <circle cx="{cx-128}" cy="346" r="22" fill="{ROSE}" opacity=".42"/>
    <circle cx="{cx+128}" cy="346" r="22" fill="{ROSE}" opacity=".42"/>"""
    return out

def cloud_defs(p):
    return f"""
    <g id="{p}cs">{CLOUD_SHAPES}</g>
    <linearGradient id="{p}cg" gradientUnits="userSpaceOnUse" x1="300" y1="20" x2="300" y2="400">
      <stop offset="0" stop-color="#ffffff"/><stop offset=".55" stop-color="#e9eeff"/><stop offset="1" stop-color="#bfcaf7"/>
    </linearGradient>
    <linearGradient id="{p}warm" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="{SUNSET}" stop-opacity="0"/><stop offset="1" stop-color="{SUNSET}" stop-opacity=".4"/></linearGradient>
    <filter id="{p}b6" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="6"/></filter>
    <filter id="{p}b16" x="-50%" y="-300%" width="200%" height="700%"><feGaussianBlur stdDeviation="18"/></filter>
    <filter id="{p}b3" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="3"/></filter>
    <mask id="{p}shade" maskUnits="userSpaceOnUse" x="-50" y="-50" width="700" height="520">
      <use href="#{p}cs" fill="#fff"/>
      <use href="#{p}cs" fill="#000" transform="translate(0 -26)" filter="url(#{p}b16)"/>
    </mask>
    <mask id="{p}rim" maskUnits="userSpaceOnUse" x="-50" y="-50" width="700" height="520">
      <use href="#{p}cs" fill="#fff"/>
      <use href="#{p}cs" fill="#000" transform="translate(0 7)" filter="url(#{p}b3)"/>
    </mask>"""

def cloud(p, detail=True):
    return f"""
    <use href="#{p}cs" fill="url(#{p}cg)"/>
    <g mask="url(#{p}shade)"><rect x="-50" y="-50" width="700" height="520" fill="#6f7be6" opacity=".55"/>
       <rect x="-50" y="230" width="700" height="190" fill="url(#{p}warm)"/></g>
    <g mask="url(#{p}rim)"><rect x="-50" y="-50" width="700" height="520" fill="#fff" opacity=".95"/></g>
    {face(p, detail)}"""

# ---------- icon ----------
def icon_svg(margin=True, detail=True, stars=True, p="i_"):
    S = 1024
    T = 824 if margin else 1024
    off = (S - T) / 2 if margin else 0
    k = T / 824.0
    path = squircle(0, 0, 824)
    sc = 1.1
    ox, oy = 412 - CLOUD_CX * sc, 392 - CLOUD_CY * sc
    shy = oy + 400 * sc + 34
    random.seed(7)
    star_svg = ""
    if stars:
        for _ in range(34):
            x, y = random.uniform(30, 794), random.uniform(30, 520)
            if (x - 412) ** 2 + (y - 400) ** 2 < 300 ** 2 and random.random() < .8:
                continue
            r = random.choice([1.2, 1.6, 2.0, 2.6])
            star_svg += f'<circle cx="{x:.0f}" cy="{y:.0f}" r="{r}" fill="#dfe6ff" opacity="{random.uniform(.25,.8):.2f}"/>'
        def spark(cx, cy, r, col, op):
            return (f'<path d="M{cx} {cy-r} Q{cx+r*.12} {cy-r*.12} {cx+r} {cy} Q{cx+r*.12} {cy+r*.12} {cx} {cy+r} '
                    f'Q{cx-r*.12} {cy+r*.12} {cx-r} {cy} Q{cx-r*.12} {cy-r*.12} {cx} {cy-r} Z" fill="{col}" opacity="{op}"/>')
        star_svg += spark(668, 150, 30, SUNSET, 1) + spark(150, 214, 15, "#cfd8ff", .85) + spark(712, 208, 9, "#fff", .8)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {S} {S}" width="{S}" height="{S}">
  <title>Skywalker app icon</title>
  <defs>
    <path id="{p}sq" d="{path}"/>
    <clipPath id="{p}clip"><use href="#{p}sq"/></clipPath>
    <linearGradient id="{p}bg" x1="0.15" y1="0" x2="0.85" y2="1">
      <stop offset="0" stop-color="{NAVY_TOP}"/><stop offset=".5" stop-color="{NAVY_MID}"/><stop offset="1" stop-color="{NAVY_BOT}"/>
    </linearGradient>
    <radialGradient id="{p}glow" cx="412" cy="400" r="400" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#5b7cff" stop-opacity=".55"/><stop offset=".5" stop-color="#6a5cf0" stop-opacity=".18"/><stop offset="1" stop-color="#6a5cf0" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="{p}horizon" cx="412" cy="880" r="520" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="{SUNSET}" stop-opacity=".55"/><stop offset=".45" stop-color="{ROSE}" stop-opacity=".18"/><stop offset="1" stop-color="{ROSE}" stop-opacity="0"/>
    </radialGradient>
    <linearGradient id="{p}sheen" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#fff" stop-opacity=".08"/><stop offset=".35" stop-color="#fff" stop-opacity=".015"/><stop offset="1" stop-color="#fff" stop-opacity="0"/>
    </linearGradient>
    <linearGradient id="{p}edge" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#fff" stop-opacity=".5"/><stop offset=".3" stop-color="#fff" stop-opacity=".08"/><stop offset=".8" stop-color="#fff" stop-opacity="0"/><stop offset="1" stop-color="{SUNSET}" stop-opacity=".25"/>
    </linearGradient>
    <filter id="{p}drop" x="-20%" y="-20%" width="140%" height="150%"><feGaussianBlur stdDeviation="14"/></filter>
    <filter id="{p}halo" x="-30%" y="-40%" width="160%" height="180%"><feGaussianBlur stdDeviation="26"/></filter>
    {cloud_defs(p)}{face_defs(p)}
  </defs>
  <g transform="translate({off} {off}) scale({k:.5f})">
    {'<use href="#%ssq" fill="#000" opacity=".5" transform="translate(0 14)" filter="url(#%sdrop)"/>' % (p,p) if margin else ''}
    <g clip-path="url(#{p}clip)">
      <rect width="824" height="824" fill="url(#{p}bg)"/>
      <rect width="824" height="824" fill="url(#{p}horizon)"/>
      <rect width="824" height="824" fill="url(#{p}glow)"/>
      {star_svg}
      <ellipse cx="412" cy="{shy:.0f}" rx="230" ry="16" fill="#000" opacity=".5" filter="url(#{p}b16)"/>
      <g transform="translate({ox:.1f} {oy:.1f}) scale({sc})">
        <use href="#{p}cs" fill="#8fa6ff" opacity=".5" filter="url(#{p}halo)"/>
        {cloud(p, detail)}
      </g>
      <rect width="824" height="824" fill="url(#{p}sheen)"/>
    </g>
    <use href="#{p}sq" fill="none" stroke="url(#{p}edge)" stroke-width="3"/>
  </g>
</svg>
"""

# ---------- glyph (monochrome) ----------
def glyph_svg(color="#000", size=512, p="g_"):
    """Cloud with sunglasses (and smile) knocked out. Single colour, transparent elsewhere."""
    cx = CLOUD_CX
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 640 440" width="{size*640//440}" height="{size}">
  <title>Skywalker glyph</title>
  <defs>
    <g id="{p}cs">{CLOUD_SHAPES}</g>
    <mask id="{p}m" maskUnits="userSpaceOnUse" x="-20" y="0" width="660" height="440">
      <use href="#{p}cs" fill="#fff"/>
      <g fill="#000" stroke="#000" stroke-linejoin="round">
        <g transform="translate({cx+LENS_L} {LENS_Y})"><path d="{LENS}" stroke-width="0"/></g>
        <g transform="translate({cx+LENS_R} {LENS_Y})"><path d="{LENS}" stroke-width="0"/></g>
        <path d="M{cx-34} {LENS_Y-30} Q{cx} {LENS_Y-52} {cx+34} {LENS_Y-30}" fill="none" stroke-width="12" stroke-linecap="round"/>
        <path d="M{cx-30} 352 Q{cx} 380 {cx+30} 352" fill="none" stroke-width="12" stroke-linecap="round"/>
      </g>
      <g fill="none" stroke="#fff" stroke-width="0"></g>
    </mask>
  </defs>
  <g transform="translate(10 10)"><rect x="-20" y="-10" width="660" height="440" fill="{color}" mask="url(#{p}m)"/></g>
</svg>
"""

# ---------- wordmark (hand-drawn monoline geometry, no font) ----------
STROKE = 19
GLYPHS = [  # (name, minx, maxx, [path d...])
 ("S", 12, 80, ["M80 34 C74 18 60 10 45 10 C26 10 12 22 12 42 C12 62 30 70 46 80 C64 90 80 98 80 118 C80 138 66 150 46 150 C28 150 14 142 10 126"]),
 ("k", 12, 64, ["M12 10 V150", "M64 58 L12 108", "M30 90 L64 150"]),
 ("y", 8, 74, ["M10 58 L42 132", "M74 58 L34 160 Q26 180 8 182"]),
 ("w", 10, 100, ["M10 58 L32 150 L55 74 L78 150 L100 58"]),
 ("a", 0, 92, ["M92 58 V150", "M92 104 A46 46 0 1 0 0 104 A46 46 0 1 0 92 104"]),
 ("l", 6, 18, ["M12 10 V150"]),
 ("k", 12, 64, ["M12 10 V150", "M64 58 L12 108", "M30 90 L64 150"]),
 ("e", 0, 92, ["M0 104 H92 A46 46 0 1 0 79 137"]),
 ("r", 12, 56, ["M12 58 V150", "M12 104 C12 76 30 60 56 62"]),
]
GAP = 20
def wordmark_group(fill_attr):
    x = 0
    parts = []
    for name, mn, mx, ds in GLYPHS:
        dx = x - mn + STROKE / 2
        d = " ".join(ds)
        parts.append(f'<path transform="translate({dx:.1f} 0)" d="{d}"/>')
        x += (mx - mn) + STROKE + GAP
    width = x - GAP
    return width, f'<g fill="none" {fill_attr} stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round">{"".join(parts)}</g>'

WM_W, _ = wordmark_group("stroke=\"#000\"")
WM_H = 200  # incl descender; ascender top at -STROKE/2

def tagline(x, y, color, size=22):
    return (f'<text x="{x}" y="{y}" font-family="\'Inter\',\'SF Pro Text\',\'Avenir Next\',\'Helvetica Neue\',sans-serif" '
            f'font-size="{size}" font-weight="500" letter-spacing="0.6" fill="{color}">The game engine built for AI agents</text>')

def sparkle(cx, cy, r, col):
    return (f'<path d="M{cx} {cy-r} Q{cx+r*.12} {cy-r*.12} {cx+r} {cy} Q{cx+r*.12} {cy+r*.12} {cx} {cy+r} '
            f'Q{cx-r*.12} {cy+r*.12} {cx-r} {cy} Q{cx-r*.12} {cy-r*.12} {cx} {cy-r} Z" fill="{col}"/>')

def mark_inline(x, y, size, p):
    """The app icon (margin-less squircle) embedded as nested <svg>."""
    s = icon_svg(margin=False, p=p)
    inner = s.split("\n", 1)[1].rsplit("</svg>", 1)[0]
    return f'<svg x="{x}" y="{y}" width="{size}" height="{size}" viewBox="0 0 1024 1024">{inner}</svg>'

def horizontal(theme="dark", tag=True):
    dark = theme == "dark"
    bg = "#0c0e1e" if dark else "#f6f7fc"
    ink = "url(#wmg)" if dark else "#14172e"
    sub = "#9aa3c7" if dark else "#5a6080"
    wmgx = 800
    W, H = 1400, 420
    msize = 260
    mx, my = 90, (H - msize) / 2
    ws = 1.0
    scale = 1.0
    wx = mx + msize + 56
    wy = 150 if tag else 168
    wm_w, wm = wordmark_group(f'stroke="{ink}"')
    sc = 0.9
    out = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <title>Skywalker, horizontal lockup ({theme})</title>
  <defs>
    <linearGradient id="wmg" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{wmgx}" y2="0"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#cfd8ff"/></linearGradient>
  </defs>
  <rect width="{W}" height="{H}" fill="{bg}"/>
  {mark_inline(mx, my, msize, "h_")}
  <g transform="translate({wx} {85 if tag else 105}) scale({sc})">{wm}{sparkle(wm_w + 30, 22, 16, SUNSET)}</g>
  {tagline(wx + 8, 318, sub, 24) if tag else ''}
</svg>
"""
    return out

def stacked(theme="dark", tag=True):
    dark = theme == "dark"
    bg = "#0c0e1e" if dark else "#f6f7fc"
    ink = "url(#wmg)" if dark else "#14172e"
    sub = "#9aa3c7" if dark else "#5a6080"
    wmgx = 800
    W, H = 1000, 840
    msize = 360
    wm_w, wm = wordmark_group(f'stroke="{ink}"')
    sc = 0.9
    wx = (W - wm_w * sc) / 2
    tag_svg = ('<text x="%d" y="770" text-anchor="middle" font-family="Inter, SF Pro Text, Avenir Next, sans-serif" font-size="26" font-weight="500" letter-spacing="0.8" fill="%s">The game engine built for AI agents</text>' % (W//2, sub)) if tag else ''
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <title>Skywalker, stacked lockup ({theme})</title>
  <defs>
    <linearGradient id="wmg" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{wmgx}" y2="0"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#cfd8ff"/></linearGradient>
  </defs>
  <rect width="{W}" height="{H}" fill="{bg}"/>
  {mark_inline((W-msize)/2, 70, msize, "s_")}
  <g transform="translate({wx:.1f} 520) scale({sc})">{wm}{sparkle(wm_w + 30, 22, 16, SUNSET)}</g>
  {tag_svg}
</svg>
"""

def wordmark_only(color):
    wm_w, wm = wordmark_group(f'stroke="{color}"')
    pad = 20
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="{-pad} {-pad} {wm_w+2*pad+60:.0f} {WM_H+2*pad}" width="{(wm_w+2*pad+60):.0f}" height="{WM_H+2*pad}">
  <title>Skywalker wordmark</title>{wm}{sparkle(wm_w + 30, 22, 16, SUNSET)}</svg>
"""
