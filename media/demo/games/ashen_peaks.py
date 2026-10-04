"""Ashen Peaks — a mythic mountain valley (original IP; benchmark: modern AAA open worlds).

The Monastery of the Kindled Crane (Hakuen-ji) clings to a cliff above a still mountain lake.
A river winds down the valley through meadows and pine forest; mist pools on the valley floor at
dawn and storm light rakes the peaks at dusk. Everything is either Poly Haven (CC0) photoscans or
modelled procedurally in Blender by the crew (pagoda, halls, gate, stairs, lanterns, banners).
"""
import hashlib
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import polyhaven as ph  # noqa: E402
from kit import Studio, rnd  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ashen_peaks_art as art  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PROJECT = os.path.join(ROOT, "examples", "ashen_peaks")

META = dict(
    id="ashen_peaks", title="Ashen Peaks", genre="Mythic Action-Adventure", mood="misty sunrise",
    pitch="Nine hundred steps to the Kindled Crane. The lanterns have not gone out in a thousand years.",
    view=dict(eye=[60, 40, 140], target=[-300, 80, -190]),
    assets="Poly Haven (CC0) + procedural (Blender)",
)

# --- Layout (world meters; the valley runs along z, the floor is ~13-19 m high) -------------
WATER_Y = 11.6                       # river + lake surface
LAKE = (-150.0, -185.0, 95.0)        # center x, z and radius of the lake bed
PAD = (-335.0, -190.0, 88.0)         # monastery terrace center x, z and height
RIVER = [(40, 985), (15, 880), (-45, 700), (-20, 520), (20, 360), (-15, 200), (-70, 70), (-110, -60),
         (-150, -185), (-120, -300), (-70, -430), (-40, -600), (15, -780), (-5, -975)]
# Pilgrim stairs from the river up to the gate: (x, z, height) control points; landings in between.
STAIRS = [(-176.0, -22.0, 14.2), (-205.0, -47.0, 30.0), (-236.0, -76.0, 47.0), (-262.0, -104.0, 63.0), (-284.0, -127.0, 77.0),
          (-300.0, -145.0, PAD[2])]
TERRACE = [(-335.0, -190.0, 46.0), (-314.0, -160.0, 26.0), (-358.0, -148.0, 26.0), (-352.0, -200.0, 32.0),
           (-306.0, -214.0, 22.0), (-322.0, -176.0, 34.0)]  # flattened discs (x, z, radius) forming the terrace
AXIS = 45.0                          # the monastery faces north-east, down its stairs
# Buildings: (asset, name, x, z, yaw). Positions on the terrace (height PAD[2]).
BUILDINGS = [("gate", "Crane Gate", -307.0, -152.0, AXIS), ("hall", "Hall of the Kindled Crane", -350.0, -196.0, AXIS),
             ("pagoda", "Ember Pagoda", -362.0, -146.0, AXIS), ("bell", "Bell Pavilion", -306.0, -214.0, AXIS)]


def catmull(points, step):
    """Samples a Catmull-Rom spline through 2D points about every `step` meters."""
    pts = [points[0]] + list(points) + [points[-1]]
    out = []
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        seg = math.dist(p1, p2)
        n = max(1, int(seg / step))
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[c]) + (-p0[c] + p2[c]) * t + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2 +
                                    (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3) for c in range(2)))
    out.append(tuple(points[-1]))
    return out


LANDING = 2.6  # flat landing at the start of each flight (m), shared with the Blender stairs


def stair_path(step=1.0):
    """Points along the stairs (x, z, y): a flat landing at each control point, then a straight flight."""
    out = []
    for (x0, z0, y0), (x1, z1, y1) in zip(STAIRS, STAIRS[1:]):
        run = math.dist((x0, z0), (x1, z1))
        n = max(1, int(run / step))
        for k in range(n):
            d = run * k / n
            y = y0 if d < LANDING else y0 + (y1 - y0) * (d - LANDING) / (run - LANDING)
            out.append((x0 + (x1 - x0) * d / run, z0 + (z1 - z0) * d / run, y))
    out.append(STAIRS[-1])
    return out


# --- Terrain ---------------------------------------------------------------------------------
def sculpt_valley(cir):
    r = rnd(77)
    strokes = []
    # Close both ends of the valley with high spurs so it reads as a hidden basin (and the far
    # ranges, not the world edge, fill the gaps); the river cuts gorges through them.
    for zc in (1130, -1130):
        for x in range(-520, 521, 130):
            strokes.append(dict(x=x + r.uniform(-30, 30), z=zc + r.uniform(-40, 40), radius=r.uniform(190, 260),
                                strength=r.uniform(90, 150), mode="raise", falloff=0.9))
    # The monastery spur: raise a promontory out of the west slope, then cut the terrace.
    px, pz, py = PAD
    strokes += [dict(x=px + 10, z=pz, radius=95, strength=14, mode="raise", falloff=0.7)]
    strokes += [dict(x=x, z=z, radius=r + 40, strength=1, mode="flatten", target=py - 5.5, falloff=0.75) for x, z, r in TERRACE]
    strokes += [dict(x=x, z=z, radius=r, strength=1, mode="flatten", target=py, falloff=0.02) for x, z, r in TERRACE]
    # The lake: a basin against the foot of the cliff.
    lx, lz, lr = LAKE
    strokes += [dict(x=lx, z=lz, radius=lr * 1.45, strength=1, mode="flatten", target=WATER_Y + 0.8, falloff=0.6),
                dict(x=lx, z=lz, radius=lr, strength=1, mode="flatten", target=WATER_Y - 7, falloff=0.75),
                dict(x=lx - 30, z=lz + 40, radius=lr * 0.6, strength=1, mode="flatten", target=WATER_Y - 4, falloff=0.8),
                dict(x=lx + 20, z=lz - 50, radius=lr * 0.55, strength=1, mode="flatten", target=WATER_Y - 3, falloff=0.8)]
    cir.call("terrain_sculpt", entity="Valley", strokes=strokes)
    # The river: a meandering channel with soft banks.
    strokes = []
    for k, (x, z) in enumerate(catmull(RIVER, 3.0)):
        if math.dist((x, z), (lx, lz)) < lr * 0.95:
            continue
        w = 1.0 + 0.25 * math.sin(k * 0.05) + 0.15 * math.sin(k * 0.13)
        strokes.append(dict(x=x, z=z, radius=17 * w, strength=0.5, mode="flatten", target=WATER_Y + 0.9, falloff=0.9))
        strokes.append(dict(x=x, z=z, radius=9.5 * w, strength=1, mode="flatten", target=WATER_Y - 1.8, falloff=0.7))
    cir.call("terrain_sculpt", entity="Valley", strokes=strokes)
    # The pilgrim stairs ride a ramp of packed earth.
    strokes = []
    for x, z, y in stair_path(1.5):
        strokes.append(dict(x=x, z=z, radius=8, strength=1, mode="flatten", target=y - 0.28, falloff=0.75))
    cir.call("terrain_sculpt", entity="Valley", strokes=strokes)
    # Break up the brush shapes a little (cliff and spur faces only, not the floor).
    strokes = [dict(x=px + 60 + r.uniform(-15, 15), z=pz + r.uniform(-60, 60), radius=r.uniform(14, 26), strength=r.uniform(1.5, 3),
                    mode="noise", falloff=0.6) for _ in range(30)]
    cir.call("terrain_sculpt", entity="Valley", strokes=strokes)


def terrain_layers(pix):
    forest = ph.texture(pix, "forest_floor", tiling=1.0, triplanar=False)
    meadow = ph.texture(pix, "leafy_grass", tiling=1.0, triplanar=False)
    mossy = ph.texture(pix, "aerial_grass_rock", tiling=1.0, triplanar=False)
    cliff = ph.texture(pix, "rock_face_03", tiling=1.0, triplanar=True)
    snow = ph.texture(pix, "snow_02", tiling=1.0, triplanar=False)
    bed = ph.texture(pix, "river_small_rocks", tiling=1.0, triplanar=False)
    path = ph.texture(pix, "grass_path_2", tiling=1.0, triplanar=False)
    bank = ph.texture(pix, "brown_mud_leaves_01", tiling=1.0, triplanar=False)
    return [
        {"name": "forest floor", "material": forest, "tiling": 3.5, "color": "#a9b98f"},
        {"name": "meadow", "material": meadow, "tiling": 3.0, "color": "#b4cc8a", "heightMax": 175, "slopeMax": 21, "noise": 0.75, "sharpness": 0.35},
        {"name": "mossy scree", "material": mossy, "tiling": 7, "color": "#b9c29a", "slopeMin": 24, "slopeMax": 40, "noise": 0.6, "sharpness": 0.4},
        {"name": "cliff", "material": cliff, "tiling": 9, "color": "#b4bcc0", "slopeMin": 37, "noise": 0.5, "sharpness": 0.55, "triplanar": True},
        {"name": "snow", "material": snow, "tiling": 6, "heightMin": 300, "slopeMax": 42, "noise": 0.8, "sharpness": 0.6},
        {"name": "river bed", "material": bed, "tiling": 3, "heightMax": WATER_Y + 0.4, "sharpness": 0.7},
        {"name": "path", "material": path, "tiling": 3.5, "heightMin": 9000},
        {"name": "bank", "material": bank, "tiling": 3, "heightMin": WATER_Y + 0.2, "heightMax": WATER_Y + 1.6, "slopeMax": 30,
         "noise": 0.5, "sharpness": 0.5},
    ]


def build(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    nim.call("scene_new", name="Ashen Peaks", empty=True)
    nim.call("camera_set", eye=META["view"]["eye"], target=META["view"]["target"])

    # --- Cirro: the valley --------------------------------------------------------------------
    layers = terrain_layers(pix)
    cir.call("terrain_create", name="Valley", preset="mountain_valley", size=2400, resolution=2049, seed=11, layers=layers,
             generator={"maxHeight": 420, "featureSize": 900})
    sculpt_valley(cir)
    cir.call("terrain_layers", entity="Valley", layers=layers)  # re-paint from the rules after sculpting
    cir.call("entity_update", entity="Valley", components={"terrain": {"waterLevel": WATER_Y, "wetBand": 0.9, "macroVariation": 0.6}})
    # Hand-painted: worn earth along the stairs and the pilgrim path down to the bridge.
    cir.call("terrain_paint", entity="Valley", layer="meadow",
             strokes=[dict(x=x, z=z, radius=12, strength=1.0, falloff=0.6) for x, z, _ in stair_path(3.0)])
    cir.call("terrain_paint", entity="Valley", layer="path",
             strokes=[dict(x=x, z=z, radius=6.5, strength=1.0, falloff=0.6) for x, z, _ in stair_path(2.0)] +
             [dict(x=x, z=z, radius=r + 3, strength=1.0, falloff=0.3) for x, z, r in TERRACE])
    # The far ranges: a second, coarse terrain under the valley that fills every horizon.
    far_layers = [
        {"name": "forest", "material": layers[0]["material"], "tiling": 30, "color": "#5d7048"},
        {"name": "scree", "material": layers[2]["material"], "tiling": 40, "slopeMin": 26, "noise": 0.5},
        {"name": "cliff", "material": layers[3]["material"], "tiling": 60, "slopeMin": 36, "triplanar": True},
        {"name": "snow", "material": layers[4]["material"], "tiling": 40, "heightMin": 650, "slopeMax": 46, "noise": 0.7},
    ]
    cir.call("terrain_create", name="Far Ranges", preset="alpine", size=16000, resolution=1025, seed=29, layers=far_layers,
             position=[0, -40, 0], generator={"maxHeight": 1500, "featureSize": 2600})
    cir.call("terrain_sculpt", entity="Far Ranges", strokes=[
        dict(x=0, z=0, radius=1750, strength=1, mode="flatten", target=-40, falloff=0.6),
        dict(x=0, z=0, radius=800, strength=1, mode="flatten", target=-80, falloff=0.2)])
    cir.call("terrain_layers", entity="Far Ranges", layers=far_layers)
    cir.call("entity_update", entity="Far Ranges", components={"terrain": {"macroVariation": 0.7}})
    cir.flush("Valley")

    # --- Water: one still surface for the river and the lake ------------------------------------
    pix.call("fx_create", effect="lake", name="Kagami Lake", position=[0, WATER_Y, 0],
             overrides={"size": 2000, "windSpeed": 1.2, "waveScale": 0.22, "choppiness": 0.4, "patchSize": 40, "depth": 6,
                        "deepColor": "#06161a", "shallowColor": "#3d6a5c", "clarity": 2.6, "foam": 0.05, "roughness": 0.02})

    # --- The monastery ---------------------------------------------------------------------------------
    build_monastery(studio)

    # --- Foliage ------------------------------------------------------------------------------------
    grow(cir, pix)

    # --- Aurora: misty sunrise ------------------------------------------------------------------------
    aur.call("environment_update", **MOODS["sunrise"])
    nim.call("scene_save", path="scenes/main.sky.json")


# --- The Monastery of the Kindled Crane ---------------------------------------------------------
def local(x, z, yaw, lx, lz):
    """Building-local (lx, lz) -> world (x, z) for an entity at (x, z) rotated `yaw` degrees."""
    a = math.radians(yaw)
    return x + lx * math.cos(a) + lz * math.sin(a), z - lx * math.sin(a) + lz * math.cos(a)


def bridge_site():
    """Where the pilgrim path crosses the river: the river point nearest the foot of the stairs."""
    lx, lz, lr = LAKE
    pts = [p for p in catmull(RIVER, 2.0) if math.dist(p, (lx, lz)) > lr * 1.25]
    sx, sz, _ = STAIRS[0]
    k = min(range(len(pts)), key=lambda i: math.dist(pts[i], (sx + 40, sz + 5)))
    (x0, z0), (x1, z1) = pts[max(0, k - 2)], pts[min(len(pts) - 1, k + 2)]
    yaw = math.degrees(math.atan2(x1 - x0, z1 - z0))  # river direction; the bridge spans across it (local x)
    return pts[k][0], pts[k][1], yaw


def flag_anchors():
    _, _, px, pz, yaw = BUILDINGS[2]
    out = []
    for ang in (20, 110, 200, 290):
        lx, lz = math.sin(math.radians(ang)) * 27, math.cos(math.radians(ang)) * 27
        out.append(local(px, pz, yaw, lx, lz))
    return out


def monastery_materials(pix):
    def scan(asset_id, tiling, **extra):
        return ph.texture(pix, asset_id, tiling=tiling, triplanar=False, **extra)

    def painted(name, asset_id, color, tiling=0.6, roughness=0.75, **extra):
        scan(asset_id, tiling)  # make sure the scan is downloaded
        folder = f"downloads/{asset_id}"
        path = f"materials/{name}.mat.json"
        pix.material(path, color=color, normalMap=f"{folder}/{asset_id}_nor_gl_2k.jpg", ormMap=f"{folder}/{asset_id}_arm_2k.jpg",
                     metallic=1.0, roughness=roughness, tilingU=tiling, tilingV=tiling, occlusionStrength=1.0, triplanar=False, **extra)
        return path

    textures = os.path.join(PROJECT, "textures")
    os.makedirs(textures, exist_ok=True)
    art.banner(os.path.join(textures, "banner_crane.png"))
    art.plaque(os.path.join(textures, "plaque_crane.png"))
    M = {
        "roof": scan("grey_roof_tiles", 0.55),
        "timber": scan("dark_wood", 0.5, color="#9a8a7c"),
        "panel": scan("hinoki_planks", 0.5),
        "plaster": scan("white_plaster_rough_01", 0.4, color="#e8e0d0"),
        "stone": scan("japanese_stone_wall", 0.35),
        "granite": scan("rock_boulder_dry", 0.6, color="#a9a59c"),
        "paving": ph.texture(pix, "grey_stone_path", tiling=0.22, triplanar=True, color="#8c8984"),
        "lacquer": painted("lacquer_vermilion", "dark_wood", "#a3291a", roughness=0.62),
        "jade": painted("jade_paint", "dark_wood", "#3f7464", roughness=0.8),
        "ridge": painted("ridge_tile", "grey_roof_tiles", "#4a4a4e", tiling=0.8, roughness=0.7),
    }
    for name, fields in (
            ("paper", dict(color="#f0dcb4", emissive=[1.0, 0.66, 0.34, 0.9], roughness=0.85, subsurface=0.6)),
            ("redpaper", dict(color="#b3311f", emissive=[1.0, 0.34, 0.14, 2.6], roughness=0.8, subsurface=0.6)),
            ("ember", dict(color="#200a04", emissive=[1.0, 0.36, 0.08, 6.0], roughness=0.9)),
            ("bronze", dict(color="#6f5532", metallic=0.9, roughness=0.42)),
            ("gold", dict(color="#d8a64a", metallic=1.0, roughness=0.28)),
            ("banner", dict(texture="textures/banner_crane.png", color="#ffffff", roughness=0.9, subsurface=0.45, doubleSided=True)),
            ("plaque", dict(texture="textures/plaque_crane.png", color="#ffffff", roughness=0.45, clearcoat=0.4)),
            ("cloth", dict(color="#ffffff", roughness=0.9, subsurface=0.5, doubleSided=True))):
        path = f"materials/{name}.mat.json"
        pix.material(path, **fields)
        M[name] = path
    return M


def model_monastery(pix):
    """Pixel models the monastery kit in Blender (cached on the script + layout hash)."""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ashen_peaks_blender.py")).read()
    args = json.dumps({"stairs": [list(p) for p in STAIRS], "seed": 7, "land": LANDING,
                       "flags": {"pagoda": list(BUILDINGS[2][2:]), "pad": PAD[2], "anchors": [list(a) for a in flag_anchors()]}})
    digest = hashlib.sha1((src + args).encode()).hexdigest()
    out = os.path.join(PROJECT, "dcc", "monastery")
    stamp = os.path.join(out, ".build")
    if not (os.path.exists(stamp) and open(stamp).read() == digest):
        r = pix.call("dcc_run_script", script=src, name="monastery", out_dir="dcc/monastery", args=[args], timeout_s=1800,
                     description="Monastery of the Kindled Crane kit")
        with open(os.path.join(out, "assets.json"), "w") as f:
            json.dump(r.get("result", {}).get("assets", {}), f, indent=1)
        with open(stamp, "w") as f:
            f.write(digest)
    assets = json.load(open(os.path.join(out, "assets.json")))
    meshes = {}
    for key, parts in assets.items():
        for part, info in parts.items():
            res = pix.call("asset_import", path=f"dcc/monastery/{info['file']}", normalize=False)
            meshes.setdefault(key, {})[part] = res["mesh"]
    return meshes


PART_MAT = {"stone": "stone", "lacquer": "lacquer", "timber": "timber", "panel": "panel", "plaster": "plaster", "paper": "paper",
            "jade": "jade", "roof": "roof", "ridge": "ridge", "bronze": "bronze", "gold": "gold", "plaque": "plaque",
            "redpaper": "redpaper", "banner": "banner", "cloth": "cloth", "ember": "ember"}
GRANITE = {"lantern", "stairs", "bridge"}  # monolithic stone: no masonry pattern


def place_kit(b, meshes, M, key, name, pos, yaw=0.0, parent=None, tags=None, scale=1.0):
    b.e(name, pos=pos, rot=(0, yaw, 0), parent=parent, tags=tags, scale=scale)
    for part, mesh in meshes[key].items():
        mat = M["granite"] if (part == "stone" and key in GRANITE) else M[PART_MAT[part]]
        extra = {"castShadows": False} if part in ("paper", "redpaper", "ember", "plaque") else {}
        b.e(f"{name} {part}", mesh, parent=name, material=mat, **extra)
    return name


def build_monastery(studio):
    nim, pix, cir, aur, stra = (studio.agent(n) for n in ("Nimbus", "Pixel", "Cirro", "Aurora", "Stratus"))
    M = monastery_materials(pix)
    meshes = model_monastery(pix)
    y0 = PAD[2] - 0.08
    r = rnd(1717)

    # Courtyard paving along the axis, from the gate to the hall steps.
    gx, gz = BUILDINGS[0][2], BUILDINGS[0][3]
    hx, hz = BUILDINGS[1][2], BUILDINGS[1][3]
    cx, cz = (gx + hx) / 2, (gz + hz) / 2
    cir.e("Courtyard", "cube", pos=(cx, PAD[2] - 0.22, cz), rot=(0, AXIS, 0), scale=(30, 0.5, 66), material=M["paving"],
          tags=["ground"])
    for asset, name, x, z, yaw in BUILDINGS:
        place_kit(cir, meshes, M, asset, name, (x, y0, z), yaw, tags=["monastery"])
    place_kit(cir, meshes, M, "stairs", "Pilgrim Stairs", (0, 0, 0), 0, tags=["monastery"])
    if "flagsworld" in meshes:
        place_kit(cir, meshes, M, "flagsworld", "Prayer Flags", (0, 0, 0), 0)
    bx, bz, byaw = bridge_site()
    place_kit(cir, meshes, M, "bridge", "Moon Bridge", (bx, WATER_Y + 1.0, bz), byaw + 90, tags=["bridge"])
    cir.flush("The monastery, its stairs and the moon bridge")

    # Parapet wherever the terrace ends in a drop (found by walking out from the center).
    px, pz, py = PAD
    angles = list(range(0, 360, 5))
    radii = [30 + 1.5 * k for k in range(44)]
    pts = [[px + math.cos(math.radians(a)) * rr, pz + math.sin(math.radians(a)) * rr] for a in angles for rr in radii]
    hs = [p.get("height", 0) for p in cir.call("terrain_query", entity="Valley", points=pts)["points"]]
    rim = []
    for i, a in enumerate(angles):
        row = hs[i * len(radii):(i + 1) * len(radii)]
        k = next((j for j, h in enumerate(row) if h < py - 0.6), None)
        if k is None:
            rim.append(None)
            continue
        drop = py - row[min(len(row) - 1, k + 5)]
        rr = radii[k] - 1.6
        rim.append((px + math.cos(math.radians(a)) * rr, pz + math.sin(math.radians(a)) * rr) if drop > 4 else None)
    top = STAIRS[-1]
    k = 0
    for i in range(len(rim)):
        p0, p1 = rim[i], rim[(i + 1) % len(rim)]
        if not p0 or not p1:
            continue
        mx, mz = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
        if math.dist((mx, mz), top[:2]) < 13:
            continue
        L = math.dist(p0, p1)
        yaw = math.degrees(math.atan2(-(p1[1] - p0[1]), p1[0] - p0[0]))
        ox, oz = (p1[1] - p0[1]) / L, -(p1[0] - p0[0]) / L  # outward normal of the segment
        if ox * (mx - px) + oz * (mz - pz) < 0:
            ox, oz = -ox, -oz
        place_kit(cir, meshes, M, "parapet", f"Parapet {k + 1}", (mx - ox * 0.4, py - 0.15, mz - oz * 0.4), yaw,
                  scale=(L / 8 + 0.04, 1, 1))
        place_kit(cir, meshes, M, "terracewall", f"Terrace Wall {k + 1}", (mx - ox * 1.3, py - 0.2, mz - oz * 1.3),
                  math.degrees(math.atan2(ox, oz)), scale=(L / 8 + 0.06, 1, 1))
        k += 1
    cir.flush("Parapet along the cliff")


    # Stone lanterns: along the courtyard axis and on every stair landing.
    lamps = []
    for t in (9, 19, 29, 39):
        for side in (-1, 1):
            lamps.append((*local(gx, gz, AXIS, side * 6.5, -t), PAD[2]))
    for (x0, z0, y), (x1, z1, _) in zip(STAIRS, STAIRS[1:]):
        d = math.dist((x0, z0), (x1, z1))
        ux, uz = (x1 - x0) / d, (z1 - z0) / d
        for side in (-1, 1):
            lamps.append((x0 + ux * 1.3 - uz * side * 3.2, z0 + uz * 1.3 + ux * side * 3.2, y))
    for k, (x, z, y) in enumerate(lamps):
        place_kit(pix, meshes, M, "lantern", f"Stone Lantern {k + 1}", (x, y - 0.05, z), r.uniform(0, 60))
        pix.light(f"Lantern Light {k + 1}", (x, y + 1.85, z), "#ffad62", 2.2, 7.0, tags=["lantern"])
    pix.flush("Stone lanterns")

    # Paper lanterns under the eaves of the hall and the gate.
    hang = []
    for lx in (-11.0, -7.33, -3.67, 3.67, 7.33, 11.0):
        hang.append(("Hall", BUILDINGS[1], lx, 9.7, 7.85))
    for lx in (-4.0, 4.0):
        for lz in (-4.4, 4.4):
            hang.append(("Gate", BUILDINGS[0], lx, lz, 6.8))
    for k, (who, (_, _, bxw, bzw, yaw), lx, lz, y) in enumerate(hang):
        x, z = local(bxw, bzw, yaw, lx, lz)
        place_kit(pix, meshes, M, "paperlantern", f"{who} Lantern {k + 1}", (x, y0 + y, z), yaw, tags=["sway"])
        pix.light(f"{who} Lantern Light {k + 1}", (x, y0 + y - 0.35, z), "#ff7a3a", 2.4, 8.0, tags=["lantern"])
    pix.flush("Paper lanterns")

    # Braziers flanking the gate (real fluid fire) and banners up the stairs and in the courtyard.
    fires = []
    for side in (-1, 1):
        x, z = local(gx, gz, AXIS, side * 5.2, 7.6)
        place_kit(pix, meshes, M, "brazier", f"Brazier {'WE'[side > 0]}", (x, PAD[2] - 0.05, z), 0)
        fires.append((x, PAD[2] + 1.2, z))
    banners = []
    for i, ((x0, z0, y0s), (x1, z1, y1s)) in enumerate(zip(STAIRS, STAIRS[1:])):
        d = math.dist((x0, z0), (x1, z1))
        ux, uz = (x1 - x0) / d, (z1 - z0) / d
        side = 1 if i % 2 else -1
        t = 0.55
        x, z = x0 + (x1 - x0) * t - uz * side * 3.6, z0 + (z1 - z0) * t + ux * side * 3.6
        y = y0s + (y1s - y0s) * t
        face = math.degrees(math.atan2(-ux, -uz))  # cloth faces down the stairs
        banners.append((x, y, z, face))
    for side in (-1, 1):
        x, z = local(gx, gz, AXIS, side * 9.5, -4.0)
        banners.append((x, PAD[2], z, AXIS))
        x, z = local(hx, hz, AXIS, side * 13.5, 14.0)
        banners.append((x, PAD[2], z, AXIS))
    for k, (x, y, z, face) in enumerate(banners):
        place_kit(pix, meshes, M, "banner", f"Banner {k + 1}", (x, y - 0.3, z), face, tags=["banner"])
    pix.flush("Braziers and banners")
    for k, (x, y, z) in enumerate(fires):
        aur.call("fx_create", effect="torch", name=f"Gate Fire {k + 1}", position=[x, y, z])
    return lamps


def grow(cir, pix):
    fir = ph.model(pix, "fir_sapling_medium", res="1k")
    fir_s = ph.model(pix, "fir_sapling", res="1k")
    blossom = ph.model(pix, "jacaranda_tree", res="1k")
    broad = ph.model(pix, "island_tree_02", res="1k")
    shrubs = [ph.model(pix, f"shrub_0{i}", res="1k") for i in (1, 2, 3, 4)]
    fern = ph.model(pix, "fern_02")
    grass_m = ph.model(pix, "grass_medium_01")
    grass_m2 = ph.model(pix, "grass_medium_02")
    celandine = ph.model(pix, "celandine_01")
    periwinkle = ph.model(pix, "periwinkle_plant")
    dandelion = ph.model(pix, "dandelion_01")
    moss = ph.model(pix, "moss_01")
    rocks = ph.model(pix, "rock_moss_set_01")
    rocks2 = ph.model(pix, "rock_moss_set_02")
    boulder = ph.model(pix, "boulder_01", res="1k")
    log = ph.model(pix, "dead_tree_trunk")
    stump = ph.model(pix, "tree_stump_01")
    bark = ph.model(pix, "bark_debris_01")

    def L(res, **fields):
        layer = {"prefab": res["prefab"]} if res.get("prefab") else {"mesh": res["mesh"], "material": res.get("material", "")}
        layer.update(fields)
        return layer

    FOREST, MEADOW, SCREE, CLIFF, BANK = 0, 1, 2, 3, 7
    cir.call("foliage_add", entity="Valley", name="Forest", seed=4, layers=[
        L(fir, density=0.011, scaleMin=1.6, scaleMax=3.0, slopeMax=36, heightMin=WATER_Y + 1.5, heightMax=260, terrainLayer=FOREST,
          alignToNormal=0.05, clumping=0.85, cullDistance=2000, wind=0.3, randomTilt=2, subsurface=0.4),
        L(fir_s, density=0.012, scaleMin=1.0, scaleMax=2.2, slopeMax=36, heightMin=WATER_Y + 1.5, heightMax=280, terrainLayer=FOREST,
          clumping=0.8, cullDistance=700, wind=0.45, seed=3, subsurface=0.4),
        L(broad, density=0.0012, scaleMin=0.9, scaleMax=1.4, slopeMax=22, heightMin=WATER_Y + 1.5, heightMax=120, terrainLayer=FOREST,
          clumping=0.6, cullDistance=1500, wind=0.35, seed=8, subsurface=0.5),
        L(blossom, density=0.0005, scaleMin=0.7, scaleMax=1.1, slopeMax=18, heightMin=WATER_Y + 2, heightMax=110, terrainLayer=MEADOW,
          clumping=0.5, cullDistance=1500, wind=0.3, subsurface=0.5),
        *[L(sh, density=0.014 if i < 2 else 0.004, scaleMin=0.7, scaleMax=1.5, slopeMax=34, heightMin=WATER_Y + 0.8, heightMax=220,
            clumping=0.8, terrainLayer=FOREST if i < 2 else MEADOW, cullDistance=220, wind=0.6, seed=10 + i, subsurface=0.5)
          for i, sh in enumerate(shrubs)],
        L(fir, density=0.006, scaleMin=1.4, scaleMax=2.6, slopeMax=40, heightMin=40, heightMax=300, terrainLayer=SCREE,
          alignToNormal=0.05, clumping=0.9, cullDistance=2000, wind=0.3, randomTilt=3, seed=71, subsurface=0.4),
        L(fern, density=0.35, scaleMin=0.6, scaleMax=1.4, slopeMax=38, heightMax=220, terrainLayer=FOREST, clumping=0.8,
          cullDistance=80, wind=0.7, castShadows=False, subsurface=0.5),
        L(moss, density=0.25, scaleMin=1.0, scaleMax=2.2, slopeMax=40, heightMax=220, terrainLayer=FOREST, clumping=0.7,
          cullDistance=50, wind=0, castShadows=False, alignToNormal=1.0),
        L(bark, density=0.05, scaleMin=0.8, scaleMax=1.3, slopeMax=30, terrainLayer=FOREST, clumping=0.6, cullDistance=50,
          wind=0, castShadows=False, alignToNormal=1.0, seed=41),
        L(log, density=0.0015, scaleMin=0.8, scaleMax=1.3, slopeMax=25, terrainLayer=FOREST, clumping=0.4, cullDistance=180,
          wind=0, alignToNormal=0.9, sink=0.12, seed=42),
        L(stump, density=0.0012, scaleMin=0.8, scaleMax=1.2, slopeMax=25, terrainLayer=FOREST, clumping=0.4, cullDistance=150,
          wind=0, alignToNormal=0.5, sink=0.05, seed=43),
        L(rocks, density=0.006, scaleMin=0.8, scaleMax=2.2, slopeMax=45, clumping=0.6, cullDistance=400, alignToNormal=0.8, sink=0.3,
          wind=0, terrainLayer=SCREE),
        L(rocks2, density=0.002, scaleMin=0.6, scaleMax=1.6, slopeMax=45, clumping=0.6, cullDistance=400, alignToNormal=0.8, sink=0.3,
          wind=0, seed=44, terrainLayer=FOREST),
        L(boulder, density=0.0012, scaleMin=1.5, scaleMax=4.0, slopeMax=50, clumping=0.5, cullDistance=900, alignToNormal=0.6, sink=0.6,
          wind=0, seed=33, terrainLayer=SCREE),
        L(rocks2, density=0.004, scaleMin=0.5, scaleMax=1.2, slopeMax=45, clumping=0.7, cullDistance=300, alignToNormal=0.8, sink=0.2,
          wind=0, seed=45, terrainLayer=BANK),
        # pines clinging to the cliffs and rubble on the rock faces (an ink-painting silhouette)
        L(fir_s, density=0.004, scaleMin=1.0, scaleMax=2.0, slopeMin=30, slopeMax=70, terrainLayer=CLIFF, clumping=0.85,
          cullDistance=1500, wind=0.4, randomTilt=8, alignToNormal=0.15, seed=72, subsurface=0.4),
        L(rocks, density=0.008, scaleMin=1.5, scaleMax=4.0, slopeMax=75, terrainLayer=CLIFF, clumping=0.7, cullDistance=600,
          alignToNormal=0.9, sink=0.5, wind=0, seed=73),
    ])
    cir.call("foliage_add", entity="Valley", name="Meadow", seed=9, layers=[
        {"preset": "meadow_grass", "density": 22, "color": "#4f7a2a", "terrainLayer": MEADOW, "heightMin": WATER_Y + 0.3,
         "cullDistance": 90, "colorVariation": 0.5},
        {"preset": "tall_grass", "density": 3, "color": "#6f8a36", "terrainLayer": MEADOW, "heightMin": WATER_Y + 0.3,
         "cullDistance": 110, "clumping": 0.8},
        {"preset": "meadow_grass", "density": 10, "color": "#56742e", "terrainLayer": BANK, "cullDistance": 80},
        L(grass_m, density=2.5, scaleMin=0.7, scaleMax=1.4, slopeMax=26, terrainLayer=MEADOW, clumping=0.5, heightMin=WATER_Y + 0.3,
          cullDistance=70, wind=1.0, castShadows=False, subsurface=0.6),
        L(grass_m2, density=1.6, scaleMin=0.7, scaleMax=1.3, slopeMax=26, terrainLayer=MEADOW, clumping=0.6, heightMin=WATER_Y + 0.3,
          cullDistance=65, wind=1.0, castShadows=False, seed=21, subsurface=0.6),
        L(celandine, density=0.35, scaleMin=0.8, scaleMax=1.3, slopeMax=24, terrainLayer=MEADOW, clumping=0.9, heightMin=WATER_Y + 0.5,
          cullDistance=60, wind=0.9, castShadows=False, seed=22, subsurface=0.5),
        L(periwinkle, density=0.3, scaleMin=0.8, scaleMax=1.4, slopeMax=24, terrainLayer=MEADOW, clumping=0.9, heightMin=WATER_Y + 0.5,
          cullDistance=60, wind=0.8, castShadows=False, seed=23, subsurface=0.5),
        L(dandelion, density=0.2, scaleMin=0.8, scaleMax=1.2, slopeMax=24, terrainLayer=MEADOW, clumping=0.8, heightMin=WATER_Y + 0.5,
          cullDistance=55, wind=1.1, castShadows=False, seed=24, subsurface=0.5),
        {"preset": "flowers", "density": 0.8, "terrainLayer": MEADOW, "heightMin": WATER_Y + 0.5, "cullDistance": 70},
        {"preset": "ferns", "density": 0.5, "terrainLayer": BANK, "cullDistance": 70},
        {"preset": "beach_pebbles", "density": 0.6, "terrainLayer": 5, "cullDistance": 45, "color": "#7d7a70"},
    ])


MOODS = {
    "sunrise": dict(
        skyMode="atmosphere", sunElevation=7, sunAzimuth=180, sunIntensity=3.2, sunColor="#ffdcb0", clouds=0.42,
        cloudHeight=380, cloudThickness=900, cloudScale=1.6, cloudDensity=0.8, cloudSpeed=5,
        fogDensity=0.0014, fogHeight=0.055, fogColor="#b8c4d2", haze=0.0015, godRays=1.0,
        ambient=0.45, autoExposure=True, exposureCompensation=0.1, tonemap="agx", look="golden_hour", lookStrength=0.25,
        saturation=1.08, contrast=1.06, bloomIntensity=0.35, vignette=0.22, grain=0.05, showGrid=False, windSpeed=3,
        windDirection=200, gi=1, ssr=1, ao=1.0, shadowDistance=900, shadowSoftness=1.1, temperature=0.0),
    "sunset": dict(
        skyMode="atmosphere", sunElevation=4, sunAzimuth=255, sunIntensity=4.2, sunColor="#ff9a58", clouds=0.68,
        cloudHeight=520, cloudThickness=1600, cloudScale=1.9, cloudDensity=1.5, cloudSpeed=12,
        fogDensity=0.0016, fogHeight=0.02, fogColor="#8d7f86", haze=0.0018, godRays=1.4,
        ambient=0.32, autoExposure=True, exposureCompensation=-0.1, tonemap="agx", look="teal_orange", lookStrength=0.45,
        saturation=1.12, contrast=1.14, bloomIntensity=0.45, vignette=0.3, grain=0.06, showGrid=False, windSpeed=7,
        windDirection=230, gi=1, ssr=1, ao=1.0, shadowDistance=900, shadowSoftness=1.0, temperature=0.12),
}


if __name__ == "__main__":
    os.makedirs(PROJECT, exist_ok=True)
    st = Studio(PROJECT)
    try:
        build(st)
    finally:
        st.close()
