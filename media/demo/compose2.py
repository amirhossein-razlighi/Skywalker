#!/usr/bin/env python3
"""Composites the Skywalker showcase video: crew-built games (engine footage), the live
build timelapses recorded from the editor, feature footage, motion graphics, Kokoro
voiceover and procedural music  ->  1920x1080 H.264 MP4.

  compose2.py WORK_DIR OUT.mp4 [--preview]
WORK_DIR: footage/<game>/<shot>/, rec/<game>/ (frames, events.jsonl, edits.jsonl),
features/, previews/, assets/ (avatars), vo/ (narration2.json ids)
"""
import json
import math
import os
import subprocess
import sys
from functools import lru_cache

import numpy as np
import soundfile as sf
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WORK, OUT = sys.argv[1], sys.argv[2]
W, H, FPS = 1920, 1080, 30
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FOOT = os.path.join(WORK, "footage")
FEAT = os.path.join(WORK, "features")
ASSETS = os.path.join(WORK, "assets")
BRAND = os.path.join(ROOT, "assets", "brand")

INK = (236, 238, 244)
DIM = (165, 170, 184)
FAINT = (110, 114, 126)
ACCENT = (79, 141, 255)
GOLD = (255, 196, 92)
CREW = {
    "Nimbus": ((139, 115, 250), "nimbus", "Creative Director"),
    "Cirro": ((92, 143, 237), "cirro", "Level Designer"),
    "Aurora": ((255, 184, 115), "aurora", "Lighting Artist"),
    "Stratus": ((84, 204, 173), "stratus", "Gameplay Programmer"),
    "Pixel": ((232, 107, 214), "pixel", "Asset Artist"),
}
GAMES = ["hollow_manor", "abyss", "hearthside", "harvest_fair", "neon_drift", "star_lancer", "cloudhopper",
         "toy_kart_rally", "zen_garden", "cyber_alley", "frostlight", "sky_dash"]
META = {g: json.load(open(os.path.join(ROOT, "examples", g, "game.json"))) for g in GAMES}


@lru_cache(maxsize=None)
def font(size, weight="Regular", mono=False):
    if mono:
        return ImageFont.truetype("/System/Library/Fonts/SFNSMono.ttf", size)
    f = ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", size)
    f.set_variation_by_name(weight)
    return f


@lru_cache(maxsize=512)
def load(path, size=None):
    im = Image.open(path).convert("RGB")
    if size and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    return im


@lru_cache(maxsize=64)
def rgba(path, h=None, w=None):
    im = Image.open(path).convert("RGBA")
    if h:
        im = im.resize((int(im.width * h / im.height), h), Image.LANCZOS)
    elif w:
        im = im.resize((w, int(im.height * w / im.width)), Image.LANCZOS)
    return im


def avatar(key, h):
    return rgba(os.path.join(ASSETS, f"{key}.png"), h=h)


def ease(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def ease_out(x):
    x = max(0.0, min(1.0, x))
    return 1 - (1 - x) ** 3


@lru_cache(maxsize=None)
def frames_in(d):
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".png"))


def shot(game, name, t, speed=1.0):
    fs = frames_in(os.path.join(FOOT, game, name))
    return load(fs[min(len(fs) - 1, int(t * FPS * speed))], (W, H)).copy()


def feat(name, t, speed=1.0):
    fs = frames_in(os.path.join(FEAT, name))
    return load(fs[min(len(fs) - 1, max(0, int(t * FPS * speed)))], (W, H)).copy()


def text(d, xy, s, size, weight="Regular", fill=INK, anchor="la", mono=False, alpha=1.0):
    c = tuple(int(v) for v in fill) + (int(255 * max(0, min(1, alpha))),)
    d.text(xy, s, font=font(size, weight, mono), fill=c, anchor=anchor)


def fade_alpha(im, a):
    im = im.copy()
    im.putalpha(im.split()[-1].point(lambda v: int(v * a)))
    return im


def darken(im, k):
    return Image.blend(im, Image.new("RGB", im.size, (8, 9, 14)), k)


def rounded_panel(size, radius=18, fill=(18, 19, 25, 232), outline=(64, 66, 78, 255)):
    p = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(p).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius, fill=fill, outline=outline, width=2)
    return p


def shadowed(base, overlay, xy, blur=24, opacity=120):
    sh = Image.new("RGBA", (overlay.width + blur * 4, overlay.height + blur * 4), (0, 0, 0, 0))
    mask = overlay.split()[-1].point(lambda a: opacity if a > 0 else 0)
    sh.paste((0, 0, 0, 255), (blur * 2, blur * 2), mask)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(sh, (max(0, xy[0] - blur * 2), max(0, xy[1] - blur * 2 + 10))) if xy[0] >= blur * 2 and xy[1] >= blur * 2 else None
    base.alpha_composite(overlay, xy)


def game_card(im, game, t, dur):
    """Lower-left title card: game title, genre, and the crew members who built it."""
    m = META[game]
    a = ease_out((t - 0.15) / 0.45) * (1 - ease((t - (dur - 0.35)) / 0.35))
    if a <= 0:
        return im
    im = im.convert("RGBA")
    grad = Image.new("L", (1, 360))
    for y in range(360):
        grad.putpixel((0, y), int(200 * (y / 360) ** 1.4 * a))
    im.paste((6, 7, 12), (0, H - 360), grad.resize((W, 360)))
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    x, y = 72, H - 210 + int((1 - a) * 18)
    text(d, (x, y), m["title"], 66, "Bold", (255, 255, 255), alpha=a)
    tw = d.textlength(m["title"], font=font(66, "Bold"))
    gx = x + tw + 26
    d.rounded_rectangle([gx, y + 18, gx + d.textlength(m["genre"], font=font(24, "Semibold")) + 30, y + 58], 20,
                        fill=(255, 255, 255, int(36 * a)), outline=(255, 255, 255, int(90 * a)), width=2)
    text(d, (gx + 15, y + 25), m["genre"], 24, "Semibold", (240, 240, 248), alpha=a)
    text(d, (x, y + 88), m["pitch"], 30, "Regular", (225, 228, 238), alpha=a * 0.95)
    im.alpha_composite(ov)
    return im.convert("RGB")


def corner_tag(im, label, t, color=ACCENT, live=False):
    a = ease(t / 0.4)
    im = im.convert("RGBA")
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    f = font(24, "Semibold")
    tw = d.textlength(label, font=f)
    d.rounded_rectangle([56, 48, 56 + tw + 74, 100], 26, fill=(12, 13, 18, int(205 * a)))
    d.ellipse([74, 66, 90, 82], fill=((255, 92, 92) if live else color) + (int(255 * a),))
    d.text((102, 60), label, font=f, fill=INK + (int(255 * a),))
    im.alpha_composite(ov)
    return im.convert("RGB")


def sequence(parts, t, dissolve=0.35):
    """parts: [(dur, fn(local_t, dur) -> Image)], played back to back with dissolves."""
    start = 0.0
    for k, (dur, fn) in enumerate(parts):
        if t < start + dur or k == len(parts) - 1:
            lt = t - start
            img = fn(lt, dur)
            if k > 0 and lt < dissolve:
                pdur, pfn = parts[k - 1]
                img = Image.blend(pfn(pdur + lt, pdur), img, ease(lt / dissolve))
            return img
        start += dur
    return parts[-1][1](t, parts[-1][0])


def game_shot(game, name, speed=1.0, offset=0.0):
    return lambda lt, dur: game_card(shot(game, name, offset + lt, speed), game, lt, dur)


# ---------------------------------------------------------------------------------------
# Recording data (live editor builds)
# ---------------------------------------------------------------------------------------
REC = {}
for g in GAMES:
    d = os.path.join(WORK, "rec", g)
    frames = frames_in(os.path.join(d, "frames"))
    stamps = [json.loads(l) for l in open(os.path.join(d, "events.jsonl"))]
    edits = [json.loads(l) for l in open(os.path.join(d, "edits.jsonl"))]
    start = next((e["ts"] for e in edits if e["kind"] == "scene_new"), edits[0]["ts"])
    first = next((i for i, s in enumerate(stamps) if s["ts"] >= start + 0.4), 0)
    REC[g] = dict(frames=frames[first:], stamps=stamps[first:], edits=[e for e in edits if e["ts"] >= start])


def rec_frame(g, p, size):
    r = REC[g]
    i = min(len(r["frames"]) - 1, int(p * (len(r["frames"]) - 1)))
    return load(r["frames"][i], size), r["stamps"][min(i, len(r["stamps"]) - 1)]["ts"]


# ---------------------------------------------------------------------------------------
# Segments
# ---------------------------------------------------------------------------------------
def seg_intro(t, dur):
    im = darken(shot("harvest_fair", "aerial", t * 0.8), 0.35).convert("RGBA")
    a = ease_out((t - 0.5) / 0.9) * (1 - ease((t - (dur - 0.7)) / 0.6))
    if a > 0:
        icon = rgba(os.path.join(BRAND, "icon", "app-icon-1024.png"), h=300)
        s = 0.88 + 0.12 * ease_out((t - 0.5) / 1.0)
        ic = icon.resize((int(icon.width * s), int(icon.height * s)), Image.LANCZOS)
        im.alpha_composite(fade_alpha(ic, a), (W // 2 - ic.width // 2, 230 + (300 - ic.height) // 2))
        wm = rgba(os.path.join(BRAND, "logo", "wordmark-light.png"), h=120)
        im.alpha_composite(fade_alpha(wm, ease_out((t - 1.0) / 0.8) * a), (W // 2 - wm.width // 2, 600))
        d = ImageDraw.Draw(im)
        text(d, (W // 2, 790), "The game engine built for AI agents", 40, "Medium", (240, 236, 255), "mm",
             alpha=ease((t - 1.6) / 0.7) * a)
    return im.convert("RGB")


HEROES = [("hollow_manor", "establish"), ("abyss", "angler"), ("hearthside", "fireside"), ("harvest_fair", "wheel"),
          ("neon_drift", "low"), ("star_lancer", "flyby"), ("cloudhopper", "aerial"), ("toy_kart_rally", "stadium"),
          ("zen_garden", "torii"), ("cyber_alley", "crane"), ("frostlight", "sky"), ("sky_dash", "play")]


def seg_challenge(t, dur):
    step = (dur - 0.6) / len(HEROES)
    k = min(len(HEROES) - 1, int(t / step))
    g, s = HEROES[k]
    im = shot(g, s, 1.0 + (t - k * step)).convert("RGBA")
    d = ImageDraw.Draw(im)
    a = ease(t / 0.4)
    title = META[g]["title"]
    tw = d.textlength(title, font=font(38, "Bold"))
    d.rounded_rectangle([W - 112 - tw, 48, W - 56, 150], 26, fill=(12, 13, 18, int(210 * a)))
    text(d, (W - 84 - tw, 60), META[g]["genre"], 22, "Medium", DIM, alpha=a)
    text(d, (W - 84 - tw, 90), title, 38, "Bold", INK, alpha=a)
    d.rounded_rectangle([56, 48, 640, 100], 26, fill=(12, 13, 18, int(205 * a)))
    text(d, (84, 60), "Built by AI agents · live in the editor", 26, "Semibold", INK, alpha=a)
    return im.convert("RGB")


ALL_EDITS = sorted(((g, e) for g in GAMES for e in REC[g]["edits"]), key=lambda x: x[1]["ts"])


def seg_crew(t, dur):
    """Every live build at once (4x3 grid), with a crew activity ticker."""
    p = min(1.0, max(0.0, 0.12 + 0.88 * (t - 0.3) / (dur - 1.4)))
    im = Image.new("RGBA", (W, H), (9, 10, 14, 255))
    tw, th, gap = 440, 248, 14
    x0 = (W - (4 * tw + 3 * gap)) // 2
    y0 = 128
    done = 0
    shown = []
    for k, g in enumerate(GAMES):
        fr, ts = rec_frame(g, p, (tw, th))
        x, y = x0 + (k % 4) * (tw + gap), y0 + (k // 4) * (th + gap)
        tile = fr.convert("RGBA")
        mask = Image.new("L", (tw, th), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, tw - 1, th - 1], 14, fill=255)
        im.paste(tile, (x, y), mask)
        d = ImageDraw.Draw(im)
        d.rounded_rectangle([x + 10, y + 10, x + 22 + d.textlength(META[g]["title"], font=font(18, "Semibold")), y + 40], 14,
                            fill=(10, 11, 15, 190))
        text(d, (x + 16, y + 14), META[g]["title"], 18, "Semibold", INK)
        es = [e for e in REC[g]["edits"] if e["ts"] <= ts]
        done += len(es)
        shown += [(e["ts"] - REC[g]["edits"][0]["ts"], g, e) for e in es[-2:]]
    d = ImageDraw.Draw(im)
    a = ease(t / 0.4)
    text(d, (x0, 56), "Live in the editor", 40, "Bold", INK, alpha=a)
    text(d, (x0 + 360, 66), "5 agents · every edit attributed and undoable", 26, "Medium", DIM, alpha=a)
    text(d, (W - x0, 52), f"{done}", 52, "Bold", INK, "ra", mono=True, alpha=a)
    text(d, (W - x0, 108), "edits", 20, "Medium", DIM, "ra", alpha=a)
    # Ticker: latest edits across all builds
    shown.sort(key=lambda s: s[0])
    for k, (_, g, e) in enumerate(shown[-4:][::-1]):
        who = e["actor"]
        color, key, role = CREW.get(who, ((150, 150, 150), "nimbus", "agent"))
        y = H - 118
        x = x0 + k * 455
        al = 1.0 - k * 0.18
        d.rounded_rectangle([x, y, x + 440, y + 70], 18, fill=(18, 19, 25, int(230 * al)))
        im.alpha_composite(fade_alpha(avatar(key, 44), al), (x + 14, y + 13))
        text(d, (x + 70, y + 10), f"{who} · {META[g]['title']}", 20, "Semibold", color, alpha=al)
        text(d, (x + 70, y + 37), e["label"][:34], 21, "Regular", INK, alpha=al)
    return im.convert("RGB")


def seg_horror(t, dur):
    q = dur / 4
    return sequence([(q, game_shot("hollow_manor", "establish")), (q, game_shot("hollow_manor", "graveyard")),
                     (q, game_shot("abyss", "descent")), (q, game_shot("abyss", "angler"))], t)


def seg_cozy(t, dur):
    q = dur / 4
    return sequence([(q, game_shot("hearthside", "fireside")), (q, game_shot("harvest_fair", "wheel")),
                     (q, game_shot("harvest_fair", "carousel")), (q, game_shot("frostlight", "sky"))], t)


def code_chip(im, lines, t, xy=(64, 140), title="Wander"):
    a = ease_out((t - 0.5) / 0.5)
    if a <= 0:
        return im
    im = im.convert("RGBA")
    w = 760
    panel = rounded_panel((w, 70 + 34 * len(lines)), 20, fill=(12, 13, 18, 225))
    pd = ImageDraw.Draw(panel)
    text(pd, (24, 18), title, 18, "Semibold", DIM)
    for k, l in enumerate(lines):
        text(pd, (24, 50 + k * 34), l, 22, "Regular", (170, 230, 190) if k else (199, 146, 234), mono=True)
    im.alpha_composite(fade_alpha(panel, a), xy)
    return im.convert("RGB")


def seg_action(t, dur):
    q = dur / 4

    def neon_play(lt, d):
        im = game_card(shot("neon_drift", "play", lt), "neon_drift", lt, d)
        return code_chip(im, ["every 0.11 seconds", '  let b = spawn("prefab:prefabs/bolt.prefab.json", ...)', "  b.rotation = self.rotation",
                              "end"], lt)

    return sequence([(q, game_shot("neon_drift", "low")), (q, neon_play), (q, game_shot("star_lancer", "flyby")),
                     (q, game_shot("star_lancer", "play", offset=1.0))], t)


def seg_play(t, dur):
    q = dur / 3
    return sequence([(q, game_shot("cloudhopper", "play", offset=1.5)), (q, game_shot("sky_dash", "play", offset=2.0)),
                     (q, game_shot("toy_kart_rally", "play", offset=1.0))], t)


def seg_art(t, dur):
    q = dur / 4
    return sequence([(q, game_shot("zen_garden", "torii")), (q, game_shot("zen_garden", "blossom")),
                     (q, game_shot("cyber_alley", "crane")), (q, game_shot("cyber_alley", "detective"))], t)


FEATURE_LIST = ["Physically based materials", "Normal · ORM · emissive maps", "Image-based lighting", "4-cascade soft shadows",
                "Ambient occlusion", "Clearcoat · subsurface", "Atmospheric sky · clouds · stars", "AgX / ACES tonemapping"]


def seg_render(t, dur):
    a_end = dur * 0.42
    w_end = dur * 0.70

    def lookdev(lt, d):
        im = feat("lookdev", lt * 0.95).convert("RGBA")
        dd = ImageDraw.Draw(im)
        panel_a = ease(lt / 0.5)
        dd.rounded_rectangle([W - 560, 120, W - 60, 140 + 50 * len(FEATURE_LIST)], 22, fill=(12, 13, 18, int(200 * panel_a)))
        for k, f in enumerate(FEATURE_LIST):
            ak = ease_out((lt - 0.3 - k * 0.32) / 0.4)
            dd.ellipse([W - 530, 146 + k * 50, W - 516, 160 + k * 50], fill=GOLD + (int(255 * ak),))
            text(dd, (W - 500, 138 + k * 50), f, 26, "Medium", INK, alpha=ak)
        return corner_tag(im.convert("RGB"), "New renderer · PBR", lt, GOLD)

    def wipe(lt, d):
        toon = feat("toon", lt)
        pbr = feat("pbr", lt)
        x = int(W * (0.15 + 0.7 * ease(lt / d)))
        im = pbr.copy()
        im.paste(toon.crop((0, 0, x, H)), (0, 0))
        dd = ImageDraw.Draw(im)
        dd.rectangle([x - 2, 0, x + 2, H], fill=(255, 255, 255))
        text(dd, (x - 30, H - 90), "toon + outlines", 34, "Bold", (255, 255, 255), "ra")
        text(dd, (x + 30, H - 90), "physically based", 34, "Bold", (255, 255, 255), "la")
        return im

    def tod(lt, d):
        im = feat("timeofday", lt * (210 / FPS) / d)
        return corner_tag(im, "environment_update · skyMode: atmosphere", lt, GOLD)

    return sequence([(a_end, lookdev), (w_end - a_end, wipe), (dur - w_end, tod)], t)


SWATCHES = ["bricks", "planks", "cobblestone", "marble", "wood", "rust", "metal_brushed", "tiles", "rock", "sand", "grass",
            "fabric", "scales", "hexagons", "stylized", "dirt"]


def seg_textures(t, dur):
    bg = darken(feat("lookdev", 2.0 + t * 0.3).filter(ImageFilter.GaussianBlur(16)), 0.55).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a = ease(t / 0.4)
    text(d, (W // 2, 96), "texture_generate", 52, "Bold", INK, "mm", mono=True, alpha=a)
    text(d, (W // 2, 156), "seamless albedo · normal · ORM — and a ready material", 28, "Medium", DIM, "mm", alpha=a)
    s, g = 196, 22
    x0 = (W - (8 * s + 7 * g)) // 2
    for k, name in enumerate(SWATCHES):
        ak = ease_out((t - 0.4 - k * 0.12) / 0.4)
        if ak <= 0:
            continue
        sw = load(os.path.join(FEAT, "swatches", f"{name}.png"), (s, s)).convert("RGBA")
        mask = Image.new("L", (s, s), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], 16, fill=int(255 * ak))
        x, y = x0 + (k % 8) * (s + g), 270 + (k // 8) * (s + 80) + int((1 - ak) * 20)
        bg.paste(sw, (x, y), mask)
        text(d, (x + s // 2, y + s + 30), name.replace("_", " "), 22, "Medium", INK, "mm", alpha=ak)
    return bg.convert("RGB")


PREVIEWS = ["hollow_manor/dead_tree", "hollow_manor/gravestone", "abyss/kelp", "abyss/rock", "harvest_fair/stall",
            "harvest_fair/autumn_tree", "harvest_fair/pumpkin", "cloudhopper/mushroom", "cloudhopper/round_tree",
            "zen_garden/bamboo", "frostlight/snowy_pine", "neon_drift/drone", "toy_kart_rally/firework_gold",
            "star_lancer/raider"]


def seg_assets(t, dur):
    im = Image.new("RGBA", (W, H), (10, 11, 15, 255))
    d = ImageDraw.Draw(im)
    a = ease(t / 0.4)
    text(d, (96, 92), "Assets agents can use", 52, "Bold", INK, alpha=a)
    text(d, (96, 156), "asset_list · asset_preview · prefabs · materials · scatter", 28, "Medium", DIM, mono=True, alpha=a)
    s, g = 172, 18
    for k, name in enumerate(PREVIEWS):
        ak = ease_out((t - 0.3 - k * 0.1) / 0.4)
        if ak <= 0:
            continue
        p = os.path.join(WORK, "previews", name.replace("/", "_") + ".png")
        if not os.path.exists(p):
            continue
        tile = load(p, (s, s)).convert("RGBA")
        mask = Image.new("L", (s, s), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], 14, fill=int(255 * ak))
        x, y = 96 + (k % 5) * (s + g), 230 + (k // 5) * (s + 56)
        im.paste(tile, (x, y), mask)
        text(d, (x + s // 2, y + s + 24), name.split("/")[1].replace("_", " "), 19, "Medium", DIM, "mm", alpha=ak)
    # Right: four-view capture + formats
    mv = load(os.path.join(FEAT, "multiview.png"))
    ma = ease_out((t - 1.6) / 0.6)
    if ma > 0:
        mvs = mv.resize((780, int(780 * mv.height / mv.width)), Image.LANCZOS).convert("RGBA")
        mask = Image.new("L", mvs.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, mvs.width - 1, mvs.height - 1], 16, fill=int(255 * ma))
        im.paste(mvs, (1060, 200), mask)
        text(d, (1060, 200 + mvs.height + 20), "viewport_multi — perspective · top · front · side", 22, "Medium", DIM, mono=True, alpha=ma)
    fa = ease_out((t - 3.2) / 0.5)
    x = 1060
    for k, fmt in enumerate(["glTF / GLB", "OBJ + MTL", "PLY · vertex colors", "STL"]):
        f = font(24, "Semibold")
        w = d.textlength(fmt, font=f) + 40
        d.rounded_rectangle([x, 900, x + w, 950], 24, fill=(79, 141, 255, int(55 * fa)), outline=(79, 141, 255, int(200 * fa)), width=2)
        text(d, (x + 20, 911), fmt, 24, "Semibold", INK, alpha=fa)
        x += w + 14
    return im.convert("RGB")


def seg_download(t, dur):
    bg = darken(shot("frostlight", "camp", t * 0.7).filter(ImageFilter.GaussianBlur(14)), 0.6).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a = ease(t / 0.4)
    text(d, (W // 2, 110), "asset_download", 54, "Bold", INK, "mm", mono=True, alpha=a)
    text(d, (W // 2, 170), "openly licensed models from the web · your approval · credits tracked", 28, "Medium", DIM, "mm", alpha=a)
    # Approval card (as in the editor's Agents panel)
    ca = ease_out((t - 0.6) / 0.5)
    card = rounded_panel((900, 330), 22, fill=(40, 30, 14, 235), outline=(255, 190, 90, 200))
    cd = ImageDraw.Draw(card)
    card.alpha_composite(avatar("pixel", 56), (28, 26))
    text(cd, (100, 30), "Pixel wants to run asset_download", 30, "Bold", (255, 214, 140))
    text(cd, (100, 70), "Network permission · asks every time", 20, "Medium", (220, 190, 140))
    lines = ['url:     "https://…/lantern.glb"', 'license: "CC0-1.0"', 'author:  "…"', 'import:  true   create_entity: "Lantern"']
    for k, l in enumerate(lines):
        text(cd, (40, 122 + k * 36), l, 23, "Regular", INK, mono=True)
    allow = ease((t - 2.4) / 0.3)
    cd.rounded_rectangle([40, 268, 170, 312], 12, fill=(79, 141, 255, 255) if allow < 0.5 else (60, 200, 120, 255))
    text(cd, (105, 290), "Allow" if allow < 0.5 else "Allowed", 22, "Semibold", (255, 255, 255), "mm")
    cd.rounded_rectangle([186, 268, 316, 312], 12, outline=(200, 200, 210, 255), width=2)
    text(cd, (251, 290), "Decline", 22, "Semibold", INK, "mm")
    bg.alpha_composite(fade_alpha(card, ca), (W // 2 - 450, 250))
    # CREDITS.md
    ra = ease_out((t - 3.0) / 0.5)
    if ra > 0:
        cr = rounded_panel((1100, 200), 20, fill=(12, 13, 18, 230))
        rd = ImageDraw.Draw(cr)
        text(rd, (30, 24), "CREDITS.md", 22, "Semibold", DIM, mono=True)
        text(rd, (30, 70), "# Credits", 24, "Bold", INK, mono=True)
        text(rd, (30, 112), "- **lantern** by … — CC0-1.0 — https://… (`downloads/lantern`)", 21, "Regular", (170, 230, 190), mono=True)
        text(rd, (30, 146), "  license, author, URL and date also stored in each file's .meta", 20, "Regular", DIM, mono=True)
        bg.alpha_composite(fade_alpha(cr, ra), (W // 2 - 550, 640))
    fa = ease((t - 4.0) / 0.5)
    text(d, (W // 2, 900), "CC0 · CC-BY · MIT  ✓      unknown · all rights reserved  ✗", 28, "Semibold", INK, "mm", alpha=fa)
    return bg.convert("RGB")


def seg_agents(t, dur):
    bg = darken(shot("zen_garden", "pond", t * 0.6).filter(ImageFilter.GaussianBlur(14)), 0.6).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a = ease(t / 0.4)
    text(d, (W // 2, 96), "Design your crew", 60, "Bold", INK, "mm", alpha=a)
    names = ["Nimbus", "Cirro", "Aurora", "Stratus", "Pixel"]
    for k, n in enumerate(names):
        ak = ease_out((t - 0.3 - k * 0.18) / 0.5)
        color, key, role = CREW[n]
        x = 150 + k * 330
        card = rounded_panel((300, 250), 22, fill=(20, 21, 27, 235), outline=color + (180,))
        cdd = ImageDraw.Draw(card)
        av = avatar(key, 100)
        card.alpha_composite(av, ((300 - av.width) // 2, 26))
        text(cdd, (150, 160), n, 34, "Bold", INK, "mm")
        text(cdd, (150, 200), role, 22, "Medium", color, "mm")
        bg.alpha_composite(fade_alpha(card, ak), (x, 170 + int((1 - ak) * 24)))
    # Designer fields (the Agent Designer's sections)
    pa = ease_out((t - 1.8) / 0.5)
    panel = rounded_panel((1620, 360), 22, fill=(16, 17, 22, 238))
    pd = ImageDraw.Draw(panel)
    cols = [("Mission & instructions", ["Build spaces that read", "well from the game camera.", "Style: cozy, no clutter."]),
            ("Permissions", ["Entities  Allow", "Assets    Allow", "Network   Ask", "Files     Off"]),
            ("Memory", ["1. Project uses metric units", "2. Human prefers warm palettes"]),
            ("Run", ["Alone · delegated by the director", "Pipeline stages, in parallel"])]
    for k, (title, lines) in enumerate(cols):
        x = 36 + k * 400
        text(pd, (x, 30), title, 24, "Bold", GOLD)
        for j, l in enumerate(lines):
            text(pd, (x, 80 + j * 42), l, 22, "Regular", INK, mono=(k == 1))
    bg.alpha_composite(fade_alpha(panel, pa), (150, 470))
    text(d, (W // 2, 900), "Claude · GPT · DeepSeek · Ollama — per agent", 30, "Semibold", DIM, "mm", alpha=pa)
    return bg.convert("RGB")


def seg_connect(t, dur):
    im = Image.new("RGB", (W, H), (11, 12, 16))
    grad = Image.radial_gradient("L").resize((W * 2, H * 2)).crop((W // 2, H // 2 - 200, W // 2 + W, H // 2 + H - 200))
    im.paste((26, 30, 58), (0, 0), Image.eval(grad, lambda v: 255 - v))
    im = im.convert("RGBA")
    d = ImageDraw.Draw(im)
    a0 = ease(t / 0.5)
    text(d, (W // 2, 140), "Bring any agent", 70, "Bold", INK, "mm", alpha=a0)
    sources = ["Claude", "GPT", "DeepSeek", "Ollama · local models", "Claude Code · Codex · Cursor"]
    for k, s in enumerate(sources):
        a = ease_out((t - 0.5 - k * 0.35) / 0.4)
        if a <= 0:
            continue
        y = 300 + k * 108
        f = font(30, "Semibold")
        tw = d.textlength(s, font=f)
        d.rounded_rectangle([220, y, 220 + tw + 36, y + 52], 14, fill=ACCENT + (int(60 * a),), outline=ACCENT + (int(200 * a),), width=2)
        d.text((238, y + 10), s, font=f, fill=INK + (int(255 * a),))
        hx, hy = W // 2 + 40, 560
        for j in range(40):
            u = j / 40
            if u > ease((t - 0.9 - k * 0.35) / 0.6):
                break
            x = 640 + (hx - 120 - 640) * u
            yy = y + 30 + (hy - y - 30) * (u * u * (3 - 2 * u))
            pulse = 0.5 + 0.5 * math.sin(t * 6 - u * 10 + k)
            d.ellipse([x - 3, yy - 3, x + 3, yy + 3], fill=(120, 170, 255, int(120 + 120 * pulse * a)))
    ha = ease((t - 1.0) / 0.5)
    if ha > 0:
        d.rounded_rectangle([W // 2 - 80, 500, W // 2 + 160, 620], 30, fill=(28, 34, 60, int(240 * ha)), outline=ACCENT + (int(255 * ha),), width=3)
        text(d, (W // 2 + 40, 560), "MCP", 52, "Bold", INK, "mm", alpha=ha)
        icon = rgba(os.path.join(BRAND, "icon", "app-icon-256.png"), h=150)
        im.alpha_composite(fade_alpha(icon, ha), (W // 2 + 505, 470))
        text(d, (W // 2 + 580, 660), "Skywalker", 40, "Bold", INK, "mm", alpha=ha)
        text(d, (W // 2 + 580, 708), "57 tools · live editor or headless", 24, "Regular", DIM, "mm", alpha=ha)
    ta = ease((t - 2.4) / 0.5)
    if ta > 0:
        cmd = "$ claude mcp add skywalker -- skywalker mcp --attach"
        n = int(len(cmd) * ease((t - 2.6) / 2.0))
        d.rounded_rectangle([300, 880, W - 300, 980], 20, fill=(8, 9, 12, int(240 * ta)), outline=(60, 62, 72, int(255 * ta)), width=2)
        text(d, (340, 912), cmd[:n] + ("▍" if int(t * 2) % 2 == 0 else ""), 32, "Regular", (170, 230, 170), mono=True, alpha=ta)
    return im.convert("RGB")


def seg_outro(t, dur):
    bg = darken(shot("frostlight", "sky", 2 + t * 0.6).filter(ImageFilter.GaussianBlur(10)), 0.45).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a = ease_out(t / 0.8)
    icon = rgba(os.path.join(BRAND, "icon", "app-icon-1024.png"), h=260)
    wm = rgba(os.path.join(BRAND, "logo", "wordmark-light.png"), h=150)
    total_w = icon.width + 30 + wm.width
    x0 = W // 2 - total_w // 2
    y0 = 330 + int((1 - a) * 20)
    bg.alpha_composite(fade_alpha(icon, a), (x0, y0))
    bg.alpha_composite(fade_alpha(wm, a), (x0 + icon.width + 30, y0 + (icon.height - wm.height) // 2))
    b = ease((t - 0.8) / 0.6)
    text(d, (W // 2, 700), "Any game. One crew.", 56, "Semibold", (240, 236, 255), "mm", alpha=b)
    c = ease((t - 1.6) / 0.6)
    text(d, (W // 2, 800), "C++ engine · Metal · Swift editor · MCP · Wander", 28, "Regular", DIM, "mm", alpha=c)
    fade = ease((t - (dur - 0.9)) / 0.9)
    out = bg.convert("RGB")
    return Image.blend(out, Image.new("RGB", out.size, (0, 0, 0)), fade) if fade > 0 else out


SEGMENTS = [("01_intro", seg_intro, 0.9, 1.4), ("02_challenge", seg_challenge, 0.3, 1.2), ("03_crew", seg_crew, 0.3, 1.0),
            ("04_horror", seg_horror, 0.4, 1.4), ("05_cozy", seg_cozy, 0.3, 1.4), ("06_action", seg_action, 0.3, 1.4),
            ("07_play", seg_play, 0.3, 1.6), ("08_art", seg_art, 0.3, 1.6), ("09_render", seg_render, 0.3, 1.2),
            ("10_textures", seg_textures, 0.3, 1.2), ("11_assets", seg_assets, 0.3, 1.2), ("12_download", seg_download, 0.3, 1.2),
            ("13_agents", seg_agents, 0.3, 1.0), ("14_connect", seg_connect, 0.3, 1.2), ("15_outro", seg_outro, 0.6, 2.6)]

timeline = []
cursor = 0.0
for vo_id, fn, lead, tail in SEGMENTS:
    vo, sr = sf.read(os.path.join(WORK, "vo", f"{vo_id}.wav"), dtype="float32")
    dur = lead + len(vo) / sr + tail
    timeline.append({"id": vo_id, "fn": fn, "start": cursor, "dur": dur, "vo": vo, "sr": sr, "lead": lead})
    cursor += dur
total = cursor
print(f"total {total:.1f}s", flush=True)

if "--preview" in sys.argv:
    os.makedirs(OUT, exist_ok=True)
    only = sys.argv[sys.argv.index("--only") + 1].split(",") if "--only" in sys.argv else None
    for seg in timeline:
        if only and seg["id"] not in only:
            continue
        for frac in (0.3, 0.75):
            img = seg["fn"](seg["dur"] * frac, seg["dur"])
            img.resize((960, 540), Image.LANCZOS).save(os.path.join(OUT, f"{seg['id']}_{int(frac * 100)}.jpg"), quality=88)
    print("previews in", OUT)
    sys.exit(0)

# --- audio ---------------------------------------------------------------------------
AR = 48000
music_path = os.path.join(WORK, "music.wav")
subprocess.run([sys.executable, os.path.join(HERE, "music.py"), music_path, f"{total + 0.5:.2f}"], check=True)
music, _ = sf.read(music_path, dtype="float32")
voice = np.zeros(int(total * AR) + AR)
for seg in timeline:
    v = seg["vo"]
    idx = np.linspace(0, len(v) - 1, int(len(v) * AR / seg["sr"]))
    v48 = np.interp(idx, np.arange(len(v)), v)
    s = int((seg["start"] + seg["lead"]) * AR)
    voice[s:s + len(v48)] += v48 * 0.95
envv = np.convolve(np.abs(voice), np.ones(int(0.25 * AR)) / int(0.25 * AR), mode="same")
duck = 1.0 - 0.55 * np.clip(envv / 0.04, 0, 1)
n = min(len(music), len(voice))
mix = music[:n] * (0.75 * duck[:n, None]) + voice[:n, None]
mix /= max(1.0, np.abs(mix).max() / 0.95)
audio_path = os.path.join(WORK, "mix.wav")
sf.write(audio_path, mix.astype(np.float32), AR)

# --- video ---------------------------------------------------------------------------
ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                       "-r", str(FPS), "-i", "-", "-i", audio_path, "-c:v", "libx264", "-preset", "slow", "-crf", "17",
                       "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-movflags", "+faststart",
                       "-shortest", OUT], stdin=subprocess.PIPE)
XF = 0.35
nframes = int(total * FPS)
for f in range(nframes):
    t = f / FPS
    k = max(i for i, s in enumerate(timeline) if s["start"] <= t + 1e-9)
    s = timeline[k]
    img = s["fn"](t - s["start"], s["dur"])
    if k > 0 and t - s["start"] < XF:
        p = timeline[k - 1]
        prev = p["fn"](p["dur"] + (t - s["start"]) - 1e-3, p["dur"])
        img = Image.blend(prev, img, ease((t - s["start"]) / XF))
    if t < 0.6:
        img = Image.blend(Image.new("RGB", img.size, (0, 0, 0)), img, ease(t / 0.6))
    ff.stdin.write(img.tobytes())
    if f % 300 == 0:
        print(f"frame {f}/{nframes}", flush=True)
ff.stdin.close()
ff.wait()
print("wrote", OUT)
