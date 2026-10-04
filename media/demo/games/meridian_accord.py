"""Meridian Accord — a grand strategy map of an invented continent on the eve of a long war.

Original IP: ten fictional nations, 142 provinces, 23 cities, flags and leaders, all generated
by `meridian_maps.py`. The map is a real heightmap terrain (Poly Haven aerial photoscans) with
the political map draped over it as a terrain overlay; the sea is the FFT ocean at map scale.
On top: miniature cities and forests (Blender), flat SDF labels for nations and cities, wooden
unit blocks, 3D attack and supply arrows, capital flags, clouds drifting under the camera, and
a full strategy HUD (top bar, speed control, nation panel, province tooltip, map modes, minimap,
event popup). A Wander campaign script runs the clock, advances the front through five overlay
stages and raises an event.
"""
import hashlib
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import polyhaven as ph  # noqa: E402
from kit import rnd  # noqa: E402

META = dict(
    id="meridian_accord", title="Meridian Accord", genre="Grand Strategy · Map Wargame", mood="tense, stately, cartographic",
    pitch="Ten nations, one inland sea, and an accord that will not survive the spring of '37.",
    assets="Poly Haven (CC0) aerial photoscans; procedural maps, flags and art; Blender-generated miniatures",
    startScene="scenes/main.sky.json",
    include=["maps/*.png", "maps/*.jpg", "maps/flags/*.png", "ui/*", "art/*.png"],
)

PLAYER = "kestria"
MINIS = "generated/minis"
STAGES = 5
NATION_FONT = "serif"


def world(project):
    with open(os.path.join(project, "maps", "world.json")) as f:
        return json.load(f)


def generate_maps(project):
    """Relief, political stages, map modes, flags, counters and UI art (deterministic, ~20 s)."""
    if os.path.exists(os.path.join(project, "maps", "forest.png")) and not os.environ.get("MA_REGEN"):
        return
    import meridian_maps as mm
    mm.generate(project)
    mm.ui_art(project)
    # The overlays are flat colors with soft edges: a 256-color palette (with alpha) keeps them
    # visually identical at a fifth of the size, so the committed maps stay small.
    from PIL import Image
    for name in [f"political_{k}.png" for k in range(STAGES)] + ["terrain.png", "supply.png"]:
        path = os.path.join(project, "maps", name)
        Image.open(path).quantize(colors=256, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE).save(path, optimize=True)


class Relief:
    """Samples the heightmap the terrain is built from (world x, z -> height in meters)."""

    def __init__(self, project, w):
        import numpy as np
        from PIL import Image
        self.np = np
        self.h = np.asarray(Image.open(os.path.join(project, "maps", "height.png"))).astype(np.float32) / 65535.0
        self.h = w["minHeight"] + self.h * (w["maxHeight"] - w["minHeight"])
        self.size = w["size"]
        self.n = self.h.shape[0]

    def at(self, x, z):
        u = (x / self.size + 0.5) * self.n - 0.5
        v = (z / self.size + 0.5) * self.n - 0.5
        u = min(max(u, 0), self.n - 1.001)
        v = min(max(v, 0), self.n - 1.001)
        x0, y0 = int(u), int(v)
        tx, ty = u - x0, v - y0
        h = self.h
        return float((h[y0, x0] * (1 - tx) + h[y0, x0 + 1] * tx) * (1 - ty) + (h[y0 + 1, x0] * (1 - tx) + h[y0 + 1, x0 + 1] * tx) * ty)

    def peak(self, x, z, r):
        """Highest ground within r meters (labels and arrows float above it)."""
        best = -1e9
        for dx in (-r, -r / 2, 0, r / 2, r):
            for dz in (-r, -r / 2, 0, r / 2, r):
                best = max(best, self.at(x + dx, z + dz))
        return best


# ============================================================================================
# Blender miniatures
# ============================================================================================
def run_dcc(pix, job):
    with open(os.path.join(HERE, "meridian_dcc.py")) as f:
        script = f.read()
    blob = json.dumps(job, sort_keys=True)
    digest = hashlib.sha256((script + blob).encode()).hexdigest()[:16]
    stamp = os.path.join(pix.studio.project, MINIS, ".job")
    if not (os.path.exists(stamp) and open(stamp).read().strip() == digest):
        import shutil
        shutil.rmtree(os.path.join(pix.studio.project, MINIS), ignore_errors=True)  # no stale models or materials
        pix.call("dcc_run_script", script=script, args=[blob], name="minis", out_dir=MINIS, timeout_s=1800,
                 description="Meridian Accord map miniatures (procedural, Blender)", tags=["meridian_accord", "miniature"])
        with open(stamp, "w") as f:
            f.write(digest)
    names = [t["name"] for t in job["towns"]] + ["unit_block", "flag_pole", "flag_cloth", "tree_conifer", "tree_broadleaf"]
    names += [a["name"] for a in job["arrows"]]
    return {n: pix.call("asset_import", path=f"{MINIS}/{n}.glb", normalize=False) for n in names}


def arrow_specs(w, relief):
    """Planned offensives: for every stage, three thrusts from the attacker's front provinces into the
    provinces it takes next (north, center, south), plus a supply line from the capital."""
    specs = []
    for s in range(STAGES - 1):
        pairs = w["front"][s]
        pairs = sorted(pairs, key=lambda p: p["d"][1])
        k = len(pairs)
        groups = [pairs[: k // 3], pairs[k // 3: 2 * k // 3], pairs[2 * k // 3:]]
        for g, grp in enumerate(groups):
            if not grp:
                continue
            ax = sum(p["a"][0] for p in grp) / len(grp)
            az = sum(p["a"][1] for p in grp) / len(grp)
            dx = sum(p["d"][0] for p in grp) / len(grp)
            dz = sum(p["d"][1] for p in grp) / len(grp)
            vx, vz = dx - ax, dz - az
            L = math.hypot(vx, vz) or 1.0
            ux, uz = vx / L, vz / L
            start = (ax - ux * 70, az - uz * 70)
            end = (dx + ux * 25, dz + uz * 25)
            bend = (-uz, ux)
            curve = 26.0 * (1 if g % 2 else -1)
            pts2 = []
            for t in (0.0, 0.33, 0.66, 1.0):
                x = start[0] + (end[0] - start[0]) * t + bend[0] * curve * math.sin(math.pi * t)
                z = start[1] + (end[1] - start[1]) * t + bend[1] * curve * math.sin(math.pi * t)
                pts2.append((x, z))
            pts = [[round(x, 2), round(max(relief.peak(x, z, 18), 0.5) + 7.0, 2), round(z, 2)] for x, z in pts2]
            # keep the ribbon level-ish between samples
            for i in range(1, len(pts) - 1):
                pts[i][1] = max(pts[i][1], 0.5 * (pts[i - 1][1] + pts[i + 1][1]))
            specs.append(dict(name=f"arrow_s{s}_{g}", points=pts, width=17.0, head=48.0, thickness=2.4, stage=s, kind="attack"))
    # Supply lines: from Mirovan to the center of each stage's front (thin, green).
    cap = w["capitals"][PLAYER]
    for s in range(STAGES):
        pairs = w["front"][s]
        ax = sum(p["a"][0] for p in pairs) / len(pairs)
        az = sum(p["a"][1] for p in pairs) / len(pairs)
        dx, dz = ax - cap[0], az - cap[1]
        L = math.hypot(dx, dz) or 1.0
        start = (cap[0] + dx / L * 45, cap[1] + dz / L * 45)   # leave the capital's streets clear
        pts2 = [start, (cap[0] + dx * 0.5, cap[1] + dz * 0.5 - 30), (ax + 20, az)]
        pts = [[round(x, 2), round(max(relief.peak(x, z, 14), 0.5) + 5.0, 2), round(z, 2)] for x, z in pts2]
        specs.append(dict(name=f"supply_s{s}", points=pts, width=7.0, head=26.0, thickness=1.4, stage=s, kind="supply"))
    return specs


TOWN_MODELS = [dict(name="capital_a", kind="capital", seed=11), dict(name="capital_b", kind="capital", seed=29),
               dict(name="city_a", kind="city", seed=5), dict(name="city_b", kind="city", seed=17),
               dict(name="town_a", kind="town", seed=3), dict(name="town_b", kind="town", seed=8), dict(name="town_c", kind="town", seed=41)]


# ============================================================================================
# The continent
# ============================================================================================
def forest_strokes(project, w):
    import numpy as np
    from PIL import Image
    m = np.asarray(Image.open(os.path.join(project, "maps", "forest.png"))).astype(np.float32) / 255.0
    n = m.shape[0]
    step = 2
    cell = w["size"] / n
    out = []
    for y in range(0, n, step):
        for x in range(0, n, step):
            v = float(m[y:y + step, x:x + step].mean())
            if v > 0.35:
                out.append(dict(x=round((x + step / 2) * cell - w["size"] / 2, 1), z=round((y + step / 2) * cell - w["size"] / 2, 1),
                                radius=round(cell * step * 0.9, 1), strength=min(1.0, v * 1.2), falloff=0.5))
    return out


def continent(pix, cir, w, project):
    grass = ph.texture(pix, "aerial_grass_rock", tiling=1.0, triplanar=False)
    rocks = ph.texture(pix, "aerial_rocks_02", tiling=1.0, triplanar=True)
    beach = ph.texture(pix, "aerial_beach_01", tiling=1.0, triplanar=False)
    ground = ph.texture(pix, "aerial_ground_rock", tiling=1.0, triplanar=False)
    snow = ph.texture(pix, "snow_02", tiling=1.0, triplanar=False)
    seabed = ph.texture(pix, "aerial_sand", tiling=1.0, triplanar=False)
    forest = ph.texture(pix, "forest_ground_04", tiling=1.0, triplanar=False)
    layers = [
        {"name": "lowland", "material": grass, "tiling": 70, "color": "#9fb184"},
        {"name": "steppe", "material": ground, "tiling": 90, "color": "#c2b38e", "heightMin": 32, "noise": 0.7, "sharpness": 0.3},
        {"name": "rock", "material": rocks, "tiling": 110, "color": "#aaa090", "slopeMin": 24, "noise": 0.4, "sharpness": 0.4},
        {"name": "snow", "material": snow, "tiling": 80, "color": "#eef1f6", "heightMin": 116, "slopeMax": 42, "noise": 0.5,
         "sharpness": 0.5},
        {"name": "beach", "material": beach, "tiling": 60, "color": "#d0c098", "heightMax": 1.4, "sharpness": 0.6},
        {"name": "seabed", "material": seabed, "tiling": 120, "color": "#7a7a66", "heightMax": -0.6, "sharpness": 0.6},
        {"name": "forest", "material": forest, "tiling": 60, "color": "#4c5e38", "heightMin": 999, "heightMax": -999},
        {"name": "urban", "material": ground, "tiling": 40, "color": "#8a8070", "heightMin": 999, "heightMax": -999},
    ]
    cir.call("terrain_create", name="Continent", size=w["size"], resolution=2049, heightmap="maps/height.png", layers=layers,
             generator={"minHeight": w["minHeight"], "maxHeight": w["maxHeight"], "detailNoise": 1.0, "erosion": 0.0,
                        "thermal": 0.0})
    cir.call("terrain_paint", entity="Continent", layer="forest", strokes=forest_strokes(project, w))
    cir.call("terrain_paint", entity="Continent", layer="urban", strokes=[
        dict(x=c["x"], z=c["z"], radius=40.0 if c["capital"] else 28.0, strength=0.9, falloff=0.7) for c in w["cities"]])
    cir.call("entity_update", entity="Continent", components={"terrain": {
        "overlay": "maps/political_0.png", "overlayOpacity": 0.92, "overlayBlend": "mix", "waterLevel": 0.0, "wetBand": 0.5,
        "macroVariation": 0.45, "detail": 1.0}})


def sea(aur):
    aur.call("fx_create", effect="calm_sea", name="Meridian Sea", position=[0, 0, 0],
             overrides={"windSpeed": 5.0, "patchSize": 160, "deepColor": "#04152a", "shallowColor": "#1a5562", "clarity": 9.0,
                        "foam": 0.04, "roughness": 0.3, "reflections": 0.35, "waveScale": 0.8})


def lighting(aur):
    aur.call("environment_update", skyMode="atmosphere", sunAzimuth=205, sunElevation=38, sunIntensity=3.0, sunColor="#ffecd8",
             clouds=0.42, cloudMode="volumetric", cloudHeight=330, cloudThickness=160, cloudScale=0.1, cloudDensity=2.4,
             cloudSpeed=5.0, windDirection=70, ambient=0.9, fogDensity=0.00012, fogColor="#b9c8d8", tonemap="agx", ao=1.0,
             aoRadius=6.0, gi=0.6, giDistance=120, ssr=0.6, bloomIntensity=0.22, bloomThreshold=1.6, vignette=0.32,
             showGrid=False, autoExposure=False, exposure=1.0, saturation=1.08, contrast=1.06, shadowDistance=3500)


# ============================================================================================
# Cities, flags, labels
# ============================================================================================
def cities(cir, pix, w, relief, models):
    r = rnd(1937)
    caps = 0
    towns = 0
    for c in w["cities"]:
        x, z = c["x"], c["z"]
        y = relief.at(x, z) - 0.3
        if c["capital"]:
            model = ["capital_a", "capital_b"][caps % 2]
            caps += 1
        else:
            model = ["city_a", "town_a", "city_b", "town_b", "town_c"][towns % 5]
            towns += 1
        ph.place(cir, models[model], f"City {c['name']}", (x, y, z), yaw=r.uniform(0, 360), tags=["city", c["nation"]])
    cir.flush("Miniature cities")
    # Capital flags: a pole and a rippling cloth (each nation's flag as its material).
    for c in w["cities"]:
        if not c["capital"]:
            continue
        nid = c["nation"]
        path = f"materials/flag_{nid}.mat.json"
        pix.material(path, texture=f"maps/flags/{nid}.png", color="#ffffff", roughness=0.8, metallic=0.0, doubleSided=True)
        x, z = c["x"] + 14, c["z"] - 12
        y = relief.at(x, z)
        cir.e(f"Flag {nid.title()}", pos=(x, y, z), rot=(0, -20, 0), tags=["flag"], scale=0.8)
        ph.place(cir, models["flag_pole"], f"Flag {nid.title()} Pole", (0, 0, 0), parent=f"Flag {nid.title()}")
        cir.op("entity_create", name=f"Flag {nid.title()} Cloth", parent=f"Flag {nid.title()}", position=[0, 0, 0],
               components={"mesh": {"mesh": f"asset:{MINIS}/flag_cloth.glb", "material": path, "doubleSided": True}})
    cir.flush("Capital flags")


def arc_letters(text, cx, cz, angle_deg, width, bend):
    """Letter positions along a gentle arc centered on (cx, cz), rotated by angle (image space: +x right, +z down)."""
    letters = [ch for ch in text]
    n = len(letters)
    a = math.radians(angle_deg)
    ux, uz = math.cos(a), math.sin(a)          # along the label
    nx, nz = -uz, ux                            # toward the arc's center (bends "down" the map = +z)
    out = []
    for i, ch in enumerate(letters):
        t = (i / max(n - 1, 1)) - 0.5           # -0.5..0.5
        along = t * width
        sag = bend * (1 - (2 * t) ** 2)         # parabola: middle letters lift (away from the center)
        x = cx + ux * along - nx * sag
        z = cz + uz * along - nz * sag
        slope = bend * 8 * t / max(width, 1)    # the arc's tangent turns toward n as t grows
        yaw = -(angle_deg + math.degrees(math.atan(slope)))
        out.append((ch, x, z, yaw))
    return out


def labels(cir, w, relief):
    for nat in w["nations"]:
        name = " ".join(nat["name"].upper())
        width = max(160.0, min(nat["extent"] * 1.65, 400.0))
        size = max(30.0, min(width / len(nat["name"]) * 0.8, 62.0))
        cx, cz = nat["label"]
        letters = arc_letters(nat["name"].upper(), cx, cz, nat["angle"], width, width * 0.08)
        y = max(relief.peak(cx, cz, width * 0.5), 1.0) + 3.0
        root = f"Label {nat['name']}"
        cir.e(root, pos=(0, 0, 0), tags=["label", "nation_label"])
        for i, (ch, x, z, yaw) in enumerate(letters):
            yy = max(relief.peak(x, z, size * 1.4 + 6), 1.0) + 4.0
            cir.op("entity_create", name=f"{root} {i}", parent=root, position=[round(x, 2), round(max(yy, y * 0.6), 2), round(z, 2)],
                   rotation=[-90, round(yaw, 2), 0],
                   components={"text": {"text": ch, "font": NATION_FONT, "size": round(size, 1), "color": "#120d09c0",
                                        "align": "center", "valign": "middle", "billboard": "none"}})
        del name
    cir.flush("Nation labels")
    for c in w["cities"]:
        x, z = c["x"], c["z"]
        y = relief.at(x, z) + (34 if c["capital"] else 22)
        cir.op("entity_create", name=f"City Label {c['name']}", position=[round(x, 2), round(y, 2), round(z, 2)],
               tags=["label", "city_label"],
               components={"text": {"text": c["name"], "font": "serif", "size": 15.0 if c["capital"] else 11.0,
                                    "color": "#fff6e4f4", "align": "center", "valign": "bottom", "outline": 0.09,
                                    "outlineColor": "#1a1410c0", "billboard": "full"}})
    cir.flush("City labels")


# ============================================================================================
# Units and arrows
# ============================================================================================
def units(stra, pix, w, relief, models):
    """Wooden blocks on both sides of the front for every stage (only the current stage is enabled),
    plus garrisons on other borders."""
    wood = ph.texture(pix, "ash_veneer", tiling=0.6, triplanar=True, color="#e6c89c", roughness=0.75)
    counts = []
    for s in range(STAGES):
        seen = set()
        k = 0
        for p in w["front"][s]:
            for side, (x, z) in (("a", p["a"]), ("d", p["d"])):
                key = (side, round(x), round(z))
                if key in seen:
                    continue
                seen.add(key)
                # pull blocks toward the front line so the two sides face each other
                ox, oz = (p["d"][0] - p["a"][0]), (p["d"][1] - p["a"][1])
                L = math.hypot(ox, oz) or 1
                f = 0.28 if side == "a" else -0.28
                bx, bz = x + ox / L * L * f, z + oz / L * L * f
                y = relief.at(bx, bz)
                nid = PLAYER if side == "a" else "varosse"
                kind = "armor" if (k % 3 == 0 and side == "a") else "infantry"
                name = f"Unit S{s} {k}"
                stra.e(name, pos=(bx, y + (0 if s == 0 else -30), bz), rot=(0, (k * 7) % 11 - 5, 0), tags=["unit", f"stage{s}"],
                       vars={"base_y": round(y, 2)})
                ph.place(stra, models["unit_block"], f"{name} Block", (0, 0, 0), parent=name)
                stra.op("entity_create", name=f"{name} Face", parent=name, position=[0, 5.5, 3.06], scale=[13.0, 9.6, 1.0],
                        components={"mesh": {"mesh": "quad", "texture": f"art/counter_{nid}_{kind}.png", "roughness": 0.7,
                                             "castShadows": False}})
                k += 1
        counts.append(k)
    stra.flush("Unit blocks")
    for s in range(STAGES):
        for k in range(counts[s]):
            stra.op("entity_update", entity=f"Unit S{s} {k} Block", components={"mesh": {"material": wood}})
            if s != 0:
                stra.op("entity_update", entity=f"Unit S{s} {k}", enabled=False)
    stra.flush("Blocks: wood, stage 0 only")
    return counts


def arrows(stra, pix, specs, models):
    pix.material("materials/arrow_attack.mat.json", color="#9c0d07", roughness=0.5, metallic=0.0, clearcoat=0.25,
                 emissive=[1.0, 0.12, 0.05, 0.2])
    pix.material("materials/arrow_supply.mat.json", color="#c9a24a", roughness=0.55, metallic=0.15, clearcoat=0.2,
                 emissive=[1.0, 0.8, 0.4, 0.03])
    for sp in specs:
        mat = "materials/arrow_attack.mat.json" if sp["kind"] == "attack" else "materials/arrow_supply.mat.json"
        name = f"Arrow {sp['name']}"
        stra.op("entity_create", name=name, position=[0, 0, 0], tags=["arrow", sp["kind"], f"stage{sp['stage']}"],
                components={"mesh": {"mesh": f"asset:{MINIS}/{sp['name']}.glb", "material": mat, "castShadows": True}})
    stra.flush("Attack and supply arrows")
    for sp in specs:
        if sp["stage"] != 0:
            stra.op("entity_update", entity=f"Arrow {sp['name']}", enabled=False)
    stra.flush("Arrows: stage 0 only")


def forests(cir, models):
    cir.call("foliage_add", entity="Continent", name="Forests", seed=7, layers=[
        {"preset": "custom", "mesh": f"asset:{MINIS}/tree_conifer.glb", "density": 0.016, "scaleMin": 4.5, "scaleMax": 7.5,
         "terrainLayer": 6, "slopeMax": 35, "heightMin": 2.5, "wind": 0.0, "cullDistance": 5000, "castShadows": True,
         "clumping": 0.5, "color": "#3e5a34", "impostors": False},
        {"preset": "custom", "mesh": f"asset:{MINIS}/tree_broadleaf.glb", "density": 0.011, "scaleMin": 4.5, "scaleMax": 7.0,
         "terrainLayer": 6, "slopeMax": 30, "heightMin": 2.0, "wind": 0.0, "cullDistance": 5000, "castShadows": True,
         "clumping": 0.5, "color": "#5d7438", "impostors": False},
    ])


# ============================================================================================
# HUD
# ============================================================================================
UI_STYLE = {
    "format": "skywalker.uistyle", "extends": "dark",
    "vars": {"gold": "#d9b779", "ink": "#ece4d2", "muted": "#a59c8a", "panel": "#141a22e8", "panel2": "#0c1016f0",
             "line": "#d9b77955"},
    "rules": {
        "canvas": {"font": "Inter", "fontSize": 16, "color": "$ink"},
        ".bar": {"background": "#1a212bf2", "background2": "#0d1117f2", "borderWidth": 1, "borderColor": "$line",
                 "shadowColor": "#000000a0", "shadowOffset": [0, 4], "shadowBlur": 18},
        ".frame": {"background": "$panel", "background2": "$panel2", "borderWidth": 1, "borderColor": "$line", "radius": 3,
                   "shadowColor": "#000000b0", "shadowOffset": [0, 8], "shadowBlur": 26},
        ".res": {"fontSize": 17, "bold": True, "color": "$ink"},
        ".res_up": {"fontSize": 13, "color": "#8fd18a"},
        ".res_down": {"fontSize": 13, "color": "#e07a62"},
        ".cap": {"fontSize": 11, "letterSpacing": 0.18, "textTransform": "uppercase", "color": "$muted"},
        ".title": {"font": "serif", "fontSize": 30, "color": "#f3e6c8", "letterSpacing": 0.04},
        ".nation": {"font": "serif", "fontSize": 25, "color": "#f3e6c8"},
        ".body": {"font": "serif", "fontSize": 19, "lineSpacing": 1.3, "color": "#e6dccb"},
        ".date": {"font": "serif", "fontSize": 24, "color": "#f3e6c8", "textAlign": "center"},
        ".chip": {"background": "#00000040", "borderWidth": 1, "borderColor": "#ffffff14", "radius": 2, "padding": [5, 10]},
        "button": {"background": "#232b36", "background2": "#161c24", "borderWidth": 1, "borderColor": "#d9b77966",
                   "radius": 2, "color": "$ink", "fontSize": 15, "padding": [8, 14],
                   "hover": {"borderColor": "#f0d49a", "background": "#2c3644"},
                   "pressed": {"background": "#3a2f1e"}},
        ".mode": {"fontSize": 11, "letterSpacing": 0.08, "textTransform": "uppercase", "padding": [6, 7]},
        ".active": {"background": "#6a5230", "background2": "#3e2f1a", "borderColor": "#f0d49a", "color": "#fff3d6"},
        ".speed": {"fontSize": 15, "padding": [4, 10]},
        ".pip": {"radius": 1, "background": "#e0bd78", "background2": "#a8843f"},
        ".pip_off": {"radius": 1, "background": "#ffffff1a"},
        ".choice": {"font": "serif", "fontSize": 19, "textAlign": "left", "padding": [12, 18], "background": "#2a2418",
                    "background2": "#1b170f", "borderColor": "#d9b77999", "hover": {"borderColor": "#ffe3a8"}},
        ".event": {"background": "#1b1914f6", "background2": "#100f0cf6", "borderWidth": 2, "borderColor": "#c9a96a",
                   "radius": 2, "shadowColor": "#000000d0", "shadowOffset": [0, 16], "shadowBlur": 60},
        ".tip": {"background": "#0f141bf0", "borderWidth": 1, "borderColor": "#d9b77980", "radius": 2, "padding": [10, 14],
                 "shadowColor": "#00000090", "shadowOffset": [0, 6], "shadowBlur": 16},
        "progress": {"accent": "#d9b779", "track": "#ffffff14", "trackHeight": 6, "radius": 3},
    },
}


def res(name, icon, value, delta=None, up=True):
    kids = [{"type": "image", "name": f"{name} Icon", "image": f"ui/icon_{icon}.png", "size": [22, 22]},
            {"type": "text", "name": name, "text": value, "style": "res", "fit": "both"}]
    if delta:
        kids.append({"type": "text", "name": f"{name} Delta", "text": delta, "style": "res_up" if up else "res_down", "fit": "both"})
    return {"type": "panel", "name": f"{name} Chip", "layout": "row", "gap": 7, "align": "center", "fit": "both", "style": "chip",
            "children": kids}


def hud(stra, w):
    pips = [{"type": "panel", "name": f"Pip {k + 1}", "size": [9, 18], "style": "pip" if k < 3 else "pip_off"} for k in range(5)]
    modes = [{"type": "button", "name": m, "text": m, "style": "mode" + (" active" if m == "Political" else ""), "fit": "both"}
             for m in ("Political", "Terrain", "Supply", "Paper")]
    stra.call("ui_create", canvas={"name": "HUD", "theme": "dark", "styleSheet": "ui/meridian.uistyle.json", "sortOrder": 10}, elements=[
        # --- top bar: flag, resources, date and speed ---
        {"type": "panel", "name": "Top Bar", "anchor": "top_stretch", "size": [0, 58], "margin": [0, 0, 0, 0], "style": "bar",
         "layout": "row", "gap": 10, "align": "center", "padding": [0, 18, 0, 196], "children": [
             res("Political Power", "pp", "148", "+2.10"), res("Stability", "stability", "62%"),
             res("War Support", "war", "71%", "+0.4%"), res("Manpower", "manpower", "1.42M"),
             res("Factories", "industry", "38 | 12"), res("Fuel", "fuel", "4.2k", "-31", up=False),
             {"type": "spacer", "name": "Top Spacer", "flex": 1}]},
        {"type": "panel", "name": "Clock", "anchor": "top_right", "position": [-14, 8], "size": [330, 74], "style": "frame",
         "layout": "column", "gap": 6, "align": "center", "padding": [8, 12], "children": [
             {"type": "text", "name": "Date", "text": "14 March 1937", "style": "date", "fit": "both"},
             {"type": "panel", "name": "Speed Row", "layout": "row", "gap": 6, "align": "center", "fit": "both", "children": [
                 {"type": "button", "name": "Pause", "text": "II", "style": "speed", "fit": "both"},
                 {"type": "button", "name": "Slower", "text": "-", "style": "speed", "fit": "both"},
                 {"type": "panel", "name": "Pips", "layout": "row", "gap": 4, "fit": "both", "children": pips},
                 {"type": "button", "name": "Faster", "text": "+", "style": "speed", "fit": "both"}]}]},
        # --- the player's flag and leader (top left, over the bar) ---
        {"type": "panel", "name": "Leader Frame", "anchor": "top_left", "position": [12, 8], "size": [170, 128], "style": "frame",
         "children": [
             {"type": "image", "name": "Leader Portrait", "image": "ui/portrait_leader.png", "anchor": "top_left", "position": [8, 8],
              "size": [90, 112]},
             {"type": "image", "name": "Player Flag", "image": f"maps/flags/{PLAYER}.png", "anchor": "top_left", "position": [106, 10],
              "size": [56, 37]},
             {"type": "text", "name": "Player Rank", "text": "#3", "anchor": "top_left", "position": [108, 56], "style": "cap",
              "fit": "both"},
             {"type": "text", "name": "Player Power", "text": "Major", "anchor": "top_left", "position": [108, 74], "fit": "both",
              "css": {"fontSize": 14, "color": "#e0bd78"}}]},
        # --- nation panel ---
        {"type": "panel", "name": "Nation Panel", "anchor": "top_left", "position": [12, 146], "size": [330, 0], "fit": "height",
         "style": "frame", "layout": "column", "gap": 9, "padding": [14, 16], "children": [
             {"type": "text", "name": "Nation Name", "text": "Republic of Kestria", "style": "nation", "fit": "both"},
             {"type": "text", "name": "Nation Gov", "text": "Directorial Republic", "style": "cap", "fit": "both"},
             {"type": "text", "name": "Nation Leader", "text": "Marshal Aurel Kostin, Head of State", "fit": "both",
              "css": {"fontSize": 14, "color": "#cfc6b4"}},
             {"type": "text", "name": "Focus Label", "text": "National focus", "style": "cap", "fit": "both"},
             {"type": "text", "name": "Focus Name", "text": "The Vey Offensive", "fit": "both", "css": {"font": "serif", "fontSize": 19}},
             {"type": "progress", "name": "Focus Progress", "value": 0.35, "size": [298, 6]},
             {"type": "text", "name": "War Label", "text": "At war with", "style": "cap", "fit": "both"},
             {"type": "panel", "name": "War Row", "layout": "row", "gap": 10, "align": "center", "fit": "both", "children": [
                 {"type": "image", "name": "Enemy Flag", "image": "maps/flags/varosse.png", "size": [42, 28]},
                 {"type": "text", "name": "Enemy Name", "text": "Kingdom of Varosse", "fit": "both", "css": {"font": "serif", "fontSize": 18}},
                 {"type": "text", "name": "War Score", "text": "+12", "fit": "both", "css": {"color": "#8fd18a", "bold": True}}]},
             {"type": "text", "name": "Front Label", "text": "Army Group West  ·  14 divisions", "style": "cap", "fit": "both"},
             {"type": "progress", "name": "Front Strength", "value": 0.78, "size": [298, 6]}]},
        # --- province tooltip ---
        {"type": "panel", "name": "Tooltip", "anchor": "top_left", "position": [1030, 500], "size": [300, 0], "fit": "height",
         "style": "tip", "layout": "column", "gap": 5, "visible": False, "children": [
             {"type": "text", "name": "Tip Title", "text": "Pont-Aurel", "fit": "both", "css": {"font": "serif", "fontSize": 22, "color": "#f3e6c8"}},
             {"type": "text", "name": "Tip Owner", "text": "Kingdom of Varosse", "style": "cap", "fit": "both"},
             {"type": "text", "name": "Tip Body", "text": "Plains  ·  River crossing\nVictory points: 5\nSupply: 64%",
              "fit": "both", "css": {"fontSize": 14, "lineSpacing": 1.35, "color": "#cfc6b4"}}]},
        # --- minimap and map modes ---
        {"type": "panel", "name": "Map Panel", "anchor": "bottom_right", "position": [-14, -14], "size": [300, 0], "fit": "height",
         "style": "frame",
         "layout": "column", "gap": 8, "padding": [10, 10], "children": [
             {"type": "panel", "name": "Mode Row", "layout": "row", "gap": 4, "justify": "space_between", "fit": "height",
              "children": modes},
             {"type": "image", "name": "Minimap", "image": "maps/minimap_0.png", "size": [280, 280]}]},
        # --- event popup ---
        {"type": "panel", "name": "Event", "anchor": "center", "position": [0, -10], "size": [640, 0], "fit": "height",
         "style": "event", "layout": "column", "gap": 14, "padding": [20, 24, 22, 24], "visible": False, "children": [
             {"type": "text", "name": "Event Kicker", "text": "Kestrian Front  ·  News event", "style": "cap", "fit": "both"},
             {"type": "text", "name": "Event Title", "text": "The Crossing at Pont-Aurel", "style": "title", "fit": "both"},
             {"type": "image", "name": "Event Image", "image": "ui/event_crossing.jpg", "size": [592, 254]},
             {"type": "text", "name": "Event Body", "style": "body", "size": [592, 0], "fit": "height",
              "text": "At first light the engineers of the Fourth Corps threw a pontoon across the Vey under fire. By noon two "
                      "regiments stood on the western bank. Castelvaro has recalled its ambassador, and the Accord of Meridian, "
                      "eleven years old this spring, is now a dead letter."},
             {"type": "button", "name": "Choice A", "text": "Press on to Castelvaro.   <color=#8fd18a>+10% war support</color>",
              "style": "choice", "size": [592, 0], "fit": "height"},
             {"type": "button", "name": "Choice B", "text": "Hold the bridgehead and sue for terms.   <color=#e0bd78>+50 political power</color>",
              "style": "choice", "size": [592, 0], "fit": "height"}]},
        # --- a pointer for filmed UI interactions (hidden in play) ---
        {"type": "image", "name": "Cursor", "image": "ui/cursor.png", "anchor": "top_left", "position": [1500, 700], "size": [30, 38],
         "visible": False, "interactable": False},
    ])


def ui_art(project):
    """A pointer for filmed interactions."""
    from PIL import Image, ImageDraw
    os.makedirs(os.path.join(project, "ui"), exist_ok=True)
    im = Image.new("RGBA", (120, 152), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pts = [(8, 6), (8, 118), (36, 92), (56, 140), (78, 130), (58, 84), (98, 84)]
    d.polygon([(x + 4, y + 6) for x, y in pts], fill=(0, 0, 0, 110))
    d.polygon(pts, fill=(250, 244, 230, 255), outline=(20, 16, 12, 255))
    d.line(pts + [pts[0]], fill=(20, 16, 12, 255), width=5)
    im.save(os.path.join(project, "ui", "cursor.png"))
    with open(os.path.join(project, "ui", "meridian.uistyle.json"), "w") as f:
        json.dump(UI_STYLE, f, indent=2)


# ============================================================================================
# Wander
# ============================================================================================
def letter_names(w):
    out = []
    for nat in w["nations"]:
        out += [f"Label {nat['name']} {i}" for i in range(len(nat["name"]))]
    return out


def campaign_source(counts, arrow_names, letters, cities):
    return f"""behavior Campaign
  intent "The grand campaign: the clock ticks (pause and five speeds), Kestria's offensive pushes the front into Varosse through five map stages, the crossing at Pont-Aurel raises a news event, and the map modes switch the overlay."
  var day = 14
  var month = 3
  var year = 1937
  var speed = 3
  var paused = false
  var acc = 0
  var stage = 0
  var days = 0
  var mode = "political"
  var event_done = false
  var pp = 148.0
  var war = 71.0
  var counts = {counts}
  var arrows = {arrow_names}
  var months = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"]
  var lengths = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
  var rates = [0, 1, 2, 4, 8, 15]
  var stage_days = 20
  var auto_front = true
  var letters = {letters}
  var cities = {cities}
  var label_a = 1.0
  var label_target = 1.0
  var city_a = 1.0
  var city_target = 1.0

  fn show_stage(s: number)
    for k in 0..{STAGES}
      for i in 0..counts[k]
        let u = find("Unit S" + str(k) + " " + str(i))
        if k == s then
          if not u.enabled then
            u.enabled = true
            u.position = (u.position.x, u.base_y - 26, u.position.z)
          end
        else
          u.enabled = false
        end
      end
    end
    for a in arrows
      find(a).enabled = a.contains("_s" + str(s) + "_") or a.ends_with("supply_s" + str(s))
    end
    find("Minimap").ui.image = "maps/minimap_" + str(s) + ".png"
    if mode == "political" then
      find("Continent").terrain.overlay = "maps/political_" + str(s) + ".png"
    end
    find("Focus Progress").ui.value = 0.35 + s * 0.15
    find("War Score").ui.text = "+" + str(12 + s * 9)
    if s >= 2 then
      find("Tip Owner").ui.text = "Kingdom of Varosse  ·  occupied by Kestria"
    else
      find("Tip Owner").ui.text = "Kingdom of Varosse"
    end
  end

  fn set_mode(m: string)
    mode = m
    let t = find("Continent")
    find("Meridian Sea").enabled = m != "paper"
    if m == "political" then
      t.terrain.overlay = "maps/political_" + str(stage) + ".png"
      t.terrain.overlayOpacity = 0.92
    elif m == "terrain" then
      t.terrain.overlay = "maps/terrain.png"
      t.terrain.overlayOpacity = 0.85
    elif m == "supply" then
      t.terrain.overlay = "maps/supply.png"
      t.terrain.overlayOpacity = 0.9
    else
      t.terrain.overlay = "maps/paper.jpg"
      t.terrain.overlayOpacity = 1.0
    end
    for b in ["Political", "Terrain", "Supply", "Paper"]
      if b.lower() == m then
        find(b).ui.style = "mode active"
      else
        find(b).ui.style = "mode"
      end
    end
  end

  fn set_speed(s: number)
    speed = clamp(s, 1, 5)
    for k in 1..6
      if k <= speed then
        find("Pip " + str(k)).ui.style = "pip"
      else
        find("Pip " + str(k)).ui.style = "pip_off"
      end
    end
  end

  fn next_day()
    day += 1
    days += 1
    if day > lengths[month - 1] then
      day = 1
      month += 1
      if month > 12 then
        month = 1
        year += 1
      end
    end
    pp += 2.1
    war = min(99, war + 0.08)
    find("Political Power").ui.text = str(floor(pp))
    find("War Support").ui.text = str(floor(war)) + "%"
    if auto_front and stage < {STAGES - 1} and days >= (stage + 1) * stage_days then
      stage += 1
      show_stage(stage)
      if stage == 2 and not event_done then
        emit "event_show"
      end
    end
  end

  fn go_stage(s: number)
    stage = s
    days = s * stage_days
    show_stage(s)
  end

  on start
    show_stage(0)
    set_speed(2)
  end

  on tick
    if abs(label_a - label_target) > 0.001 then
      label_a = approach(label_a, label_target, dt * 1.6)
      for n in letters
        find(n).text.color = color(0.07, 0.05, 0.035, 0.76 * label_a)
      end
    end
    if abs(city_a - city_target) > 0.001 then
      city_a = approach(city_a, city_target, dt * 2.0)
      for n in cities
        let t = find(n)
        t.text.color = color(1.0, 0.965, 0.894, 0.96 * city_a)
        t.text.outlineColor = color(0.1, 0.08, 0.06, 0.75 * city_a)
      end
    end
    if not paused then
      acc += dt * rates[speed]
      while acc >= 1
        acc -= 1
        next_day()
      end
    end
    find("Date").ui.text = str(day) + " " + months[month - 1] + " " + str(year)
    if paused then
      find("Pause").ui.text = ">"
    else
      find("Pause").ui.text = "II"
    end
  end

  on key "space"
    paused = not paused
  end
  on ui "Pause"
    paused = not paused
  end
  on ui "Faster"
    set_speed(speed + 1)
  end
  on ui "Slower"
    set_speed(speed - 1)
  end
  on key "1"
    set_mode("political")
  end
  on key "2"
    set_mode("terrain")
  end
  on key "3"
    set_mode("supply")
  end
  on key "4"
    set_mode("paper")
  end
  on ui "Political"
    set_mode("political")
  end
  on ui "Terrain"
    set_mode("terrain")
  end
  on ui "Supply"
    set_mode("supply")
  end
  on ui "Paper"
    set_mode("paper")
  end
  on event "mode_political"
    set_mode("political")
  end
  on event "mode_terrain"
    set_mode("terrain")
  end
  on event "mode_supply"
    set_mode("supply")
  end
  on event "mode_paper"
    set_mode("paper")
  end
  on event "event_show"
    event_done = true
    paused = true
    find("Event").ui.visible = true
  end
  on event "event_hide"
    find("Event").ui.visible = false
  end
  on ui "Choice A"
    find("Event").ui.visible = false
    war = min(99, war + 10)
    paused = false
  end
  on ui "Choice B"
    find("Event").ui.visible = false
    pp += 50
    paused = false
  end
  on event "stage_next"
    if stage < {STAGES - 1} then
      stage += 1
      days = stage * stage_days
      show_stage(stage)
    end
  end
  on event "freeze"
    paused = true
  end
  on event "hold_front"
    auto_front = false
  end
  on event "stage_0"
    go_stage(0)
  end
  on event "stage_1"
    go_stage(1)
  end
  on event "stage_2"
    go_stage(2)
  end
  on event "stage_3"
    go_stage(3)
  end
  on event "stage_4"
    go_stage(4)
  end
  on event "tip_show"
    find("Tooltip").ui.visible = true
  end
  on event "tip_hide"
    find("Tooltip").ui.visible = false
  end
  on event "labels_near"
    label_target = 0
  end
  on event "labels_far"
    label_target = 1
  end
  on event "city_labels_off"
    city_target = 0
  end
  on event "city_labels_on"
    city_target = 1
  end
  on event "hud_hide"
    find("HUD").enabled = false
  end
  on event "hud_show"
    find("HUD").enabled = true
  end
end"""


BLOCK_SETTLE = """behavior Settle
  intent "A unit block drops onto the map when its stage begins and settles with a small bounce."
  var base_y = 0
  var v = 0
  on tick
    let p = self.position
    if p.y < base_y - 0.01 or abs(v) > 0.01 then
      v += (base_y - p.y) * 60 * dt - v * 9 * dt
      self.position = (p.x, p.y + v * dt * 6, p.z)
    end
  end
end"""

ARROW_PULSE = """behavior Pulse
  intent "Offensive arrows breathe so the eye follows the push."
  on tick
    self.mesh.emissive = color(1.0, 0.16, 0.08, 0.15 + 0.35 * (0.5 + 0.5 * sin(time * 2.6)))
  end
end"""

FLAG_SWAY = """behavior Sway
  intent "Capital flags turn a little in the wind."
  var yaw0 = 0
  on start
    yaw0 = self.rotation.y
  end
  on tick
    self.rotation = (0, yaw0 + sin(time * 0.7 + self.position.x * 0.01) * 6, 0)
  end
end"""

MAP_CAMERA = """behavior MapCamera
  intent "Strategy camera: WASD / arrows pan over the map, Q / E zoom; the view tilts as it zooms in."
  param pan = 1.0 in 0.2..3 "pan speed (fraction of height per second)"
  var cx = -140
  var cz = 40
  var h = 1700
  var near = false
  on tick
    if h < 700 and not near then
      near = true
      emit "labels_near"
    elif h > 760 and near then
      near = false
      emit "labels_far"
    end
    let k = pan * h * dt
    if key("w") or key("up") then
      cz -= k
    end
    if key("s") or key("down") then
      cz += k
    end
    if key("a") or key("left") then
      cx -= k
    end
    if key("d") or key("right") then
      cx += k
    end
    if key("q") then
      h = max(140, h * (1 - 1.2 * dt))
    end
    if key("e") then
      h = min(2600, h * (1 + 1.2 * dt))
    end
    cx = clamp(cx, -1100, 1100)
    cz = clamp(cz, -1100, 1100)
    let back = h * (0.42 + 0.35 * (1 - h / 2600))
    self.position = (cx, h, cz + back)
    look self at (cx, 0, cz)
  end
end"""


# ============================================================================================
# Build
# ============================================================================================
def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    project = studio.project
    generate_maps(project)
    ui_art(project)
    w = world(project)
    relief = Relief(project, w)
    nim.call("scene_new", name="Meridian Accord", empty=True)

    specs = arrow_specs(w, relief)
    models = run_dcc(pix, dict(towns=TOWN_MODELS, block=True, flag=True, trees=True,
                               arrows=[{k: v for k, v in s.items() if k not in ("stage", "kind")} for s in specs]))

    continent(pix, cir, w, project)
    sea(aur)
    lighting(aur)
    forests(cir, models)
    cities(cir, pix, w, relief, models)
    labels(cir, w, relief)
    counts = units(stra, pix, w, relief, models)
    arrows(stra, pix, specs, models)

    # Camera, HUD and the campaign script.
    stra.op("entity_create", name="Map Camera", position=[-140, 1700, 900],
            components={"camera": {"fov": 38, "nearPlane": 2.0, "farPlane": 12000, "primary": True}})
    stra.flush("Map camera")
    stra.behave("Map Camera", "MapCamera", "Pan with WASD, zoom with Q/E; tilt follows zoom.", MAP_CAMERA)
    hud(stra, w)
    stra.op("entity_create", name="Campaign", position=[0, 0, 0])
    stra.flush("Campaign")
    arrow_names = [f"Arrow {s['name']}" for s in specs]
    stra.behave("Campaign", "Campaign", "Clock, front stages, event and map modes.",
                campaign_source(counts, arrow_names, letter_names(w), [f"City Label {c['name']}" for c in w["cities"]]))
    for s in range(STAGES):
        for k in range(counts[s]):
            stra.behave(f"Unit S{s} {k}", "Settle", "Drop in and settle.", BLOCK_SETTLE)
    for sp in specs:
        if sp["kind"] == "attack":
            stra.behave(f"Arrow {sp['name']}", "Pulse", "Breathe.", ARROW_PULSE)
    for c in w["cities"]:
        if c["capital"]:
            stra.behave(f"Flag {c['nation'].title()}", "Sway", "Turn in the wind.", FLAG_SWAY)
    stra.flush("Behaviors")
    add_shots(nim, w)
    nim.call("scene_save", path="scenes/main.sky.json")


# ============================================================================================
# Film: hero shots. Each is a sequence asset (cinematics/<name>.sequence.json) that starts with
# the simulation: a camera path (Catmull-Rom through points, looking at a target) plus events that
# drive the campaign (front stage, map mode, HUD, event popup) and a pointer for UI interactions.
# `still` is the moment the 1920x1080 still is taken.
# ============================================================================================
PONT_AUREL = (-518.0, 30.0, 20.0)
MIROVAN = (-120.0, 38.0, -31.0)

SHOTS = [
    dict(name="continent", dur=7.0, still=4.0, hud=True, fov=40,
         points=[(-150, 2150, 1640), (-150, 2000, 1500), (-150, 1860, 1380)], target=(-150, 0, -60),
         events=[(0.0, "hold_front")]),
    dict(name="above_the_clouds", dur=7.0, still=3.5, hud=False, fov=44,
         points=[(-1000, 1080, 1620), (-920, 1020, 1480), (-840, 960, 1340)], target=(-300, 60, 100),
         events=[(0.0, "freeze"), (0.0, "stage_1")]),
    dict(name="zoom_to_pont_aurel", dur=8.0, still=7.4, hud=True, fov=38,
         points=[(-160, 2050, 1500), (-330, 980, 820), (-470, 420, 360), (-495, 330, 300)], target=PONT_AUREL,
         events=[(0.0, "freeze"), (0.0, "stage_1"), (4.6, "labels_near"), (6.4, "tip_show")]),
    dict(name="front_line", dur=7.0, still=5.0, hud=False, fov=36, aperture=2.8, focus=200,
         points=[(-270, 108, 262), (-290, 102, 238), (-310, 98, 214)], target=(-470, 32, 110),
         events=[(0.0, "freeze"), (0.0, "labels_near"), (0.0, "stage_1"), (1.2, "stage_2")]),
    dict(name="capital_mirovan", dur=7.0, still=4.0, hud=False, fov=30, aperture=2.2, focus=135,
         points=[(-30, 112, 56), (-68, 106, 84), (-108, 102, 96)], target=MIROVAN,
         events=[(0.0, "freeze"), (0.0, "labels_near"), (0.0, "city_labels_off")]),
    dict(name="map_modes", dur=7.0, still=6.2, hud=True, fov=40,
         points=[(-60, 1500, 1050), (-110, 1450, 1000), (-160, 1400, 950)], target=(-120, 0, -80),
         events=[(0.0, "hold_front"), (0.0, "stage_2"), (2.4, "mode_terrain"), (4.6, "mode_supply")],
         cursor=[(0.0, (1380, 620)), (2.0, (1728, 754)), (2.6, (1730, 756)), (4.2, (1797, 754)), (6.8, (1800, 756))]),
    dict(name="news_event", dur=7.0, still=5.5, hud=True, fov=38,
         points=[(-300, 1250, 900), (-320, 1200, 860), (-340, 1150, 820)], target=(-420, 0, 0),
         events=[(0.0, "freeze"), (0.0, "stage_2"), (0.8, "event_show")],
         cursor=[(0.0, (1300, 980)), (3.6, (900, 784)), (5.0, (905, 786)), (7.0, (910, 788))]),
    dict(name="war_table", dur=7.0, still=4.5, hud=False, fov=34, aperture=2.4, focus=230, exposure=0.78,
         points=[(-340, 130, 400), (-368, 122, 372), (-396, 116, 344)], target=(-520, 26, 160),
         events=[(0.0, "freeze"), (0.0, "stage_3"), (0.0, "mode_paper")]),
]


def add_shots(nim, w):
    for sh in SHOTS:
        path = f"cinematics/{sh['name']}.sequence.json"
        cam = f"Cam {sh['name']}"
        seq = f"Shot {sh['name']}"
        nim.call("sequence_create", path=path, duration=sh["dur"], entity=seq, play_on_start=False, overwrite=True)
        nim.call("sequence_camera_shot", sequence=path, camera=cam, shot="path", start=0, duration=sh["dur"],
                 points=[list(p) for p in sh["points"]], target=list(sh["target"]), fov=sh["fov"], ease="smooth")
        lens = {"fov": sh["fov"], "primary": False, "nearPlane": 2.0, "farPlane": 12000}
        if sh.get("aperture"):
            lens.update(aperture=sh["aperture"], focusDistance=sh["focus"])
        nim.call("entity_update", entity=cam, components={"camera": lens})
        # One event per key time (a key at an existing time replaces it): nudge simultaneous ones apart.
        events, used = [], set()
        for t, e in [(0.0, "hud_show" if sh["hud"] else "hud_hide")] + list(sh["events"]):
            while round(t, 3) in used:
                t += 0.005
            used.add(round(t, 3))
            events.append({"t": round(t, 3), "event": e, "target": "Campaign"})
        keys = []
        if sh.get("cursor"):
            keys.append({"entity": "Cursor", "property": "ui.visible", "t": 0.0, "value": True, "ease": "step"})
            for t, (x, y) in sh["cursor"]:
                keys.append({"entity": "Cursor", "property": "ui.position", "t": t, "value": [x, y], "ease": "smooth"})
        if sh.get("exposure"):
            keys.append({"property": "environment.exposure", "t": 0.0, "value": sh["exposure"], "ease": "step"})
        nim.call("sequence_key", sequence=path, events=events, keys=keys)


def shots():
    """showcase.py render hooks: start each shot's sequence with the simulation, then film it."""
    out = []
    for sh in SHOTS:
        seq = f"cinematics/{sh['name']}.sequence.json"

        def start(sky, seq=seq):
            sky.call("sequence_play", sequence=seq)
        out.append(dict(name=sh["name"], frames=int(sh["dur"] * 30), warmup=0, view="scene", start=start, still=sh["still"]))
    return out


def render_stills(project, out_dir="shots", width=1920, height=1080, samples=16, only=None):
    """Final stills: each shot's sequence is played from the start and captured at its `still` time (JPEG q90)."""
    import subprocess

    from sky import Sky
    os.makedirs(os.path.join(project, out_dir), exist_ok=True)
    for sh in SHOTS:
        if only and sh["name"] not in only:
            continue
        sky = Sky(project=project, scene="scenes/main.sky.json")
        sky.call("sim_control", action="play")
        sky.call("sequence_play", sequence=f"cinematics/{sh['name']}.sequence.json")
        sky.call("sim_control", action="step", ticks=max(1, int(round(sh["still"] * 60))))
        png = os.path.join(project, out_dir, sh["name"] + ".png")
        sky.call("viewport_capture", view="scene", width=width, height=height, samples=samples, annotate=False, overlays=False,
                 include_image=False, save_path=f"{out_dir}/{sh['name']}.png")
        sky.close()
        jpg = png[:-4] + ".jpg"
        subprocess.run(["sips", "-s", "format", "jpeg", "-s", "formatOptions", "90", png, "--out", jpg], check=True,
                       capture_output=True)
        os.remove(png)
        print(sh["name"], os.path.getsize(jpg) // 1024, "KB", flush=True)


def contact_sheet(project, out_dir="shots", cols=2, cell=(960, 540), gutter=8):
    from PIL import Image
    names = [sh["name"] for sh in SHOTS if os.path.exists(os.path.join(project, out_dir, sh["name"] + ".jpg"))]
    rows = (len(names) + cols - 1) // cols
    W, H = cols * cell[0] + (cols + 1) * gutter, rows * cell[1] + (rows + 1) * gutter
    sheet = Image.new("RGB", (W, H), (18, 18, 22))
    for i, n in enumerate(names):
        im = Image.open(os.path.join(project, out_dir, n + ".jpg")).convert("RGB").resize(cell, Image.LANCZOS)
        sheet.paste(im, (gutter + (i % cols) * (cell[0] + gutter), gutter + (i // cols) * (cell[1] + gutter)))
    path = os.path.join(project, out_dir, "contact_sheet.jpg")
    sheet.save(path, quality=88)
    print("contact sheet", os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
    proj = os.path.join(root, "examples", "meridian_accord")
    if len(sys.argv) > 1 and sys.argv[1] == "stills":
        render_stills(proj, only=sys.argv[2:] or None)
        contact_sheet(proj)
    elif len(sys.argv) > 1 and sys.argv[1] == "sheet":
        contact_sheet(proj)
