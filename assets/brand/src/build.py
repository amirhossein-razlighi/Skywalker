#!/usr/bin/env python3
"""Rebuild every brand asset. Requires: python3, rsvg-convert (brew install librsvg), iconutil, magick (optional, for .ico), Pillow (sheet)."""
import os, subprocess, shutil, sys
sys.path.insert(0, os.path.dirname(__file__))
import brand as B

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ICON, LOGO = f"{ROOT}/icon", f"{ROOT}/logo"
def w(path, s):
    open(path, "w").write(s)
def png(svg, out, width=None, height=None, bg=None):
    cmd = ["rsvg-convert", svg, "-o", out]
    if width: cmd += ["-w", str(width)]
    if height: cmd += ["-h", str(height)]
    if bg: cmd += ["-b", bg]
    subprocess.run(cmd, check=True)

# icon
w(f"{ICON}/app-icon.svg", B.icon_svg())
w(f"{ICON}/app-icon-flat.svg", B.icon_svg(margin=False))
# simplified small-size art (no stars/blush/smile) used for 16-64 px
w(f"{ICON}/favicon.svg", B.icon_svg(margin=False, detail=False, stars=False, p="f_"))
png(f"{ICON}/app-icon.svg", f"{ICON}/app-icon-1024.png", 1024)
png(f"{ICON}/app-icon-flat.svg", f"{ICON}/app-icon-flat-1024.png", 1024)
for s in (512, 256, 128, 64, 32, 16):
    png(f"{ICON}/app-icon.svg", f"{ICON}/app-icon-{s}.png", s)

# iconset -> icns
iset = f"{ICON}/AppIcon.iconset"
shutil.rmtree(iset, ignore_errors=True); os.makedirs(iset)
small = f"{ICON}/favicon.svg"
for base in (16, 32, 128, 256, 512):
    for r in (1, 2):
        px = base * r
        name = f"icon_{base}x{base}{'@2x' if r == 2 else ''}.png"
        # tiny sizes: use the simplified art on the macOS tile (via margin version of simplified)
        src = f"{ICON}/_simple.svg" if px <= 64 else f"{ICON}/app-icon.svg"
        if px <= 64: w(src, B.icon_svg(margin=True, detail=False, stars=False, p="m_"))
        png(src, f"{iset}/{name}", px)
if os.path.exists(f"{ICON}/_simple.svg"): os.remove(f"{ICON}/_simple.svg")
subprocess.run(["iconutil", "-c", "icns", iset, "-o", f"{ICON}/AppIcon.icns"], check=True)
shutil.copy(f"{ICON}/AppIcon.icns", f"{ROOT}/../../editor/Resources/AppIcon.icns")
shutil.copy(f"{ICON}/app-icon-1024.png", f"{ROOT}/app-icon.png")  # keep legacy path in sync

# favicon
for s in (16, 32, 48, 180, 512):
    png(f"{ICON}/favicon.svg", f"{ICON}/favicon-{s}.png", s)
if shutil.which("magick"):
    subprocess.run(["magick", f"{ICON}/favicon-16.png", f"{ICON}/favicon-32.png", f"{ICON}/favicon-48.png", f"{ICON}/favicon.ico"], check=True)

# glyph
w(f"{ICON}/glyph-black.svg", B.glyph_svg("#000"))
w(f"{ICON}/glyph-white.svg", B.glyph_svg("#fff"))
for s in (16, 18, 32, 36, 64, 512):
    png(f"{ICON}/glyph-black.svg", f"{ICON}/glyph-black-{s}.png", height=s)
png(f"{ICON}/glyph-white.svg", f"{ICON}/glyph-white-512.png", height=512)
# menu-bar template images (macOS "Template" convention): 18pt @1x/@2x
png(f"{ICON}/glyph-black.svg", f"{ICON}/MenuBarTemplate.png", height=18)
png(f"{ICON}/glyph-black.svg", f"{ICON}/MenuBarTemplate@2x.png", height=36)

# lockups
for theme in ("dark", "light"):
    w(f"{LOGO}/horizontal-{theme}.svg", B.horizontal(theme))
    w(f"{LOGO}/horizontal-{theme}-notag.svg", B.horizontal(theme, tag=False))
    w(f"{LOGO}/stacked-{theme}.svg", B.stacked(theme))
    png(f"{LOGO}/horizontal-{theme}.svg", f"{LOGO}/horizontal-{theme}.png", 2560)
    png(f"{LOGO}/horizontal-{theme}-notag.svg", f"{LOGO}/horizontal-{theme}-notag.png", 2560)
    png(f"{LOGO}/stacked-{theme}.svg", f"{LOGO}/stacked-{theme}.png", 1800)
w(f"{LOGO}/wordmark-dark.svg", B.wordmark_only("#14172e"))
w(f"{LOGO}/wordmark-light.svg", B.wordmark_only("#f2f4ff"))
png(f"{LOGO}/wordmark-dark.svg", f"{LOGO}/wordmark-dark.png", 1600)
png(f"{LOGO}/wordmark-light.svg", f"{LOGO}/wordmark-light.png", 1600)
# keep legacy logo.svg path pointing at the new horizontal lockup
shutil.copy(f"{LOGO}/horizontal-light.svg", f"{ROOT}/logo.svg")
print("ok")

# ---- contact sheet ----
from PIL import Image, ImageDraw, ImageFont
def load(p, h=None, w=None):
    im = Image.open(p).convert("RGBA")
    if h: im = im.resize((int(im.width*h/im.height), h), Image.LANCZOS)
    if w: im = im.resize((w, int(im.height*w/im.width)), Image.LANCZOS)
    return im
SHEET_W, SHEET_H = 2400, 2000
sheet = Image.new("RGBA", (SHEET_W, SHEET_H), (22, 23, 28, 255))
d = ImageDraw.Draw(sheet)
try: F = ImageFont.truetype("/System/Library/Fonts/HelveticaNeue.ttc", 26)
except Exception: F = ImageFont.load_default()
def label(x, y, t): d.text((x, y), t, font=F, fill=(155, 157, 166, 255))
def panel(x, y, w, h, col):
    d.rounded_rectangle((x, y, x+w, y+h), 24, fill=col)
# row 1: app icon big + sizes + favicon
label(60, 30, "App icon  1024 / 512 / 256 / 128 / 64 / 32 / 16")
big = load(f"{ICON}/app-icon-1024.png", h=720); sheet.alpha_composite(big, (40, 70))
x = 800
for s in (256, 128, 64, 32, 16):
    im = load(f"{ICON}/app-icon-{s}.png"); sheet.alpha_composite(im, (x, 120)); x += s + 40
label(800, 420, "Light surface:")
panel(800, 460, 760, 300, (246, 247, 252, 255))
x = 830
for s in (256, 128, 64, 32, 16):
    im = load(f"{ICON}/app-icon-{s}.png"); sheet.alpha_composite(im, (x, 480)); x += s + 20
label(1620, 30, "Favicon (simplified)  180 / 48 / 32 / 16")
x = 1620
for s in (180, 48, 32, 16):
    im = load(f"{ICON}/favicon-{s}.png"); sheet.alpha_composite(im, (x, 100)); x += s + 30
label(1620, 330, "Glyph (template)  black on light / white on dark")
panel(1620, 370, 360, 190, (246, 247, 252, 255)); panel(1990, 370, 360, 190, (12, 14, 30, 255))
g = load(f"{ICON}/glyph-black-512.png", h=130); sheet.alpha_composite(g, (1620+(360-g.width)//2, 400))
gw = load(f"{ICON}/glyph-white-512.png", h=130); sheet.alpha_composite(gw, (1990+(360-gw.width)//2, 400))
x = 1650
for s in (64, 36, 18):
    im = load(f"{ICON}/glyph-black-{s if s!=64 else 64}.png"); sheet.alpha_composite(im, (x, 585)); x += im.width + 30
x = 2020
for s in (64, 36, 18):
    i = load(f"{ICON}/glyph-white-512.png", h=s); sheet.alpha_composite(i, (x, 585)); x += i.width + 30
# row 2: horizontal lockups
label(60, 800, "Horizontal lockup, dark / light")
a = load(f"{LOGO}/horizontal-dark.png", w=1150); sheet.alpha_composite(a, (40, 840))
b = load(f"{LOGO}/horizontal-light.png", w=1150); sheet.alpha_composite(b, (1210, 840))
a = load(f"{LOGO}/horizontal-dark-notag.png", w=1150); sheet.alpha_composite(a, (40, 840+a.height+30))
b = load(f"{LOGO}/horizontal-light-notag.png", w=1150); sheet.alpha_composite(b, (1210, 840+a.height+30))
# row 3: stacked
y3 = 840 + 2*(a.height+30) + 20
label(60, y3-40, "Stacked lockup, dark / light")
h3 = SHEET_H - y3 - 20
s1 = load(f"{LOGO}/stacked-dark.png", h=h3); sheet.alpha_composite(s1, (40, y3))
s2 = load(f"{LOGO}/stacked-light.png", h=h3); sheet.alpha_composite(s2, (40+s1.width+30, y3))
sheet.convert("RGB").save(f"{ROOT}/brand-sheet.png")
print("sheet")
