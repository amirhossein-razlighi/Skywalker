#!/usr/bin/env python3
"""Composites the Skywalker demo video: engine footage + real editor screenshots + motion
graphics + Kokoro voiceover + procedural music  ->  1920x1080 H.264 MP4.

  compose.py WORK_DIR OUT.mp4
WORK_DIR contains: footage/, rec_village/, assets/, vo/
"""
import json, math, os, subprocess, sys
from functools import lru_cache

import numpy as np
import soundfile as sf
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WORK, OUT = sys.argv[1], sys.argv[2]
W, H, FPS = 1920, 1080, 30
HERE = os.path.dirname(os.path.abspath(__file__))
FOOT = os.path.join(WORK, "footage")
ASSETS = os.path.join(WORK, "assets")
REC = os.path.join(WORK, "rec_village")

# ---------------------------------------------------------------------------------------
# Style
# ---------------------------------------------------------------------------------------
INK = (232, 234, 240)
DIM = (160, 164, 176)
FAINT = (110, 114, 126)
ACCENT = (79, 141, 255)
AI = (154, 134, 255)
PANEL = (24, 25, 30)
CREW = {
    "Nimbus": ((139, 115, 250), "nimbus", "Creative Director"),
    "Cirro": ((92, 143, 237), "cirro", "Level Designer"),
    "Aurora": ((255, 184, 115), "aurora", "Lighting Artist"),
    "Stratus": ((84, 204, 173), "stratus", "Gameplay Programmer"),
}


@lru_cache(maxsize=None)
def font(size, weight="Regular", mono=False):
    if mono:
        f = ImageFont.truetype("/System/Library/Fonts/SFNSMono.ttf", size)
        try:
            f.set_variation_by_name(weight)
        except Exception:
            pass
        return f
    f = ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", size)
    f.set_variation_by_name(weight)
    return f


@lru_cache(maxsize=256)
def load(path, size=None):
    im = Image.open(path).convert("RGB")
    if size and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    return im


@lru_cache(maxsize=16)
def avatar(name, h):
    im = Image.open(os.path.join(ASSETS, f"{name}.png")).convert("RGBA")
    return im.resize((int(im.width * h / im.height), h), Image.LANCZOS)


def ease(x):
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def ease_out(x):
    x = max(0.0, min(1.0, x))
    return 1 - (1 - x) ** 3


def frames_of(name):
    d = os.path.join(FOOT, name)
    return sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith(".png"))


def clip_frame(name, t, speed=1.0):
    fs = frames_of(name)
    i = min(len(fs) - 1, int(t * FPS * speed))
    return load(fs[i], (W, H)).copy()


def darken(im, k):
    return Image.blend(im, Image.new("RGB", im.size, (8, 9, 14)), k)


def rounded_panel(size, radius=18, fill=(20, 21, 26, 230), outline=(60, 62, 72, 255)):
    p = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(p)
    d.rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius, fill=fill, outline=outline, width=2)
    return p


def shadowed(base, overlay, xy, blur=24, opacity=110):
    sh = Image.new("RGBA", (overlay.width + blur * 4, overlay.height + blur * 4), (0, 0, 0, 0))
    mask = overlay.split()[-1].point(lambda a: opacity if a > 0 else 0)
    sh.paste((0, 0, 0, 255), (blur * 2, blur * 2), mask)
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base.paste(sh, (xy[0] - blur * 2, xy[1] - blur * 2 + 10), sh)
    base.paste(overlay, xy, overlay)


def text(d, xy, s, size, weight="Regular", fill=INK, anchor="la", mono=False, alpha=1.0):
    c = tuple(int(v) for v in fill)
    if alpha < 1.0:
        c = c + (int(255 * alpha),)
    d.text(xy, s, font=font(size, weight, mono), fill=c, anchor=anchor)


def caption(im, s, t, start=0.2, size=44):
    """Lower-third caption with a soft gradient for legibility."""
    a = ease((t - start) / 0.5)
    if a <= 0:
        return
    grad = Image.new("L", (1, 320))
    for y in range(320):
        grad.putpixel((0, y), int(235 * (y / 320) ** 1.2 * a))
    g = grad.resize((W, 320))
    im.paste((5, 6, 10), (0, H - 320), g)
    ov = Image.new("RGBA", (W, 200), (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    tw = d.textlength(s, font=font(size, "Semibold"))
    y = 100 + (1 - a) * 16
    d.rounded_rectangle([W // 2 - tw / 2 - 34, y - size * 0.85, W // 2 + tw / 2 + 34, y + size * 0.85], 30,
                        fill=(12, 13, 18, int(225 * a)), outline=(70, 72, 84, int(255 * a)), width=2)
    text(d, (W // 2, y), s, size, "Semibold", INK, "mm", alpha=a)
    im.paste(ov, (0, H - 230), ov)


def chip(d, xy, label, color, size=26, alpha=1.0):
    f = font(size, "Semibold")
    tw = d.textlength(label, font=f)
    x, y = xy
    d.rounded_rectangle([x, y, x + tw + 36, y + size + 22], 14, fill=color + (int(60 * alpha),),
                        outline=color + (int(200 * alpha),), width=2)
    d.text((x + 18, y + 10), label, font=f, fill=INK + (int(255 * alpha),))
    return tw + 36


def corner_tag(im, label, t):
    a = ease(t / 0.4)
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    f = font(24, "Semibold")
    tw = d.textlength(label, font=f)
    d.rounded_rectangle([48, 44, 48 + tw + 70, 96], 26, fill=(14, 15, 20, int(200 * a)))
    d.ellipse([66, 62, 82, 78], fill=(255, 92, 92, int(255 * a)) if "LIVE" in label else ACCENT + (int(255 * a),))
    d.text((94, 56), label, font=f, fill=INK + (int(255 * a),))
    im.paste(ov, (0, 0), ov)


# ---------------------------------------------------------------------------------------
# Data from the real session
# ---------------------------------------------------------------------------------------
history = json.load(open(os.path.join(REC, "history.json")))
frames_ids = {}
for path in ("events_part1.jsonl", "events.jsonl"):
    p = os.path.join(REC, path)
    if os.path.exists(p):
        for line in open(p):
            e = json.loads(line)
            if "ids" in e:
                frames_ids[e["frame"]] = set(e["ids"])
rec_frames = sorted(os.listdir(os.path.join(REC, "frames")))
first_seen = {}
for f in sorted(frames_ids):
    for i in frames_ids[f]:
        first_seen.setdefault(i, f)
anchors = []
for idx, e in enumerate(history):
    fs = [first_seen[i] for i in e.get("entities", []) if i in first_seen]
    anchors.append(min(fs) if fs else None)
# Fill unknown anchors (edits without new entities) by interpolating between neighbors.
known = [(i, a) for i, a in enumerate(anchors) if a is not None]
for i, a in enumerate(anchors):
    if a is None:
        prev = max(((j, b) for j, b in known if j < i), default=(0, 0))
        nxt = min(((j, b) for j, b in known if j > i), default=(len(anchors), len(rec_frames) - 1))
        anchors[i] = int(prev[1] + (nxt[1] - prev[1]) * (i - prev[0]) / max(1, nxt[0] - prev[0]))
# Part 1 of the recording logged attributed history directly, with real frame numbers.
part1 = [json.loads(l) for l in open(os.path.join(REC, "events_part1.jsonl"))] if os.path.exists(os.path.join(REC, "events_part1.jsonl")) else []
part1 = [e for e in part1 if "actor" in e]
for i, e in enumerate(history[:len(part1)]):
    anchors[i] = part1[i]["frame"]
last_known = part1[-1]["frame"] if part1 else 0
for i in range(len(part1), len(anchors)):
    anchors[i] = max(anchors[i], last_known)
    last_known = anchors[i]
events = sorted(((anchors[i], e) for i, e in enumerate(history)), key=lambda x: x[0])
behaviors = json.load(open(os.path.join(ASSETS, "behaviors.json")))
coins = [tuple(map(int, l.split())) for l in open(os.path.join(FOOT, "dash_coins.txt"))]
vision_list = None


def actor_name(actor):
    return actor.split(":", 1)[1] if ":" in actor else actor


# ---------------------------------------------------------------------------------------
# Segments
# ---------------------------------------------------------------------------------------
def seg_intro(t, dur):
    im = clip_frame("intro", t)
    a = ease((t - 0.6) / 0.9) * (1 - ease((t - (dur - 0.9)) / 0.6))
    if a > 0:
        ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
        d = ImageDraw.Draw(ov)
        mark = avatar("nimbus", 110)
        text(d, (W // 2 + 40, 430 + (1 - a) * 20), "Skywalker", 150, "Bold", (255, 255, 255), "mm", alpha=a)
        text(d, (W // 2, 545 + (1 - a) * 20), "The game engine built for AI agents", 46, "Medium", (255, 246, 238), "mm", alpha=a)
        im = im.convert("RGBA")
        shade = Image.new("L", (W, H), 0)
        ImageDraw.Draw(shade).ellipse([W // 2 - 820, 320, W // 2 + 820, 620], fill=int(95 * a))
        shade = shade.filter(ImageFilter.GaussianBlur(120))
        im.paste((20, 12, 30), (0, 0), shade)
        im.alpha_composite(ov)
        m = mark.copy()
        m.putalpha(m.split()[-1].point(lambda v: int(v * a)))
        tw = ImageDraw.Draw(im).textlength("Skywalker", font=font(150, "Bold"))
        im.alpha_composite(m, (int(W // 2 + 40 - tw / 2 - m.width - 24), int(430 - 58 + (1 - a) * 20)))
        im = im.convert("RGB")
    return im


def seg_crew(t, dur):
    bg = darken(clip_frame("hero2", t).filter(ImageFilter.GaussianBlur(14)), 0.62).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a0 = ease(t / 0.5)
    text(d, (W // 2, 170), "Meet your crew", 72, "Bold", INK, "mm", alpha=a0)
    text(d, (W // 2, 240), "Four AI agents · each powered by Claude · each with a role", 32, "Regular", DIM, "mm", alpha=a0)
    names = ["Nimbus", "Cirro", "Aurora", "Stratus"]
    cw, ch, gap = 380, 420, 36
    x0 = (W - (4 * cw + 3 * gap)) // 2
    for k, n in enumerate(names):
        appear = 1.4 + k * 1.15
        a = ease_out((t - appear) / 0.6)
        if a <= 0:
            continue
        color, key, role = CREW[n]
        card = rounded_panel((cw, ch), 26, fill=(22, 23, 29, 235), outline=color + (180,))
        cd = ImageDraw.Draw(card)
        av = avatar(key, 150)
        card.alpha_composite(av, ((cw - av.width) // 2, 60))
        text(cd, (cw // 2, 270), n, 46, "Bold", INK, "mm")
        text(cd, (cw // 2, 322), role, 28, "Medium", color, "mm")
        text(cd, (cw // 2, 370), "Claude · via MCP", 22, "Regular", FAINT, "mm")
        card.putalpha(card.split()[-1].point(lambda v: int(v * a)))
        shadowed(bg, card, (x0 + k * (cw + gap), int(330 + (1 - a) * 40)))
    return bg.convert("RGB")


def seg_brief(t, dur):
    first = load(os.path.join(REC, "frames", rec_frames[30]), (W, H))
    bg = darken(first.filter(ImageFilter.GaussianBlur(10)), 0.55).convert("RGBA")
    panel = rounded_panel((1240, 640), 28, fill=(20, 21, 27, 240), outline=(70, 72, 84, 255))
    pd = ImageDraw.Draw(panel)
    panel.alpha_composite(avatar("nimbus", 70), (44, 40))
    text(pd, (160, 52), "Nimbus", 34, "Bold")
    text(pd, (160, 94), "Creative Director", 24, "Medium", CREW["Nimbus"][0])
    idea = "“A cozy village floating in the sky.”"
    n = int(len(idea) * ease((t - 0.3) / 1.4))
    text(pd, (48, 170), idea[:n] + ("▍" if n < len(idea) else ""), 50, "Semibold", INK)
    tasks = [("Cirro", "Layout: plaza, 7 cottages, windmill, trees, lighthouse, airships"),
             ("Aurora", "Light: golden-hour sun, lanterns, glowing windows"),
             ("Stratus", "Behaviors: clouds, fireflies, windmill, airships, a villager")]
    for k, (who, task) in enumerate(tasks):
        a = ease_out((t - 1.9 - k * 0.7) / 0.5)
        if a <= 0:
            continue
        y = 280 + k * 104
        row = Image.new("RGBA", (1150, 88), (0, 0, 0, 0))
        rd = ImageDraw.Draw(row)
        rd.rounded_rectangle([0, 0, 1149, 87], 18, fill=(30, 31, 38, 255))
        row.alpha_composite(avatar(CREW[who][1], 54), (20, 17))
        text(rd, (120, 16), f"{who} · {CREW[who][2]}", 24, "Semibold", CREW[who][0])
        text(rd, (120, 48), task, 26, "Regular", INK)
        row.putalpha(row.split()[-1].point(lambda v: int(v * a)))
        panel.alpha_composite(row, (44, int(y + (1 - a) * 20)))
    shadowed(bg, panel, ((W - 1240) // 2, 220))
    return bg.convert("RGB")


def seg_timelapse(t, dur):
    p = min(1.0, t / (dur - 0.4))
    idx = int(p * (len(rec_frames) - 1))
    im = load(os.path.join(REC, "frames", rec_frames[idx]), (W, H)).copy().convert("RGBA")
    corner_tag(im, "LIVE · 3 agents editing in parallel", t)
    ov = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(ov)
    secs = idx * 0.5
    clock = f"{int(secs // 60):02d}:{int(secs % 60):02d}"
    done = [e for f, e in events if f <= idx]
    d.rounded_rectangle([W - 420, 44, W - 48, 150], 22, fill=(14, 15, 20, 200))
    text(d, (W - 396, 58), "real time", 22, "Medium", DIM)
    text(d, (W - 396, 88), clock, 46, "Bold", INK, mono=True)
    text(d, (W - 220, 58), "edits", 22, "Medium", DIM)
    text(d, (W - 220, 88), f"{len(done)}", 46, "Bold", INK, mono=True)
    # Activity ticker (latest first)
    recent = done[-5:][::-1]
    for k, e in enumerate(recent):
        who = actor_name(e["actor"])
        color, key, role = CREW.get(who, ((150, 150, 150), "nimbus", "agent"))
        y = H - 120 - k * 74
        alpha = 1.0 - k * 0.17
        d.rounded_rectangle([48, y, 760, y + 62], 18, fill=(14, 15, 20, int(205 * alpha)))
        label = e["label"].replace(f"{who}: ", "")
        text(d, (124, y + 8), who, 22, "Semibold", color, alpha=alpha)
        text(d, (124, y + 32), label[:46], 22, "Regular", INK, alpha=alpha)
        av = avatar(key, 40)
        av2 = av.copy()
        av2.putalpha(av2.split()[-1].point(lambda v: int(v * alpha)))
        ov.alpha_composite(av2, (66, y + 11))
    im.alpha_composite(ov)
    return im.convert("RGB")


def seg_vision(t, dur):
    z = 1.0 + 0.08 * ease(t / dur)
    clean = load(os.path.join(FOOT, "vision_clean.png"), (W, H))
    marked = load(os.path.join(FOOT, "vision.png"), (W, H))
    a = ease((t - 0.8) / 0.8)
    im = Image.blend(clean, marked, a)
    cw, chh = int(W / z), int(H / z)
    im = im.crop(((W - cw) // 2, (H - chh) // 2, (W + cw) // 2, (H + chh) // 2)).resize((W, H), Image.LANCZOS).convert("RGBA")
    corner_tag(im, "What an agent sees · viewport_capture(annotate: true)", t)
    pa = ease((t - 1.8) / 0.6)
    if pa > 0:
        vis = json.load(open(os.path.join(FOOT, "vision.json")))["visible"]
        pick, seen_prefix = [], set()
        for v in vis:
            prefix = next((p for p in ("Windmill", "Cottage", "Plaza", "Lighthouse", "Airship", "Well") if v["name"].startswith(p)), None)
            if prefix and prefix not in seen_prefix and v["box"][2] > 40 and v["box"][3] > 25:
                seen_prefix.add(prefix)
                pick.append(v)
            if len(pick) == 4:
                break
        lines = ["visible entities (nearest first):"] + [
            f"#{v['id']:<4} {v['name'][:16]:<16} box {v['box']}" for v in pick] + [
            f"…  {len(vis)} entities, each with a screen box"]
        panel = rounded_panel((760, 300), 20, fill=(14, 15, 20, 225))
        pd = ImageDraw.Draw(panel)
        for k, l in enumerate(lines):
            text(pd, (28, 26 + k * 44), l, 24, "Regular", INK if k else ACCENT, mono=True)
        panel.putalpha(panel.split()[-1].point(lambda v: int(v * pa)))
        im.alpha_composite(panel, (W - 820, H - 360))
    return im.convert("RGB")


def code_card(title, who, intent, source, reveal):
    card = rounded_panel((800, 820), 24, fill=(16, 17, 22, 238))
    d = ImageDraw.Draw(card)
    color, key, role = CREW[who]
    card.alpha_composite(avatar(key, 46), (30, 26))
    text(d, (102, 26), f"{who} · {role}", 22, "Semibold", color)
    text(d, (102, 54), f"behavior “{title}”", 26, "Bold", INK)
    text(d, (30, 112), "INTENT", 18, "Semibold", DIM)
    words, line, y = intent.split(), "", 140
    for w_ in words:
        if d.textlength(line + " " + w_, font=font(24)) > 740:
            text(d, (30, y), line.strip(), 24, "Regular", INK)
            y += 32
            line = ""
        line += " " + w_
    text(d, (30, y), line.strip(), 24, "Regular", INK)
    y += 56
    text(d, (30, y), "WANDER", 18, "Semibold", DIM)
    y += 30
    kw = {"behavior", "intent", "var", "on", "end", "if", "then", "else", "let", "rotate", "look", "self", "by", "at", "start", "tick"}
    src = [l for l in source.split("\n") if l.strip() and not l.strip().startswith("intent") and not l.strip().startswith("--")]
    shown = int(len(src) * reveal)
    import re
    fn_names = ("sin", "cos", "normalize", "vec", "length", "random", "pi", "time", "dt")
    for l in src[:min(shown, 17)]:
        x = 30
        for tok in re.findall(r"\w+|\s+|[^\w\s]", l.rstrip()):
            c = (199, 146, 234) if tok in kw else (247, 140, 108) if tok.isdigit() else (124, 199, 255) if tok in fn_names else INK
            if not tok.isspace():
                text(d, (x, y), tok, 21, "Regular", c, mono=True)
            x += d.textlength(tok, font=font(21, mono=True))
            if x > 770:
                break
        y += 29
    return card


def seg_wander(t, dur):
    parts = [("windmill", "Windmill Blades", "Spin", 0.0), ("airship", "Airship 1", "Cruise", 3.9), ("fireflies", "Firefly 1", "Hover", 7.9)]
    cur = [p for p in parts if t >= p[3]][-1]
    lt = t - cur[3]
    im = clip_frame(cur[0], lt).convert("RGBA")
    b = behaviors[cur[1]]
    card = code_card(cur[2], "Stratus", b["intent"], b["source"], ease(lt / 1.6))
    a = ease_out(lt / 0.5)
    card.putalpha(card.split()[-1].point(lambda v: int(v * a)))
    shadowed(im, card, (60 - int((1 - a) * 40), 130))
    corner_tag(im, "ECPS · intent + deterministic code, running live", t)
    return im.convert("RGB")


def seg_light(t, dur):
    im = clip_frame("night", t * 1.05).convert("RGBA")
    d = ImageDraw.Draw(im)
    a = ease((t - 0.3) / 0.4)
    d.rounded_rectangle([60, H - 170, 940, H - 80], 22, fill=(14, 15, 20, int(210 * a)))
    text(d, (92, H - 150), "environment_update", 30, "Semibold", ACCENT, alpha=a, mono=True)
    phase = "golden hour → night" if t < dur * 0.45 else "night · lanterns on"
    text(d, (470, H - 148), phase, 30, "Medium", INK, alpha=a)
    return im.convert("RGB")


def seg_2d(t, dur):
    im = clip_frame("dash", t).convert("RGBA")
    d = ImageDraw.Draw(im)
    fi = int(t * FPS)
    c = max([cv for i, cv in coins if i <= fi] or [0])
    d.rounded_rectangle([W - 300, 44, W - 48, 120], 26, fill=(14, 15, 20, 190))
    d.ellipse([W - 280, 62, W - 240, 102], fill=(244, 196, 48, 255))
    text(d, (W - 220, 56), f"× {c}", 44, "Bold", (255, 255, 255))
    corner_tag(im, "Sky Dash · 2D sample · built through engine tools", t)
    return im.convert("RGB")


def framed_screenshot(path, t, dur, zoom_to=None):
    shot = load(path)
    bg = Image.new("RGB", (W, H), (12, 13, 17))
    grad = Image.radial_gradient("L").resize((W * 2, H * 2)).crop((W // 2, H // 2, W // 2 + W, H // 2 + H))
    bg.paste((30, 34, 52), (0, 0), Image.eval(grad, lambda v: 255 - v))
    s = 1.28
    shot_big = shot.resize((int(shot.width * s), int(shot.height * s)), Image.LANCZOS)
    pos = ((W - shot_big.width) // 2, (H - shot_big.height) // 2 - 60)
    bgr = bg.convert("RGBA")
    win = Image.new("RGBA", shot_big.size)
    win.paste(shot_big)
    mask = Image.new("L", shot_big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, shot_big.width - 1, shot_big.height - 1], 18, fill=255)
    win.putalpha(mask)
    shadowed(bgr, win, pos, blur=30, opacity=160)
    im = bgr.convert("RGB")
    z = 1.0 + 0.06 * ease(t / dur)
    cx, cy = W / 2, H / 2
    if zoom_to:
        k = ease(t / dur)
        z = 1.0 + (zoom_to[2] - 1.0) * k
        cx = W / 2 + (zoom_to[0] - W / 2) * k
        cy = H / 2 + (zoom_to[1] - H / 2) * k
    cw, chh = W / z, H / z
    box = (int(cx - cw / 2), int(cy - chh / 2), int(cx + cw / 2), int(cy + chh / 2))
    return im.crop(box).resize((W, H), Image.LANCZOS)


def seg_editor(t, dur):
    shots = [("ui_activity.jpg", "Every edit attributed to the agent that made it", (820, 760, 1.22)),
             ("ui_behavior.jpg", "Plain-language intent, compiled Wander, live gizmos", (1320, 560, 1.22)),
             ("ui_rotate.jpg", "A real editor: outliner, details, transform tools", None)]
    seg = dur / len(shots)
    k = min(len(shots) - 1, int(t / seg))
    lt = t - k * seg
    name, cap, zoom = shots[k]
    im = framed_screenshot(os.path.join(ASSETS, name), lt, seg, zoom)
    caption(im, cap, lt, 0.1, 40)
    return im


def seg_connect(t, dur):
    im = Image.new("RGB", (W, H), (11, 12, 16))
    grad = Image.radial_gradient("L").resize((W * 2, H * 2)).crop((W // 2, H // 2 - 200, W // 2 + W, H // 2 + H - 200))
    im.paste((24, 30, 56), (0, 0), Image.eval(grad, lambda v: 255 - v))
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
        chip(d, (220, y), s, ACCENT if k < 4 else AI, 30, a)
        # flowing line to the hub
        hx, hy = W // 2 + 40, 560
        steps = 40
        for j in range(steps):
            u = j / steps
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
        for j in range(30):
            u = j / 30
            if u > ease((t - 1.3) / 0.5):
                break
            x = W // 2 + 170 + u * 300
            d.ellipse([x - 3, 557, x + 3, 563], fill=(150, 130, 255, int(220 * ha)))
        av = avatar("nimbus", 120)
        im.alpha_composite(av, (W // 2 + 500, 500))
        text(d, (W // 2 + 580, 660), "Skywalker", 40, "Bold", INK, "mm", alpha=ha)
        text(d, (W // 2 + 580, 708), "35 tools · live editor or headless", 24, "Regular", DIM, "mm", alpha=ha)
    ta = ease((t - 2.4) / 0.5)
    if ta > 0:
        cmd = "$ claude mcp add skywalker -- skywalker mcp --attach"
        n = int(len(cmd) * ease((t - 2.6) / 2.0))
        d.rounded_rectangle([300, 880, W - 300, 980], 20, fill=(8, 9, 12, int(240 * ta)), outline=(60, 62, 72, int(255 * ta)), width=2)
        text(d, (340, 912), cmd[:n] + ("▍" if int(t * 2) % 2 == 0 else ""), 32, "Regular", (170, 230, 170), mono=True, alpha=ta)
    return im.convert("RGB")


def seg_outro(t, dur):
    bg = darken(clip_frame("night", 5.9).filter(ImageFilter.GaussianBlur(16)), 0.5).convert("RGBA")
    d = ImageDraw.Draw(bg)
    a = ease_out(t / 0.8)
    av = avatar("nimbus", 160)
    av2 = av.copy()
    av2.putalpha(av2.split()[-1].point(lambda v: int(v * a)))
    bg.alpha_composite(av2, (W // 2 - av.width // 2, int(250 + (1 - a) * 30)))
    text(d, (W // 2, 520), "Skywalker", 140, "Bold", (255, 255, 255), "mm", alpha=a)
    b = ease((t - 0.9) / 0.6)
    text(d, (W // 2, 640), "One person. A whole game.", 54, "Semibold", (230, 225, 255), "mm", alpha=b)
    c = ease((t - 1.8) / 0.6)
    text(d, (W // 2, 760), "v0.0.1 · C++ engine · Metal · Swift editor · MCP", 30, "Regular", DIM, "mm", alpha=c)
    fade = ease((t - (dur - 0.8)) / 0.8)
    out = bg.convert("RGB")
    return Image.blend(out, Image.new("RGB", out.size, (0, 0, 0)), fade) if fade > 0 else out


SEGMENTS = [
    ("01_intro", seg_intro, 0.9, 0.8), ("02_crew", seg_crew, 0.4, 0.7), ("03_brief", seg_brief, 0.3, 0.9),
    ("04_timelapse", seg_timelapse, 0.3, 1.2), ("05_vision", seg_vision, 0.4, 0.8), ("06_wander", seg_wander, 0.3, 1.0),
    ("07_light", seg_light, 0.5, 1.4), ("08_2d", seg_2d, 0.4, 1.0), ("09_editor", seg_editor, 0.3, 0.8),
    ("10_connect", seg_connect, 0.4, 1.1), ("11_outro", seg_outro, 0.6, 2.4),
]

# Timeline from voiceover durations: lead-in + line + tail.
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
    for seg in timeline:
        for frac in (0.35, 0.85):
            img = seg["fn"](seg["dur"] * frac, seg["dur"])
            img.resize((960, 540), Image.LANCZOS).save(os.path.join(OUT, f"{seg['id']}_{int(frac*100)}.jpg"), quality=88)
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
# Duck the music under the voice (smoothed envelope).
envv = np.abs(voice)
win = int(0.25 * AR)
envv = np.convolve(envv, np.ones(win) / win, mode="same")
duck = 1.0 - 0.55 * np.clip(envv / 0.04, 0, 1)
n = min(len(music), len(voice))
mix = music[:n] * (0.75 * duck[:n, None]) + voice[:n, None]
mix /= max(1.0, np.abs(mix).max() / 0.95)
audio_path = os.path.join(WORK, "mix.wav")
sf.write(audio_path, mix.astype(np.float32), AR)

# --- video ---------------------------------------------------------------------------
ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                       "-r", str(FPS), "-i", "-", "-i", audio_path, "-c:v", "libx264", "-preset", "slow", "-crf", "17",
                       "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", "-shortest", OUT],
                      stdin=subprocess.PIPE)
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
    if f % 150 == 0:
        print(f"frame {f}/{nframes}", flush=True)
ff.stdin.close()
ff.wait()
print("wrote", OUT)
